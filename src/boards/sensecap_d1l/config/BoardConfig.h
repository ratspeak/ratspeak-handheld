#pragma once

// =============================================================================
// Seeed SenseCAP Indicator D1L — Pin Definitions
// =============================================================================
// Pin/address facts below are taken directly from an already-completed,
// real-hardware port of this exact physical device in a separate project
// (meshcore-dev, MIT-licensed — facts reused, no code copied; see that
// repo's variants/sensecap_indicator-SX1262/platformio.ini). Two values
// (TCXO voltage, DIO2-as-RF-switch) are flagged there as unverified guesses
// too — not independently confirmed here either. See TODO.md Open Items.

// --- Board Identity / Branding ---
#define BOARD_COMPONENT_ID "sensecap_d1l"
#define BOARD_BOOT_NAMESPACE "d1l"
#define DEVICE_NAME              "rsIndicator"
#define DEVICE_AP_PREFIX         "d1l"
#define BOARD_CONFIRM_INPUT_NAME "touch"
#define BOARD_DEFAULT_BRIGHTNESS 100

#include "config/FirmwareVersion.h"
#define BOARD_RELEASE_REPO    "ratspeak/ratspeak-handheld"
#define HAS_CONTACT_RENAME    true

// --- Feature Flags ---
#define HAS_DISPLAY     true
#define HAS_KEYBOARD    false   // Touch-only — no physical keyboard or encoder.
#define HAS_TOUCH       true
#define HAS_TRACKBALL   false
#define HAS_SCROLLWHEEL false
#define HAS_LORA        true
#define HAS_WIFI        true
#define HAS_SD          false   // Not present on this board.
#define HAS_AUDIO       false   // Not present on this board.
#define HAS_GPS         false   // Not present on this board.
#define HAS_BATTERY_MODEL false // No confirmed battery ADC/gauge pin yet.

// --- Persisted Names (NEVER rename once shipped — existing user data depends on them) ---
#define NVS_NS_IDENTITY      "d1l_id"
#define NVS_NS_MSG           "d1l_msg"
#define SD_PATH_ROOT         "/d1l"
#define SD_PATH_CONFIG_DIR   "/d1l/config"
#define SD_PATH_USER_CONFIG  "/d1l/config/user.json"
#define SD_PATH_MESSAGES     "/d1l/messages"
#define SD_PATH_CONTACTS     "/d1l/contacts"
#define SD_PATH_IDENTITY_DIR "/d1l/identity"
#define SD_PATH_IDENTITY     "/d1l/identity/identity.key"
#define SD_PATH_IMPORT_IDENTITY "/d1l/identity/import.identity"
#define SD_PATH_IMPORT_ID    "/d1l/identity/import.key"
#define SD_PATH_TRANSPORT    "/d1l/transport"

// --- I2C Bus (shared: IO expander + touch) ---
#define I2C_SDA              39
#define I2C_SCL              40
#define I2C_FREQUENCY        400000

// --- I2C GPIO Expander (TCA9535/PCA9535-family, 16-bit) ---
// Confirmed via a real I2C bus scan on real hardware in meshcore-dev —
// Meshtastic's own pin definitions for this board have this address wrong
// (0x40), so this is the hard-won correct value, not the "obvious" one.
#define IO_EXPANDER_ADDR     0x20
#define IO_EXPANDER_IRQ      42   // Shared across all expander inputs, not just radio DIO1.

// --- Shared SPI Bus (radio + display) ---
#define SPI_SCK              41
#define SPI_MISO             47
#define SPI_MOSI             48

// --- SX1262 LoRa Radio ---
// CS/RESET/BUSY/IRQ are virtual pin numbers routed through the IO expander
// (bits 0-3), not real ESP32-S3 GPIOs — see SensecapSX1262Radio.h.
#define LORA_CS_EXPANDER_BIT     0
#define LORA_RST_EXPANDER_BIT   1
#define LORA_BUSY_EXPANDER_BIT  2
#define LORA_IRQ_EXPANDER_BIT   3

// --- SX1262 Radio Configuration ---
// UNVERIFIED (inherited as-is from meshcore-dev's own flagged guess — not
// confirmed against a schematic in either project). RadioLib's SX1262
// begin() auto-retries at 0V TCXO on failure, so a wrong guess here fails
// safe rather than bricking radio init.
#define LORA_TCXO_VOLTAGE_V     2.4f
#define LORA_DIO2_AS_RF_SWITCH  true
#define LORA_USE_DCDC_REGULATOR true
#define LORA_DEFAULT_FREQ       915000000
#define LORA_DEFAULT_BW         250000   // Medium Fast preset
#define LORA_DEFAULT_SF         9
#define LORA_DEFAULT_CR         5
#define LORA_DEFAULT_TX_POWER   17       // Medium Fast preset; 22 is the real ceiling (see RadioLib's checkOutputPower)
#define LORA_DEFAULT_PREAMBLE   18

// --- Display (ST7701 RGB panel via LovyanGFX) ---
// CS/RESET also route through the IO expander (bits 4/5) — see
// SensecapD1LDisplay.h. The panel's CS must be asserted low *before* its
// bit-banged init command sequence runs, not just released high afterward —
// confirmed-necessary fix from meshcore-dev's own hardest-won bug (the
// display otherwise only works by electrical coincidence and fails
// specifically across a real power cycle).
#define TFT_CS_EXPANDER_BIT      4
#define TFT_RST_EXPANDER_BIT     5
#define DISPLAY_WIDTH            480
#define DISPLAY_HEIGHT           480

// --- Touchscreen (FT6336U-family capacitive, plain Wire I2C) ---
// Must NOT use LovyanGFX's native touch driver — confirmed in meshcore-dev
// to permanently break the shared I2C bus on this exact hardware (breaks
// the IO expander too, not just touch, since they're the same bus).
#define TOUCH_I2C_ADDR          0x48

// --- One physical button (role TBD — not a keyboard substitute) ---
#define PIN_USER_BTN             38

// --- Hardware Constants ---
#define MAX_PACKET_SIZE     255
#define SPI_FREQUENCY       8000000   // 8 MHz SPI clock, shared with radio and display
