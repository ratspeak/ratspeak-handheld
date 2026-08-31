// =============================================================================
// rsDeck — Main Entry Point
// LilyGo T-Deck Plus: LovyanGFX Direct UI + Rust Reticulum backend + LXMF Messaging
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_netif.h>
#include <lvgl.h>

#include "config/BoardConfig.h"
#include "config/Config.h"
#include "platform/RsDeckModeSwitch.h"
#include "hal/Display.h"
#include "hal/TouchInput.h"
#include "hal/Trackball.h"
#include "hal/Keyboard.h"
#include "hal/Power.h"
#if HAS_GPS
#include "hal/GPSManager.h"
#endif
#include "radio/SX1262.h"
#include "input/InputManager.h"
#include "input/HotkeyManager.h"
#include "UIManager.h"
#include "Theme.h"
#include "LvTabBar.h"
#include "LvInput.h"
#include "screens/LvBootScreen.h"
#include "screens/LvHomeScreen.h"
#include "screens/LvNodesScreen.h"
#include "screens/LvMessagesScreen.h"
#include "screens/LvMessageView.h"
#include "screens/LvContactsScreen.h"
#include "screens/LvSettingsScreen.h"
#include "screens/LvHelpOverlay.h"
#include "screens/LvQrOverlay.h"
#include "screens/LvNameInputScreen.h"
#include "screens/LvTimezoneScreen.h"
#include "screens/LvDataCleanScreen.h"
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "storage/MessageStore.h"
#include "reticulum/AnnounceManager.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/IdentityManager.h"
#include "protocol/ProtocolRuntime.h"
#include "transport/LoRaInterface.h"
#include "transport/WiFiInterface.h"
#include "runtime/DeviceService.h"
#include "runtime/RuntimeMetrics.h"
#include "hal/SharedSPIBus.h"
#include "runtime/ServiceClient.h"
#include "runtime/ServiceRunner.h"
#include "runtime/WiFiConnection.h"
#include "transport/TCPClientInterface.h"
#include "transport/RnsAutoInterface.h"
#include "config/UserConfig.h"
#include "audio/AudioNotify.h"
#include "util/PerfTrace.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>

// Keep stack headroom for initialization and cooperative debug builds.
// Normal builds transfer protocol ownership to the dedicated service task.
SET_LOOP_TASK_STACK_SIZE(16384);

// --- Hardware ---
// Single shared SPI bus for display, LoRa, and SD card
// IMPORTANT: On ESP32-S3, Arduino FSPI=0 maps to SPI2 hardware.
// Do NOT use SPI2_HOST (IDF constant = 1) — Arduino treats index 1 as HSPI/SPI3!
SPIClass sharedSPI(FSPI);

SX1262 radio(&sharedSPI,
    LORA_CS, SPI_SCK, SPI_MOSI, SPI_MISO,
    LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
    LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);

Display display;
TouchInput touch;
Trackball trackball;
Keyboard keyboard;

// --- Subsystems ---
InputManager inputManager;
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
AnnounceManager* announceManager = nullptr;
WiFiInterface* wifiImpl = nullptr;
std::vector<TCPClientInterface*> tcpClients;
std::list<TCPClientInterface*> retiredTcpClients;
bool tcpReloadRequested = false;
UserConfig userConfig;
Power powerMgr;
AudioNotify audio;
IdentityManager identityMgr;
handheld::DeviceService deviceService(*backend, messageStore, userConfig, identityMgr, flash, sdStore);
handheld::ServiceClient serviceClient(deviceService.mailbox());
handheld::ServiceRunner serviceRunner(deviceService);
handheld::WiFiConnection wifiConnection;
static bool homeAnnounceRequested = false;
static bool serviceAvailable = false;
static void serviceNetworkPoll();
static void applyUiSettings();
#if HAS_GPS
GPSManager gps;
#endif

// --- LVGL Screens ---
LvBootScreen lvBootScreen;
LvHomeScreen lvHomeScreen;
LvNodesScreen lvNodesScreen;
LvMessagesScreen lvMessagesScreen;
LvContactsScreen lvContactsScreen;
LvMessageView lvMessageView;
LvSettingsScreen lvSettingsScreen;
LvHelpOverlay lvHelpOverlay;
LvQrOverlay lvQrOverlay;
LvNameInputScreen lvNameInputScreen;
LvTimezoneScreen lvTimezoneScreen;
LvDataCleanScreen lvDataCleanScreen;

// Tab-screen mapping (4 tabs) — LVGL versions
LvScreen* lvTabScreens[LvTabBar::TAB_COUNT] = {};

// --- State ---
bool radioOnline = false;
bool bootComplete = false;
bool bootLoopRecovery = false;
bool sdHadExistingData = false;
bool wifiSTAStarted = false;
bool wifiSTAConnected = false;
unsigned long lastAutoAnnounce = 0;
bool bootAnnouncePending = false;
uint8_t bootAnnounceAttempts = 0;
unsigned long bootAnnounceAt = 0;
constexpr unsigned long BOOT_ANNOUNCE_DELAY_MS = 5000;
constexpr uint8_t BOOT_ANNOUNCE_MAX_ATTEMPTS = 3;

static void applyRadioSettingsToHardware(const UserSettings& s, const char* context) {
    if (!radioOnline) return;

    if (!s.loraEnabled) {
        radio.sleep();
        Serial.printf("[%s] LoRa disabled by config\n", context);
        return;
    }

    radio.setFrequency(s.loraFrequency);
    radio.setSpreadingFactor(s.loraSF);
    radio.setSignalBandwidth(s.loraBW);
    radio.setCodingRate4(s.loraCR);
    radio.setTxPower(s.loraTxPower);
    radio.setPreambleLength(s.loraPreamble);
    radio.receive();
    Serial.printf("[%s] Radio: %lu Hz, SF%d, BW%lu, CR4/%d, %d dBm, pre=%ld\n",
                  context,
                  (unsigned long)s.loraFrequency, s.loraSF,
                  (unsigned long)s.loraBW, s.loraCR, s.loraTxPower,
                  s.loraPreamble);
}

unsigned long lastStatusUpdate = 0;
constexpr unsigned long STATUS_UPDATE_MS = 1000;                // 1 Hz status bar update
unsigned long lastHeartbeat = 0;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 5000;
unsigned long loopCycleStart = 0;
unsigned long maxLoopTime = 0;
unsigned long lastLvglTime = 0;
constexpr unsigned long LVGL_INTERVAL_MS = 33;          // ~30 FPS
constexpr unsigned long TCP_GLOBAL_BUDGET_MS = 35;      // Max cumulative TCP time per loop

RnsAutoInterface autoIface;  // device-owned driver on the rust pump (iface id 5)
bool autoIfaceDeferredStart = false;
unsigned long autoIfaceDeferredAt = 0;
unsigned long lastAutoIfaceLinkCheck = 0;

// LXMF diagnostic counters (reset each heartbeat)
static uint32_t diagTcpSkipEvents = 0;

// =============================================================================
// Timezone helper — returns POSIX TZ string for current config
// =============================================================================

static const char* currentPosixTZ() {
    uint8_t idx = userConfig.settings().timezoneIdx;
    if (idx < TIMEZONE_COUNT) return TIMEZONE_TABLE[idx].posixTZ;
    return "EST5EDT,M3.2.0,M11.1.0";  // Fallback
}

// =============================================================================
// Announce with display name (MessagePack-encoded app_data)
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

static bool hasUsableAnnounceTransport() {
    // Rust env: usable when the rust node is open and a pumped interface is live
    // (LoRa, TCP client, AutoInterface with peers, AP with clients). BLE was dropped with the micro retirement.
    if (!backend->isTransportActive()) return false;
    if (protocolRuntime.pump().loraOnline()) return true;
    for (auto* tcp : tcpClients) {
        if (tcp && tcp->isConnected()) return true;
    }
    if (autoIface.isOnline() && autoIface.peerCount() > 0) return true;
    if (wifiImpl && wifiImpl->isAPActive() && wifiImpl->getClientCount() > 0) return true;
    return false;
}

static ProtocolBackend::AnnounceResult announceWithName(bool silent = false) {
    // Honest runtime gate: real announce once the backend is up (protocolReady).
    if (!backend->protocolReady()) {
        if (!silent) deviceService.notice("Network not ready");
        Serial.println("[ANNOUNCE-TX] skipped: rust backend protocol not ready");
        return ProtocolBackend::AnnounceResult::Failed;
    }
    if (!hasUsableAnnounceTransport()) {
        if (!silent) deviceService.notice("No active transport");
        Serial.println("[ANNOUNCE-TX] skipped: no active transport");
        return ProtocolBackend::AnnounceResult::Failed;
    }
    rs::Bytes appData = encodeAnnounceName(userConfig.settings().displayName);
    Serial.printf("[ANNOUNCE-TX] name=\"%s\" appData=%d bytes silent=%s\n",
        userConfig.settings().displayName.c_str(), (int)appData.size(),
        silent ? "yes" : "no");
    const auto result = backend->announce(appData.data(), appData.size());
    if (!silent) {
        if (result == ProtocolBackend::AnnounceResult::Sent) {
            deviceService.notice("Announce sent!");
        } else if (result == ProtocolBackend::AnnounceResult::Deferred) {
            deviceService.notice("Announce queued");
        } else {
            deviceService.notice("Announce not sent");
        }
    }
    return result;
}

static void manualAnnounce() {
    const auto result = announceWithName();
    if (result == ProtocolBackend::AnnounceResult::Sent)
        Serial.println("[ANNOUNCE] Manual announce sent");
    else if (result == ProtocolBackend::AnnounceResult::Deferred)
        Serial.println("[ANNOUNCE] Manual announce queued");
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
                Serial.printf("[TCP] Created client: %s:%d (registered with Transport, mode=FULL)\n", ep.host.c_str(), ep.port);
            }
        }
    }

    if (tcpClients.empty()) {
        Serial.println("[TCP] No active TCP connections");
    }
}

