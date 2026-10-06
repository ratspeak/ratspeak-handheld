#pragma once

// =============================================================================
// Seeed Wio Tracker L2 — ESP32-S3, 16MB flash, 8MB octal PSRAM.
//
// Touch-only handheld: no keyboard or pointer, so text entry uses the LVGL
// on-screen keyboard. A PCA9555 expander gates the LCD, touch, GNSS, SD,
// audio amplifier and battery-sense rails; nothing else answers until
// Power::enablePeripherals() has brought it up. Pin assignments follow the
// vendor firmware variant and its LovyanGFX configuration.
// =============================================================================

#define DEVICE_NAME "Wio Tracker L2"
#define DEVICE_AP_PREFIX "wiol2"
#define BOARD_CONFIRM_INPUT_NAME "tap"
#define BOARD_DEFAULT_BRIGHTNESS 80
#define BOARD_COMPONENT_ID "wio_tracker_l2"
#define BOARD_BOOT_NAMESPACE "wiol2"
#include "config/FirmwareVersion.h"
#define BOARD_RELEASE_REPO "ratspeak/ratspeak-handheld"
#define HAS_CONTACT_RENAME true

#define HAS_DISPLAY true
#define HAS_KEYBOARD false
#define HAS_TOUCH true
#define HAS_TRACKBALL false
#define HAS_SCROLLWHEEL false
#define HAS_DPAD false
#define HAS_SOFT_KEYBOARD true
#define HAS_LORA true
#define HAS_WIFI true
#define HAS_SD true
#define HAS_AUDIO true
#define HAS_ES8311_AUDIO true
#define HAS_GPS true
#define HAS_BATTERY_MODEL false
#define HAS_RNODE_MODE false

// --- Persisted Names (NEVER rename — existing user data depends on them) ---
#define NVS_NS_IDENTITY "wiol2_id"
#define NVS_NS_MSG "wiol2_msg"
#define SD_PATH_ROOT "/wiol2"
#define SD_PATH_CONFIG_DIR "/wiol2/config"
#define SD_PATH_USER_CONFIG "/wiol2/config/user.json"
#define SD_PATH_MESSAGES "/wiol2/messages"
#define SD_PATH_CONTACTS "/wiol2/contacts"
#define SD_PATH_IDENTITY_DIR "/wiol2/identity"
#define SD_PATH_IDENTITY "/wiol2/identity/identity.key"
#define SD_PATH_IMPORT_IDENTITY "/wiol2/identity/import.identity"
#define SD_PATH_IMPORT_ID "/wiol2/identity/import.key"
#define SD_PATH_TRANSPORT "/wiol2/transport"

// --- Shared I2C bus: expander, touch, backlight, battery ADC, audio codec ---
#define I2C_SDA 47
#define I2C_SCL 48
#define I2C_FREQUENCY 100000

// --- PCA9555 I/O expander (I2C 0x21) ---
#define EXPANDER_ADDR 0x21
#define EXP_WAKE_BUTTON 0      // input, active low
#define EXP_I2C_INT 1          // input
#define EXP_SD_DETECT 2        // input
#define EXP_TOUCH_INT 3        // held low: GT911 answers at 0x5D, polled
#define EXP_LCD_CTRL 4         // held high after LCD reset
#define EXP_LCD_POWER 5
#define EXP_LCD_RST 6
#define EXP_GROVE_POWER 7
#define EXP_TOUCH_RST 8
#define EXP_GNSS_RST 9         // active high
#define EXP_USER_LED 10
#define EXP_USB_OTG 11
#define EXP_AUDIO_PA 12
#define EXP_GNSS_POWER 13
#define EXP_SD_POWER 14
#define EXP_BATT_SENSE 15

// --- SX1262 LoRa radio (dedicated SPI bus) ---
#define SPI_SCK 4
#define SPI_MISO 5
#define SPI_MOSI 6
#define SPI_FREQUENCY 8000000
#define MAX_PACKET_SIZE 255

#define LORA_CS 21
#define LORA_IRQ 9             // DIO1
#define LORA_RST 7
#define LORA_BUSY 8
#define LORA_RXEN -1
#define LORA_TXEN -1
#define LORA_HAS_TCXO true
#define LORA_DIO2_AS_RF_SWITCH true
#define LORA_TCXO_VOLTAGE 0x02  // MODE_TCXO_1_8V_6X
#define LORA_USE_DCDC_REGULATOR true
#define LORA_OCP_TUNED 0x38
#define LORA_DEFAULT_FREQ 915000000
#define LORA_DEFAULT_BW 250000
#define LORA_DEFAULT_SF 11
#define LORA_DEFAULT_CR 5
#define LORA_DEFAULT_TX_POWER 22
#define LORA_DEFAULT_PREAMBLE 18

// --- NV3031B display on a quad-SPI bus (no DC line; reset is expander) ---
#define TFT_SPI_HOST SPI3_HOST
#define TFT_SPI_SCK 42
#define TFT_QSPI_IO0 41
#define TFT_QSPI_IO1 40
#define TFT_QSPI_IO2 39
#define TFT_QSPI_IO3 38
#define TFT_CS 46
#define TFT_SPI_MODE 3
#define TFT_SPI_FREQ 75000000
#define TFT_WIDTH 320
#define TFT_HEIGHT 240
// Panel offset 1 plus logical rotation 0 gives LovyanGFX internal rotation 1.
#define TFT_PANEL_OFFSET_ROTATION 1
#define TFT_ROTATION 0

// --- LP5814 backlight driver (I2C) ---
#define LP5814_ADDR 0x2C

// --- GT911 touch (polled; INT is an expander output) ---
#define TOUCH_INT -1
#define TOUCH_I2C_ADDR_1 0x5D
#define TOUCH_I2C_ADDR_2 0x14
// GT911 reports native portrait panel coordinates. With panel offset 1 and
// touch offset 2, LovyanGFX's conversion resolves to screen x = 319 - raw y,
// screen y = raw x.
#define TOUCH_PANEL_PORTRAIT_RAW 1

// --- microSD: one-bit SDMMC, powered by expander bit 14 ---
#define SD_CS -1
#define SD_USE_MMC 1
#define SDMMC_CLK 2
#define SDMMC_CMD 3
#define SDMMC_D0 1

// --- GNSS (L76K-class UART; power and reset are expander bits) ---
#define GPS_RX 18
#define GPS_TX 17
#define GPS_BAUD 9600

// --- Battery: ADS1115 AIN0 sees battery / 2; sense gated by expander ---
#define ADS1115_ADDR 0x48
#define BATT_DIVIDER 2.0f

// --- Buttons: BOOT on GPIO0, top wake button on the expander ---
#define USER_BUTTON_PIN 0

// --- Audio: ES8311 codec on I2S, amplifier powered by expander bit 12 ---
#define I2S_BCK 11
#define I2S_WS 12
#define I2S_DOUT 16
#define I2S_DIN 15
#define I2S_MCLK 10
#define AUDIO_AMP_SETTLE_MS 250
