#include "runtime/FactoryResetRecovery.h"
#include "runtime/DiscoveryStartup.h"
#include "hal/NetworkTime.h"
#include "radio/RadioSettings.h"
#include "diagnostics/DeviceDiagnostics.h"
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
#include "storage/LegacyMessageArena.h"
#include "reticulum/AnnounceManager.h"
#include "reticulum/LXMFManager.h"
#include "reticulum/IdentityManager.h"
#include "protocol/ProtocolRuntime.h"
#include "transport/WiFiInterface.h"
#include "transport/TcpClientSet.h"
#include "runtime/NetworkCoordinator.h"
#include "runtime/AnnounceScheduler.h"
#include "transport/RnsAutoInterface.h"
#include "config/UserConfig.h"
#include "config/SettingsTransaction.h"
#include "screens/NodesScreen.h"
#include "screens/MessagesScreen.h"
#include "screens/MessageView.h"
#include "screens/SettingsScreen.h"
#include "screens/NameInputScreen.h"
#include "screens/MaintenanceScreen.h"
#include "runtime/MaintenanceOperation.h"
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
// ProtocolRuntime owns the shared Rust FFI backend over the Lite crates.
// Announce, LXMF, Link and Resource operations require protocol readiness.
ProtocolRuntime protocolRuntime;
ProtocolBackend* backend = &protocolRuntime;
LoRaInterface rustLoraIface(&radio);  // pump-owned raw driver (id 0)
IdentityManager identityMgr;
AnnounceManager* announceManager = nullptr;
TcpClientSet tcpClients;
RnsAutoInterface autoIface;  // coordinator-borrowed driver
handheld::WiFiConnection wifiConnection;
handheld::NetworkCoordinator network(wifiConnection, protocolRuntime.pump(), tcpClients, autoIface);
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
MaintenanceScreen maintenanceScreen;
handheld::MaintenanceBarrier maintenance;
handheld::Request maintenanceRequest;
uint32_t maintenanceNextId = 0;
bool maintenanceRestart = false;
enum class CardSettingsStep : uint8_t { Settings, Name, Timezone };
CardSettingsStep pendingSettingsStep = CardSettingsStep::Settings;
uint32_t lastSettingsRetry = 0;
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

// --- Timing state (millis-based throttling) ---
unsigned long lastRNS = 0;
unsigned long lastRender = 0;
handheld::AnnounceScheduler announceScheduler;
unsigned long lastHeartbeat = 0;
unsigned long lastStatusUpdate = 0;
unsigned long loopCycleStart = 0;
unsigned long maxLoopTime = 0;

// --- Intervals ---
constexpr unsigned long RNS_INTERVAL_MS = 10;         // 10 ms minimum protocol poll interval
constexpr unsigned long RENDER_INTERVAL_MS = 50;       // 20 FPS
constexpr unsigned long STATUS_UPDATE_MS = 1000;       // 1 Hz status bar
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 5000;

// Power-aware RNS interval
unsigned long rnsInterval = RNS_INTERVAL_MS;

static const char* currentPosixTZ() {
    if (userConfig.settings().timezoneIdx < TIMEZONE_COUNT) {
        return TIMEZONE_TABLE[userConfig.settings().timezoneIdx].posixTZ;
    }
    return "EST5EDT,M3.2.0,M11.1.0";  // Fallback (core config has no raw UTC offset)
}

// =============================================================================
// TCP client management — stop old clients, create new from config
// =============================================================================

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

static void pollScheduledAnnounces() {
    using Scheduler = handheld::AnnounceScheduler;
    const auto event = announceScheduler.poll(uint32_t(millis()), userConfig.settings().announceInterval,
        rustLoraIface.isOnline() && rustLoraIface.airtimeUtilization() > LoRaInterface::AIRTIME_THROTTLE,
        [](Scheduler::Action action, uint8_t) {
            const auto result = announceWithName(action == Scheduler::Action::Periodic && !power.isScreenOn());
            return result == ProtocolBackend::AnnounceResult::Sent ? Scheduler::Result::Sent :
                result == ProtocolBackend::AnnounceResult::Deferred ? Scheduler::Result::Deferred : Scheduler::Result::Failed;
        });
    if (event.action != Scheduler::Action::None)
        Serial.printf("[%s] Announce %s (attempt %u)\n",
            event.action == Scheduler::Action::Startup ? "BOOT" : "AUTO",
            event.result == Scheduler::Result::Sent ? "sent" : event.result == Scheduler::Result::Deferred ? "queued" :
            event.result == Scheduler::Result::Skipped ? "skipped: airtime busy" : "not sent", unsigned(event.attempt));
}


