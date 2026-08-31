#pragma once

// =============================================================================
// rsCardputer — M5Stack Cardputer Adv + Cap LoRa-1262 Pin Definitions
// =============================================================================

// --- Board Identity / Branding ---
#define DEVICE_NAME              "rsCardputer"
#define DEVICE_AP_PREFIX         "rscardputer"
#define BOARD_CONFIRM_INPUT_NAME "enter"
#define BOARD_DEFAULT_BRIGHTNESS 100

#define RSDECK_VERSION_MAJOR  2
#define RSDECK_VERSION_MINOR  1
#define RSDECK_VERSION_PATCH  0
#define RSDECK_VERSION_STRING "2.1.0"
#define BOARD_RELEASE_REPO    "ratspeak/ratspeak-handheld"
#define HAS_CONTACT_RENAME    false

// --- Feature Flags ---
#define HAS_DISPLAY     true
#define HAS_KEYBOARD    true
#define HAS_TOUCH       false
#define HAS_TRACKBALL   false
#define HAS_SCROLLWHEEL false
#define HAS_LORA        true
#define HAS_WIFI        true
// Core BLE transport stays compiled out; boards/cardputer/transport/BLEStub is
// the board-local no-op kept for main.cpp wiring parity.
#define HAS_BLE         0
#define HAS_SD          true
#define HAS_AUDIO       true
#define HAS_GPS         true    // Cap LoRa-1262 GNSS module
// ADC battery via M5Unified — no user-tunable discharge-model API
#define HAS_BATTERY_MODEL false

// --- Persisted Names (NEVER rename — existing user data depends on them) ---
#define NVS_NS_IDENTITY      "ratcom_id"
#define NVS_NS_MSG           "ratcom"
#define NVS_NS_CFG           "ratcom_cfg"
// Legacy path kept intentionally so existing standalone users keep their data.
#define SD_PATH_ROOT         "/ratcom"
#define SD_PATH_CONFIG_DIR   "/ratcom/config"
#define SD_PATH_USER_CONFIG  "/ratcom/config/user.json"
#define SD_PATH_MESSAGES     "/ratcom/messages"
#define SD_PATH_CONTACTS     "/ratcom/contacts"
#define SD_PATH_IDENTITY_DIR "/ratcom/identity"
#define SD_PATH_IDENTITY     "/ratcom/identity/identity.key"
#define SD_PATH_IMPORT_IDENTITY "/ratcom/identity/import.identity"
#define SD_PATH_IMPORT_ID    "/ratcom/identity/import.key"
#define SD_PATH_TRANSPORT    "/ratcom/transport"

// --- Board knobs (override core Config.h defaults; sized for no-PSRAM/8MB) ---
#define BOARD_DEFAULT_SD_STORAGE     1      // Legacy installs always used SD when present
#define BOARD_DEFAULT_WIFI_MODE      RAT_WIFI_OFF
#define BOARD_DEFAULT_AUTOIFACE_MAX_PEERS 4
#define STORAGE_ASYNC_WRITES         1      // WriteQueue task defers 0.5-4s LittleFS writes
#define USERCONFIG_NVS_BACKUP        1      // Tier-3 full-JSON config copy in NVS
#define TCP_SHARED_BUFFERS           1      // One static buffer set (~18KB heap at 4 conns)
#define LEGACY_READ_CTR_MIGRATION    1      // One-time .read_ctr -> read:true rewrite
#define LEGACY_MSG_FILENAME_MIGRATION 1     // Pre-2.0 filename rename pass (!fs_migrated)
#define LEGACY_GPS_LOC_MIGRATION     1      // Donor v2.0.3 wrote gps_loc
#define ANNOUNCE_MAX_NODES           50
#define ANNOUNCE_MAX_NAME_CACHE      60
#define ANNOUNCE_MAX_GLOBAL_PER_SEC  8
#define RSDECK_MAX_MESSAGES_PER_CONV 5000   // High per-conversation cap matching donor storage scale

// --- Legacy aliases so unmodified lite UI files compile ---
#define RSCARDPUTER_VERSION_MAJOR  RSDECK_VERSION_MAJOR
#define RSCARDPUTER_VERSION_MINOR  RSDECK_VERSION_MINOR
#define RSCARDPUTER_VERSION_PATCH  RSDECK_VERSION_PATCH
#define RSCARDPUTER_VERSION_STRING RSDECK_VERSION_STRING

// --- SX1262 LoRa Radio (external SPI bus — FSPI/SPI2) ---
#define LORA_SCK    40
#define LORA_MISO   39
#define LORA_MOSI   14
#define LORA_CS      5
#define LORA_IRQ     4   // DIO1, falling edge interrupt
#define LORA_RST     3   // Active low, 100us reset pulse
#define LORA_BUSY    6   // Poll before SPI transactions
#define LORA_RXEN   -1   // Not connected
#define LORA_TXEN   -1   // Not connected

// --- SX1262 Radio Configuration ---
#define LORA_HAS_TCXO           true
#define LORA_DIO2_AS_RF_SWITCH  true
#define LORA_TCXO_VOLTAGE       0x06   // MODE_TCXO_3_0V_6X — proven on Cap LoRa-1262
#define LORA_USE_DCDC_REGULATOR false  // M5 Cap LoRa-1262 examples use LDO-only regulator mode
#define LORA_OCP_TUNED          0x28   // RNode Cardputer Adv board tuning
#define LORA_DEFAULT_FREQ       915000000
#define LORA_DEFAULT_BW         250000   // Long Fast preset (matches rsDeck)
#define LORA_DEFAULT_SF         11
#define LORA_DEFAULT_CR         5
#define LORA_MAX_TX_POWER       22       // Cap LoRa-1262 documented maximum
#define LORA_DEFAULT_TX_POWER   22       // Long Fast preset
#define LORA_DEFAULT_PREAMBLE   18

// Cap LoRa-1262 RF antenna switch enable (PI4IOE5V6408 on Cardputer I2C).
// P0 must be driven HIGH before radio TX/RX for the external antenna path.
#define LORA_CAP_IOE_ADDR       0x43
#define LORA_CAP_RF_SW_PIN      0

// --- Keyboard (TCA8418 via I2C) ---
#define KB_SDA       8
#define KB_SCL       9
#define KB_INT      11   // Active-low, falling edge

// --- Display (ST7789V2 via M5Unified) ---
// DISPLAY_WIDTH/DISPLAY_HEIGHT come from the env -D flags (240x135)
#define TFT_WIDTH           240
#define TFT_HEIGHT          135

// --- Touchscreen ---
// Cardputer has no touch panel. Sentinels keep core TouchInput compiling
// (never begun — HAS_TOUCH false); same pattern as tpager.
#define TOUCH_INT           -1
#define TOUCH_I2C_ADDR_1    0x00
#define TOUCH_I2C_ADDR_2    0x00

// --- GNSS (UART — reserved for v1.1) ---
#define GPS_RX      15   // GPS TX -> ESP RX
#define GPS_TX      13   // GPS RX <- ESP TX
#define GPS_BAUD    115200

// --- Audio ---
// ES8311 codec + NS4150B amp, managed by M5Unified

// --- Battery ---
// ADC via M5Unified, 1750mAh, TP4057 charger

// --- SD Card (shares FSPI/SPI2 with LoRa) ---
#define SD_CS       12   // Separate CS from LoRa (CS=5)

// --- Hardware Constants ---
#define MAX_PACKET_SIZE  255
#define SPI_FREQUENCY    8000000   // 8 MHz SPI clock for SX1262
