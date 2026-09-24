#include "config/BoardConfig.h"
#if defined(RSDECK) || defined(RSM9)
#include "runtime/LvglApplication.h"
#include "LvMemory.h"
#include "runtime/FactoryResetRecovery.h"
#include "runtime/DiscoveryStartup.h"
#include "hal/NetworkTime.h"
#include "radio/RadioSettings.h"
#include "diagnostics/DeviceDiagnostics.h"
#include "diagnostics/LvglUiSnapshot.h"
// =============================================================================
// Shared LVGL application composition for boards with the Deck-style lifecycle.
// Hardware adapters provide display, input, power and radio behavior.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <lvgl.h>

#include "config/BoardConfig.h"
#include "config/Config.h"
#if defined(RSDECK)
#include "platform/RsDeckModeSwitch.h"
#endif
#include "hal/Display.h"
#include "hal/TouchInput.h"
#if HAS_TRACKBALL
#include "hal/Trackball.h"
#endif
#include "hal/Keyboard.h"
#include "hal/Power.h"
#if HAS_GPS
#include "hal/GPSManager.h"
#endif
#include "radio/BoardRadio.h"
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
#include "runtime/NetworkCoordinator.h"
#include "runtime/AnnounceScheduler.h"
#include "transport/TcpClientSet.h"
#include "transport/RnsAutoInterface.h"
#include "config/UserConfig.h"
#include "config/SettingsTransaction.h"
#include "audio/AudioNotify.h"
#include "util/PerfTrace.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
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

#if defined(RSM9)
BoardRadio radio(&sharedSPI);
#else
BoardRadio radio(&sharedSPI,
    LORA_CS, SPI_SCK, SPI_MOSI, SPI_MISO,
    LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
    LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);
#endif

Display display;
TouchInput touch;
#if HAS_TRACKBALL
Trackball trackball;
#endif
Keyboard keyboard;

// --- Subsystems ---
InputManager inputManager;
HotkeyManager hotkeys;
UIManager ui;
FlashStore flash;
SDStore sdStore;
MessageStore messageStore;
LXMFManager lxmf;
// ProtocolRuntime owns the shared Rust FFI backend over the Lite crates.
// Announce, LXMF, Link and Resource operations require protocol readiness.
ProtocolRuntime protocolRuntime;
ProtocolBackend* backend = &protocolRuntime;
LoRaInterface rustLoraIface(&radio);  // pump-owned raw driver (id 0)
AnnounceManager* announceManager = nullptr;
TcpClientSet tcpClients;
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
handheld::AnnounceScheduler announceScheduler;

static void applyRadioSettingsToHardware(const UserSettings& s, const char* context) {
    if (!radioOnline) return;

    if (!s.loraEnabled) {
        radio.sleep();
        Serial.printf("[%s] LoRa disabled by config\n", context);
        return;
    }

    applyRadioSettings(radio, s);
    radioOnline = radio.isRadioOnline();
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

RnsAutoInterface autoIface;  // coordinator-borrowed driver
handheld::NetworkCoordinator network(wifiConnection, protocolRuntime.pump(), tcpClients, autoIface);

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


static bool hasUsableAnnounceTransport() {
    // Rust env: usable when the rust node is open and a pumped interface is live
    // (LoRa, TCP client, AutoInterface with peers, AP with clients).
    if (!backend->isTransportActive()) return false;
    if (protocolRuntime.pump().loraOnline()) return true;
    for (auto* tcp : tcpClients) {
        if (tcp && tcp->isConnected()) return true;
    }
    if (autoIface.isOnline() && autoIface.peerCount() > 0) return true;
    if (network.accessPoint() && network.accessPoint()->isAPActive() && network.accessPoint()->getClientCount() > 0) return true;
    return false;
}

static ProtocolBackend::AnnounceResult announceWithName(bool silent = false) {
    if (deviceService.settingsApplyPending()) return ProtocolBackend::AnnounceResult::Failed;
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

static void pollScheduledAnnounces() {
    using Scheduler = handheld::AnnounceScheduler;
    const auto event = announceScheduler.poll(uint32_t(millis()), userConfig.settings().announceInterval,
        rustLoraIface.isOnline() && rustLoraIface.airtimeUtilization() > LoRaInterface::AIRTIME_THROTTLE,
        [](Scheduler::Action, uint8_t) {
            const auto result = announceWithName(true);
            return result == ProtocolBackend::AnnounceResult::Sent ? Scheduler::Result::Sent :
                result == ProtocolBackend::AnnounceResult::Deferred ? Scheduler::Result::Deferred : Scheduler::Result::Failed;
        });
    if (event.action != Scheduler::Action::None)
        Serial.printf("[%s] Announce %s (attempt %u)\n",
            event.action == Scheduler::Action::Startup ? "BOOT" : "AUTO",
            event.result == Scheduler::Result::Sent ? "sent" : event.result == Scheduler::Result::Deferred ? "queued" :
            event.result == Scheduler::Result::Skipped ? "skipped: airtime busy" : "not sent", unsigned(event.attempt));
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

static void requestTCPClientsReload() {
    network.requestTcpReload();
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
        network.autoDeferred() ? "YES" : "no",
        static_cast<unsigned long>(network.autoDeferredElapsed()));
    Serial.println("======================");
}
static handheld::diagnostics::RemoteUiBridge remoteUi;
static handheld::DeviceDiagnostics deviceDiagnostics(radio, rustLoraIface, *backend, announceManager,
    radioOnline, DEVICE_NAME, "RSDECK-LXMF-TEST:", "RSDECK_TEST_1234567890", manualAnnounce);

// =============================================================================
// Helper: render boot screen immediately
// =============================================================================
void onHotkeyAutoIface() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 0); }
void onHotkeyDiag() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 1); }
void onHotkeyRadioTest() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 2); }
void onHotkeyIrqMonitor() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 3); }
void onHotkeyRssiMonitor() { serviceClient.action(handheld::Operation::Diagnostics, "", "", 4); }



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