static void finalizeBoot() {
    if (!maintenance.accepting() || userConfig.settingsPending() || settingsScreen.radioApplyPending()) return;
    ui.setBootMode(false);
    ui.setScreen(&homeScreen);
    ui.tabBar().setActiveTab(TabBar::TAB_HOME);
    if (announceScheduler.begin(uint32_t(millis()), handheld::AnnounceScheduler::Startup::ImmediateOnce))
        pollScheduledAnnounces();
}

static void finishCardSettings(CardSettingsStep step) {
    if (!maintenance.accepting()) return;
    const auto& settings = userConfig.settings();
#if HAS_GPS
    gps.setTimeEnabled(settings.gpsTimeEnabled);
    gps.setLocationEnabled(settings.gpsLocationEnabled);
    if (settings.gpsTimeEnabled || settings.gpsLocationEnabled) {
        gps.setPosixTZ(currentPosixTZ());
        if (!gps.isRunning()) gps.begin();
    } else if (gps.isRunning()) gps.stop();
#endif
    if (settings.timezoneIdx < TIMEZONE_COUNT) {
        setenv("TZ", TIMEZONE_TABLE[settings.timezoneIdx].posixTZ, 1); tzset();
#if HAS_GPS
        if (gps.isRunning()) gps.setPosixTZ(TIMEZONE_TABLE[settings.timezoneIdx].posixTZ);
#endif
    }
    const auto data = encodeAnnounceName(settings.displayName);
    protocolRuntime.seedAnnounceAppData(data.data(), data.size());
    nameInputScreen.setSaveStatus(nullptr, false);
    timezoneScreen.setSaveStatus(nullptr, false);
    if (step != CardSettingsStep::Settings && ui.isBootMode()) {
        if (!settings.timezoneSet) {
            timezoneScreen.setSelectedIndex(settings.timezoneIdx); ui.setScreen(&timezoneScreen);
        } else if (!settings.nameComplete) ui.setScreen(&nameInputScreen);
        else finalizeBoot();
    }
    ui.markAllDirty();
}

static SettingsTransaction::Result saveCardSettings(UserConfig& candidate, CardSettingsStep step) {
    if (!maintenance.accepting()) return {SettingsTransaction::State::Invalid, "Shutdown in progress"};
    if (settingsScreen.radioApplyPending()) return {SettingsTransaction::State::Invalid, "Waiting for radio"};
    const auto result = SettingsTransaction::apply(userConfig, candidate, identityMgr, sdStore, flash);
    const bool pending = result.state == SettingsTransaction::State::Pending;
    if (pending) { pendingSettingsStep = step; lastSettingsRetry = millis(); }
    const char* caption = pending ? "Save pending; please wait" : result.complete() ? nullptr : "Save failed; Enter retries";
    if (step == CardSettingsStep::Name) nameInputScreen.setSaveStatus(caption, pending);
    if (step == CardSettingsStep::Timezone) timezoneScreen.setSaveStatus(caption, pending);
    if (result.complete()) {
        if (step != CardSettingsStep::Settings) settingsScreen.applyCommitted();
        if (settingsScreen.radioApplyPending()) pendingSettingsStep = step;
        else finishCardSettings(step);
    }
    ui.markAllDirty();
    return result;
}

static void pollCardSettings() {
    if (!userConfig.settingsPending() || millis() - lastSettingsRetry < 1000) return;
    lastSettingsRetry = millis();
    const auto result = SettingsTransaction::recover(userConfig, identityMgr, sdStore, flash);
    if (!result.complete()) return;
    // Failed maintenance can finish cleanup, never invoke an old UI/hardware continuation.
    if (maintenance.accepting()) {
        settingsScreen.applyCommitted();
        if (settingsScreen.radioApplyPending()) return;
        finishCardSettings(pendingSettingsStep);
    }
    pendingSettingsStep = CardSettingsStep::Settings;
}

static void pollCardRadioSettings() {
    if (!settingsScreen.pollRadioApply(maintenance.accepting())) return;
    if (maintenance.accepting() && pendingSettingsStep != CardSettingsStep::Settings)
        finishCardSettings(pendingSettingsStep);
    pendingSettingsStep = CardSettingsStep::Settings;
    ui.markContentDirty();
}