static void requestTCPClientsReload() {
    tcpReloadRequested = true;
}

// =============================================================================
// Hotkey callbacks
// =============================================================================

void onHotkeyHelp() {
    lvHelpOverlay.toggle();
}
void onHotkeyMessages() {
    ui.lvTabBar().setActiveTab(LvTabBar::TAB_MSGS);
    ui.setScreen(&lvMessagesScreen);
}
void onHotkeyNewMsg() {
    bool hasContacts = false;
    {
        for (const auto& node : serviceClient.nodes.nodes()) {
            if (node.saved) { hasContacts = true; break; }
        }
    }
    if (hasContacts) {
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_CONTACTS);
        ui.setScreen(&lvContactsScreen);
    } else {
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_NODES);
        ui.setScreen(&lvNodesScreen);
        ui.lvStatusBar().showToast("Pick a peer to message", 1200);
    }
}
void onHotkeySettings() {
    ui.lvTabBar().setActiveTab(LvTabBar::TAB_SETTINGS);
    ui.setScreen(&lvSettingsScreen);
}
void onHotkeyAnnounce() {
    serviceClient.action(handheld::Operation::Announce);
}
static void printAutoIface() {
    Serial.println("=== AUTOIFACE DUMP ===");
    Serial.printf("Enabled in settings : %s\n",
        userConfig.settings().autoIfaceEnabled ? "YES" : "no");
    Serial.printf("Online              : %s\n", autoIface.isOnline() ? "YES" : "no");
    if (autoIface.isOnline()) {
        Serial.printf("Multicast address   : %s\n", autoIface.multicastAddress().c_str());
        Serial.printf("Link-local          : %s\n", WiFi.localIPv6().toString().c_str());
        Serial.printf("Peers               : %u\n", (unsigned)autoIface.peerCount());
    }
    Serial.printf("Deferred-start armed: %s (elapsed=%lums)\n",
        autoIfaceDeferredStart ? "YES" : "no",
        autoIfaceDeferredStart ? (millis() - autoIfaceDeferredAt) : 0UL);
    Serial.println("======================");
}
static void printDiagnostics() {
    Serial.println("=== DIAGNOSTIC DUMP ===");
    Serial.printf("Device: rsDeck T-Deck Plus\n");
    Serial.printf("Protocol: %s\n", backend->backendName());
    // Diagnostic state read through the backend facade (proves live delegation;
    // identical to rns.* since MicroReticulumBackend is pure delegation).
    Serial.printf("Identity: %s\n", backend->identityHash().c_str());
    Serial.printf("Transport: %s\n", backend->isTransportActive() ? "ACTIVE" : "OFFLINE");
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
        Serial.printf("Bitrate: %lu bps  LDRO: %s  frame255: %.0f ms\n",
                      (unsigned long)radio.getBitrate(),
                      radio.lowDataRateEnabled() ? "ON" : "off",
                      radio.getAirtime(MAX_PACKET_SIZE));
        LoRaInterface* loraIf = &rustLoraIface;
        if (loraIf) {
            Serial.printf("LoRaIF: bitrate=%lu bps split_timeout=%lu ms frame=%.0f ms airtime=%.2f%%\n",
                          (unsigned long)loraIf->bitrate(),
                          loraIf->splitRxTimeoutMs(),
                          loraIf->singleFrameAirtimeMs(),
                          loraIf->airtimeUtilization() * 100.0f);
        }
        Serial.printf("IQ invert: %s\n", radio.getInvertIQ() ? "ON" : "off");
        Serial.printf("SyncWord regs: 0x%02X%02X\n",
            radio.readRegister(REG_SYNC_WORD_MSB_6X),
            radio.readRegister(REG_SYNC_WORD_LSB_6X));
        uint16_t devErr = radio.getDeviceErrors();
        uint8_t status = radio.getStatus();
        Serial.printf("DevErrors: 0x%04X  Status: 0x%02X (mode=%d cmd=%d)\n",
            devErr, status, (status >> 4) & 0x07, (status >> 1) & 0x07);
        if (devErr & 0x40) Serial.println("  *** PLL LOCK FAILED ***");
        Serial.printf("IRQ flags: 0x%04X\n", radio.getIrqFlags());
        Serial.printf("Current RSSI: %d dBm\n", radio.currentRssi());
        uint8_t packetType = radio.getPacketType();
        const char* packetTypeName =
            (packetType == 0x00) ? "GFSK" :
            (packetType == 0x01) ? "LoRa" :
            (packetType == 0x02) ? "LR-FHSS" : "unknown";
        Serial.printf("Packet type: 0x%02X (%s)%s\n",
                      packetType, packetTypeName,
                      packetType == 0x01 ? "" : " *** NOT LoRa ***");
    }
    Serial.printf("Free heap: %lu bytes  PSRAM: %lu bytes\n",
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
    Serial.printf("Uptime: %lu s\n", millis() / 1000);
    Serial.println("=======================");
}

static void printIrqFlags(uint16_t flags) {
    Serial.printf("0x%04X", flags);
    if (flags & 0x0001) Serial.print(" TX_DONE");
    if (flags & 0x0002) Serial.print(" RX_DONE");
    if (flags & 0x0004) Serial.print(" PREAMBLE");
    if (flags & 0x0008) Serial.print(" SYNC");
    if (flags & 0x0010) Serial.print(" HEADER_VALID");
    if (flags & 0x0020) Serial.print(" HEADER_ERR");
    if (flags & 0x0040) Serial.print(" CRC_ERR");
    if (flags & 0x0080) Serial.print(" CAD_DONE");
    if (flags & 0x0100) Serial.print(" CAD_DET");
    if (flags & 0x0200) Serial.print(" TIMEOUT");
}

static bool irqMonitorActive = false;
static uint32_t irqMonitorStart = 0, irqLastSample = 0;
static void startIrqMonitor() {
    if (!radioOnline) return;
    irqMonitorActive = !irqMonitorActive;
    irqMonitorStart = millis();
}

// RSSI monitor — non-blocking state machine (sampled in main loop)
volatile bool rssiMonitorActive = false;
unsigned long rssiMonitorStart = 0;
unsigned long rssiLastSample = 0;
int rssiMinVal = 0, rssiMaxVal = -200, rssiSampleCount = 0;

static void startRssiMonitor() {
    if (!radioOnline) { Serial.println("[RSSI] Radio offline"); return; }
    if (rssiMonitorActive) {
        // Already running — cancel
        rssiMonitorActive = false;
        Serial.printf("[RSSI] Stopped: %d samples, min=%d max=%d dBm\n",
                      rssiSampleCount, rssiMinVal, rssiMaxVal);
        return;
    }
    Serial.println("[RSSI] Sampling for 5 seconds (non-blocking)...");
    rssiMonitorActive = true;
    rssiMonitorStart = millis();
    rssiLastSample = 0;
    rssiMinVal = 0;
    rssiMaxVal = -200;
    rssiSampleCount = 0;
}

static void runRadioTest() {
    Serial.println("[TEST] Sending raw test packet...");
    uint8_t header = 0xA0;
    const char* testPayload = "RSDECK_TEST_1234567890";
    radio.beginPacket();
    radio.write(header);
    radio.write((const uint8_t*)testPayload, strlen(testPayload));
    bool ok = radio.endPacket();
    Serial.printf("[TEST] TX %s (%d bytes)\n", ok ? "OK" : "FAILED", (int)(1 + strlen(testPayload)));
    radio.receive();
}

