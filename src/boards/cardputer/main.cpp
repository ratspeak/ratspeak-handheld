// =============================================================================
// rsCardputer Standalone — Main Entry Point
// C1-C7: Radio, Keyboard, Display, Reticulum, Nodes, WiFi, LXMF
// =============================================================================

#include <Arduino.h>
#include <M5Unified.h>
#include <M5Cardputer.h>
#include <utility/PI4IOE5V6408_Class.hpp>
#include <SPI.h>

#include "config/BoardConfig.h"
#include "config/Config.h"
#include "radio/SX1262.h"
#include "hal/Keyboard.h"
#include "input/HotkeyManager.h"
#include "UIManager.h"
#include "screens/BootScreen.h"
#include "screens/HomeScreen.h"
#include "storage/FlashStore.h"
#include "transport/LoRaInterface.h"
#include "storage/SDStore.h"
#include "storage/MessageStore.h"
#include "reticulum/AnnounceManager.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/IdentityManager.h"
#include "protocol/ProtocolRuntime.h"
#include "transport/WiFiInterface.h"
#include "transport/TCPClientInterface.h"
#include "transport/RnsAutoInterface.h"
#include "config/UserConfig.h"
#include <esp_netif.h>
#include "screens/NodesScreen.h"
#include "screens/MessagesScreen.h"
#include "screens/MessageView.h"
#include "screens/SettingsScreen.h"
#include "screens/NameInputScreen.h"
#include "screens/DataCleanScreen.h"
#include "screens/HelpOverlay.h"
#include "screens/TimezoneScreen.h"
#include "power/PowerManager.h"
#include "audio/AudioNotify.h"
#include "transport/BLEStub.h"
#include "hal/GPSManager.h"
#include "platform/RsCardputerModeSwitch.h"
#include <Preferences.h>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>
#include <esp_system.h>
#include <freertos/task.h>

SET_LOOP_TASK_STACK_SIZE(16384);  // 16KB — needed for Ed25519 crypto in RNS boot

// --- Hardware ---
// Cardputer ADV display uses Arduino HSPI/SPI3 via M5GFX. Keep the external
// LoRa/SD bus on FSPI/SPI2 so display refreshes cannot reconfigure radio SPI.
SPIClass loraSPI(FSPI);
SX1262 radio(&loraSPI,
    LORA_CS, LORA_SCK, LORA_MOSI, LORA_MISO,
    LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
    LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);

// --- Subsystems ---
Keyboard keyboard;
HotkeyManager hotkeys;
UIManager ui;
FlashStore flash;
SDStore sdStore;
MessageStore messageStore;
LXMFManager lxmf;
// Single Protocol runtime (micro retired 2026-08-13): ProtocolRuntime runs over the FFI
// staticlib (announce/LXMF/link/resource live; surfaces gate on protocolReady).
ProtocolRuntime protocolRuntime;
ProtocolBackend* backend = &protocolRuntime;
LoRaInterface rustLoraIface(&radio);  // pump-owned raw driver (id 0)
IdentityManager identityMgr;
AnnounceManager* announceManager = nullptr;
WiFiInterface* wifiImpl = nullptr;
std::vector<TCPClientInterface*> tcpClients;
std::list<TCPClientInterface*> retiredTcpClients;
RnsAutoInterface autoIface;  // device-owned driver on the rust pump (iface id 5)
bool autoIfaceDeferredStart = false;
unsigned long autoIfaceDeferredAt = 0;
unsigned long lastAutoIfaceLinkCheck = 0;
UserConfig userConfig;
PowerManager power;
AudioNotify audio;
BLEStub ble;
#if HAS_GPS
GPSManager gps;
#endif

// --- Screens ---
BootScreen bootScreen;
HomeScreen homeScreen;
NodesScreen nodesScreen;
MessagesScreen messagesScreen;
MessageView messageView;
NameInputScreen nameInputScreen;
DataCleanScreen dataCleanScreen;
SettingsScreen settingsScreen;
TimezoneScreen timezoneScreen;
HelpOverlay helpOverlay;

// Tab-screen mapping
Screen* tabScreens[4] = {nullptr, nullptr, nullptr, nullptr};

// --- State ---
bool radioOnline = false;
bool bootComplete = false;
volatile bool pendingMessageSound = false;  // Deferred audio from packet callback
bool bootLoopRecovery = false;
bool wifiSTAStarted = false;
bool wifiSTAConnected = false;

// STA reconnects are scheduled from WiFi events and fired from loop().
std::atomic<bool> wifiNeedsReconnect{false};
std::atomic<unsigned long> wifiReconnectAt{0};
std::atomic<uint8_t> wifiReconnectAttempt{0};
constexpr unsigned long WIFI_BACKOFF_MS[4] = {5000, 15000, 60000, 300000};
constexpr unsigned long WIFI_NETIF_SETTLE_MS = 1500;

// --- Timing state (millis-based throttling) ---
unsigned long lastRNS = 0;
unsigned long lastRender = 0;
unsigned long lastAutoAnnounce = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastStatusUpdate = 0;
unsigned long loopCycleStart = 0;
unsigned long maxLoopTime = 0;

// --- Intervals ---
constexpr unsigned long RNS_INTERVAL_MS = 10;         // 100 Hz (matches rsDeck)
constexpr unsigned long RENDER_INTERVAL_MS = 50;       // 20 FPS
constexpr unsigned long STATUS_UPDATE_MS = 1000;       // 1 Hz status bar
constexpr unsigned long ANNOUNCE_INTERVAL_MS = 120000; // 2 minutes
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 5000;
constexpr unsigned long TCP_GLOBAL_BUDGET_MS = 12;      // Max cumulative TCP time per loop

// Power-aware RNS interval
unsigned long rnsInterval = RNS_INTERVAL_MS;

static void scheduleWiFiReconnect() {
    uint8_t attempt = wifiReconnectAttempt.load();
    uint8_t idx = attempt < 4 ? attempt : 3;
    unsigned long backoff = WIFI_BACKOFF_MS[idx];
    if (backoff < WIFI_NETIF_SETTLE_MS) backoff = WIFI_NETIF_SETTLE_MS;
    wifiReconnectAt.store(millis() + backoff);
    wifiNeedsReconnect.store(true);
    if (attempt < 4) wifiReconnectAttempt.store(attempt + 1);
}

static void onWiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (wifiNeedsReconnect.load()) break;
            scheduleWiFiReconnect();
            WiFi.disconnect(false, true);
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
            wifiNeedsReconnect.store(false);
            wifiReconnectAttempt.store(0);
            break;
        default:
            break;
    }
}

// Cardputer connects to a single STA network: the selected slot of the core
// multi-network model (created by the lite Settings UI).
static const WiFiNetwork& staNetworkRO() {
    static const WiFiNetwork kEmpty;
    const auto& s = userConfig.settings();
    if (s.wifiSTANetworks.empty()) return kEmpty;
    size_t idx = s.wifiSTASelected < s.wifiSTANetworks.size() ? s.wifiSTASelected : 0;
    return s.wifiSTANetworks[idx];
}

static void beginSTAConnection() {
    auto& s = userConfig.settings();
    const WiFiNetwork& net = staNetworkRO();
    if (net.ssid.isEmpty()) return;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    if (s.autoIfaceEnabled) WiFi.enableIpV6();
    WiFi.begin(net.ssid.c_str(), net.password.c_str());
    wifiSTAStarted = true;
    Serial.printf("[WIFI] STA begin (SSID: %s)\n", net.ssid.c_str());
}

static const char* currentPosixTZ() {
    if (userConfig.settings().timezoneIdx < TIMEZONE_COUNT) {
        return TIMEZONE_TABLE[userConfig.settings().timezoneIdx].posixTZ;
    }
    return "EST5EDT,M3.2.0,M11.1.0";  // Fallback (core config has no raw UTC offset)
}

// =============================================================================
// TCP client management — stop old clients, create new from config
// =============================================================================

static void drainRetiredTCPClients() {
    for (auto it = retiredTcpClients.begin(); it != retiredTcpClients.end(); ) {
        TCPClientInterface* tcp = *it;
        if (!tcp || tcp->canDestroy()) {
            if (tcp) delete tcp;
            it = retiredTcpClients.erase(it);
        } else {
            ++it;
        }
    }
}