void onHotkeyAnnounce() {
    Serial.println("[HOTKEY] Force announce");
    announceWithName();
}
static handheld::DeviceDiagnostics diagnostics(radio, rustLoraIface, *backend, announceManager,
    radioOnline, "Cardputer Adv", "RSCARDPUTER-LXMF-TEST:", "RSCARDPUTER_TEST_1234567890",
    []() { announceWithName(false); });
void onHotkeyDiag() { diagnostics.printDiagnostics(); }
void onHotkeyRssiMonitor() { diagnostics.startRssiMonitor(); }
void onHotkeyRadioTest() { diagnostics.runRadioTest(); }

static bool requestMaintenance(handheld::Operation operation) {
    if (maintenanceNextId == UINT32_MAX) return false;
    switch (operation) {
    case handheld::Operation::Restart: case handheld::Operation::FormatSD:
    case handheld::Operation::WipeSD: case handheld::Operation::FactoryReset:
    case handheld::Operation::ClearOldDataAndRestart: break;
    default: return false;
    }
    if (maintenance.phase() == handheld::MaintenanceBarrier::Phase::Failed &&
        operation == handheld::Operation::Restart && !maintenanceRestart) {
        // A new explicit request after the on-screen data-loss warning. Never
        // run the old wipe or free a retained writer to simulate a clean stop.
        ++maintenanceNextId;
        maintenanceRestart = true;
        return true;
    }
    if (!maintenance.accepting()) return false;
    handheld::Request request;
    request.id = ++maintenanceNextId; request.generation = 1;
    request.operation = operation; request.admittedAt = millis();
    if (!maintenance.begin({request.id, request.generation}, request.admittedAt)) return false;
    maintenanceRequest = request;
    announceScheduler.stop();
    settingsScreen.pollRadioApply(false);
    network.closeAdmissions();
    protocolRuntime.beginMaintenance(rustLoraIface);
    ui.setOverlay(nullptr);
    ui.setScreen(&maintenanceScreen); // Closes visible read-marker admission.
    maintenanceScreen.show("Saving and closing connections");
    ui.markAllDirty();
    power.activity();
    return true;
}

static void pollMaintenance() {
    if (maintenance.accepting()) return;
    if (maintenanceScreen.takeRestart()) requestMaintenance(handheld::Operation::Restart);
    // Before stop(), maintain may start a deferred reload. Once helpers have
    // begun retiring, it only completes the already-owned client destruction.
    network.pollSettlements();
    handheld::MaintenanceBarrier::Snapshot snapshot;
    snapshot.normalPending = wifiConnection.scanning() || userConfig.settingsPending() || settingsScreen.radioApplyPending();
    snapshot.applicationPending = !backend->lxmfDrained() || !diagnostics.resultsDrained() ||
        messageStore.writeQueue().drainCount() != 0;
    snapshot.helpersPending = snapshot.normalPending || !protocolRuntime.maintenanceDrained() ||
        !network.quiescent();
    snapshot.storageStopped = maintenance.storageStopStarted() && messageStore.finishStop();
    snapshot.error = backend->lxmfDrainError();
    snapshot.unrecoverable = protocolRuntime.maintenanceFailed();
    const auto actions = maintenance.step(millis(), snapshot);
    if (actions & handheld::MaintenanceBarrier::ReportFailure) {
        maintenanceScreen.show(snapshot.unrecoverable ? "Radio could not finish." :
            userConfig.settingsPending() ? "Settings recovery is pending." :
            snapshot.error != handheld::storage::Error::None ? "Message status needs saving." :
            snapshot.applicationPending ? "Message work is still pending." :
            snapshot.helpersPending ? "Connection is still closing." :
            "Storage work is still pending.", true);
        Serial.printf("[MAINTENANCE] failed owners=%u error=%u\n",
            unsigned(maintenance.failedPending()), unsigned(maintenance.lastError()));
        ui.markAllDirty();
        power.activity();
    }
    if (actions & handheld::MaintenanceBarrier::BeginHelpers) {
#if HAS_GPS
        gps.stop();
#endif
        network.stopHelpers();
    }
    if (actions & handheld::MaintenanceBarrier::StopStorage) messageStore.requestStop();
    if (actions & handheld::MaintenanceBarrier::Perform) {
        const auto result = handheld::performMaintenance(maintenance, maintenanceRequest,
            *backend, userConfig, identityMgr, flash, sdStore, announceManager);
        maintenanceScreen.show(result.ok ? "Ready to restart" : result.detail, !result.ok);
        Serial.printf("[MAINTENANCE] %s\n", result.detail);
        ui.markAllDirty();
        power.activity();
        maintenanceRestart = result.ok;
    }
    if (maintenanceRestart) {
        maintenanceRestart = false;
        ui.render(); ui.flush();
        ESP.restart();
    }
}