static void cycleDiagnosticTxPower() {
    static constexpr int8_t kPowers[] = {-9, -3, 0, 2, 6, 10, 14, 17, 22};
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
    static constexpr int kMaxDiagnosticTxPower = 22;
    if (powerDbm < -9 || powerDbm > kMaxDiagnosticTxPower) {
        Serial.printf("[SERIAL] TX power out of range: %d dBm (allowed -9..%d)\n",
                      powerDbm, kMaxDiagnosticTxPower);
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
    static constexpr char kPrefix[] = "RSDECK-LXMF-TEST:";
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

static constexpr uint8_t LITE_TRANSPORT_ID[16] = {
    'r', 's', 'l', 'i', 't', 'e', '-', 'h',
    'e', 'l', 't', 'e', 'c', '-', 'v', '3'
};
static constexpr size_t RNODE_DIAG_SINGLE_MTU = RSDECK_RNODE_SINGLE_FRAME_RAW_MAX;
static rs::Bytes diagnosticLiteLinkId;

static bool sendDiagnosticRawReticulum(const rs::Bytes& raw, const char* label) {
    if (!radioOnline || !radio.isRadioOnline()) {
        Serial.println("[SERIAL] lite diag failed: radio offline");
        return false;
    }
    if (raw.empty() || raw.size() > RNODE_DIAG_SINGLE_MTU) {
        Serial.printf("[SERIAL] lite diag %s rejected: raw len=%u (allowed 1..%u)\n",
                      label ? label : "packet",
                      (unsigned)raw.size(),
                      (unsigned)RNODE_DIAG_SINGLE_MTU);
        return false;
    }

    uint8_t rnodeHeader = (uint8_t)(random(256)) & 0xF0;
    radio.beginPacket();
    radio.write(rnodeHeader);
    radio.write(raw.data(), raw.size());
    bool ok = radio.endPacket();
    radio.receive();
    Serial.printf("[SERIAL] lite diag %s TX %s raw=%u air=%u rnode=0x%02X\n",
                  label ? label : "packet",
                  ok ? "OK" : "FAILED",
                  (unsigned)raw.size(),
                  (unsigned)(raw.size() + 1),
                  rnodeHeader);
    return ok;
}

static bool buildDiagnosticHeader2(uint8_t packetType, uint8_t context,
                                   const rs::Bytes& destHash, const uint8_t* payload,
                                   size_t payloadLen, rs::Bytes& out) {
    if (destHash.size() != 16) return false;
    uint8_t raw[RNODE_DIAG_SINGLE_MTU];
    size_t rawLen = 0;
    out.clear();
    if (rs_handheld_rns_packet_build(1, packetType, 0, context, LITE_TRANSPORT_ID,
                                     destHash.data(), payload, payloadLen, raw, sizeof(raw),
                                     &rawLen) !=
        RS_HANDHELD_OK)
        return false;
    out.append(raw, rawLen);
    return true;
}

static bool buildDiagnosticLinkPacket(uint8_t packetType, uint8_t context, const uint8_t* payload,
                                      size_t payloadLen, rs::Bytes& out) {
    if (diagnosticLiteLinkId.size() != 16) {
        Serial.println("[SERIAL] lite link diag failed: send J [dest_hash] first");
        return false;
    }
    uint8_t raw[RNODE_DIAG_SINGLE_MTU];
    size_t rawLen = 0;
    out.clear();
    if (rs_handheld_rns_packet_build(0, packetType, 3, context, nullptr,
                                     diagnosticLiteLinkId.data(), payload, payloadLen, raw,
                                     sizeof(raw), &rawLen) != RS_HANDHELD_OK)
        return false;
    out.append(raw, rawLen);
    return true;
}
static bool parseSerialContextByte(const char* p, uint8_t defaultContext, uint8_t& context) {
    p = skipSerialSeparators(p);
    if (!p || *p == '\0') {
        context = defaultContext;
        return true;
    }

    char* end = nullptr;
    long parsed = std::strtol(p, &end, 16);
    if (end == p || parsed < 0 || parsed > 0xFF) {
        Serial.println("[SERIAL] invalid context; expected one hex byte, for example K0E or YFD");
        return false;
    }
    context = (uint8_t)parsed;
    return true;
}

static void fillDiagnosticPayload(uint8_t* payload, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++) {
        payload[i] = (uint8_t)(seed + i);
    }
}

static bool sendDiagnosticLiteHeader2Data(size_t length, const char* explicitDest) {
    static constexpr size_t kMaxDiagnosticTransportPayload = 160;
    if (length == 0 || length > kMaxDiagnosticTransportPayload) {
        Serial.printf("[SERIAL] usage: H<len> [dest_hash], length 1..%u\n",
                      (unsigned)kMaxDiagnosticTransportPayload);
        return false;
    }

    rs::Bytes destHash;
    std::string peerLabel;
    if (!selectDiagnosticPeer(explicitDest, destHash, peerLabel)) return false;

    uint8_t payload[kMaxDiagnosticTransportPayload];
    fillDiagnosticPayload(payload, length, 0x48);

    rs::Bytes raw;
    if (!buildDiagnosticHeader2(0x00, 0x00, destHash, payload, length, raw)) {
        Serial.println("[SERIAL] lite Header2 data build failed");
        return false;
    }

    Serial.printf("[SERIAL] lite Header2 DATA to Heltec transport, dest=%s payload=%u\n",
                  peerLabel.c_str(), (unsigned)length);
    return sendDiagnosticRawReticulum(raw, "H2-DATA");
}

static bool sendDiagnosticLiteLinkRequest(const char* explicitDest) {
    rs::Bytes destHash;
    std::string peerLabel;
    if (!selectDiagnosticPeer(explicitDest, destHash, peerLabel)) return false;

    uint8_t payload[64];
    fillDiagnosticPayload(payload, sizeof(payload), 0xA5);

    rs::Bytes raw;
    if (!buildDiagnosticHeader2(0x02, 0x00, destHash, payload, sizeof(payload), raw)) {
        Serial.println("[SERIAL] lite link request build failed");
        return false;
    }

    uint8_t linkId[16];
    if (rs_handheld_rns_link_id(destHash.data(), payload, sizeof(payload), linkId) != RS_HANDHELD_OK)
        return false;
    diagnosticLiteLinkId = rs::Bytes(linkId, sizeof(linkId));
    Serial.printf("[SERIAL] lite LINKREQUEST to Heltec transport, dest=%s link=%s\n",
                  peerLabel.c_str(), diagnosticLiteLinkId.toHex().c_str());
    return sendDiagnosticRawReticulum(raw, "LINKREQUEST");
}

static bool sendDiagnosticLiteLinkData(const char* contextArg) {
    uint8_t context = 0x0E;  // Channel
    if (!parseSerialContextByte(contextArg, context, context)) return false;

    uint8_t payload[24];
    fillDiagnosticPayload(payload, sizeof(payload), context);

    rs::Bytes raw;
    if (!buildDiagnosticLinkPacket(0x00, context, payload, sizeof(payload), raw)) {
        Serial.println("[SERIAL] lite link data build failed");
        return false;
    }

    Serial.printf("[SERIAL] lite LINK DATA context=0x%02X link=%s\n",
                  context, diagnosticLiteLinkId.toHex().c_str());
    return sendDiagnosticRawReticulum(raw, "LINK-DATA");
}

static bool sendDiagnosticLiteLinkProof(const char* contextArg) {
    uint8_t context = 0xFD;  // LinkProof
    if (!parseSerialContextByte(contextArg, context, context)) return false;

    uint8_t payload[64];
    fillDiagnosticPayload(payload, sizeof(payload), 0x7A);

    rs::Bytes raw;
    if (!buildDiagnosticLinkPacket(0x03, context, payload, sizeof(payload), raw)) {
        Serial.println("[SERIAL] lite link proof build failed");
        return false;
    }

    Serial.printf("[SERIAL] lite LINK PROOF context=0x%02X link=%s\n",
                  context, diagnosticLiteLinkId.toHex().c_str());
    return sendDiagnosticRawReticulum(raw, "LINK-PROOF");
}

static void handleSerialLineCommand(const char* line) {
    if (!line || !*line) return;

    switch ((char)std::toupper((unsigned char)line[0])) {
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
        case 'H': {
            long length = 0;
            const char* rest = nullptr;
            if (!parseSerialLong(line + 1, length, &rest) || length <= 0) {
                Serial.println("[SERIAL] usage: H<len> [dest_hash], for example H32 2db8...");
                return;
            }
            sendDiagnosticLiteHeader2Data((size_t)length, rest);
            break;
        }
        case 'J': {
            sendDiagnosticLiteLinkRequest(line + 1);
            break;
        }
        case 'K': {
            sendDiagnosticLiteLinkData(line + 1);
            break;
        }
        case 'Y': {
            sendDiagnosticLiteLinkProof(line + 1);
            break;
        }
#ifdef PROTOCOL_PACKET_TRACE
        case 'W': {
            // Bench-only: connect WiFi STA. Usage: W<ssid>|<password>
            const char* rest = line + 1;
            const char* sep = strchr(rest, '|');
            if (!sep) { Serial.println("[BENCH-WIFI] usage: W<ssid>|<password>"); break; }
            String ssid(rest); ssid = ssid.substring(0, sep - rest);
            String pass(sep + 1);
            WiFi.mode(WIFI_STA);
            WiFi.onEvent(onWiFiEvent);
            Serial.printf("[BENCH-WIFI] connecting to \"%s\"...\n", ssid.c_str());
            uint8_t st = WiFi.begin(ssid.c_str(), pass.c_str());
            wifiSTAStarted = true;
            // Deliberately NOT mirrored into userConfig.settings().wifiMode: any later
            // settings save would persist it (STA-on-boot with a stale profile). The
            // Home chip's warning-toned OFF is the designed config!=runtime rendering.
            Serial.printf("[BENCH-WIFI] run=%u connected=%d ip=%s\n",
                          st, WiFi.status() == WL_CONNECTED ? 1 : 0,
                          WiFi.localIP().toString().c_str());
            break;
        }
        case 'C': {
            // Bench-only: point the rust backend at a TCP peer. Usage: C<host>|<port>
            const char* rest = line + 1;
            const char* sep = strchr(rest, '|');
            if (!sep) { Serial.println("[BENCH-TCP] usage: C<host>|<port>"); break; }
            String host(rest); host = host.substring(0, sep - rest);
            long port = atol(sep + 1);
            TCPEndpoint ep; ep.host = host; ep.port = (uint16_t)port; ep.autoConnect = true;
            userConfig.settings().tcpConnections.clear();
            userConfig.settings().tcpConnections.push_back(ep);
            requestTCPClientsReload();
            Serial.printf("[BENCH-TCP] queued %s:%ld (reloads next loop)\n", host.c_str(), port);
            break;
        }
#endif
        default:
            Serial.printf("[SERIAL] unknown line command '%c'\n", line[0]);
            break;
    }
}

static void printSerialHelp() {
    Serial.println("[SERIAL] commands: ? help | a announce | t raw-test | d diag | r rssi | i irq | p tx-power-cycle | m min-power | q iq | +/- freq");
    Serial.println("[SERIAL] line commands: F<hz> exact-frequency | P<dBm> exact-tx-power | L<len> [dest_hash] LXMF test");
    Serial.println("[SERIAL] lite relay diag: H<len> [dest] Header2 data | J [dest] linkreq | K<ctx_hex> link-data | Y<ctx_hex> link-proof");
}

static void handleSerialCommands() {
    static char line[96];
    static size_t lineLen = 0;
    static bool lineActive = false;

    for (unsigned read = 0; read < 96 && Serial.available() > 0; ++read) {
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
        if (c == 'F' || c == 'P' || c == 'L' || c == 'H' || c == 'J' || c == 'K' || c == 'Y'
#ifdef PROTOCOL_PACKET_TRACE
            || c == 'W' || c == 'C'
#endif
        ) {
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
                manualAnnounce();
                break;
            case 't':
            case 'T':
                runRadioTest();
                break;
            case 'd':
            case 'D':
                printDiagnostics();
                break;
            case 'r':
            case 'R':
                startRssiMonitor();
                break;
            case 'i':
            case 'I':
                startIrqMonitor();
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
            default:
                Serial.printf("[SERIAL] unknown command '%c'\n", c);
                printSerialHelp();
                break;
        }
    }
}

// =============================================================================
// Helper: render boot screen immediately
// =============================================================================
void onHotkeyAutoIface() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 0); }
void onHotkeyDiag() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 1); }
void onHotkeyRadioTest() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 2); }
void onHotkeyIrqMonitor() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 3); }
void onHotkeyRssiMonitor() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 4); }

