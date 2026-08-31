#pragma once

// =============================================================================
// rsDeck — LilyGo T-Deck Plus Pin Definitions
// =============================================================================

// --- Board Identity / Branding ---
#define DEVICE_NAME              "rsDeck"
#define DEVICE_AP_PREFIX         "rsdeck"
#define BOARD_CONFIRM_INPUT_NAME "trackball"
#define BOARD_DEFAULT_BRIGHTNESS 100

#define RSDECK_VERSION_MAJOR  2
#define RSDECK_VERSION_MINOR  1
#define RSDECK_VERSION_PATCH  0
#define RSDECK_VERSION_STRING "2.1.0"
#define BOARD_RELEASE_REPO    "ratspeak/ratspeak-handheld"
#define HAS_CONTACT_RENAME    true

// --- Feature Flags ---
#define HAS_DISPLAY     true
#define HAS_KEYBOARD    true
#define HAS_TOUCH       true
#define HAS_TRACKBALL   true
#define HAS_SCROLLWHEEL false
#define HAS_LORA        true
#define HAS_WIFI        true
// TODO(BLE): Revisit as a dedicated Ratspeak BLE Peer transport based on the
// rsReticulum GATT/fragmentation/anti-loop design, after T-Deck Plus memory,
// flash, power, and interop behavior are tested end to end.
#define HAS_SD          true
#define HAS_AUDIO       true
#define HAS_GPS         true    // UBlox MIA-M10Q UART GPS
// ADC battery sense with user-tunable discharge model (Power::setBatteryModel etc.)
#define HAS_BATTERY_MODEL true

// --- Persisted Names (NEVER rename — existing user data depends on them) ---
#define NVS_NS_IDENTITY      "ratdeck_id"
#define NVS_NS_MSG           "ratdeck_msg"
// Legacy path kept intentionally so existing Standalone users keep their data.
#define SD_PATH_ROOT         "/ratdeck"
#define SD_PATH_CONFIG_DIR   "/ratdeck/config"
#define SD_PATH_USER_CONFIG  "/ratdeck/config/user.json"
#define SD_PATH_MESSAGES     "/ratdeck/messages"
#define SD_PATH_CONTACTS     "/ratdeck/contacts"
#define SD_PATH_IDENTITY_DIR "/ratdeck/identity"
#define SD_PATH_IDENTITY     "/ratdeck/identity/identity.key"
#define SD_PATH_IMPORT_IDENTITY "/ratdeck/identity/import.identity"
#define SD_PATH_IMPORT_ID    "/ratdeck/identity/import.key"
#define SD_PATH_TRANSPORT    "/ratdeck/transport"

// --- Power Control ---
// CRITICAL: Must be set HIGH at boot to enable all peripherals
#define BOARD_POWER_PIN     10

// --- SX1262 LoRa Radio (shared SPI bus) ---
#define LORA_CS              9
#define LORA_IRQ            45   // DIO1
#define LORA_RST            17
#define LORA_BUSY           13
#define LORA_RXEN           -1   // Not connected
#define LORA_TXEN           -1   // Not connected

// --- SX1262 Radio Configuration ---
#define LORA_HAS_TCXO           true
#define LORA_DIO2_AS_RF_SWITCH  true
// TCXO voltage: 1.8V for T-Deck Plus integrated SX1262 (Cardputer Cap LoRa uses 3.0V/0x06)
#define LORA_TCXO_VOLTAGE       0x02   // MODE_TCXO_1_8V_6X
#define LORA_USE_DCDC_REGULATOR true    // T-Deck Plus integrated SX1262 works in DC-DC regulator mode
#define LORA_OCP_TUNED          0x38    // Existing T-Deck PA tuning
#define LORA_DEFAULT_FREQ       915000000
#define LORA_DEFAULT_BW         250000   // Long Fast preset
#define LORA_DEFAULT_SF         11
#define LORA_DEFAULT_CR         5
#define LORA_DEFAULT_TX_POWER   22       // Long Fast preset
#define LORA_DEFAULT_PREAMBLE   18

// --- Shared SPI Bus (display + LoRa + SD) ---
#define SPI_SCK             40
#define SPI_MISO            38
#define SPI_MOSI            41

// --- Display (ST7789V via LovyanGFX) ---
#define TFT_CS              12
#define TFT_DC              11
#define TFT_BL              42   // Backlight PWM
#define TFT_WIDTH           320
#define TFT_HEIGHT          240
#define TFT_SPI_FREQ        15000000  // 15MHz (30MHz overclockable)

// --- Keyboard (ESP32-C3 over I2C) ---
#define KB_I2C_ADDR         0x55
#define KB_INT              46   // Interrupt pin

// --- I2C Bus (shared: keyboard + touchscreen) ---
#define I2C_SDA             18
#define I2C_SCL              8

// --- Touchscreen (GT911 capacitive) ---
#define TOUCH_INT           16
// GT911 I2C address: typically 0x5D or 0x14 (depends on INT state at boot)
#define TOUCH_I2C_ADDR_1    0x5D
#define TOUCH_I2C_ADDR_2    0x14

// --- Trackball ---
#define TBALL_UP             3
#define TBALL_DOWN           2
#define TBALL_LEFT           1
#define TBALL_RIGHT         15
#define TBALL_CLICK          0   // Shared with BOOT button

// --- SD Card (shared SPI bus) ---
#define SD_CS               39

// --- GPS (UBlox MIA-M10Q UART) ---
#define GPS_TX              43   // ESP TX -> GPS RX
#define GPS_RX              44   // GPS TX -> ESP RX
#define GPS_BAUD            38400   // UBlox MIA-M10Q factory default

// --- Battery ADC ---
#define BAT_ADC_PIN          4

// --- Audio (ES7210 I2S) ---
#define I2S_WS               5   // LRCK
#define I2S_DOUT             6
#define I2S_BCK              7
#define I2S_DIN             14
#define I2S_SCK             47
#define I2S_MCLK            48

// --- Hardware Constants ---
#define MAX_PACKET_SIZE     255
#define SPI_FREQUENCY       8000000   // 8 MHz SPI clock for SX1262