// =============================================================================
// Announce with display name
// =============================================================================


static ProtocolBackend::AnnounceResult announceWithName(bool silent) {
    if (userConfig.settingsPending() || settingsScreen.radioApplyPending()) return ProtocolBackend::AnnounceResult::Failed;
    if (!maintenance.accepting()) return ProtocolBackend::AnnounceResult::Failed;
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

// =============================================================================
// Setup
// =============================================================================

void setup() {
    ratspeakRetainComponentId(RATSPEAK_COMPONENT_ID("cardputer", "standalone"));
    diagnostics.boardHelp = "[SERIAL] f enable Cap LoRa RF switch";
    diagnostics.boardCommand = [](char command) {
        if (command != 'f') return false;
        enableCapLoRaRfSwitch();
        return true;
    };
    diagnostics.extraDump = []() {
        Serial.printf("WriteQ pending: %d\n", messageStore.writeQueue().drainCount());
    };
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
    if (!ui.begin()) {
        Serial.println("[BOOT] Canvas memory unavailable; restart to retry");
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawString("Display memory unavailable", 8, 45);
        M5.Display.drawString("Restart to retry", 8, 65);
        while (true) delay(1000);
    }
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
    const bool flashReady = flash.begin();
    if (!flashReady) {
        Serial.println("[BOOT] Flash startup blocked; preserving reset/data recovery");
        bootScreen.setProgress(0.25f, "Storage recovery pending");
        ui.render();
    }
    bootScreen.setProgress(0.3f, "Storage ready");
    ui.render();

    // Boot loop detection (NVS — separate from LittleFS)
    if (flashReady) {
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
    // This board owns the LoRa/SD bus. The shared driver expects it to be
    // initialized before its first probe; keep both chip selects idle.
    pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
    pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
    loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
    if (flashReady && enableCapLoRaRfSwitch() && radio.begin(LORA_DEFAULT_FREQ)) {
        radio.setSpreadingFactor(LORA_DEFAULT_SF);
        radio.setSignalBandwidth(LORA_DEFAULT_BW);
        radio.setCodingRate4(LORA_DEFAULT_CR);
        radio.setTxPower(LORA_DEFAULT_TX_POWER);
        radio.setPreambleLength(LORA_DEFAULT_PREAMBLE);
        radio.receive();
        radioOnline = radio.isRadioOnline();
        ui.statusBar().setLoRaOnline(radioOnline);
        Serial.println(radioOnline ? "[RADIO] SX1262 online at 915 MHz" : "[RADIO] Configuration failed");
        bootScreen.setProgress(0.6f, radioOnline ? "Radio online" : "Radio: OFFLINE");
    } else {
        Serial.println("[RADIO] SX1262 not detected!");
        bootScreen.setProgress(0.6f, "Radio: OFFLINE");
    }
    ui.render();

    // Initialize SD card (shares FSPI/SPI2 with radio — must init after radio)
    bool sdInitializationFailed = false;
    bootScreen.setProgress(0.65f, "Checking SD card...");
    ui.render();
    // The bus is also ready for SD recovery when flash blocked modem startup.
    if (sdStore.begin(&loraSPI, SD_CS)) {
        if (flashReady) {
            sdInitializationFailed = !sdStore.formatForRsDeck();
        }
        bootScreen.setProgress(0.68f, sdInitializationFailed ? "SD setup failed" : "SD card ready");
    } else {
        bootScreen.setProgress(0.68f, "No SD card");
    }
    ui.render();

    handheld::factoryResetRecovery(flash, sdStore, keyboard, M5Cardputer.Display, []() { ESP.restart(); });

    // Refuse a known SD initialization failure before dependent imports/writes.
    if (sdInitializationFailed) {
        bootScreen.setProgress(0.68f, "SD setup failed; remove & restart");
        ui.render();
        for (;;) delay(20);
    }

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

    if (identityMgr.activeIndex() >= 0 || userConfig.settingsPending()) {
        if (!SettingsTransaction::recover(userConfig, identityMgr, sdStore, flash).complete()) {
            bootScreen.setProgress(0.9f, "Settings recovery - restart to retry");
            ui.render(); for (;;) delay(20);
        }
    }

    // Initialize message store + LXMF
    bootScreen.setProgress(0.91f, "Starting messaging...");
    ui.render();
    if (!handheld::storage::legacyMessageArena().reserve() ||
        !messageStore.begin(&flash, &sdStore, userConfig.settings().sdStorageEnabled)) {
        bootScreen.setProgress(0.91f, "Storage error - data preserved");
        ui.render();
        for (;;) delay(20);
    }
    // Protocol runtime lifecycle: init -> identity -> boot-seed ->
    // placement open_transport (MICRO node in internal heap) -> pump. LXMF
    // runs store-only so the UI read surface works; sends go through the backend engines.
    if (protocolRuntime.begin(&flash, &sdStore, &identityMgr, &messageStore, nullptr,
                          RS_HANDHELD_PROFILE_MICRO,
                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
        if (!SettingsTransaction::recover(userConfig, identityMgr, sdStore, flash).complete()) {
            bootScreen.setProgress(0.9f, "Settings error - no interfaces started");
            ui.render(); for (;;) delay(20);
        }
        protocolRuntime.pump().attachLoRa(&rustLoraIface);
        if (radioOnline) applyRadioSettings(radio, userConfig.settings());
        if (radioOnline && userConfig.settings().loraEnabled) rustLoraIface.start();
        Serial.printf("[BOOT] Rust transport up: dest=%s\n",
                      protocolRuntime.destinationHashHex().c_str());
        bootScreen.setProgress(0.92f, "Reticulum ready");
    } else {
        bootScreen.setProgress(0.92f, "Messaging error - data preserved");
        ui.render();
        for (;;) delay(20);
    }
    lxmf.beginStoreOnly(&messageStore);
    // All receive paths publish a committed record on the protocol owner. The
    // callback invalidates views; bodies remain in the authoritative store.
    backend->setMessageCallback([](const LXMFManager::CommittedMessage& message) {
        const auto* peer = message.key.peer;
        Serial.printf("[LXMF] New message from %02x%02x%02x%02x\n", peer[0], peer[1], peer[2], peer[3]);
        ui.tabBar().setUnreadCount(TabBar::TAB_MSGS, lxmf.unreadCount());
        ui.markContentDirty();
        ui.markTabDirty();
        messagesScreen.notifyNewMessage();
        messageView.notifyNewMessage(message.key);
        pendingMessageSound = true;  // Played from main loop — delay() in callback blocks transport
    });

    // Status callback — update UI when send completes (SENT/FAILED)
    backend->setStatusCallback([](const std::string& peerHex, double, uint32_t counter, LXMFStatus status) {
        messageView.notifyStatusChange(peerHex, counter, status);
        ui.markContentDirty();
    });

    // Unread counts load lazily on first MessagesScreen open (deferred from boot)

    // Register announce handler
    bootScreen.setProgress(0.93f, "Starting discovery...");
    ui.render();
    // Filter to lxmf.delivery so we don't capture every aspect (lxmf.propagation,
    // nomadnetwork.node, etc.) from the same peer as separate "doubled" entries.
    if (!handheld::prepareDiscovery(protocolRuntime, sdStore, flash,
                                    userConfig.settings().displayName, announceManager)) {
        Serial.println("[BOOT] Discovery memory unavailable; restart to retry");
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.drawString("Memory unavailable", 8, 45);
        M5.Display.drawString("Restart to retry", 8, 65);
        while (true) delay(1000);
    }
    // Backends consumed the boot seeds above (dedup ids + pending requeue) — free them.
    messageStore.releaseStartupSeeds();





    // Seed default TCP hubs if no connections configured (off by default)
    if (userConfig.settings().tcpConnections.empty()) {
        TCPEndpoint ep1;
        ep1.host = RATSPEAK_HUB_HOST;
        ep1.port = TCP_DEFAULT_PORT;
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

    // Mode-based WiFi startup
    RatWiFiMode wifiMode = userConfig.settings().wifiMode;

    bootScreen.setProgress(0.95f, wifiMode == RAT_WIFI_AP ? "Starting WiFi AP..." :
        wifiMode == RAT_WIFI_STA ? "WiFi STA starting..." : "WiFi disabled");
    ui.render();
    if (!network.begin(userConfig.settings())) Serial.println("[WIFI] Selected mode could not start");

    // BLE disabled
    Serial.println("[BLE] Disabled (stub — v1.1)");

    // Initialize GPS (Cap LoRa-1262 GNSS module)
#if HAS_GPS
    gps.setTimeEnabled(userConfig.settings().gpsTimeEnabled);
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
    settingsScreen.setRadioApply([](const UserSettings& settings, bool accepting) {
        return applyLiveRadioSettings(radio, rustLoraIface, settings, accepting);
    });
    settingsScreen.setAudio(&audio);
    settingsScreen.setPower(&power);
    settingsScreen.setNetworkActions({
        []() { return wifiConnection.startScan(); },
        [](String& json) { return wifiConnection.finishScan(json); },
        []() { return network.begin(userConfig.settings()); },
        []() { network.disconnect(); },
        []() { return wifiConnection.connecting(); }
    });
    settingsScreen.setBackend(backend);
    settingsScreen.setMaintenanceCallback(requestMaintenance);
    settingsScreen.setIdentityHash(backend->destinationHashHex());
    settingsScreen.setSaveCallback([](UserConfig& candidate) {
        return saveCardSettings(candidate, CardSettingsStep::Settings);
    });

    tabScreens[TabBar::TAB_HOME]  = &homeScreen;
    tabScreens[TabBar::TAB_MSGS]  = &messagesScreen;
    tabScreens[TabBar::TAB_NODES] = &nodesScreen;
    tabScreens[TabBar::TAB_SETUP] = &settingsScreen;

    // Boot flow: each accepted step waits for the shared durable transaction.
    nameInputScreen.setDoneCallback([](const String& name) {
        if (userConfig.settingsPending()) return;
        UserConfig candidate;
        if (!candidate.tryAssign(userConfig) || !UserConfig::trySetString(candidate.settings().displayName, name.c_str(), name.length())) {
            nameInputScreen.setSaveStatus("Memory unavailable; Enter retries", false); return;
        }
        candidate.settings().nameComplete = true; // Explicit empty is complete.
        saveCardSettings(candidate, CardSettingsStep::Name);
    });
    nameInputScreen.setBackCallback([]() {
        if (userConfig.settingsPending()) return;
        timezoneScreen.setSelectedIndex(userConfig.settings().timezoneIdx);
        ui.setScreen(&timezoneScreen);
    });
    timezoneScreen.setDoneCallback([](int tzIdx) {
        if (userConfig.settingsPending() || tzIdx < 0 || tzIdx >= TIMEZONE_COUNT) return;
        UserConfig candidate;
        if (!candidate.tryAssign(userConfig)) {
            timezoneScreen.setSaveStatus("Memory unavailable; Enter retries", false); return;
        }
        candidate.settings().timezoneIdx = uint8_t(tzIdx);
        // Region suggestion is only this explicit first-setup choice. A later
        // timezone edit/recovery preserves the established manual frequency.
        if (!candidate.settings().timezoneSet) {
            const uint8_t region = TIMEZONE_TABLE[tzIdx].radioRegion;
            candidate.settings().radioRegion = region;
            candidate.settings().loraFrequency = REGION_FREQ[region];
        }
        candidate.settings().timezoneSet = true;
        saveCardSettings(candidate, CardSettingsStep::Timezone);
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
    } else if (!userConfig.settings().nameComplete) {
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
    // Keep the cooperative UI running, but service TX completion before
    // background storage/settings work can occupy the radio owner.
    const bool radioReady = backend->pollRadioBeforeBlockingWork();
    messageStore.poll(); // Completed result settlement never performs storage I/O.
    M5.update();
    if (radioReady) { pollCardSettings(); pollCardRadioSettings(); }
    if (maintenance.accepting() && radioReady) {
        diagnostics.poll();
        diagnostics.pollSamples();
    } else if (!maintenance.accepting()) {
        protocolRuntime.pollMaintenance();
        diagnostics.pollResults();
    }
    static unsigned long lastMetadataRetry = 0;
    if (maintenance.accepting() && now - lastMetadataRetry >= 30000 &&
        backend->pollRadioBeforeBlockingWork()) {
        lastMetadataRetry = now;
        if (!identityMgr.flushPending()) Serial.println("[STORAGE] Identity metadata retry pending");
        if (!userConfig.flushPending(sdStore, flash)) Serial.println("[STORAGE] Settings backup retry pending");
    }

    // 1. Input (keyboard refresh is INT-gated, with fallback polling)
    const bool inputScreenWasOn = power.isScreenOn();
    keyboard.update();
    if (keyboard.hasEvent()) {
        const KeyEvent& evt = keyboard.getEvent();
        if (inputScreenWasOn) handheld::inputObserved(millis());
        power.activity();

        if (!inputScreenWasOn) {
            keyboard.discardPending();
        } else if (!maintenance.accepting()) {
            maintenanceScreen.handleKey(evt);
        } else if (ui.isBootMode()) {
            ui.handleKey(evt);
        }
        else if (helpOverlay.isVisible()) {
            helpOverlay.handleKey(evt);
            ui.setOverlay(helpOverlay.isVisible() ? &helpOverlay : nullptr);
        }
        else if (!hotkeys.process(evt)) {
            bool consumed = ui.handleKey(evt);

            if (!consumed && !evt.ctrl && !evt.repeat) {
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
    if (maintenance.accepting() && now - lastRNS >= rnsInterval) {
        lastRNS = now;
        unsigned long rnsStart = millis();
        backend->loop();
        rnsDuration = millis() - rnsStart;
    }
    if (backend->pollRadioBeforeBlockingWork() || messageStore.deferredIO()) {
        if (messageView.pollSubmission()) ui.markContentDirty();
        if (messageView.pollReadMarker()) ui.markContentDirty();
        if (settingsScreen.pollNetworkResults()) ui.markContentDirty();
        if (messageView.pollHistory(maintenance.accepting())) ui.markContentDirty();
        if (messagesScreen.pollConversations(maintenance.accepting())) ui.markContentDirty();
        if (messagesScreen.pollDeletion()) ui.markContentDirty();
    }
    pollMaintenance();

    // Saved periodic cadence; startup begins only after committed onboarding.
    if (maintenance.accepting() && bootComplete && backend->pollRadioBeforeBlockingWork()) pollScheduledAnnounces();

    if (maintenance.accepting()) {
        const auto events = network.poll(userConfig.settings(), rnsDuration,
            handheld::NetworkCoordinator::CardBudget);
        if (events & handheld::NetworkCoordinator::Connected) {
            Serial.printf("[WIFI] STA connected: %s\n", WiFi.localIP().toString().c_str());
            static bool ntpStarted = false;
            if (!ntpStarted) {
                handheld::configureNetworkTime(currentPosixTZ());
                ntpStarted = true;
            }
        }
        if (events & handheld::NetworkCoordinator::Disconnected)
            Serial.println("[WIFI] STA disconnected, old transports detached");
        if (events & handheld::NetworkCoordinator::AutoTimeout)
            Serial.println("[AUTOIFACE] Still waiting for IPv6 link-local; discovery will retry");
        if (events & handheld::NetworkCoordinator::AutoFailed)
            Serial.println("[AUTOIFACE] Socket startup failed; discovery will retry");
    }

    // 7. Announce manager deferred saves (contacts + name cache)
    if (maintenance.accepting() && announceManager && backend->pollRadioBeforeBlockingWork()) {
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
    if (maintenance.accepting() && gps.isRunning()) gps.loop();
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
                ui.statusBar().setLoRaOnline(rustLoraIface.isOnline());
                ui.statusBar().setWiFiState(WiFi.getMode() != WIFI_OFF,
                    WiFi.status() == WL_CONNECTED || (network.accessPoint() && network.accessPoint()->isAPActive()));
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
            const auto& latency = handheld::inputToFlush;
            uint32_t workerStack = 0;
            const bool workerLive = messageStore.writeQueue().workerStackHighWater(workerStack);
            Serial.printf("[LATENCY] samples=%lu p95_le=%lu p99_le=%lu max=%lu flash_ms=%lu worker_stack_live=%u worker_stack=%lu\n",
                (unsigned long)latency.samples, (unsigned long)latency.percentile(95),
                (unsigned long)latency.percentile(99), (unsigned long)latency.maximum,
                (unsigned long)handheld::maximumFlashWriteMs.load(), unsigned(workerLive),
                (unsigned long)workerStack);
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