static void bootRender() {
    // LVGL boot screen calls lv_timer_handler() internally via setProgress()
    // Legacy render kept as fallback
}

static unsigned long bootTraceStartMs = 0;
static unsigned long bootTraceLastMs = 0;

static void bootTraceBegin(unsigned long startMs) {
#if RSDECK_PERF_TRACE
    bootTraceStartMs = startMs;
    bootTraceLastMs = startMs;
#else
    (void)startMs;
#endif
}

static void bootTraceStage(const char* label) {
#if RSDECK_PERF_TRACE
    const unsigned long now = millis();
    Serial.printf("[BOOT-PERF] %-22s +%lums total=%lums heap=%lu psram_free=%lu psram_largest=%lu\n",
                  label ? label : "?",
                  now - bootTraceLastMs,
                  now - bootTraceStartMs,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    bootTraceLastMs = now;
#else
    (void)label;
#endif
}

// =============================================================================
// Setup — 26-step boot sequence
// =============================================================================

void setup() {
    const unsigned long setupStartMs = millis();
    bool flashMounted = false;

    // Step 1: Power pin — CRITICAL: enables all T-Deck Plus peripherals
    Power::enablePeripherals();

    // Step 2: Serial
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println();
    Serial.println("=================================");
    Serial.printf("  rsDeck v%s\n", RSDECK_VERSION_STRING);
    Serial.println("  LilyGo T-Deck Plus");
    Serial.printf("  Protocol: %s\n", backend->backendName());
    Serial.println("=================================");

    esp_reset_reason_t reason = esp_reset_reason();
    const char* reasonStr = "UNKNOWN";
    switch (reason) {
        case ESP_RST_POWERON:   reasonStr = "POWER_ON"; break;
        case ESP_RST_SW:        reasonStr = "SOFTWARE"; break;
        case ESP_RST_PANIC:     reasonStr = "PANIC"; break;
        case ESP_RST_INT_WDT:   reasonStr = "INT_WDT"; break;
        case ESP_RST_TASK_WDT:  reasonStr = "TASK_WDT"; break;
        case ESP_RST_WDT:       reasonStr = "WDT"; break;
        case ESP_RST_BROWNOUT:  reasonStr = "BROWNOUT"; break;
        case ESP_RST_DEEPSLEEP: reasonStr = "DEEP_SLEEP"; break;
        default: break;
    }
    Serial.printf("[BOOT] Reset: %s (%d)\n", reasonStr, (int)reason);
    Serial.printf("[BOOT] Heap: %lu  PSRAM: %lu\n",
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getPsramSize());
    bootTraceBegin(setupStartMs);
    bootTraceStage("serial-online");

    // Dual-boot layout: re-arm the launcher so the next reset shows the chooser.
    auto launcherBoot = rs_deck::returnToLauncherNextBoot();
    if (!launcherBoot.ok) {
        Serial.printf("[BOOT] Launcher return unavailable: %s\n", launcherBoot.message);
    }
    bootTraceStage("launcher-return");
    if (!psramFound() || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 1024 * 1024) {
        Serial.printf("[BOOT] FATAL: PSRAM unavailable or too fragmented (largest=%lu)\n",
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        while (true) delay(1000);
    }
    bootTraceStage("psram-check");

    // Step 3: Initialize I2C bus (shared by keyboard + touchscreen)
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(20);

    // Step 3.5: Initialize shared SPI bus
    sharedSPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    // Deassert all slave CS pins to prevent bus contention
    pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
    pinMode(SD_CS, OUTPUT);   digitalWrite(SD_CS, HIGH);
    bootTraceStage("i2c-spi");

    // Mount flash before radio bring-up so persisted RF settings are used from
    // the first SX1262 init, instead of always booting at the US default first.
    Serial.println("[BOOT] Mounting flash for early config...");
    if (flash.begin()) {
        flashMounted = true;
        userConfig.load(flash);
    } else {
        Serial.println("[BOOT] Early flash mount failed; using default radio config");
    }
    // Select palette before any LVGL styles are built
    Theme::setScheme(userConfig.settings().themeLight ? Theme::Scheme::LIGHT : Theme::Scheme::DARK);
    bootTraceStage("early-flash-config");

    // Step 4: Radio + SD init BEFORE display
    // Radio and SD must init while SPIClass exclusively owns SPI2_HOST.
    // LovyanGFX's init() later joins the bus via spi_bus_add_device().
    // This avoids any bus re-init dance that would invalidate device handles.
    Serial.println("[BOOT] Initializing radio...");
    if (radio.begin(userConfig.settings().loraFrequency)) {
        radioOnline = true;
        applyRadioSettingsToHardware(userConfig.settings(), "RADIO");
        Serial.printf("[RADIO] SX1262 online at %lu Hz\n",
                      (unsigned long)userConfig.settings().loraFrequency);
    } else {
        Serial.println("[RADIO] SX1262 not detected!");
    }
    bootTraceStage("radio-init");

    // SD card init (shared SPI, right after radio)
    digitalWrite(LORA_CS, HIGH);
    delay(10);
    if (sdStore.begin(&sharedSPI, SD_CS)) {
        sdHadExistingData = sdStore.hasExistingData();
        sdStore.formatForRsDeck();
        Serial.println("[SD] Card ready");
    } else {
        Serial.println("[SD] Not detected");
    }
    bootTraceStage("sd-probe");

    // Verify radio SPI still works after SD init
    if (radioOnline) {
        uint8_t sw_msb = radio.readRegister(0x0740);
        uint8_t sw_lsb = radio.readRegister(0x0741);
        Serial.printf("[BOOT] Radio SPI pre-display: syncword=0x%02X%02X %s\n",
            sw_msb, sw_lsb, (sw_msb == 0xFF && sw_lsb == 0xFF) ? "DEAD!" : "OK");
    }

    // Step 5: Display HAL — LovyanGFX + ST7789V
    // LovyanGFX's Bus_SPI::init() calls spi_bus_initialize() which will
    // return ESP_ERR_INVALID_STATE (bus already owned by SPIClass) and
    // then spi_bus_add_device() to join the existing bus. Both LGFX and
    // SPIClass get valid device handles on the same SPI2_HOST bus.
    display.begin();
    Serial.println("[BOOT] Display initialized (LovyanGFX direct)");
    bootTraceStage("display-init");

    // Step 5.5: Initialize LVGL display driver
    if (!display.beginLVGL()) {
        display.gfx().fillScreen(TFT_BLACK);
        display.gfx().setTextColor(TFT_RED, TFT_BLACK);
        display.gfx().drawString("LVGL/PSRAM failed", 24, 106);
        display.setBrightness(160);
        while (true) delay(1000);
    }
    Serial.println("[BOOT] LVGL initialized");
    bootTraceStage("lvgl-init");

    // Verify radio SPI survives display init
    if (radioOnline) {
        uint8_t sw_msb = radio.readRegister(0x0740);
        uint8_t sw_lsb = radio.readRegister(0x0741);
        Serial.printf("[BOOT] Radio SPI post-display: syncword=0x%02X%02X %s\n",
            sw_msb, sw_lsb, (sw_msb == 0xFF && sw_lsb == 0xFF) ? "DEAD!" : "OK");
    }

    // Step 6: UI manager (initializes both legacy and LVGL UI layers)
    ui.begin();
    ui.setBootMode(true);
    ui.setScreen(&lvBootScreen);
    ui.lvStatusBar().setLoRaOnline(radioOnline);
    lvBootScreen.setProgress(0.45f, radioOnline ? "Radio online" : "Radio FAILED");

    // Display::begin() left the backlight at 0 to hide an unpainted
    // framebuffer; the setProgress() above has now flushed the boot screen.
    // powerMgr at step 24 overrides with the user's configured value.
    display.setBrightness(128);
    bootTraceStage("boot-screen-painted");

    // Step 7: Touch HAL — GT911 I2C
    touch.begin();
    lvBootScreen.setProgress(0.50f, "Touch ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("touch-init");

    // Step 8: Keyboard HAL — ESP32-C3 I2C
    keyboard.begin();
    lvBootScreen.setProgress(0.52f, "Keyboard ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("keyboard-init");

    // Step 9: Trackball HAL — GPIO interrupts
    trackball.begin();
    lvBootScreen.setProgress(0.54f, "Trackball ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("trackball-init");

    // Step 10: Input manager
    inputManager.begin(&keyboard, &trackball, &touch);
    inputManager.setPowerMgr(&powerMgr);

    // Step 10.5: LVGL input drivers
    LvInput::init(&keyboard, &trackball, &touch);

    lvBootScreen.setProgress(0.55f, "Input ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("input-init");

    // Step 11: Register hotkeys
    hotkeys.registerHotkey('h', "Help", onHotkeyHelp);
    hotkeys.registerHotkey('m', "Messages", onHotkeyMessages);
    hotkeys.registerHotkey('n', "New Message", onHotkeyNewMsg);
    hotkeys.registerHotkey('s', "Settings", onHotkeySettings);
    hotkeys.registerHotkey('a', "Announce", onHotkeyAnnounce);
    hotkeys.registerHotkey('d', "Diagnostics", onHotkeyDiag);
    hotkeys.registerHotkey('i', "AutoIface dump", onHotkeyAutoIface);
    hotkeys.registerHotkey('t', "Radio Test", onHotkeyRadioTest);
    hotkeys.registerHotkey('r', "RSSI Monitor", onHotkeyRssiMonitor);
    hotkeys.setTabCycleCallback([](int dir) {
        ui.lvTabBar().cycleTab(dir);
        int tab = ui.lvTabBar().getActiveTab();
        if (lvTabScreens[tab]) ui.setScreen(lvTabScreens[tab]);
    });
    lvBootScreen.setProgress(0.58f, "Hotkeys registered");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("hotkeys");

    // Step 12: Mount LittleFS
    lvBootScreen.setProgress(0.60f, "Mounting flash...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    if (flashMounted) {
        Serial.println("[BOOT] LittleFS already mounted OK");
    } else if (!flash.begin()) {
        Serial.println("[BOOT] Flash init failed; preserving data and stopping startup");
        lvBootScreen.showError("Storage unavailable.\nNo data has been erased.");
        while (true) { lv_timer_handler(); delay(20); }
    } else {
        flashMounted = true;
        Serial.println("[BOOT] LittleFS mounted OK");
    }
    bootTraceStage("flash-mounted");

    // Step 13: Boot loop detection (NVS)
    {
        Preferences prefs;
        if (prefs.begin("ratdeck", false)) {
            int bc = prefs.getInt("bootc", 0);
            prefs.putInt("bootc", bc + 1);
            prefs.end();
            if (bc >= 3) {
                Serial.printf("[BOOT] Boot loop detected (%d failures)\n", bc);
                bootLoopRecovery = true;
            }
        }
    }
    bootTraceStage("bootloop-nvs");

    lvBootScreen.setProgress(0.64f, "Loading config...");
    userConfig.load(sdStore, flash);
    if (userConfig.recoveryRequired()) {
        lvBootScreen.showError("Saved settings could not be loaded.\nExisting files have been preserved.");
        while (true) { lv_timer_handler(); delay(20); }
    }
    // An SD recovery on first boot may supply the palette; re-sync it.
    {
        Theme::Scheme want = userConfig.settings().themeLight ? Theme::Scheme::LIGHT : Theme::Scheme::DARK;
        if (want != Theme::scheme()) { Theme::setScheme(want); ui.applyTheme(); }
    }
    inputManager.setTrackballSpeed(userConfig.settings().trackballSpeed);
    applyRadioSettingsToHardware(userConfig.settings(), "BOOT PRE-RNS");
    bootTraceStage("config-load");

    lvBootScreen.setProgress(0.65f, "Starting Reticulum...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    // Protocol runtime boots after the stores (identity slots + message store).
    Serial.printf("[BOOT] Protocol runtime selected — FFI %s\n", ProtocolRuntime::versionString());
    lvBootScreen.setProgress(0.72f, "Starting Reticulum");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("reticulum-begin");

    // Step 15.5: Identity manager
    if (!identityMgr.begin(&flash, &sdStore)) {
        lvBootScreen.showError("Identity recovery required.\nExisting keys have been preserved.");
        while (true) { lv_timer_handler(); delay(20); }
    }
    bootTraceStage("identity-manager");

    // Step 16: Message store
    lvBootScreen.setProgress(0.72f, "Starting messaging...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    messageStore.begin(&flash, &sdStore, userConfig.settings().sdStorageEnabled);
    bootTraceStage("message-store");

    // Protocol runtime lifecycle: init -> identity -> boot-seed ->
    // placement open_transport (SMALL node in PSRAM) -> pump. LXMF runs
    // store-only so the UI read surface works; sends go through the backend engines.
    if (protocolRuntime.begin(&flash, &sdStore, &identityMgr, &messageStore, nullptr,
                          RS_HANDHELD_PROFILE_SMALL,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        protocolRuntime.pump().attachLoRa(&rustLoraIface);
        protocolRuntime.pump().attachAuto(&autoIface);  // driver starts on SLAAC (iface 5)
        if (radioOnline && userConfig.settings().loraEnabled) rustLoraIface.start();
        Serial.printf("[BOOT] Rust transport up: dest=%s\n",
                      protocolRuntime.destinationHashHex().c_str());
        lvBootScreen.setProgress(0.75f, "Reticulum ready");
    } else {
        lvBootScreen.showError("Messaging could not start.\nYour saved data has not been reset.");
        for (;;) { lv_timer_handler(); delay(20); }
    }
    lxmf.beginStoreOnly(&messageStore);
    // Inbound notify (both envs: micro delegates to lxmf, rust fires from the
    // engines on every delivery path — opportunistic, link packet, resource).

    // Pre-cache unread counts so first tab switch to Messages is instant
    lxmf.unreadCount();
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("lxmf-begin");

    // Step 18: Announce manager
    lvBootScreen.setProgress(0.78f, "Loading contacts...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
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
    bootTraceStage("contacts-cache");

    // No default TCP hub.  Users opt in via Settings → TCP Server →
    // "Ratspeak Hub" (seeds rns.ratspeak.org) or "Custom" (host/port).

    // Sync display name between active identity slot and config.
    // The identity slot is the source of truth for the name.
    {
        String slotName;
        if (identityMgr.syncNameFromActive(slotName)) {
            if (!slotName.isEmpty()) {
                // Slot has a name — use it (overrides any stale config value)
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
    bootTraceStage("identity-name-sync");

    // Step 20: Boot loop recovery
    if (bootLoopRecovery) {
        userConfig.settings().wifiMode = RAT_WIFI_OFF;
        Serial.println("[BOOT] WiFi forced OFF (boot loop recovery)");
    }
    lvBootScreen.setProgress(0.83f, "Config loaded");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("bootloop-recovery");

    // Step 21: Apply radio config
    if (radioOnline && userConfig.settings().loraEnabled) {
        applyRadioSettingsToHardware(userConfig.settings(), "BOOT");
        ui.lvStatusBar().setLoRaOnline(true);
    } else if (radioOnline) {
        radio.sleep();
        ui.lvStatusBar().setLoRaOnline(false);
        Serial.println("[BOOT] LoRa disabled by config");
    }
    lvBootScreen.setProgress(0.84f, "Radio configured");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("radio-config");

    // Step 22: WiFi start
    RatWiFiMode wifiMode = userConfig.settings().wifiMode;
    ui.lvStatusBar().setWiFiEnabled(wifiMode != RAT_WIFI_OFF);
    if (wifiMode == RAT_WIFI_AP) {
        lvBootScreen.setProgress(0.87f, "Starting WiFi AP...");
        // (LVGL boot renders via lv_timer_handler in setProgress)
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
        ui.lvStatusBar().setWiFiActive(true);
    } else if (wifiMode == RAT_WIFI_STA) {
        lvBootScreen.setProgress(0.87f, "WiFi STA starting...");
        wifiConnection.begin(userConfig.settings());
        wifiSTAStarted = true;
    } else {
        lvBootScreen.setProgress(0.87f, "WiFi disabled");
        // (LVGL boot renders via lv_timer_handler in setProgress)
    }
    bootTraceStage("wifi-start");

    // Step 23: BLE stays disabled in default builds.
    lvBootScreen.setProgress(0.90f, "Links ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("links-ready");

    // Step 24: Power manager
    lvBootScreen.setProgress(0.92f, "Power manager...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    powerMgr.begin();
    powerMgr.setDimTimeout(userConfig.settings().screenDimTimeout);
    powerMgr.setOffTimeout(userConfig.settings().screenOffTimeout);
    powerMgr.setBrightness(userConfig.settings().brightness);
    powerMgr.setKbBrightness(userConfig.settings().keyboardBrightness, true);
    powerMgr.setKbAutoOn(userConfig.settings().keyboardAutoOn);
    powerMgr.setKbAutoOff(userConfig.settings().keyboardAutoOff);
    bootTraceStage("power-manager");

    // Step 24.5: GPS init
#if HAS_GPS
    if (userConfig.settings().gpsTimeEnabled) {
        lvBootScreen.setProgress(0.93f, "Starting GPS...");
        gps.setPosixTZ(currentPosixTZ());
        gps.setLocationEnabled(userConfig.settings().gpsLocationEnabled);
        gps.begin();
        Serial.println("[BOOT] GPS UART started (MIA-M10Q)");
        bootTraceStage("gps-start");
    }
#endif

    // Step 25: Audio init
    lvBootScreen.setProgress(0.94f, "Audio...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    audio.setEnabled(userConfig.settings().audioEnabled);
    audio.setVolume(userConfig.settings().audioVolume);
    audio.begin();
    bootTraceStage("audio-init");


    // Step 26: Battery init
    ui.lvStatusBar().setBatteryDisplay(userConfig.settings().batteryDisplay);
    powerMgr.setBatteryModel(userConfig.settings().batteryModel);
    powerMgr.setChargeThreshold(userConfig.settings().chargeThresholdV);
    powerMgr.setFullBatteryVoltage(userConfig.settings().fullBatteryV);



    // Boot complete — transition to Home screen
    // Yield to LVGL instead of blocking delay
    lvBootScreen.setProgress(0.98f, "Ready");
    for (int i = 0; i < 6; i++) { lv_timer_handler(); delay(1); }
    lvBootScreen.setProgress(1.0f, "Ready");
    audio.playBoot();
    bootTraceStage("boot-ready-screen");

    bootComplete = true;

    radio.setYieldCallback([]() { yield(); });

    serviceAvailable = deviceService.begin(announceManager);
    serviceClient.initialize(userConfig);
    serviceClient.onNotice = [](const char* message) {
        ui.lvStatusBar().showToast(message, 2000);
        if (strcmp(message, "New message") == 0) audio.requestMessage();
    };
    serviceClient.onConfigApplied = applyUiSettings;
    deviceService.pollNetwork = serviceNetworkPoll;
    deviceService.networkStatus = [](handheld::Status& status) {
#if HAS_GPS
        status.gpsFix = gps.hasTimeFix();
#endif
        status.radio = radioOnline && radio.isRadioOnline();
        status.lora = rustLoraIface.isOnline();
        status.wifiEnabled = WiFi.getMode() != WIFI_OFF;
        status.wifi = wifiSTAConnected;
        status.ap = wifiImpl && wifiImpl->isAPActive();
        status.autoPeers = autoIface.isOnline() ? int(autoIface.peerCount()) : -1;
        status.tcpTotal = tcpClients.size(); status.tcpUp = 0;
        for (auto* tcp : tcpClients) if (tcp && tcp->isConnected()) ++status.tcpUp;
        status.frequency = radio.getFrequency(); status.bandwidth = radio.getSignalBandwidth();
        status.sf = radio.getSpreadingFactor(); status.txPower = radio.getTxPower();
    };
    deviceService.applyRadio = [](const UserSettings& settings) { applyRadioSettingsToHardware(settings, "SETTINGS"); };
    deviceService.applyPeripherals = [](const UserSettings& s) {
#if HAS_GPS
        if (s.gpsTimeEnabled) {
            if (s.timezoneIdx < TIMEZONE_COUNT) gps.setPosixTZ(TIMEZONE_TABLE[s.timezoneIdx].posixTZ);
            gps.setLocationEnabled(s.gpsLocationEnabled);
            if (!gps.isRunning()) gps.begin();
        } else if (gps.isRunning()) gps.stop();
#endif
    };
    deviceService.homeReady = []() {
        bootAnnouncePending = true; bootAnnounceAttempts = 0;
        bootAnnounceAt = millis() + BOOT_ANNOUNCE_DELAY_MS;
    };
    deviceService.diagnostics = [](uint32_t action) {
        switch (action) {
        case 0: printAutoIface(); break;
        case 1: printDiagnostics(); break;
        case 2: runRadioTest(); break;
        case 3: startIrqMonitor(); break;
        case 4: startRssiMonitor(); break;
        }
    };
    deviceService.startScan = []() { wifiConnection.startScan(); };
    deviceService.finishScan = [](String& json) { return wifiConnection.finishScan(json); };
    deviceService.beginQuiesce = []() {
#if HAS_GPS
        gps.stop();
#endif
        wifiConnection.stop();
        protocolRuntime.pump().detachTcpAll();
        for (auto* tcp : tcpClients) retireTCPClient(tcp);
        tcpClients.clear();
        autoIface.stop();
        if (wifiImpl) wifiImpl->stop();
    };
    deviceService.quiescent = []() {
        drainRetiredTCPClients();
        return retiredTcpClients.empty() && !lvSettingsScreen.firmwareCheckRunning();
    };

    lvSettingsScreen.setService(&serviceClient);
    lvMessageView.setService(&serviceClient);
    lvMessagesScreen.setService(&serviceClient);
    lvContactsScreen.setService(&serviceClient);
    lvNodesScreen.setService(&serviceClient);
    lvHomeScreen.setService(&serviceClient);
    // Wire up LVGL screen dependencies
    lvHomeScreen.setBackend(&serviceClient.protocol);
    lvHomeScreen.setUserConfig(&serviceClient.config);
    lvHomeScreen.setLXMFManager(&serviceClient.messages);
    lvHomeScreen.setAnnounceManager(&serviceClient.nodes);
    lvHomeScreen.setAnnounceCallback([]() {
        serviceClient.action(handheld::Operation::Announce);
        Serial.println("[HOME] Announce triggered via Enter");
    });
    lvHomeScreen.setAudioToggleCallback([]() {
        serviceClient.config.settings().audioEnabled = !serviceClient.config.settings().audioEnabled;
        serviceClient.applySettings({}, false);
    });
    lvHomeScreen.setLoraToggleCallback([]() {
        auto& s = serviceClient.config.settings();
        s.loraEnabled = !s.loraEnabled;
        serviceClient.applySettings([](const handheld::Result& result) {
            if (result.outcome == handheld::Outcome::Ok) ui.lvStatusBar().showToast("Saved; reboot to apply", 2500);
        }, false);
    });
    lvHomeScreen.setTCPToggleCallback([]() {
        auto& s = serviceClient.config.settings();
        bool enabled = false;
        bool hasSavedTcpServer = false;
        for (const auto& ep : s.tcpConnections) {
            if (!ep.host.isEmpty()) hasSavedTcpServer = true;
            if (!ep.host.isEmpty() && ep.autoConnect) { enabled = true; break; }
        }
        if (enabled) {
            for (auto& ep : s.tcpConnections) ep.autoConnect = false;
        } else if (hasSavedTcpServer) {
            for (auto& ep : s.tcpConnections) {
                if (!ep.host.isEmpty()) ep.autoConnect = true;
            }
        } else {
            s.tcpConnections.clear();
            TCPEndpoint ep;
            ep.host = "rns.ratspeak.org";
            ep.port = TCP_DEFAULT_PORT;
            ep.autoConnect = true;
            s.tcpConnections.push_back(ep);
        }
        serviceClient.applySettings([](const handheld::Result& result) {
            if (result.outcome == handheld::Outcome::Ok) ui.lvStatusBar().showToast("Saved; reboot to apply", 2500);
        }, false);
    });
    lvHomeScreen.setWiFiToggleCallback([]() {
        auto& s = serviceClient.config.settings();
        if (s.wifiMode == RAT_WIFI_OFF) {
            RatWiFiMode restoreMode = s.wifiRestoreMode == RAT_WIFI_OFF ? RAT_WIFI_STA : s.wifiRestoreMode;
            if (restoreMode == RAT_WIFI_STA) {
                size_t slot = s.wifiSTASelected < s.wifiSTANetworks.size() ? s.wifiSTASelected : 0;
                if (slot >= s.wifiSTANetworks.size() || s.wifiSTANetworks[slot].ssid.isEmpty()) {
                    ui.lvStatusBar().showToast("Add WiFi in Settings", 2000);
                    return;
                }
            } else if (restoreMode != RAT_WIFI_AP) {
                ui.lvStatusBar().showToast("Add WiFi in Settings", 2000);
                return;
            }
            s.wifiMode = restoreMode;
        } else {
            s.wifiRestoreMode = s.wifiMode;
            s.wifiMode = RAT_WIFI_OFF;
        }
        serviceClient.applySettings([](const handheld::Result& result) {
            if (result.outcome == handheld::Outcome::Ok) ui.lvStatusBar().showToast("Saved; reboot to apply", 2500);
        }, false);
    });
#if HAS_GPS
    lvHomeScreen.setGPSToggleCallback([]() {
        auto& settings = serviceClient.config.settings();
        settings.gpsTimeEnabled = !settings.gpsTimeEnabled;
        serviceClient.applySettings({}, false);
    });
#else
    lvHomeScreen.setGPSToggleCallback([]() {
        ui.lvStatusBar().showToast("GPS unavailable", 1500);
    });
#endif
    lvHomeScreen.setPeersCallback([]() {
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_NODES);
        ui.setScreen(&lvNodesScreen);
    });

    lvContactsScreen.setAnnounceManager(&serviceClient.nodes);
    lvContactsScreen.setUIManager(&ui);
    lvContactsScreen.setNodeSelectedCallback([](const std::string& peerHex) {
        lvMessageView.setPeerHex(peerHex);
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_MSGS);
        ui.setScreen(&lvMessageView);
    });

    lvNodesScreen.setAnnounceManager(&serviceClient.nodes);
    lvNodesScreen.setUIManager(&ui);
    lvNodesScreen.setUserConfig(&serviceClient.config);
    lvNodesScreen.setNodeSelectedCallback([](const std::string& peerHex) {
        lvMessageView.setPeerHex(peerHex);
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_MSGS);
        ui.setScreen(&lvMessageView);
    });

    lvMessagesScreen.setLXMFManager(&serviceClient.messages);
    lvMessagesScreen.setAnnounceManager(&serviceClient.nodes);
    lvMessagesScreen.setBackend(&serviceClient.protocol);
    lvMessagesScreen.setUIManager(&ui);
    lvMessagesScreen.setOpenCallback([](const std::string& peerHex) {
        lvMessageView.setPeerHex(peerHex);
        ui.setScreen(&lvMessageView);
    });

    lvMessageView.setLXMFManager(&serviceClient.messages);
    lvMessageView.setBackend(&serviceClient.protocol);
    lvMessageView.setAnnounceManager(&serviceClient.nodes);
    lvMessageView.setUIManager(&ui);
    lvMessageView.setBackCallback([]() {
        ui.setScreen(&lvMessagesScreen);
    });

    lvSettingsScreen.setUserConfig(&serviceClient.config);
    lvSettingsScreen.setAudio(&audio);
    lvSettingsScreen.setPower(&powerMgr);
    lvSettingsScreen.setBackend(&serviceClient.protocol);
    lvSettingsScreen.setUIManager(&ui);
    lvSettingsScreen.setIdentityHash(serviceClient.protocol.identityHash());
    lvSettingsScreen.setDestinationHash(serviceClient.protocol.destinationHashHex());

    auto showQr = []() {
        // Honest gate: flips with serviceClient.protocol.protocolReady().
        if (!serviceClient.protocol.protocolReady()) {
            ui.lvStatusBar().showToast("QR not available: network not ready", 1500);
            return;
        }
        // Share the public Ratspeak contact card; the overlay retains legacy QR.
        if (!lvQrOverlay.show(serviceClient.config.settings().displayName,
                serviceClient.protocol.destinationHashHex(), serviceClient.protocol.identityHashHex(),
                serviceClient.protocol.publicKeyHex()))
            ui.lvStatusBar().showToast("Contact card unavailable", 2000);
    };
    lvSettingsScreen.setShowQrCallback(showQr);
    lvContactsScreen.setShowQrCallback(showQr);

    // LVGL help overlay
    lvHelpOverlay.create();
    lvQrOverlay.create();

    // Tab bar callbacks — LVGL
    lvTabScreens[LvTabBar::TAB_HOME]     = &lvHomeScreen;
    lvTabScreens[LvTabBar::TAB_CONTACTS] = &lvContactsScreen;
    lvTabScreens[LvTabBar::TAB_MSGS]     = &lvMessagesScreen;
    lvTabScreens[LvTabBar::TAB_NODES]    = &lvNodesScreen;
    lvTabScreens[LvTabBar::TAB_SETTINGS] = &lvSettingsScreen;

    ui.lvTabBar().setTabCallback([](int tab) {
        if (lvTabScreens[tab]) ui.setScreen(lvTabScreens[tab]);
    });
    bootTraceStage("screen-wiring");

    // Data clean screen (first boot only — when SD has old data)
    lvDataCleanScreen.setDoneCallback([](bool wipe) {
        lvDataCleanScreen.showStatus("Finishing storage operation...");
        serviceClient.lifecycle(wipe ? handheld::Operation::ClearOldDataAndRestart : handheld::Operation::EnableSDAndRestart);
    });

    // --- Boot flow helpers ---
    // Transition to home screen (shared by name input, timezone, and normal boot)
    auto goHome = []() {
        ui.setBootMode(false);
        ui.setScreen(&lvHomeScreen);
        ui.lvTabBar().setActiveTab(LvTabBar::TAB_HOME);
        homeAnnounceRequested = true;
        Serial.println("[BOOT] Home ready; startup announce scheduled");
    };

    // Show timezone screen, then go home
    auto showTimezone = [goHome]() {
        if (!serviceClient.config.settings().timezoneSet) {
            lvTimezoneScreen.setSelectedIndex(serviceClient.config.settings().timezoneIdx);
            ui.setScreen(&lvTimezoneScreen);
            Serial.println("[BOOT] Showing timezone selection");
        } else {
            goHome();
        }
    };

    // Timezone screen done callback
    lvTimezoneScreen.setDoneCallback([goHome](int tzIdx) {
        if (tzIdx < 0 || tzIdx >= TIMEZONE_COUNT) return;
        serviceClient.config.settings().timezoneIdx = uint8_t(tzIdx);
        serviceClient.config.settings().timezoneSet = true;
        serviceClient.applySettings([goHome, tzIdx](const handheld::Result& result) {
            if (result.outcome != handheld::Outcome::Ok) return;
            goHome();
            if (TIMEZONE_TABLE[tzIdx].radioRegion != serviceClient.config.settings().radioRegion)
                ui.lvStatusBar().showToast("Check radio region in Settings", 3000);
        }, false);
    });

    // Name input screen (first boot only — when no display name is set)
    lvNameInputScreen.setDoneCallback([showTimezone](const String& name) {
        serviceClient.config.settings().displayName = name.isEmpty()
            ? "Ratspeak.org-" + serviceClient.protocol.destinationHashHex().substring(0, 3) : name;
        serviceClient.applySettings([showTimezone](const handheld::Result& result) {
            if (result.outcome == handheld::Outcome::Ok) showTimezone();
        }, false);
    });

    if (sdHadExistingData && !serviceClient.config.settings().sdStorageEnabled) {
        ui.setScreen(&lvDataCleanScreen);
        Serial.println("[BOOT] Existing SD data found; waiting for user choice");
    } else if (serviceClient.config.settings().displayName.isEmpty()) {
        // First boot — go to name input
        ui.setScreen(&lvNameInputScreen);
        Serial.println("[BOOT] Showing name input screen");
    } else if (!serviceClient.config.settings().timezoneSet) {
        // Name set but timezone not — show timezone picker
        lvTimezoneScreen.setSelectedIndex(serviceClient.config.settings().timezoneIdx);
        ui.setScreen(&lvTimezoneScreen);
        Serial.println("[BOOT] Showing timezone selection (name already set)");
    } else {
        // Everything configured — go straight to home
        goHome();
    }
    bootTraceStage("boot-routing");

    // Clear boot loop counter — we survived!
    {
        Preferences prefs;
        if (prefs.begin("ratdeck", false)) {
            prefs.putInt("bootc", 0);
            prefs.end();
        }
    }
    bootTraceStage("bootcounter-clear");

    if (userConfig.settings().keyboardAutoOn) {
        // We are in ACTIVE power state here, switch keyboard backlight ON
        keyboard.backlightOn();
    }
    bootTraceStage("keyboard-auto");

    Serial.println("[BOOT] rsDeck ready");
    Serial.printf("[BOOT] Summary: radio=%s flash=%s sd=%s\n",
                  radioOnline ? "ONLINE" : "OFFLINE",
                  flash.isReady() ? "OK" : "FAIL",
                  sdStore.isReady() ? "OK" : "FAIL");
    bootTraceStage("setup-complete");
    if (serviceAvailable) serviceRunner.start();
    else ui.lvStatusBar().showToast("Runtime memory unavailable; reset device", 10000);
}

// =============================================================================
// Main Loop
// =============================================================================

static void applyUiSettings() {
    const auto& s = serviceClient.config.settings();
    const auto scheme = s.themeLight ? Theme::Scheme::LIGHT : Theme::Scheme::DARK;
    if (scheme != Theme::scheme()) { Theme::setScheme(scheme); ui.applyTheme(); }
    powerMgr.setBrightness(s.brightness);
    powerMgr.setDimTimeout(s.screenDimTimeout);
    powerMgr.setOffTimeout(s.screenOffTimeout);
    powerMgr.setKbBrightness(s.keyboardBrightness, true);
    powerMgr.setKbAutoOn(s.keyboardAutoOn);
    powerMgr.setKbAutoOff(s.keyboardAutoOff);
#if HAS_BATTERY_MODEL
    powerMgr.setBatteryModel(s.batteryModel);
    powerMgr.setChargeThreshold(s.chargeThresholdV);
    powerMgr.setFullBatteryVoltage(s.fullBatteryV);
#endif
    audio.setEnabled(s.audioEnabled); audio.setVolume(s.audioVolume);
    inputManager.setTrackballSpeed(s.trackballSpeed);


}

static void serviceNetworkPoll() {
#if HAS_GPS
    if (userConfig.settings().gpsTimeEnabled) gps.loop();
#endif
    wifiConnection.poll();
    handleSerialCommands();
    // 4. Reticulum loop (radio RX via LoRaInterface) — throttle to ~100Hz
    unsigned long rnsDuration = 0;
    {
        static unsigned long lastRNS = 0;
        unsigned long now = millis();
        if (now - lastRNS >= 10) {
            lastRNS = now;
            unsigned long rnsStart = millis();
            backend->loop();
            rnsDuration = millis() - rnsStart;
        }
    }


    if (bootComplete && bootAnnouncePending && (long)(millis() - bootAnnounceAt) >= 0) {
        bootAnnounceAttempts++;
        const auto result = announceWithName(true);
        if (result != ProtocolBackend::AnnounceResult::Failed) {
            bootAnnouncePending = false;
            lastAutoAnnounce = millis();
            Serial.println(result == ProtocolBackend::AnnounceResult::Sent
                               ? "[BOOT] Startup announce sent"
                               : "[BOOT] Startup announce queued");
        } else if (bootAnnounceAttempts < BOOT_ANNOUNCE_MAX_ATTEMPTS) {
            bootAnnounceAt = millis() + BOOT_ANNOUNCE_DELAY_MS;
            Serial.printf("[BOOT] Startup announce retry scheduled (%u/%u)\n",
                          (unsigned)bootAnnounceAttempts,
                          (unsigned)BOOT_ANNOUNCE_MAX_ATTEMPTS);
        } else {
            bootAnnouncePending = false;
            Serial.println("[BOOT] Startup announce skipped after retries");
        }
    }

    // 5. Auto-announce every 30-360 minutes from boot. Manual announces do
    // not reset this schedule.
    const unsigned long announceInterval = (unsigned long)userConfig.settings().announceInterval * 60000; // m -> ms
    if (bootComplete && millis() - lastAutoAnnounce >= announceInterval) {
        lastAutoAnnounce = millis();
        if (rustLoraIface.isOnline() && rustLoraIface.airtimeUtilization() > LoRaInterface::AIRTIME_THROTTLE) {
            Serial.println("[AUTO] Skipping announce: LoRa airtime > 25%");
        } else {
            const auto result = announceWithName(true);
            Serial.println(result == ProtocolBackend::AnnounceResult::Sent
                               ? "[AUTO] Periodic announce sent"
                               : (result == ProtocolBackend::AnnounceResult::Deferred
                                      ? "[AUTO] Periodic announce queued"
                                      : "[AUTO] Periodic announce not sent"));
        }
    }

    // 6. LXMF outgoing queue + announce manager deferred saves
    if (announceManager) announceManager->loop();

    // 7. WiFi STA connection handler
    if (wifiSTAStarted) {
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !wifiSTAConnected) {
            wifiSTAConnected = true;
            Serial.printf("[WIFI] STA connected: %s\n", WiFi.localIP().toString().c_str());

            // NTP time sync (DST-aware POSIX TZ string)
            {
                const char* tz = currentPosixTZ();
                configTzTime(tz, "pool.ntp.org", "time.nist.gov");
                Serial.printf("[NTP] Time sync started (TZ=%s)\n", tz);
            }

            // Recreate TCP clients on every WiFi connect (old clients may have stale sockets)
            reloadTCPClients();
            // Arm AutoInterface deferred-start; SLAAC needs ~1.5–10s to assign
            // a link-local IPv6 address, so we don't start the interface here.
            // Trigger link-local creation AFTER association (calling
            // esp_netif_create_ip6_linklocal pre-association is a no-op on
            // some Arduino-ESP32 versions).
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

    // 7.6. AutoInterface deferred start — fire once SLAAC assigns a link-local
    // IPv6 address.  Arduino's IPv6Address::toString returns the expanded
    // form ("0000:0000:..." for unset; "fe80:0000:..." once SLAAC completes),
    // so check the prefix bytes directly: link-local is fe80::/10.
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

    // 7.7. AutoInterface link-local rotation watch — covers SLAAC privacy
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

    // 7.8. Deferred TCP reload from Settings. Avoid tearing down/recreating
    // Transport interfaces inside the LVGL key event path.
    if (tcpReloadRequested) {
        tcpReloadRequested = false;
        Serial.println("[TCP] Applying deferred settings reload...");
        reloadTCPClients();
        if (announceManager) announceManager->clearTransientNodes();
    }

    // 8. WiFi + TCP loops (with global budget) — skip only if RNS severely overloaded
    {
        drainRetiredTCPClients();
        bool skipTcp = (rnsDuration > 500);
        if (skipTcp) diagTcpSkipEvents++;
        if (!skipTcp && wifiImpl) wifiImpl->loop();
        if (!skipTcp) {
            unsigned long tcpBudgetStart = millis();
            static size_t nextTcp = 0;
            for (size_t count = 0; count < tcpClients.size(); ++count) {
                if (millis() - tcpBudgetStart >= TCP_GLOBAL_BUDGET_MS) break;
                nextTcp %= tcpClients.size();
                tcpClients[nextTcp++]->loop();
                yield();
            }
        }
        // AutoInterface always runs — its loop is non-blocking, capped at 4
        // packets per socket per call, time-gated for announces/peer-jobs.
        // Skipping it under TCP load causes peers to time out (22 s silence
        // window) when a TCP flood holds the loop above the skip threshold.
        autoIface.loop();
    }

    // 9. BLE loops

    // 12.5. RSSI monitor (non-blocking, one sample per loop iteration)
    if (rssiMonitorActive && radioOnline) {
        unsigned long now = millis();
        if (now - rssiMonitorStart >= 5000) {
            rssiMonitorActive = false;
            Serial.printf("[RSSI] Done: %d samples, min=%d max=%d dBm\n",
                          rssiSampleCount, rssiMinVal, rssiMaxVal);
        } else if (now - rssiLastSample >= 100) {
            rssiLastSample = now;
            int rssi = radio.currentRssi();
            if (rssi < rssiMinVal) rssiMinVal = rssi;
            if (rssi > rssiMaxVal) rssiMaxVal = rssi;
            rssiSampleCount++;
            Serial.printf("[RSSI] %d dBm\n", rssi);
        }
    }

    if (irqMonitorActive) {
        const auto now = millis();
        if (now - irqMonitorStart >= 5000) irqMonitorActive = false;
        else if (now - irqLastSample >= 100) {
            irqLastSample = now;
            Serial.printf("[IRQ] flags=0x%04X rssi=%d\n", radio.getIrqFlags(), radio.currentRssi());
        }
    }
    if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeat = millis();
        const auto& status = deviceService.ownerStatus();
        Serial.printf("[SERVICE] heap=%lu psram=%lu max=%lu stack=%lu paths=%u links=%u queued=%d\n",
            (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram(),
            (unsigned long)status.serviceMaxMs, (unsigned long)status.stackFree,
            (unsigned)backend->pathCount(), (unsigned)backend->linkCount(), backend->lxmfQueuedCount());
    }
}

void loop() {
    serviceRunner.cooperativeTick();
    serviceClient.poll();
    if (homeAnnounceRequested && serviceClient.status().state == handheld::ServiceState::Running &&
        serviceClient.action(handheld::Operation::HomeReady)) homeAnnounceRequested = false;
    static uint32_t previousInputLoop = 0, maxInputGap = 0, lastUiMetrics = 0;
    const uint32_t loopNow = millis();
    if (previousInputLoop) maxInputGap = std::max(maxInputGap, loopNow - previousInputLoop);
    previousInputLoop = loopNow;
    if (loopNow - lastUiMetrics >= 5000) {
        lastUiMetrics = loopNow;
        Serial.printf("[UI] max_gap=%lu queued=%u peak=%u busy=%lu\n", (unsigned long)maxInputGap,
            (unsigned)deviceService.mailbox().outstanding(), (unsigned)deviceService.mailbox().peak(),
            (unsigned long)deviceService.mailbox().busyCount());
        const auto spi = sharedSPIStats();
        const auto& input = handheld::inputToFlush;
        Serial.printf("[LATENCY] samples=%lu p95_le=%lu p99_le=%lu max=%lu spi_wait_us=%lu spi_hold_us=%lu flash_ms=%lu ui_stack=%lu internal_largest=%lu psram_largest=%lu\n",
            (unsigned long)input.samples, (unsigned long)input.percentile(95), (unsigned long)input.percentile(99),
            (unsigned long)input.maximum, (unsigned long)spi.maxWaitUs, (unsigned long)spi.maxHoldUs,
            (unsigned long)handheld::maximumFlashWriteMs.load(), (unsigned long)uxTaskGetStackHighWaterMark(nullptr),
            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        maxInputGap = 0;
    }
    if (serviceClient.lifecycleComplete) {
        serviceClient.lifecycleComplete = false;
        ESP.restart();
    }
    if (serviceClient.lifecycleStarted && !serviceClient.lifecycleFailed &&
        millis() - serviceClient.lifecycleStarted > 5000) {
        ui.lvStatusBar().showToast("Still finishing; wait or hold power", 3000);
        serviceClient.lifecycleStarted = millis();
    }

    // 1. Input polling
    bool screenWasOn = powerMgr.isScreenOn();
    inputManager.update();
    if (inputManager.hadActivity() && powerMgr.isScreenOn()) handheld::inputObserved(millis());
    bool wakeOnlyInput = !screenWasOn && inputManager.hadStrongActivity();
    if (inputManager.hadStrongActivity()) {
        powerMgr.activity();       // Keyboard/touch: wake from any state
    } else if (inputManager.hadActivity()) {
        powerMgr.weakActivity();   // Trackball: wake from dim only
    }

    // 2. Long-press dispatch — screen blanking is the default if no screen consumes it
    if (inputManager.hadLongPress()) {
        if (lvQrOverlay.isVisible() || !ui.handleLongPress()) {
            powerMgr.forceScreenOff();
        }
    }

    // 3. Key event dispatch
    if (inputManager.hasKeyEvent() && !wakeOnlyInput) {
        const KeyEvent& evt = inputManager.getKeyEvent();

        // Help overlay intercepts all keys when visible
        if (lvHelpOverlay.isVisible()) {
            lvHelpOverlay.handleKey(evt);
        }
        // QR controls own navigation while the overlay is visible.
        else if (lvQrOverlay.isVisible()) {
            lvQrOverlay.handleKey(evt);
        }
        else {
            // Screen-local input owns the keyboard. This keeps message and
            // settings text entry from being preempted by global shortcuts.
            bool consumed = ui.handleKey(evt);
            if (!consumed) {
                bool hotkeyAllowed = !ui.isBootMode() || (evt.ctrl && evt.character == 'h');
                bool hotkeyConsumed = hotkeyAllowed && hotkeys.process(evt);
                if (!hotkeyConsumed) {

                    // Feed to LVGL input system only if the screen didn't consume it
                    LvInput::feedKey(evt);

                    // Tab cycling: ,=left /=right OR trackball left/right (only if screen didn't consume)
                    if (!evt.ctrl && !ui.isBootMode()) {
                        bool tabLeft  = (evt.character == ',') || evt.left;
                        bool tabRight = (evt.character == '/') || evt.right;
                        if (tabLeft) {
                            ui.lvTabBar().cycleTab(-1);
                            int tab = ui.lvTabBar().getActiveTab();
                            if (lvTabScreens[tab]) ui.setScreen(lvTabScreens[tab]);
                        }
                        if (tabRight) {
                            ui.lvTabBar().cycleTab(1);
                            int tab = ui.lvTabBar().getActiveTab();
                            if (lvTabScreens[tab]) ui.setScreen(lvTabScreens[tab]);
                        }
                    }
                }
            }
        }
    }

    // 3. LVGL timer handler — 30 FPS active, 5 FPS dimmed.
    // Bypass the throttle on input activity so a keypress/scroll renders this
    // iteration instead of waiting up to a full frame interval.
    {
        unsigned long now = millis();
        unsigned long lvglInterval = powerMgr.isDimmed() ? 200 : LVGL_INTERVAL_MS;
        bool inputBurst = inputManager.hadActivity();
        if (powerMgr.isScreenOn() && (inputBurst || now - lastLvglTime >= lvglInterval)) {
            lastLvglTime = now;
            lv_timer_handler();
        }
    }

    audio.loop();
    powerMgr.loop();
    if (millis() - lastStatusUpdate >= 100) {
        lastStatusUpdate = millis();
        const auto& status = serviceClient.status();
        ui.lvTabBar().setUnreadCount(LvTabBar::TAB_MSGS, status.unread);
        ui.lvStatusBar().setLoRaOnline(status.lora);
        ui.lvStatusBar().setWiFiEnabled(status.wifiEnabled);
        ui.lvStatusBar().setWiFiActive(status.wifi || status.ap);
        ui.lvStatusBar().setTCPConnected(status.tcpUp != 0);
        ui.lvStatusBar().setAutoIfacePeers(status.autoPeers);
        if (powerMgr.isScreenOn()) {
            ui.lvStatusBar().setBatteryPercent(powerMgr.batteryPercent());
            ui.lvStatusBar().setCharging(powerMgr.isCharging());
            ui.lvStatusBar().setBatteryDisplay(serviceClient.config.settings().batteryDisplay);
            ui.lvStatusBar().setUse24Hour(serviceClient.config.settings().use24HourTime);
            ui.lvStatusBar().updateTime();
#if HAS_GPS
            ui.lvStatusBar().setGPSFix(status.gpsFix);
#endif
            ui.update();
        }
    }
    yield();
}