static void retireTCPClient(TCPClientInterface* tcp) {
    if (!tcp) return;
    tcp->stop();
    if (tcp->canDestroy()) {
        delete tcp;
    } else {
        retiredTcpClients.push_back(tcp);
    }
}

static void reloadTCPClients() {
    // Stop and deregister existing clients
    protocolRuntime.pump().detachTcpAll();
    for (auto* tcp : tcpClients) {
        retireTCPClient(tcp);
    }
    tcpClients.clear();
    drainRetiredTCPClients();

    // Create new clients from current config
    if (WiFi.status() == WL_CONNECTED) {
        for (auto& ep : userConfig.settings().tcpConnections) {
            if (ep.autoConnect && !ep.host.isEmpty()) {
                char name[32];
                snprintf(name, sizeof(name), "TCP.%s", ep.host.c_str());
                auto* tcp = new TCPClientInterface(ep.host.c_str(), ep.port, name);
                if (protocolRuntime.pump().attachTcp(tcp) < 0) {
                    Serial.printf("[TCP] Pump full; dropping %s:%d\n", ep.host.c_str(), ep.port);
                    delete tcp;
                    continue;
                }
                tcp->start();
                tcpClients.push_back(tcp);
                Serial.printf("[TCP] Created client: %s:%d\n", ep.host.c_str(), ep.port);
            }
        }
    }

    if (tcpClients.empty()) {
        Serial.println("[TCP] No active TCP connections");
    }
}

// =============================================================================
// Hotkey callbacks
// =============================================================================