void handheld::lvgl_application::setup() {
    deviceDiagnostics.remoteUi = &remoteUi;
    ratspeakRetainComponentId(RATSPEAK_COMPONENT_ID(BOARD_COMPONENT_ID, "standalone"));
#ifdef PROTOCOL_PACKET_TRACE
    deviceDiagnostics.traceWifi = [](const char* ssid, const char* password) {
        // Bench settings are transient; the shared coordinator owns callbacks
        // and reconnection exactly as for a selected saved network.
        UserSettings temporary;
        temporary.wifiMode = RAT_WIFI_STA;
        temporary.autoIfaceEnabled = userConfig.settings().autoIfaceEnabled;
        temporary.wifiSTANetworks.push_back({String(ssid), String(password)});
        network.begin(temporary);
        Serial.println("[BENCH-WIFI] connection requested");
    };
    deviceDiagnostics.traceTcp = [](const char* host, uint16_t port) {
        TCPEndpoint endpoint;
        endpoint.host = host; endpoint.port = port; endpoint.autoConnect = true;
        userConfig.settings().tcpConnections.clear();
        userConfig.settings().tcpConnections.push_back(endpoint);
        requestTCPClientsReload();
        Serial.println("[BENCH-TCP] reload requested");
    };
#endif
    const unsigned long setupStartMs = millis();
    bool flashMounted = false;

    // Step 1: Power pin — CRITICAL: enables all T-Deck Plus peripherals
    Power::enablePeripherals();

    // Step 2: Serial
#if (ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE) || defined(RSM9)
    // Fit an entire semantic reply plus ordinary logs before any writer starts.
    // HWCDC's default 256-byte queue forces large replies through its lossy
    // disconnected fallback or blocking drain path.
    const bool remoteUiTxReady = Serial.setTxBufferSize(
        handheld::diagnostics::RemoteUiReplyDelivery::TxCapacity) ==
        handheld::diagnostics::RemoteUiReplyDelivery::TxCapacity;
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    Serial.setTxTimeoutMs(handheld::diagnostics::RemoteUiReplyDelivery::WriteTimeoutMs);
#endif
#endif
    Serial.begin(SERIAL_BAUD);
#if (ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE) || defined(RSM9)
    if (!remoteUiTxReady) {
        remoteUi.close();
        Serial.println("[SERIAL] Serial UI unavailable: TX queue allocation failed");
    }
#endif
    delay(100);
    Serial.println();
    Serial.println("=================================");
    Serial.printf("  %s v%s\n", DEVICE_NAME, RSDECK_VERSION_STRING);
#ifdef BOARD_BETA_LABEL
    Serial.println("  " BOARD_BETA_LABEL);
#endif
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
#if defined(RSDECK)
    auto launcherBoot = rs_deck::returnToLauncherNextBoot();
    if (!launcherBoot.ok) {
        Serial.printf("[BOOT] Launcher return unavailable: %s\n", launcherBoot.message);
    }
    bootTraceStage("launcher-return");
#endif
    if (!psramFound() || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 1024 * 1024) {
        Serial.printf("[BOOT] FATAL: PSRAM unavailable or too fragmented (largest=%lu)\n",
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        while (true) delay(1000);
    }
    bootTraceStage("psram-check");

    // Step 3: Initialize the board's keyboard/touch bus at its supported rate.
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);
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
    if (flash.resetState() == FlashStore::ResetState::Clear &&
        radio.begin(userConfig.settings().loraFrequency)) {
        radioOnline = true;
        applyRadioSettingsToHardware(userConfig.settings(), "RADIO");
        Serial.printf("[RADIO] LoRa online at %lu Hz\n",
                      (unsigned long)userConfig.settings().loraFrequency);
    } else {
        Serial.println("[RADIO] LoRa unavailable");
    }
    bootTraceStage("radio-init");

    // SD card init (shared SPI, right after radio)
    bool sdInitializationFailed = false;
    digitalWrite(LORA_CS, HIGH);
    delay(10);
    if (sdStore.begin(&sharedSPI, SD_CS)) {
        if (flash.resetState() == FlashStore::ResetState::Clear) {
            sdHadExistingData = sdStore.hasExistingData();
            sdInitializationFailed = !sdStore.formatForRsDeck();
            Serial.println(sdInitializationFailed ? "[SD] Initialization failed; data preserved" : "[SD] Card ready");
        }
    } else {
        Serial.println("[SD] Not detected");
    }
    bootTraceStage("sd-probe");

#if !defined(RSM9)
    // Verify radio SPI still works after SD init
    if (radioOnline) {
        uint8_t sw_msb = radio.readRegister(0x0740);
        uint8_t sw_lsb = radio.readRegister(0x0741);
        Serial.printf("[BOOT] Radio SPI pre-display: syncword=0x%02X%02X %s\n",
            sw_msb, sw_lsb, (sw_msb == 0xFF && sw_lsb == 0xFF) ? "DEAD!" : "OK");
    }
#endif

    // Step 5: Display HAL — LovyanGFX + ST7789V
    // LovyanGFX's Bus_SPI::init() calls spi_bus_initialize() which will
    // return ESP_ERR_INVALID_STATE (bus already owned by SPIClass) and
    // then spi_bus_add_device() to join the existing bus. Both LGFX and
    // SPIClass get valid device handles on the same SPI2_HOST bus.
    if (!display.begin()) {
        // The shared display bus is unavailable; do not draw through it.
        Serial.println("[BOOT] Display initialization failed; restart to retry");
        while (true) delay(1000);
    }
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

#if !defined(RSM9)
    // Verify radio SPI survives display init
    if (radioOnline) {
        uint8_t sw_msb = radio.readRegister(0x0740);
        uint8_t sw_lsb = radio.readRegister(0x0741);
        Serial.printf("[BOOT] Radio SPI post-display: syncword=0x%02X%02X %s\n",
            sw_msb, sw_lsb, (sw_msb == 0xFF && sw_lsb == 0xFF) ? "DEAD!" : "OK");
    }
#endif

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
#if HAS_TOUCH
    touch.begin();
    lvBootScreen.setProgress(0.50f, "Touch ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("touch-init");
#endif

    // Step 8: Board keyboard HAL
    const bool keyboardReady = keyboard.begin();
    lvBootScreen.setProgress(0.52f, keyboardReady ? "Keyboard ready" : "Keyboard unavailable");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("keyboard-init");

#if HAS_TRACKBALL
    trackball.begin();
    inputManager.begin(&keyboard, &trackball, &touch);
    LvInput::init(&keyboard, &trackball, &touch);
#else
    inputManager.begin(&keyboard);
    LvInput::init(&keyboard);
#endif
    inputManager.setPowerMgr(&powerMgr);

    lvBootScreen.setProgress(0.55f, "Input ready");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("input-init");

    // A reset intent is checked before early config load. Display and input
    // are now ready; recover explicitly before any config/identity import.
    handheld::factoryResetRecovery(flash, sdStore, keyboard, display.gfx(), []() { ESP.restart(); });

    // Refuse a known SD initialization failure before dependent imports/writes.
    if (sdInitializationFailed) {
        lvBootScreen.showError("SD setup failed.\nRemove the card and restart.\nNo data has been erased.");
        while (true) { lv_timer_handler(); delay(20); }
    }

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
        if (prefs.begin(BOARD_BOOT_NAMESPACE, false)) {
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

    // Existing identities settle a committed name intent before any settings
    // drive hardware or messaging. A fresh identity is created by Runtime below.
    if (identityMgr.activeIndex() >= 0 || userConfig.settingsPending()) {
        const auto recovered = SettingsTransaction::recover(userConfig, identityMgr, sdStore, flash);
        if (!recovered.complete()) {
            lvBootScreen.showError("Settings recovery pending.\nRestart to retry; files preserved.");
            for (;;) { lv_timer_handler(); delay(20); }
        }
    }

    // An SD recovery on first boot may supply the palette; re-sync it.
    {
        Theme::Scheme want = userConfig.settings().themeLight ? Theme::Scheme::LIGHT : Theme::Scheme::DARK;
        if (want != Theme::scheme()) { Theme::setScheme(want); ui.applyTheme(); }
    }
#if HAS_TRACKBALL
    inputManager.setTrackballSpeed(userConfig.settings().trackballSpeed);
#endif
    bootTraceStage("config-load");

    // Step 16: Message store
    lvBootScreen.setProgress(0.72f, "Starting messaging...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    if (!messageStore.begin(&flash, &sdStore, userConfig.settings().sdStorageEnabled)) {
        lvBootScreen.showError("Storage could not start.\nYour saved messages have been preserved.");
        for (;;) { lv_timer_handler(); delay(20); }
    }
    bootTraceStage("message-store");

    // Protocol runtime lifecycle: init -> identity -> boot-seed ->
    // placement open_transport (SMALL node in PSRAM) -> pump. LXMF runs
    // store-only so the UI read surface works; sends go through the backend engines.
    if (protocolRuntime.begin(&flash, &sdStore, &identityMgr, &messageStore, nullptr,
                          RS_HANDHELD_PROFILE_SMALL,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        if (!SettingsTransaction::recover(userConfig, identityMgr, sdStore, flash).complete()) {
            lvBootScreen.showError("Settings recovery required.\nNo interfaces have started.");
            for (;;) { lv_timer_handler(); delay(20); }
        }
        applyRadioSettingsToHardware(userConfig.settings(), "BOOT COMMITTED");
        protocolRuntime.pump().attachLoRa(&rustLoraIface);
        if (radioOnline && userConfig.settings().loraEnabled) rustLoraIface.start();
        Serial.printf("[BOOT] Rust transport up: dest=%s\n",
                      protocolRuntime.destinationHashHex().c_str());
        lvBootScreen.setProgress(0.75f, "Reticulum ready");
    } else {
        lvBootScreen.showError("Messaging could not start.\nYour saved data has not been reset.");
        for (;;) { lv_timer_handler(); delay(20); }
    }
    lxmf.beginStoreOnly(&messageStore);
    // The incoming owner handles opportunistic, Link and Resource delivery.
    // It notifies the UI only after the durable storage result is accepted.

    // Pre-cache unread counts so first tab switch to Messages is instant
    lxmf.unreadCount();
    // (LVGL boot renders via lv_timer_handler in setProgress)
    bootTraceStage("lxmf-begin");

    // Step 18: Announce manager
    lvBootScreen.setProgress(0.78f, "Loading contacts...");
    // (LVGL boot renders via lv_timer_handler in setProgress)
    // Filter to lxmf.delivery so we don't capture every aspect (lxmf.propagation,
    // nomadnetwork.node, etc.) from the same peer as separate "doubled" entries.
    if (!handheld::prepareDiscovery(protocolRuntime, sdStore, flash,
                                    userConfig.settings().displayName, announceManager)) {
        Serial.println("[BOOT] Discovery memory unavailable; restart to retry");
        lvBootScreen.showAllocationError("Memory unavailable\nRestart to retry");
        while (true) { lv_timer_handler(); delay(20); }
    }
    // Backends consumed the boot seeds above (dedup ids + pending requeue) — free them.
    messageStore.releaseStartupSeeds();
    bootTraceStage("contacts-cache");

    // No default TCP hub.  Users opt in via Settings → TCP Server →
    // "Ratspeak Hub" (seeds rns.ratspeak.org) or "Custom" (host/port).


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
    lvBootScreen.setProgress(0.87f, wifiMode == RAT_WIFI_AP ? "Starting WiFi AP..." :
        wifiMode == RAT_WIFI_STA ? "WiFi STA starting..." : "WiFi disabled");
    if (!network.begin(userConfig.settings())) Serial.println("[WIFI] Selected mode could not start");
    ui.lvStatusBar().setWiFiActive(network.accessPoint() && network.accessPoint()->isAPActive());
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
#if defined(RSM9)
    gps.setPowerControl([](bool enabled) {
        const uint8_t revision = keyboard.revision();
        if (!revision) return false;
        digitalWrite(GPS_ENABLE_PIN, enabled == (revision == 2) ? HIGH : LOW);
        return true;
    });
#endif
    gps.setTimeEnabled(userConfig.settings().gpsTimeEnabled);
    if (userConfig.settings().gpsTimeEnabled || userConfig.settings().gpsLocationEnabled) {
        lvBootScreen.setProgress(0.93f, "Starting GPS...");
        gps.setPosixTZ(currentPosixTZ());
        gps.setLocationEnabled(userConfig.settings().gpsLocationEnabled);
        gps.begin();
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
#if HAS_BATTERY_MODEL
    powerMgr.setBatteryModel(userConfig.settings().batteryModel);
    powerMgr.setChargeThreshold(userConfig.settings().chargeThresholdV);
    powerMgr.setFullBatteryVoltage(userConfig.settings().fullBatteryV);
#endif



    // Allocate the command bridge before advertising readiness or routing UI.
    serviceAvailable = deviceService.begin(announceManager);
    if (!serviceAvailable) {
        Serial.println("[BOOT] Service memory unavailable; restart to retry");
        lvBootScreen.showAllocationError("Memory unavailable\nRestart to retry");
        while (true) { lv_timer_handler(); delay(20); }
    }

    if (!serviceClient.initialize(userConfig)) {
        Serial.println("[BOOT] Settings snapshot memory unavailable; restart to retry");
        lvBootScreen.showAllocationError("Memory unavailable\nRestart to retry");
        while (true) { lv_timer_handler(); delay(20); }
    }

    // Boot complete — transition to Home screen
    // Yield to LVGL instead of blocking delay
    lvBootScreen.setProgress(0.98f, "Ready");
    for (int i = 0; i < 6; i++) { lv_timer_handler(); delay(1); }
    lvBootScreen.setProgress(1.0f, "Ready");
    audio.playBoot();
    bootTraceStage("boot-ready-screen");

    bootComplete = true;

    radio.setYieldCallback([]() { yield(); });

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
        status.wifi = network.connected();
        status.ap = network.accessPoint() && network.accessPoint()->isAPActive();
        status.autoPeers = autoIface.isOnline() ? int(autoIface.peerCount()) : -1;
        status.tcpTotal = tcpClients.size(); status.tcpUp = 0;
        for (auto* tcp : tcpClients) if (tcp && tcp->isConnected()) ++status.tcpUp;
        status.frequency = radio.getFrequency(); status.bandwidth = radio.getSignalBandwidth();
        status.sf = radio.getSpreadingFactor(); status.txPower = radio.getTxPower();
    };
    deviceService.applyRadio = [](const UserSettings& settings, bool accepting) {
        return applyLiveRadioSettings(radio, rustLoraIface, settings, accepting);
    };
    deviceService.applyPeripherals = [](const UserSettings& s) {
        const auto data = encodeAnnounceName(s.displayName);
        protocolRuntime.seedAnnounceAppData(data.data(), data.size());
#if HAS_GPS
        gps.setTimeEnabled(s.gpsTimeEnabled);
        gps.setLocationEnabled(s.gpsLocationEnabled);
        if (s.gpsTimeEnabled || s.gpsLocationEnabled) {
            if (s.timezoneIdx < TIMEZONE_COUNT) gps.setPosixTZ(TIMEZONE_TABLE[s.timezoneIdx].posixTZ);
            if (!gps.isRunning()) gps.begin();
        } else if (gps.isRunning()) gps.stop();
#endif
    };
    deviceService.homeReady = []() {
        announceScheduler.begin(uint32_t(millis()), handheld::AnnounceScheduler::Startup::DelayedThree);
    };
    deviceService.diagnostics = [](uint32_t action) {
        switch (action) {
        case 0: printAutoIface(); break;
        case 1: deviceDiagnostics.printDiagnostics(); break;
        case 2: deviceDiagnostics.runRadioTest(); break;
        case 3: deviceDiagnostics.startIrqMonitor(); break;
        case 4: deviceDiagnostics.startRssiMonitor(); break;
        }
    };
    deviceService.startScan = []() { wifiConnection.startScan(); };
    deviceService.finishScan = [](String& json) { return wifiConnection.finishScan(json); };
    deviceService.closeAdmissions = []() {
        remoteUi.close();
        announceScheduler.stop();
        network.closeAdmissions(); protocolRuntime.beginMaintenance(rustLoraIface);
    };
    deviceService.pollSettlements = []() {
        protocolRuntime.pollMaintenance();
        deviceDiagnostics.pollResults();
        network.pollSettlements();
    };
    deviceService.settlementFailed = []() { return protocolRuntime.maintenanceFailed(); };
    deviceService.beginQuiesce = []() {
#if HAS_GPS
        gps.stop();
#endif
        network.stopHelpers();
    };
    deviceService.quiescent = []() {
        network.pollSettlements();
        return protocolRuntime.maintenanceDrained() && deviceDiagnostics.resultsDrained() && network.quiescent() && !lvSettingsScreen.firmwareCheckRunning();
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
            try {
                TCPEndpoint ep;
                if (!UserConfig::trySetString(ep.host, "rns.ratspeak.org", 15)) throw std::bad_alloc();
                ep.port = TCP_DEFAULT_PORT; ep.autoConnect = true;
                std::vector<TCPEndpoint> prepared;
                prepared.push_back(std::move(ep));
                s.tcpConnections.swap(prepared);
            } catch (const std::bad_alloc&) {
                ui.lvStatusBar().showToast("Settings memory unavailable; retry", 2000); return;
            }
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

    lvMessagesScreen.setAnnounceManager(&serviceClient.nodes);
    lvMessagesScreen.setBackend(&serviceClient.protocol);
    lvMessagesScreen.setUIManager(&ui);
    lvMessagesScreen.setOpenCallback([](const std::string& peerHex) {
        lvMessageView.setPeerHex(peerHex);
        ui.setScreen(&lvMessageView);
    });

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
    lvSettingsScreen.setShowHelpCallback(onHotkeyHelp);
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
        if (serviceClient.settingsPending() || tzIdx < 0 || tzIdx >= TIMEZONE_COUNT) return;
        serviceClient.config.settings().timezoneIdx = uint8_t(tzIdx);
        serviceClient.config.settings().timezoneSet = true;
        lvTimezoneScreen.setSaving(true);
        if (!serviceClient.applySettings([goHome, tzIdx](const handheld::Result& result) {
            lvTimezoneScreen.setSaving(false);
            if (result.outcome != handheld::Outcome::Ok) return;
            goHome();
            if (TIMEZONE_TABLE[tzIdx].radioRegion != serviceClient.config.settings().radioRegion)
                ui.lvStatusBar().showToast("Check radio region in Settings", 3000);
        }, false)) lvTimezoneScreen.setSaving(false);
    });

    // Name input screen (first boot only — when no display name is set)
    lvNameInputScreen.setDoneCallback([showTimezone](const String& name) {
        if (serviceClient.settingsPending()) return;
        char fallback[17];
        snprintf(fallback, sizeof fallback, "Ratspeak.org-%.3s", serviceClient.status().destination);
        const char* value = name.isEmpty() ? fallback : name.c_str();
        const size_t length = name.isEmpty() ? strlen(fallback) : name.length();
        if (!UserConfig::trySetString(serviceClient.config.settings().displayName, value, length)) {
            ui.lvStatusBar().showToast("Settings memory unavailable; retry", 2000); return;
        }
        serviceClient.config.settings().nameComplete = true;
        lvNameInputScreen.setSaving(true);
        if (!serviceClient.applySettings([showTimezone](const handheld::Result& result) {
            lvNameInputScreen.setSaving(false);
            if (result.outcome == handheld::Outcome::Ok) showTimezone();
        }, false)) lvNameInputScreen.setSaving(false);
    });

    if (sdHadExistingData && !serviceClient.config.settings().sdStorageEnabled) {
        ui.setScreen(&lvDataCleanScreen);
        Serial.println("[BOOT] Existing SD data found; waiting for user choice");
    } else if (!serviceClient.config.settings().nameComplete) {
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

    if (userConfig.settings().keyboardAutoOn) {
        // We are in ACTIVE power state here, switch keyboard backlight ON
        keyboard.backlightOn();
    }
    bootTraceStage("keyboard-auto");

    Serial.println("[BOOT] Handheld ready");
    Serial.printf("[BOOT] Summary: radio=%s flash=%s sd=%s\n",
                  radioOnline ? "ONLINE" : "OFFLINE",
                  flash.isReady() ? "OK" : "FAIL",
                  sdStore.isReady() ? "OK" : "FAIL");
    bootTraceStage("setup-complete");
    serviceRunner.start();
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
#if HAS_TRACKBALL
    inputManager.setTrackballSpeed(s.trackballSpeed);
#endif


}

// UI owner only. Physical and USB keys use exactly the same screen, overlay,
// shortcut and LVGL arbitration; USB has no separate settings/send path.
static void dispatchKey(const KeyEvent& evt) {
    LvInput::noteKeyActivity();

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
                const bool tabNavigation = !evt.ctrl && !ui.isBootMode() &&
                    (evt.character == ',' || evt.character == '/' || evt.left || evt.right);
                if (!tabNavigation) LvInput::feedKey(evt);

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

// Both input sources use the screen's existing hold action. QR keeps ownership;
// an unconsumed hold blanks the screen, as the trackball hold always has.
static void dispatchLongPress() {
    if (lvQrOverlay.isVisible() || !ui.handleLongPress()) {
        powerMgr.forceScreenOff();
    }
}

static KeyEvent remoteKey(const handheld::diagnostics::RemoteUiRequest& request) {
    using handheld::diagnostics::RemoteUiAction;
    using handheld::diagnostics::RemoteUiKey;
    KeyEvent event;
    if (request.action == RemoteUiAction::Character) {
        event.character = request.character;
        event.space = request.character == ' ';
        event.ctrl = request.ctrl;
    } else switch (request.key) {
        case RemoteUiKey::Up: event.up = true; break;
        case RemoteUiKey::Down: event.down = true; break;
        case RemoteUiKey::Left: event.left = true; break;
        case RemoteUiKey::Right: event.right = true; break;
        case RemoteUiKey::Enter: event.enter = true; break;
        case RemoteUiKey::Backspace: event.del = true; break;
        case RemoteUiKey::Escape: event.character = 0x1b; break;
        case RemoteUiKey::Tab: event.tab = true; break;
    }
    return event;
}

static void finishRemoteUi(const handheld::diagnostics::RemoteUiRequest& request, bool wokeOnly) {
    handheld::diagnostics::LvglUiState state;
    state.title = ui.getScreen() ? ui.getScreen()->title() : "";
    state.tab = ui.lvTabBar().getActiveTab();
    state.asleep = !powerMgr.isScreenOn();
    state.boot = ui.isBootMode();
    state.overlay = lvHelpOverlay.isVisible() ? "help" : lvQrOverlay.isVisible() ? "qr" : "none";
    state.focus = LvInput::group() ? lv_group_get_focused(LvInput::group()) : nullptr;
    lv_obj_update_layout(lv_scr_act());
    remoteUi.finish(handheld::diagnostics::LvglUiSnapshot::encode(
        remoteUi.buffer(), handheld::diagnostics::RemoteUiBridge::Capacity,
        request, state, wokeOnly, lv_scr_act(), lv_layer_top(), lv_layer_sys()));
}

static void serviceNetworkPoll() {
    // This callback runs only after successful storage/Service owner adoption.
    // Claim once before persistence; Failed startup must retain its boot count.
    static bool successRecorded = false;
    if (!successRecorded && backend->pollRadioBeforeBlockingWork()) {
        successRecorded = true;
        Preferences prefs;
        bool saved = false;
        if (prefs.begin(BOARD_BOOT_NAMESPACE, false)) {
            saved = prefs.putInt("bootc", 0) == sizeof(int32_t);
            prefs.end();
        }
        if (!saved) Serial.println("[BOOT] Boot success counter save failed");
    }
#if HAS_GPS
    if (gps.isRunning()) gps.loop();
#endif
    if (backend->pollRadioBeforeBlockingWork()) deviceDiagnostics.poll();
    // Poll protocol/radio no more often than every 10 ms; owner work can delay a poll.
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


    if (bootComplete && backend->pollRadioBeforeBlockingWork()) pollScheduledAnnounces();

    // Metadata persistence can block; TCP/AutoInterface progress below cannot.
    if (announceManager && backend->pollRadioBeforeBlockingWork()) announceManager->loop();

    // The shared owner retains this board's TCP budget and always polls AutoInterface.
    const auto networkEvents = network.poll(userConfig.settings(), rnsDuration,
        handheld::NetworkCoordinator::LargeBudget);
    if (networkEvents & handheld::NetworkCoordinator::Connected) {
        Serial.printf("[WIFI] STA connected: %s\n", WiFi.localIP().toString().c_str());
        const char* tz = currentPosixTZ();
        handheld::configureNetworkTime(tz);
        Serial.printf("[NTP] Time sync started (TZ=%s)\n", tz);
    }
    if (networkEvents & handheld::NetworkCoordinator::Disconnected)
        Serial.println("[WIFI] STA disconnected, old transports detached");
    if (networkEvents & handheld::NetworkCoordinator::AutoTimeout)
        Serial.println("[AUTOIFACE] Still waiting for IPv6 link-local; discovery will retry");
    if (networkEvents & handheld::NetworkCoordinator::AutoFailed)
        Serial.println("[AUTOIFACE] Socket startup failed; discovery will retry");
    if (networkEvents & handheld::NetworkCoordinator::TcpSkipped) ++diagTcpSkipEvents;
    if ((networkEvents & handheld::NetworkCoordinator::TcpReloaded) && announceManager)
        announceManager->clearTransientNodes();

    // Publish sampled diagnostics after network work.

    if (backend->pollRadioBeforeBlockingWork()) deviceDiagnostics.pollSamples();
    if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeat = millis();
        const auto& status = deviceService.ownerStatus();
        Serial.printf("[SERVICE] heap=%lu psram=%lu max=%lu stack=%lu paths=%u links=%u queued=%d\n",
            (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram(),
            (unsigned long)status.serviceMaxMs, (unsigned long)status.stackFree,
            (unsigned)backend->pathCount(), (unsigned)backend->linkCount(), backend->lxmfQueuedCount());
    }
}

void handheld::lvgl_application::loop() {
    lvSettingsScreen.pollFirmwareCheck();
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
        const auto memory = handheld_lvgl_memory_stats();
        Serial.printf("[LVGL] retained=%lu peak=%lu allocations=%lu refused=%lu\n",
            (unsigned long)memory.retained, (unsigned long)memory.peak,
            (unsigned long)memory.allocations, (unsigned long)memory.refused);
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
    LvInput::setEnabled(screenWasOn);
    inputManager.update(LvInput::canAcceptKey());
    if (inputManager.hadActivity() && powerMgr.isScreenOn()) handheld::inputObserved(millis());
    bool wakeOnlyInput = !screenWasOn && inputManager.hadStrongActivity();
    if (inputManager.hadStrongActivity()) {
        powerMgr.activity();       // Keyboard/touch: wake from any state
    } else if (inputManager.hadActivity()) {
        powerMgr.weakActivity();   // Trackball: wake from dim only
    }

    // 2. Long-press dispatch — screen blanking is the default if no screen consumes it
    if (inputManager.hadLongPress()) {
        dispatchLongPress();
    }

    // 3. Key event dispatch
    if (inputManager.hasKeyEvent() && !wakeOnlyInput) {
        const KeyEvent& evt = inputManager.getKeyEvent();
        dispatchKey(evt);
    }

    handheld::diagnostics::RemoteUiRequest remoteRequest;
    bool remotePending = remoteUi.take(remoteRequest);
    bool remoteInput = false, remoteWokeOnly = false;
    if (remotePending) {
        if (!remoteUi.accepting() || !serviceClient.available() ||
            serviceClient.status().state != handheld::ServiceState::Running || serviceClient.lifecycleStarted) {
            remoteUi.fail(remoteRequest.id, "unavailable");
            remotePending = false;
        } else if (remoteRequest.action != handheld::diagnostics::RemoteUiAction::View) {
            if (inputManager.hadActivity() || !LvInput::canAcceptKey()) {
                remoteUi.fail(remoteRequest.id, "busy");
                remotePending = false;
            } else {
                remoteWokeOnly = !powerMgr.isScreenOn();
                powerMgr.activity();
                handheld::inputObserved(millis());
                LvInput::setEnabled(true);
                remoteInput = true;
                // Match a physical first press while asleep: wake, never act.
                if (!remoteWokeOnly) {
                    if (remoteRequest.action == handheld::diagnostics::RemoteUiAction::Hold) dispatchLongPress();
                    else dispatchKey(remoteKey(remoteRequest));
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
        bool inputBurst = inputManager.hadActivity() || remoteInput;
        LvInput::setEnabled(powerMgr.isScreenOn());
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
    if (remotePending) finishRemoteUi(remoteRequest, remoteWokeOnly);
    yield();
}

#endif