void onHotkeyHelp() {
    Serial.println("[HOTKEY] Help overlay");
    helpOverlay.toggle();
    ui.setOverlay(helpOverlay.isVisible() ? &helpOverlay : nullptr);
}
void onHotkeyMessages() {
    Serial.println("[HOTKEY] Jump to Messages");
    ui.tabBar().setActiveTab(TabBar::TAB_MSGS);
    ui.setScreen(&messagesScreen);
}
void onHotkeyNewMsg() {
    Serial.println("[HOTKEY] New message");
    ui.tabBar().setActiveTab(TabBar::TAB_MSGS);
    ui.setScreen(&messagesScreen);
}
void onHotkeySettings() {
    Serial.println("[HOTKEY] Jump to Settings");
    ui.tabBar().setActiveTab(TabBar::TAB_SETUP);
    ui.setScreen(&settingsScreen);
}
static ProtocolBackend::AnnounceResult announceWithName(bool silent = false);
void onHotkeyAnnounce() {
    Serial.println("[HOTKEY] Force announce");
    announceWithName();
}
void onHotkeyDiag() {
    Serial.println("=== DIAGNOSTIC DUMP ===");
    Serial.printf("Protocol: %s\n", backend->backendName());
    // Diagnostic state read through the backend facade (proves live delegation;
    // identical to rns.* since MicroReticulumBackend is pure delegation).
    Serial.printf("Identity: %s\n", backend->identityHash().c_str());
    Serial.printf("Node: %s (endpoint, no forwarding)\n", backend->isTransportActive() ? "ONLINE" : "OFFLINE");
    Serial.printf("Paths: %d  Links: %d\n", (int)backend->pathCount(), (int)backend->linkCount());
    Serial.printf("Delivery: %s  Resources: %u\n", backend->deliveryBackendDetail(),
                  (unsigned)backend->activeResourceTransfers());
    Serial.printf("Radio: %s\n", radioOnline ? "ONLINE" : "OFFLINE");
    if (radioOnline) {
        Serial.printf("Freq: %lu Hz  SF: %d  BW: %lu  CR: 4/%d  TXP: %d dBm\n",
                      (unsigned long)radio.getFrequency(),
                      radio.getSpreadingFactor(),
                      (unsigned long)radio.getSignalBandwidth(),
                      radio.getCodingRate4(),
                      radio.getTxPower());
        Serial.printf("Regulator: %s\n", LORA_USE_DCDC_REGULATOR ? "DC-DC" : "LDO");
        Serial.printf("Preamble: %ld symbols\n", radio.getPreambleLength());
        Serial.printf("IQ invert: %s\n", radio.getInvertIQ() ? "ON" : "off");
        Serial.printf("SyncWord regs: 0x%02X%02X\n",
            radio.readRegister(REG_SYNC_WORD_MSB_6X),
            radio.readRegister(REG_SYNC_WORD_LSB_6X));
        uint16_t devErr = radio.getDeviceErrors();
        uint8_t status = radio.getStatus();
        Serial.printf("DevErrors: 0x%04X  Status: 0x%02X (mode=%d cmd=%d)\n",
            devErr, status, (status >> 4) & 0x07, (status >> 1) & 0x07);
        if (devErr & 0x40) Serial.println("  *** PLL LOCK FAILED ***");
        Serial.printf("Current RSSI: %d dBm\n", radio.currentRssi());

        Serial.println("--- SX1262 Register Dump ---");
        Serial.printf("  0x0740 (SyncWordMSB): 0x%02X\n", radio.readRegister(0x0740));
        Serial.printf("  0x0741 (SyncWordLSB): 0x%02X\n", radio.readRegister(0x0741));
        Serial.printf("  0x0889 (IQ polarity): 0x%02X\n", radio.readRegister(0x0889));
        Serial.printf("  0x0736 (IQ config):   0x%02X\n", radio.readRegister(0x0736));
        Serial.printf("  0x08AC (LNA):         0x%02X\n", radio.readRegister(0x08AC));
        Serial.printf("  0x08E7 (OCP):         0x%02X\n", radio.readRegister(0x08E7));
        Serial.printf("  0x08D8 (TX clamp):    0x%02X\n", radio.readRegister(REG_TX_CLAMP_CONFIG_6X));
        uint8_t packetType = radio.getPacketType();
        const char* packetTypeName =
            (packetType == 0x00) ? "GFSK" :
            (packetType == 0x01) ? "LoRa" :
            (packetType == 0x02) ? "LR-FHSS" : "unknown";
        Serial.printf("  packet_type:          0x%02X (%s)%s\n",
            packetType, packetTypeName,
            packetType == 0x01 ? "" : " *** NOT LoRa ***");
        Serial.println("----------------------------");
    }
    Serial.printf("Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf("Flash: used/total (see heartbeat for heap)\n");
    Serial.printf("WriteQ pending: %d\n", messageStore.writeQueue().drainCount());
    Serial.printf("Uptime: %lu s\n", millis() / 1000);
    Serial.println("=======================");
}

// Ctrl+R: Continuous RSSI sampling for 5 seconds
volatile bool rssiMonitorActive = false;

void onHotkeyRssiMonitor() {
    if (!radioOnline) {
        Serial.println("[RSSI] Radio offline");
        return;
    }
    Serial.println("[RSSI] Sampling RSSI for 5 seconds — transmit from another device now...");
    rssiMonitorActive = true;
    int minRssi = 0, maxRssi = -200;
    unsigned long start = millis();
    int samples = 0;
    while (millis() - start < 5000) {
        int rssi = radio.currentRssi();
        if (rssi < minRssi) minRssi = rssi;
        if (rssi > maxRssi) maxRssi = rssi;
        samples++;
        Serial.printf("[RSSI] %d dBm\n", rssi);
        delay(100);
    }
    rssiMonitorActive = false;
    Serial.printf("[RSSI] Done: %d samples, min=%d max=%d dBm\n", samples, minRssi, maxRssi);
    Serial.printf("[RSSI] If max stayed near %d dBm, RX front-end may not be receiving RF\n", minRssi);
}

void onHotkeyRadioTest() {
    Serial.println("[TEST] Sending raw radio test packet...");
    uint8_t header = 0xA0;
    const char* testPayload = "RSCARDPUTER_TEST_1234567890";
    size_t totalLen = 1 + strlen(testPayload);

    radio.beginPacket();
    radio.write(header);
    radio.write((const uint8_t*)testPayload, strlen(testPayload));
    bool ok = radio.endPacket();

    Serial.printf("[TEST] TX %s (%d bytes)\n", ok ? "OK" : "FAILED", (int)totalLen);

    uint8_t verify[32] = {0};
    radio.readBuffer(verify, totalLen);
    Serial.printf("[TEST] FIFO verify: ");
    for (size_t i = 0; i < totalLen; i++) Serial.printf("%02X ", verify[i]);
    Serial.println();

    radio.receive();
}

// =============================================================================
// Announce with display name
// =============================================================================

// LXMF announce app_data:
//   [display_name(bin), stamp_cost(nil|uint), supported_functionality(array)]
// Always emit fixarray(3) so Python LXMF doesn't default auto_compress=True for
// our destinations. stamp_cost=nil means no inbound stamp is required. Empty
// supported_functionality list = we do NOT support SF_COMPRESSION (bz2).
rs::Bytes encodeAnnounceName(const String& name) {
    size_t nameLen = name.length();
    if (nameLen > 31) nameLen = 31;
    uint8_t buf[5 + 31];
    size_t i = 0;
    buf[i++] = 0x93;                   // fixarray(3)
    buf[i++] = 0xC4;                   // bin 8
    buf[i++] = (uint8_t)nameLen;
    if (nameLen) { memcpy(buf + i, name.c_str(), nameLen); i += nameLen; }
    buf[i++] = 0xC0;                   // stamp_cost = nil (no stamp required)
    buf[i++] = 0x90;                   // empty fixarray (no SF_* supported)
    return rs::Bytes(buf, i);
}

static ProtocolBackend::AnnounceResult announceWithName(bool silent) {
    // Honest runtime gate: real announce once the backend is up (protocolReady).
    if (!backend->protocolReady()) {
        (void)silent;
        Serial.println("[ANNOUNCE-TX] skipped: rust backend protocol not ready");
        return ProtocolBackend::AnnounceResult::Failed;
    }
    rs::Bytes appData = encodeAnnounceName(userConfig.settings().displayName);
    Serial.printf("[ANNOUNCE-TX] name=\"%s\" appData=%d bytes silent=%s\n",
        userConfig.settings().displayName.c_str(), (int)appData.size(),
        silent ? "yes" : "no");
    const auto result = backend->announce(appData.data(), appData.size());
    if (!silent && result == ProtocolBackend::AnnounceResult::Sent) {
        ui.statusBar().flashAnnounce();
    }
    Serial.println(result == ProtocolBackend::AnnounceResult::Sent
                       ? "[ANNOUNCE-TX] accepted"
                       : (result == ProtocolBackend::AnnounceResult::Deferred
                              ? "[ANNOUNCE-TX] queued"
                              : "[ANNOUNCE-TX] not accepted"));
    return result;
}

static bool enableCapLoRaRfSwitch() {
    if (!m5::In_I2C.isEnabled()) {
        if (!m5::In_I2C.begin(I2C_NUM_0, KB_SDA, KB_SCL)) {
            Serial.println("[RADIO] Cap LoRa-1262 IOE init failed: I2C unavailable");
            return false;
        }
    }

    m5::PI4IOE5V6408_Class ioe(LORA_CAP_IOE_ADDR, 400000, &m5::In_I2C);
    if (!ioe.begin()) {
        Serial.println("[RADIO] Cap LoRa-1262 IOE not detected; RF switch enable skipped");
        return false;
    }

    ioe.setDirection(LORA_CAP_RF_SW_PIN, true);
    ioe.setHighImpedance(LORA_CAP_RF_SW_PIN, false);
    ioe.digitalWrite(LORA_CAP_RF_SW_PIN, true);
    delay(5);
    Serial.println("[RADIO] Cap LoRa-1262 RF antenna switch enabled (IOE P0=HIGH)");
    return true;
}

static void cycleDiagnosticTxPower() {
    static constexpr int8_t kPowers[] = {-9, -3, 0, 2, 6, 10, 14, 17, LORA_MAX_TX_POWER};
    int current = radio.getTxPower();
    size_t next = 0;
    for (size_t i = 0; i < sizeof(kPowers) / sizeof(kPowers[0]); i++) {
        if (current == kPowers[i]) {
            next = (i + 1) % (sizeof(kPowers) / sizeof(kPowers[0]));
            break;
        }
    }

    radio.setTxPower(kPowers[next]);
    radio.receive();
    Serial.printf("[SERIAL] transient TX power set to %d dBm\n", (int)kPowers[next]);
}

static void setDiagnosticMinTxPower() {
    radio.setTxPower(-9);
    radio.receive();
    Serial.println("[SERIAL] transient TX power set to -9 dBm");
}

static bool setDiagnosticTxPower(int powerDbm) {
    if (powerDbm < -9 || powerDbm > LORA_MAX_TX_POWER) {
        Serial.printf("[SERIAL] TX power out of range: %d dBm (allowed -9..%d)\n",
                      powerDbm, (int)LORA_MAX_TX_POWER);
        return false;
    }
    radio.setTxPower((int8_t)powerDbm);
    radio.receive();
    Serial.printf("[SERIAL] transient TX power set to %d dBm\n", powerDbm);
    return true;
}

static void toggleDiagnosticInvertIQ() {
    radio.setInvertIQ(!radio.getInvertIQ());
    radio.receive();
    Serial.printf("[SERIAL] IQ inversion %s\n", radio.getInvertIQ() ? "ON" : "off");
}

static bool setDiagnosticFrequency(uint32_t frequencyHz) {
    if (frequencyHz < 150000000UL || frequencyHz > 960000000UL) {
        Serial.printf("[SERIAL] frequency out of range: %lu Hz (allowed 150000000..960000000)\n",
                      (unsigned long)frequencyHz);
        return false;
    }
    radio.setFrequency(frequencyHz);
    radio.receive();
    Serial.printf("[SERIAL] transient frequency set to %lu Hz\n", (unsigned long)frequencyHz);
    return true;
}

static void nudgeDiagnosticFrequency(int32_t deltaHz) {
    uint32_t next = radio.getFrequency() + deltaHz;
    radio.setFrequency(next);
    radio.receive();
    Serial.printf("[SERIAL] transient frequency set to %lu Hz\n", (unsigned long)next);
}

static const char* skipSerialSeparators(const char* p) {
    while (p && (*p == ' ' || *p == '\t' || *p == ':' || *p == '=' || *p == ',')) {
        ++p;
    }
    return p;
}

static bool hasSerialArgument(const char* p) {
    p = skipSerialSeparators(p);
    return p && *p != '\0';
}

static bool parseSerialLong(const char* p, long& value, const char** rest = nullptr) {
    p = skipSerialSeparators(p);
    if (!p || *p == '\0') return false;
    char* end = nullptr;
    value = std::strtol(p, &end, 10);
    if (end == p) return false;
    if (rest) *rest = end;
    return true;
}

static bool parseSerialDestinationHash(const char* p, rs::Bytes& hash) {
    p = skipSerialSeparators(p);
    if (!p || *p == '\0') return false;

    char hex[33] = {0};
    size_t len = 0;
    while (*p && len < 32) {
        unsigned char ch = (unsigned char)*p;
        if (std::isxdigit(ch)) {
            hex[len++] = (char)*p;
        } else if (*p != ' ' && *p != '\t' && *p != ':' && *p != '=' && *p != ',' && *p != '-') {
            return false;
        }
        ++p;
    }

    if (len != 32) return false;
    hash.assignHex(hex);
    return hash.size() == 16;
}

static bool selectDiagnosticPeer(const char* explicitArg, rs::Bytes& destHash, std::string& label) {
    if (hasSerialArgument(explicitArg)) {
        if (!parseSerialDestinationHash(explicitArg, destHash)) {
            Serial.println("[SERIAL] invalid LXMF destination hash; expected 32 hex characters");
            return false;
        }
        label = destHash.toHex();
        return true;
    }

    if (!announceManager) {
        Serial.println("[SERIAL] LXMF test failed: announce manager is not ready");
        return false;
    }

    const std::string localHex = backend->destinationHashHex().c_str();
    for (const auto& node : announceManager->nodes()) {
        if (node.hash.size() != 16) continue;
        const std::string nodeHex = node.hash.toHex();
        if (nodeHex == localHex) continue;
        destHash = node.hash;
        label = node.name.empty() ? nodeHex : (node.name + " " + nodeHex);
        return true;
    }

    Serial.println("[SERIAL] LXMF test failed: no peer known; send/receive announces first or pass a hash");
    return false;
}

static std::string makeDiagnosticLxmfPayload(size_t length) {
    static constexpr char kPrefix[] = "RSCARDPUTER-LXMF-TEST:";
    static constexpr char kPattern[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    std::string out;
    out.reserve(length);
    for (size_t i = 0; kPrefix[i] && out.size() < length; ++i) {
        out.push_back(kPrefix[i]);
    }
    for (size_t i = 0; out.size() < length; ++i) {
        out.push_back(kPattern[i % (sizeof(kPattern) - 1)]);
    }
    return out;
}

static bool sendDiagnosticLxmf(size_t length, const char* explicitDest) {
    if (!backend->protocolReady()) {
        (void)length; (void)explicitDest;
        Serial.println("[SERIAL] LXMF test failed: rust backend protocol not ready");
        return false;
    }
    static constexpr size_t kMaxDiagnosticLxmfChars = 512;
    if (length == 0 || length > kMaxDiagnosticLxmfChars) {
        Serial.printf("[SERIAL] LXMF test length out of range: %u (allowed 1..%u)\n",
                      (unsigned)length, (unsigned)kMaxDiagnosticLxmfChars);
        return false;
    }

    rs::Bytes destHash;
    std::string peerLabel;
    if (!selectDiagnosticPeer(explicitDest, destHash, peerLabel)) return false;

    std::string payload = makeDiagnosticLxmfPayload(length);
    bool ok = backend->lxmfSendMessage(destHash.data(), payload.c_str(), "", false);
    Serial.printf("[SERIAL] LXMF test %s: len=%u dest=%s queue=%d\n",
                  ok ? "queued" : "rejected",
                  (unsigned)payload.size(),
                  peerLabel.c_str(),
                  backend->lxmfQueuedCount());
    return ok;
}

static void handleSerialLineCommand(const char* line) {
    if (!line || !*line) return;

    switch (line[0]) {
        case 'F': {
            long value = 0;
            if (!parseSerialLong(line + 1, value) || value < 0) {
                Serial.println("[SERIAL] usage: F<frequency_hz>, for example F915000000");
                return;
            }
            setDiagnosticFrequency((uint32_t)value);
            break;
        }
        case 'P': {
            long value = 0;
            if (!parseSerialLong(line + 1, value)) {
                Serial.println("[SERIAL] usage: P<tx_power_dbm>, for example P1 or P5");
                return;
            }
            setDiagnosticTxPower((int)value);
            break;
        }
        case 'L': {
            long length = 0;
            const char* rest = nullptr;
            if (!parseSerialLong(line + 1, length, &rest) || length <= 0) {
                Serial.println("[SERIAL] usage: L<payload_chars> [dest_hash], for example L120");
                return;
            }
            sendDiagnosticLxmf((size_t)length, rest);
            break;
        }
        default:
            Serial.printf("[SERIAL] unknown line command '%c'\n", line[0]);
            break;
    }
}

static void printSerialHelp() {
    Serial.println("[SERIAL] commands: ? help | a announce | t raw-test | d diag | r rssi | p tx-power-cycle | m min-power | q iq | +/- freq | f rf-switch");
    Serial.println("[SERIAL] line commands: F<hz> exact-frequency | P<dBm> exact-tx-power | L<len> [dest_hash] LXMF test");
}

static void handleSerialCommands() {
    static char line[96];
    static size_t lineLen = 0;
    static bool lineActive = false;

    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (lineActive) {
            if (c == '\r' || c == '\n') {
                line[lineLen] = '\0';
                handleSerialLineCommand(line);
                lineLen = 0;
                lineActive = false;
                continue;
            }
            if (lineLen + 1 >= sizeof(line)) {
                Serial.println("[SERIAL] line command too long; discarded");
                lineLen = 0;
                lineActive = false;
                continue;
            }
            line[lineLen++] = c;
            continue;
        }

        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (c == 'F' || c == 'P' || c == 'L') {
            lineActive = true;
            lineLen = 0;
            line[lineLen++] = c;
            continue;
        }

        switch (c) {
            case '?':
                printSerialHelp();
                break;
            case 'a':
            case 'A':
                announceWithName(false);
                break;
            case 't':
            case 'T':
                onHotkeyRadioTest();
                break;
            case 'd':
            case 'D':
                onHotkeyDiag();
                break;
            case 'r':
            case 'R':
                onHotkeyRssiMonitor();
                break;
            case 'p':
                cycleDiagnosticTxPower();
                break;
            case 'm':
            case 'M':
                setDiagnosticMinTxPower();
                break;
            case 'q':
            case 'Q':
                toggleDiagnosticInvertIQ();
                break;
            case '+':
            case '=':
                nudgeDiagnosticFrequency(1000);
                break;
            case '-':
            case '_':
                nudgeDiagnosticFrequency(-1000);
                break;
            case 'f':
                enableCapLoRaRfSwitch();
                break;
            default:
                Serial.printf("[SERIAL] unknown command '%c'\n", c);
                printSerialHelp();
                break;
        }
    }
}

// =============================================================================
// Setup
// =============================================================================

void setup() {
    // Initialize M5Cardputer (includes M5Unified + keyboard)
    auto cfg = M5.config();
    // The PlatformIO target is the generic ESP32-S3 devkit, so display
    // auto-detection can return unknown. Without an explicit fallback,
    // M5Unified selects AtomS3Lite and M5Cardputer installs an empty keyboard
    // reader instead of the Cardputer ADV TCA8418 reader.
    cfg.fallback_board = m5::board_t::board_M5CardputerADV;
    cfg.serial_baudrate = SERIAL_BAUD;
    M5Cardputer.begin(cfg, true);

    Serial.println();
    Serial.println("=================================");
    Serial.printf("  rsCardputer Standalone v%s\n", RSCARDPUTER_VERSION_STRING);
    Serial.println("  M5Stack Cardputer Adv");
    Serial.printf("  Protocol: %s\n", backend->backendName());
    Serial.println("=================================");

    auto launcherBoot = rs_cardputer_adv::returnToLauncherNextBoot();
    if (!launcherBoot.ok) {
        Serial.printf("[BOOT] Launcher return unavailable: %s\n", launcherBoot.message);
    }

    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "UNKNOWN";
    switch (reason) {
        case ESP_RST_POWERON:   reasonStr = "POWER_ON"; break;
        case ESP_RST_SW:        reasonStr = "SOFTWARE"; break;
        case ESP_RST_PANIC:     reasonStr = "PANIC (crash!)"; break;
        case ESP_RST_INT_WDT:   reasonStr = "INT_WDT (interrupt watchdog!)"; break;
        case ESP_RST_TASK_WDT:  reasonStr = "TASK_WDT (task watchdog!)"; break;
        case ESP_RST_WDT:       reasonStr = "WDT (other watchdog!)"; break;
        case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT (low voltage!)"; break;
        case ESP_RST_DEEPSLEEP: reasonStr = "DEEP_SLEEP"; break;
        default: break;
    }
    Serial.printf("[BOOT] Reset reason: %s (%d)\n", reasonStr, (int)reason);
    Serial.printf("[BOOT] Free heap: %lu, min ever: %lu\n",
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());

    // Initialize UI + boot screen
    ui.begin();
    ui.setBootMode(true);
    ui.setScreen(&bootScreen);
    bootScreen.setProgress(0.1f, "Display ready");
    ui.render();

    // Initialize keyboard
    keyboard.begin();
    bootScreen.setProgress(0.15f, "Keyboard ready");
    ui.render();

    // Register hotkeys
    hotkeys.registerHotkey('h', "Help", onHotkeyHelp);
    hotkeys.registerHotkey('m', "Messages", onHotkeyMessages);
    hotkeys.registerHotkey('n', "New Message", onHotkeyNewMsg);
    hotkeys.registerHotkey('s', "Settings", onHotkeySettings);
    hotkeys.registerHotkey('a', "Announce", onHotkeyAnnounce);
    hotkeys.registerHotkey('d', "Diagnostics", onHotkeyDiag);
    hotkeys.registerHotkey('t', "Radio Test", onHotkeyRadioTest);
    hotkeys.registerHotkey('r', "RSSI Monitor", onHotkeyRssiMonitor);
    hotkeys.setTabCycleCallback([](int dir) {
        ui.tabBar().cycleTab(dir);
        int tab = ui.tabBar().getActiveTab();
        if (tabScreens[tab]) {
            ui.setScreen(tabScreens[tab]);
        }
    });
    bootScreen.setProgress(0.2f, "Hotkeys registered");
    ui.render();

    // Initialize flash storage
    bootScreen.setProgress(0.25f, "Mounting flash...");
    ui.render();
    if (!flash.begin()) {
        Serial.println("[BOOT] Flash init failed!");
        bootScreen.setProgress(0.25f, "Storage error - data preserved");
        ui.render();
        while (true) delay(20);
    }
    bootScreen.setProgress(0.3f, "Storage ready");
    ui.render();

    // Boot loop detection (NVS — separate from LittleFS)
    {
        Preferences prefs;
        if (prefs.begin("ratcom", false)) {
            int bc = prefs.getInt("bootc", 0);
            prefs.putInt("bootc", bc + 1);
            prefs.end();
            if (bc >= 3) {
                Serial.printf("[BOOT] Boot loop detected (%d consecutive failures)\n", bc);
                Serial.println("[BOOT] Falling back to WiFi OFF for safe boot");
                bootLoopRecovery = true;
            }
        }
    }

    // Initialize radio
    bootScreen.setProgress(0.4f, "Starting radio...");
    ui.render();
    enableCapLoRaRfSwitch();
    if (radio.begin(LORA_DEFAULT_FREQ)) {
        radio.setSpreadingFactor(LORA_DEFAULT_SF);
        radio.setSignalBandwidth(LORA_DEFAULT_BW);
        radio.setCodingRate4(LORA_DEFAULT_CR);
        radio.setTxPower(LORA_DEFAULT_TX_POWER);
        radio.setPreambleLength(LORA_DEFAULT_PREAMBLE);
        radio.receive();
        radioOnline = true;
        ui.statusBar().setLoRaOnline(true);
        Serial.println("[RADIO] SX1262 online at 915 MHz");
        bootScreen.setProgress(0.6f, "Radio online");
    } else {
        Serial.println("[RADIO] SX1262 not detected!");
        bootScreen.setProgress(0.6f, "Radio: OFFLINE");
    }
    ui.render();

    // Initialize SD card (shares FSPI/SPI2 with radio — must init after radio)
    bootScreen.setProgress(0.65f, "Checking SD card...");
    ui.render();
    if (sdStore.begin(&loraSPI, SD_CS)) {
        sdStore.ensureDir("/ratcom");
        sdStore.ensureDir("/ratcom/messages");
        sdStore.ensureDir("/ratcom/contacts");
        sdStore.ensureDir("/ratcom/identity");
        bootScreen.setProgress(0.68f, "SD card ready");
    } else {
        bootScreen.setProgress(0.68f, "No SD card");
    }
    ui.render();

    // Initialize Reticulum
    bootScreen.setProgress(0.7f, "Starting Reticulum...");
    ui.render();
    // Protocol runtime boots after the stores (identity slots + message store).
    Serial.printf("[BOOT] Protocol runtime selected — FFI %s\n", ProtocolRuntime::versionString());
    bootScreen.setProgress(0.9f, "Starting Reticulum");
    ui.render();

    // Identity slots (imports the legacy flash identity as slot 0 on first boot)
    if (!identityMgr.begin(&flash, &sdStore)) {
        bootScreen.setProgress(0.9f, "Identity error - data preserved");
        ui.render();
        for (;;) delay(20);
    }

    // Load user config before MessageStore — sdStorageEnabled gates the SD tier
    // (legacy /ratcom installs default it on via BOARD_DEFAULT_SD_STORAGE).
    userConfig.load(sdStore, flash);
    if (userConfig.recoveryRequired()) {
        bootScreen.setProgress(0.9f, "Settings error - data preserved");
        ui.render();
        for (;;) delay(20);
    }

    // Initialize message store + LXMF
    bootScreen.setProgress(0.91f, "Starting messaging...");
    ui.render();
    messageStore.begin(&flash, &sdStore, userConfig.settings().sdStorageEnabled);
    // Protocol runtime lifecycle: init -> identity -> boot-seed ->
    // placement open_transport (MICRO node in internal heap) -> pump. LXMF
    // runs store-only so the UI read surface works; sends go through the backend engines.
    if (protocolRuntime.begin(&flash, &sdStore, &identityMgr, &messageStore, nullptr,
                          RS_HANDHELD_PROFILE_MICRO,
                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
        protocolRuntime.pump().attachLoRa(&rustLoraIface);
        protocolRuntime.pump().attachAuto(&autoIface);  // driver starts on SLAAC (iface 5)
        if (radioOnline) rustLoraIface.start();
        Serial.printf("[BOOT] Rust transport up: dest=%s\n",
                      protocolRuntime.destinationHashHex().c_str());
        bootScreen.setProgress(0.92f, "Reticulum ready");
    } else {
        bootScreen.setProgress(0.92f, "Messaging error - data preserved");
        ui.render();
        for (;;) delay(20);
    }
    lxmf.beginStoreOnly(&messageStore);
    // Inbound notify (both envs: micro delegates to lxmf, rust fires from the
    // engines on every delivery path — opportunistic, link packet, resource).
    backend->setMessageCallback([](const LXMFMessage& msg) {
        // This runs in the packet callback context — MUST be non-blocking.
        // No disk I/O, no delays, no heavy computation.
        Serial.printf("[LXMF] New message from %s\n", msg.sourceHash.toHex().substr(0, 8).c_str());
        ui.tabBar().setUnreadCount(TabBar::TAB_MSGS, lxmf.unreadCount());
        ui.markContentDirty();
        ui.markTabDirty();
        messagesScreen.notifyNewMessage();
        messageView.notifyNewMessage(msg);
        pendingMessageSound = true;  // Played from main loop — delay() in callback blocks transport
    });

    // Status callback — update UI when send completes (SENT/FAILED)
    backend->setStatusCallback([](const std::string& peerHex, double timestamp, uint32_t, LXMFStatus status) {
        messageView.notifyStatusChange(peerHex, timestamp, status);
        ui.markContentDirty();
    });

    // Unread counts load lazily on first MessagesScreen open (deferred from boot)

    // Register announce handler
    bootScreen.setProgress(0.93f, "Starting discovery...");
    ui.render();
    // Filter to lxmf.delivery so we don't capture every aspect (lxmf.propagation,
    // nomadnetwork.node, etc.) from the same peer as separate "doubled" entries.
    announceManager = new AnnounceManager("lxmf.delivery");
    announceManager->setStorage(&sdStore, &flash);
    // Protocol runtime: wire the announce contact bridge + own-announce filter.
    announceManager->setLocalDestHash(rs::Bytes(protocolRuntime.localDestHash(), 16));
    protocolRuntime.setAnnounceManager(announceManager);
    {
        // Pre-announce path responses must carry the capability app_data (no-bz2 etc.).
        rs::Bytes seed = encodeAnnounceName(userConfig.settings().displayName);
        protocolRuntime.seedAnnounceAppData(seed.data(), seed.size());
    }
    announceManager->loadContacts();
    announceManager->loadNameCache();
    // Backends consumed the boot seeds above (dedup ids + pending requeue) — free them.
    messageStore.releaseStartupSeeds();

    // Sync display name between active identity slot and config.
    // The identity slot is the source of truth for the name.
    {
        String slotName;
        if (identityMgr.syncNameFromActive(slotName)) {
            if (!slotName.isEmpty()) {
                if (userConfig.settings().displayName != slotName) {
                    Serial.printf("[BOOT] Name from identity slot: '%s'\n", slotName.c_str());
                    userConfig.settings().displayName = slotName;
                    userConfig.save(sdStore, flash);
                }
            } else if (!userConfig.settings().displayName.isEmpty()) {
                // Slot has no name but config does — seed the slot (first boot migration)
                identityMgr.setDisplayName(identityMgr.activeIndex(),
                    userConfig.settings().displayName);
                Serial.printf("[BOOT] Seeded identity slot name: '%s'\n",
                    userConfig.settings().displayName.c_str());
            }
        }
    }

    // Seed default TCP hubs if no connections configured (off by default)
    if (userConfig.settings().tcpConnections.empty()) {
        TCPEndpoint ep1;
        ep1.host = "rns.ratspeak.org";
        ep1.port = 4242;
        ep1.autoConnect = false;
        userConfig.settings().tcpConnections.push_back(ep1);

        TCPEndpoint ep2;
        ep2.host = "1.ratspeak.org";
        ep2.port = 4141;
        ep2.autoConnect = false;
        userConfig.settings().tcpConnections.push_back(ep2);

        Serial.println("[CONFIG] Default TCP hubs seeded (off by default)");
    }

    // Boot loop recovery: force WiFi OFF to break crash cycle
    if (bootLoopRecovery) {
        userConfig.settings().wifiMode = RAT_WIFI_OFF;
        Serial.println("[BOOT] WiFi forced OFF (boot loop recovery)");
    }

    // Apply radio settings
    if (radioOnline) {
        auto& s = userConfig.settings();
        radio.setFrequency(s.loraFrequency);
        radio.setSpreadingFactor(s.loraSF);
        radio.setSignalBandwidth(s.loraBW);
        radio.setCodingRate4(s.loraCR);
        radio.setTxPower(s.loraTxPower);
        radio.receive();
        Serial.printf("[BOOT] Radio configured: %lu Hz, SF%d, BW%lu, CR4/%d, %d dBm\n",
                      (unsigned long)s.loraFrequency, s.loraSF,
                      (unsigned long)s.loraBW, s.loraCR, s.loraTxPower);
    }

    // Mode-based WiFi startup
    RatWiFiMode wifiMode = userConfig.settings().wifiMode;

    if (wifiMode == RAT_WIFI_AP) {
        bootScreen.setProgress(0.95f, "Starting WiFi AP...");
        ui.render();
        // Rust env: same device-owned AP server, pumped as iface 6 — no micro
        // Transport registration (GAP-4).
        wifiImpl = new WiFiInterface("WiFi.AP");
        if (!userConfig.settings().wifiAPSSID.isEmpty()) {
            wifiImpl->setAPCredentials(
                userConfig.settings().wifiAPSSID.c_str(),
                userConfig.settings().wifiAPPassword.c_str());
        }
        protocolRuntime.pump().attachWifiAp(wifiImpl);
        wifiImpl->start();

    } else if (wifiMode == RAT_WIFI_STA) {
        bootScreen.setProgress(0.95f, "WiFi STA starting...");
        ui.render();
        if (!staNetworkRO().ssid.isEmpty()) {
            WiFi.mode(WIFI_STA);
            WiFi.onEvent(onWiFiEvent);
            // AutoInterface needs an IPv6 link-local address.  Must be
            // enabled BEFORE WiFi.begin() so SLAAC starts on STA association.
            if (userConfig.settings().autoIfaceEnabled) {
                WiFi.enableIpV6();
                Serial.println("[WIFI] IPv6 enabled (AutoInterface ON)");
            }
            beginSTAConnection();
            if (WiFi.status() != WL_CONNECTED && !wifiNeedsReconnect.load()) {
                scheduleWiFiReconnect();
            }
        } else {
            Serial.println("[WIFI] STA mode but SSID empty — skipping");
        }
    } else {
        bootScreen.setProgress(0.95f, "WiFi disabled");
        ui.render();
        Serial.println("[WIFI] Disabled by config");
    }

    // BLE disabled
    Serial.println("[BLE] Disabled (stub — v1.1)");

    // Initialize GPS (Cap LoRa-1262 GNSS module)
#if HAS_GPS
    if (userConfig.settings().gpsTimeEnabled || userConfig.settings().gpsLocationEnabled) {
        gps.setPosixTZ(currentPosixTZ());
        gps.setLocationEnabled(userConfig.settings().gpsLocationEnabled);
        gps.begin();
        Serial.println("[GPS] GNSS module started");
    } else {
        Serial.println("[GPS] Disabled by config");
    }
#endif

    // Initialize power manager + audio
    power.begin();
    power.setDimTimeout(userConfig.settings().screenDimTimeout);
    power.setOffTimeout(userConfig.settings().screenOffTimeout);
    power.setBrightness(userConfig.settings().brightness);

    audio.setEnabled(userConfig.settings().audioEnabled);
    audio.setVolume(userConfig.settings().audioVolume);
    audio.begin();

    // Boot complete
    delay(200);
    bootScreen.setProgress(1.0f, "Ready");
    ui.render();
    audio.playBoot();

    bootComplete = true;
    ui.statusBar().setTransportMode("Ratspeak.org");

    // Set up screens
    homeScreen.setBackend(backend);
    homeScreen.setRadio(&radio);
    homeScreen.setUserConfig(&userConfig);
    homeScreen.setAnnounceCallback([]() {
        return announceWithName() == ProtocolBackend::AnnounceResult::Sent;
    });
    nodesScreen.setAnnounceManager(announceManager);
    nodesScreen.setNodeSelectedCallback([](const std::string& peerHex) {
        messageView.setPeerHex(peerHex);
        ui.tabBar().setActiveTab(TabBar::TAB_MSGS);
        ui.setScreen(&messageView);
    });
    nodesScreen.setNodeSaveCallback([](const std::string& peerHex, bool save) {
        if (save) announceManager->saveNode(peerHex);
        else announceManager->unsaveNode(peerHex);
    });
    messagesScreen.setLXMFManager(&lxmf);
    messagesScreen.setAnnounceManager(announceManager);
    messagesScreen.setBackend(backend);
    messagesScreen.setOpenCallback([](const std::string& peerHex) {
        messageView.setPeerHex(peerHex);
        ui.setScreen(&messageView);
    });
    messagesScreen.setAddContactCallback([](const std::string& peerHex) {
        if (announceManager) announceManager->saveNode(peerHex);
    });
    messageView.setLXMFManager(&lxmf);
    messageView.setBackend(backend);
    messageView.setAnnounceManager(announceManager);
    messageView.setBackCallback([]() {
        ui.setScreen(&messagesScreen);
    });
    messageView.setUnreadUpdateCallback([]() {
        ui.tabBar().setUnreadCount(TabBar::TAB_MSGS, lxmf.unreadCount());
        ui.markTabDirty();
    });

    settingsScreen.setUserConfig(&userConfig);
    settingsScreen.setFlashStore(&flash);
    settingsScreen.setSDStore(&sdStore);
    settingsScreen.setRadio(&radio);
    settingsScreen.setAudio(&audio);
    settingsScreen.setPower(&power);
    settingsScreen.setWiFi(wifiImpl);
    settingsScreen.setTCPClients(&tcpClients);
    settingsScreen.setBackend(backend);
    settingsScreen.setIdentityHash(backend->destinationHashHex());

    tabScreens[TabBar::TAB_HOME]  = &homeScreen;
    tabScreens[TabBar::TAB_MSGS]  = &messagesScreen;
    tabScreens[TabBar::TAB_NODES] = &nodesScreen;
    tabScreens[TabBar::TAB_SETUP] = &settingsScreen;

    // Data clean screen (first boot only — when SD has old data)
    dataCleanScreen.setDoneCallback([](bool wipe) {
        if (wipe) {
            Serial.println("[BOOT] User chose to wipe old data");
            dataCleanScreen.setStatus("Clearing old data...");
            ui.markAllDirty();
            ui.render();
            ui.flush();
            sdStore.wipeRsDeck();
            if (announceManager) announceManager->clearAll();
            Serial.println("[BOOT] Old data cleared");
            dataCleanScreen.setStatus("Done! Rebooting...");
            ui.markAllDirty();
            ui.render();
            ui.flush();
            delay(1500);
            ESP.restart();
        } else {
            Serial.println("[BOOT] User chose to keep old data");
            ui.setScreen(&nameInputScreen);
        }
    });

    // Boot flow: timezone → name → home
    // Helper: finalize boot and go to home screen
    auto finalizeBoot = []() {
        ui.setBootMode(false);
        ui.setScreen(&homeScreen);
        ui.tabBar().setActiveTab(TabBar::TAB_HOME);
        const auto result = announceWithName();
        Serial.println(result == ProtocolBackend::AnnounceResult::Sent
                           ? "[BOOT] Initial announce sent"
                           : (result == ProtocolBackend::AnnounceResult::Deferred
                                  ? "[BOOT] Initial announce queued"
                                  : "[BOOT] Initial announce not sent"));
        lastAutoAnnounce = millis();
    };

    // Helper: save config to best available backend
    auto saveConfig = []() {
        if (sdStore.isReady()) {
            userConfig.save(sdStore, flash);
        } else {
            userConfig.save(flash);
        }
    };

    // Name input callback — shared between fresh boot and timezone-only flow
    nameInputScreen.setDoneCallback([=](const String& name) {
        if (!name.isEmpty()) {
            userConfig.settings().displayName = name;
            if (identityMgr.activeIndex() >= 0) {
                identityMgr.setDisplayName(identityMgr.activeIndex(), name);
            }
            saveConfig();
        }
        finalizeBoot();
    });
    nameInputScreen.setBackCallback([]() {
        timezoneScreen.setSelectedIndex(userConfig.settings().timezoneIdx);
        ui.setScreen(&timezoneScreen);
    });

    // Timezone selection callback — apply TZ, then proceed to name or home
    timezoneScreen.setDoneCallback([=](int tzIdx) {
        userConfig.settings().timezoneIdx = (uint8_t)tzIdx;
        userConfig.settings().timezoneSet = true;

        // Apply timezone immediately
        setenv("TZ", TIMEZONE_TABLE[tzIdx].posixTZ, 1);
        tzset();
#if HAS_GPS
        if (gps.isRunning()) gps.setPosixTZ(TIMEZONE_TABLE[tzIdx].posixTZ);
#endif

        // Auto-set radio region + frequency from timezone
        uint8_t tzRegion = TIMEZONE_TABLE[tzIdx].radioRegion;
        userConfig.settings().radioRegion = tzRegion;
        userConfig.settings().loraFrequency = REGION_FREQ[tzRegion];
        // Apply to hardware immediately
        if (radioOnline) {
            radio.setFrequency(REGION_FREQ[tzRegion]);
            radio.receive();
        }
        Serial.printf("[BOOT] Timezone set: %s (UTC%+d), radio region: %s (%lu MHz)\n",
                      TIMEZONE_TABLE[tzIdx].label, TIMEZONE_TABLE[tzIdx].baseOffset,
                      REGION_LABELS[tzRegion], (unsigned long)(REGION_FREQ[tzRegion] / 1000000));

        saveConfig();

        // If no display name yet, go to name input next
        if (userConfig.settings().displayName.isEmpty()) {
            ui.setScreen(&nameInputScreen);
        } else {
            finalizeBoot();
        }
    });

    Serial.printf("[BOOT] displayName='%s' tzSet=%d wifiMode=%d\n",
        userConfig.settings().displayName.c_str(),
        (int)userConfig.settings().timezoneSet,
        (int)userConfig.settings().wifiMode);

    // Decide which screen to show first
    if (!userConfig.settings().timezoneSet) {
        // Timezone not set — show picker first
        timezoneScreen.setSelectedIndex(userConfig.settings().timezoneIdx);
        ui.setScreen(&timezoneScreen);
        Serial.println("[BOOT] Showing timezone picker");
    } else if (userConfig.settings().displayName.isEmpty()) {
        // Timezone set but no name — show name input
        ui.setScreen(&nameInputScreen);
        Serial.println("[BOOT] Showing name input");
    } else {
        // Everything set — go straight to home
        // Apply saved timezone
        if (userConfig.settings().timezoneIdx < TIMEZONE_COUNT) {
            setenv("TZ", TIMEZONE_TABLE[userConfig.settings().timezoneIdx].posixTZ, 1);
            tzset();
        }
        finalizeBoot();
    }

    // Clear boot loop counter — setup completed successfully
    {
        Preferences prefs;
        if (prefs.begin("ratcom", false)) {
            prefs.putInt("bootc", 0);
            prefs.end();
        }
    }

    // Initialize timing
    unsigned long now = millis();
    lastRNS = now;
    lastRender = now;
    lastStatusUpdate = now;
    loopCycleStart = now;

    Serial.println("[BOOT] rsCardputer Standalone ready");
}

// =============================================================================
// Main Loop — Throttled, non-blocking
// =============================================================================

void loop() {
    unsigned long now = millis();
    M5.update();
    handleSerialCommands();

    // 1. Input (keyboard refresh is INT-gated, with fallback polling)
    keyboard.update();
    if (keyboard.hasEvent()) {
        const KeyEvent& evt = keyboard.getEvent();
        power.activity();

        if (ui.isBootMode()) {
            ui.handleKey(evt);
        }
        else if (helpOverlay.isVisible()) {
            helpOverlay.handleKey(evt);
            ui.setOverlay(helpOverlay.isVisible() ? &helpOverlay : nullptr);
        }
        else if (!hotkeys.process(evt)) {
            bool consumed = ui.handleKey(evt);

            if (!consumed && !evt.ctrl) {
                if (evt.tab || evt.navLeft()) {
                    int direction = evt.tab && evt.shift ? -1 :
                                    (evt.navLeft() ? -1 : 1);
                    ui.tabBar().cycleTab(direction);
                    int tab = ui.tabBar().getActiveTab();
                    if (tabScreens[tab]) ui.setScreen(tabScreens[tab]);
                }
                else if (evt.navRight()) {
                    ui.tabBar().cycleTab(1);
                    int tab = ui.tabBar().getActiveTab();
                    if (tabScreens[tab]) ui.setScreen(tabScreens[tab]);
                }
            }
        }
    }

    // 2. Reticulum + radio (throttled — 200Hz active, 20Hz screen off)
    unsigned long rnsDuration = 0;
    if (now - lastRNS >= rnsInterval) {
        lastRNS = now;
        unsigned long rnsStart = millis();
        backend->loop();
        rnsDuration = millis() - rnsStart;
    }

    // 4. Auto-announce (2 minutes)
    if (bootComplete && now - lastAutoAnnounce >= ANNOUNCE_INTERVAL_MS) {
        lastAutoAnnounce = now;
        if (rustLoraIface.isOnline() && rustLoraIface.airtimeUtilization() > LoRaInterface::AIRTIME_THROTTLE) {
            Serial.println("[AUTO] Skipping announce: LoRa airtime > 25%");
        } else {
            const auto result = announceWithName(!power.isScreenOn());
            Serial.println(result == ProtocolBackend::AnnounceResult::Sent
                               ? "[AUTO] Periodic announce sent"
                               : (result == ProtocolBackend::AnnounceResult::Deferred
                                      ? "[AUTO] Periodic announce queued"
                                      : "[AUTO] Periodic announce not sent"));
        }
    }

    // 5. WiFi STA non-blocking connection handler
    if (wifiSTAStarted) {
        if (wifiNeedsReconnect.load() && WiFi.status() != WL_CONNECTED &&
            (long)(millis() - wifiReconnectAt.load()) >= 0) {
            wifiNeedsReconnect.store(false);
            uint8_t attempt = wifiReconnectAttempt.load();
            Serial.printf("[WIFI] Reconnect attempt #%u\n", (unsigned)attempt);
            beginSTAConnection();
            if (WiFi.status() != WL_CONNECTED && !wifiNeedsReconnect.load()) {
                scheduleWiFiReconnect();
            }
        }

        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !wifiSTAConnected) {
            wifiSTAConnected = true;
            wifiNeedsReconnect.store(false);
            wifiReconnectAttempt.store(0);
            Serial.printf("[WIFI] STA connected: %s\n", WiFi.localIP().toString().c_str());

            // NTP time sync — configTzTime() is non-blocking (starts SNTP daemon)
            {
                static bool ntpStarted = false;
                if (!ntpStarted) {
                    const char* tz = currentPosixTZ();
                    configTzTime(tz, "pool.ntp.org", "time.nist.gov");
                    ntpStarted = true;
                    Serial.printf("[NTP] Time sync started (TZ=%s)\n", tz);
                }
            }

            // Recreate TCP clients on every WiFi connect (old clients may have stale sockets)
            reloadTCPClients();
            // Arm AutoInterface deferred-start; SLAAC needs ~1.5–10s to assign
            // a link-local IPv6 address.  Calling enableIpV6() pre-begin is
            // not enough on every Arduino-ESP32 version — call again now that
            // STA is associated.
            if (userConfig.settings().autoIfaceEnabled) {
                WiFi.enableIpV6();
                autoIfaceDeferredStart = true;
                autoIfaceDeferredAt = millis();
            }
        } else if (!connected && wifiSTAConnected) {
            wifiSTAConnected = false;
            // Stop and deregister TCP clients cleanly
            protocolRuntime.pump().detachTcpAll();
            for (auto* tcp : tcpClients) {
                retireTCPClient(tcp);
            }
            tcpClients.clear();
            Serial.println("[WIFI] STA disconnected, TCP interfaces deregistered");
            autoIface.stop();
            autoIfaceDeferredStart = false;
        }
    }

    // 5.5. AutoInterface deferred start — fire once SLAAC assigns a link-local
    // IPv6 address.  Arduino's IPv6Address::toString returns expanded
    // "0000:0000:..." form before SLAAC completes; check fe80::/10 prefix
    // bytes directly.  Give up after 10 s on hostile APs.
    if (autoIfaceDeferredStart) {
        unsigned long elapsed = millis() - autoIfaceDeferredAt;
        if (elapsed >= 1500) {
            IPv6Address ll = WiFi.localIPv6();
            bool isLinkLocal = (ll[0] == 0xfe) && ((ll[1] & 0xc0) == 0x80);
            if (isLinkLocal) {
                autoIfaceDeferredStart = false;
                esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                uint32_t scope = sta ? esp_netif_get_netif_impl_index(sta) : 1;
                // Raw bytes in: the driver formats RFC-5952 itself (Arduino
                // toString is expanded-form — would break beacon-hash interop).
                autoIface.start(
                    userConfig.settings().autoIfaceGroupId.c_str(),
                    userConfig.settings().autoIfaceMaxPeers,
                    (const uint8_t*)ll,
                    scope);
            } else if (elapsed >= 10000) {
                autoIfaceDeferredStart = false;
                Serial.println("[AUTOIFACE] SLAAC timeout — no link-local after 10s");
            }
        }
    }

    // 5.6. AutoInterface link-local rotation watch — covers SLAAC privacy
    // address rotation while STA stays associated.  notify_link_change()
    // is idempotent in the library, so polling here is cheap (string
    // compare, no socket churn) and only does real work on actual change.
    if (autoIface.isOnline() && wifiSTAConnected &&
        millis() - lastAutoIfaceLinkCheck >= 2000) {
        lastAutoIfaceLinkCheck = millis();
        IPv6Address ll = WiFi.localIPv6();
        bool isLinkLocal = (ll[0] == 0xfe) && ((ll[1] & 0xc0) == 0x80);
        if (isLinkLocal) {
            esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            uint32_t scope = sta ? esp_netif_get_netif_impl_index(sta) : 1;
            autoIface.notifyLinkChange((const uint8_t*)ll, scope);
        }
    }

    // 6. TCP transport (with global budget) — skip if RNS was overloaded
    {
        drainRetiredTCPClients();
        bool skipTcp = (rnsDuration > 200);
        if (!skipTcp && wifiImpl) wifiImpl->loop();
        if (!skipTcp) {
            unsigned long tcpBudgetStart = millis();
            for (auto* tcp : tcpClients) {
                if (millis() - tcpBudgetStart >= TCP_GLOBAL_BUDGET_MS) break;
                tcp->loop();
                yield();
            }
        }
        // AutoInterface always runs — non-blocking, time-gated.  Skipping
        // it under TCP load drops peers to a 22 s silence timeout.
        autoIface.loop();
    }

    // 7. Announce manager deferred saves (contacts + name cache)
    if (announceManager) {
        announceManager->loop();

        // Periodic stale node eviction (every 30 min)
        static unsigned long lastEvict = 0;
        if (now - lastEvict >= 1800000) {
            lastEvict = now;
            announceManager->evictStale();
        }
    }

    // 8. GPS (read UART bytes — non-blocking, <1ms per call)
#if HAS_GPS
    if (gps.isRunning()) gps.loop();
#endif

    // 9. Deferred audio (from packet callbacks — delay() can't run in callbacks)
    if (pendingMessageSound) {
        pendingMessageSound = false;
        audio.playMessage();
    }
    audio.loop();

    // 10. Power management
    power.loop();

    // 9. Power-aware RNS throttle
    if (power.state() == PowerManager::SCREEN_OFF) {
        rnsInterval = 50;  // 20 Hz when screen off
    } else {
        rnsInterval = RNS_INTERVAL_MS;  // 200 Hz when active
    }

    // 10. Render (20 FPS, skip if screen off or dimmed-frozen)
    if (now - lastRender >= RENDER_INTERVAL_MS) {
        lastRender = now;
        if (power.isScreenOn() && power.state() != PowerManager::DIMMED) {
            // Status bar needs periodic refresh for battery + connection indicators
            if (now - lastStatusUpdate >= STATUS_UPDATE_MS) {
                lastStatusUpdate = now;
                // Update TCP connection status
                bool anyTcpConnected = false;
                for (auto* tcp : tcpClients) {
                    if (tcp->isConnected()) { anyTcpConnected = true; break; }
                }
                ui.statusBar().setTCPConnected(anyTcpConnected);
                ui.statusBar().setWiFiState(WiFi.getMode() != WIFI_OFF,
                    WiFi.status() == WL_CONNECTED || (wifiImpl && wifiImpl->isAPActive()));
                ui.statusBar().setAutoIfacePeers(autoIface.isOnline() ? (int)autoIface.peerCount() : -1);
#if HAS_GPS
                ui.statusBar().setGPSTimeFix(gps.hasTimeFix());
#endif
                ui.markStatusDirty();
            }
            ui.render();
        }
    }

    // 11. Heartbeat (5s)
    {
        unsigned long cycleTime = now - loopCycleStart;
        if (cycleTime > maxLoopTime) maxLoopTime = cycleTime;

        if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
            lastHeartbeat = now;
            Serial.printf("[HEART] heap=%lu min=%lu stack=%lu loop=%lums nodes=%d paths=%d links=%d lxmfQ=%d writeQ=%d autoiface=%s peers=%u up=%lus backend=%s\n",
                          (unsigned long)ESP.getFreeHeap(),
                          (unsigned long)ESP.getMinFreeHeap(),
                          (unsigned long)uxTaskGetStackHighWaterMark(NULL),
                          maxLoopTime,
                          announceManager ? announceManager->nodeCount() : 0,
                          (int)backend->pathCount(),
                          (int)backend->linkCount(),
                          backend->lxmfQueuedCount(),
                          messageStore.writeQueue().drainCount(),
                          autoIface.isOnline() ? "ON" : "off",
                          (unsigned)autoIface.peerCount(),
                          millis() / 1000,
                          backend->backendName());
            {
                rs_handheld_transport_stats_t st = {};
                const auto& pc = protocolRuntime.pump().counters();
                if (protocolRuntime.transportStats(st)) {
                    Serial.printf("[HEART-RUST] acc=%llu dup=%llu ann=%llu ann_filt=%llu drop=%llu odrop=%llu oq=%u rx=%lu tx=%lu txdrop=%lu\n",
                        (unsigned long long)st.accepted, (unsigned long long)st.duplicates,
                        (unsigned long long)st.learned_announces,
                        (unsigned long long)st.announces_rate_dropped,
                        (unsigned long long)st.dropped,
                        (unsigned long long)st.outbound_dropped, (unsigned)st.outbound_len,
                        (unsigned long)pc.rxFrames, (unsigned long)pc.txFrames,
                        (unsigned long)pc.txDropped);
                }
            }
            maxLoopTime = 0;
        }
    }
    loopCycleStart = now;

    yield();
}
