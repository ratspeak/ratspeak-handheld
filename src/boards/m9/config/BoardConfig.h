#pragma once

#define DEVICE_NAME "ThinkNode M9"
#define DEVICE_AP_PREFIX "m9"
#define BOARD_CONFIRM_INPUT_NAME "OK"
#define BOARD_DEFAULT_BRIGHTNESS 80
#define BOARD_COMPONENT_ID "m9"
#define BOARD_BOOT_NAMESPACE "m9"
#include "config/FirmwareVersion.h"
#define BOARD_RELEASE_REPO "ratspeak/ratspeak-handheld"
#define HAS_CONTACT_RENAME true

#define HAS_DISPLAY true
#define HAS_KEYBOARD true
#define HAS_TOUCH false
#define HAS_TRACKBALL false
#define HAS_SCROLLWHEEL false
#define HAS_DPAD true
#define HAS_LORA true
#define HAS_WIFI true
#define HAS_SD true
#define HAS_AUDIO true
#define HAS_GPS true
#define HAS_BATTERY_MODEL false
#define HAS_RNODE_MODE false

#define NVS_NS_IDENTITY "m9_id"
#define NVS_NS_MSG "m9_msg"
#define SD_PATH_ROOT "/m9"
#define SD_PATH_CONFIG_DIR "/m9/config"
#define SD_PATH_USER_CONFIG "/m9/config/user.json"
#define SD_PATH_MESSAGES "/m9/messages"
#define SD_PATH_CONTACTS "/m9/contacts"
#define SD_PATH_IDENTITY_DIR "/m9/identity"
#define SD_PATH_IDENTITY "/m9/identity/identity.key"
#define SD_PATH_IMPORT_IDENTITY "/m9/identity/import.identity"
#define SD_PATH_IMPORT_ID "/m9/identity/import.key"
#define SD_PATH_TRANSPORT "/m9/transport"

#define BOARD_POWER_PIN 18
#define SPI_SCK 40
#define SPI_MISO 38
#define SPI_MOSI 47
#define SPI_FREQUENCY 8000000
#define MAX_PACKET_SIZE 255

#define LORA_CS 39
#define LORA_IRQ 42
#define LORA_RST 45
#define LORA_BUSY 41
#define LORA_DEFAULT_FREQ 915000000
#define LORA_DEFAULT_BW 250000
#define LORA_DEFAULT_SF 11
#define LORA_DEFAULT_CR 5
#define LORA_DEFAULT_TX_POWER 22
#define LORA_DEFAULT_PREAMBLE 18

#define TFT_CS 16
#define TFT_DC 15
#define TFT_RST 14
#define TFT_BL 17
#define TFT_WIDTH 320
#define TFT_HEIGHT 240
#define TFT_SPI_FREQ 15000000

// Keyboard bus and separate RTC/sensor bus; no native USB on GPIO19/20.
#define I2C_SDA 20
#define I2C_SCL 21
// STC8H keyboard: 400 kHz fails address detection on the revision-1 board.
#define I2C_FREQUENCY 100000
#define PERIPHERAL_I2C_SDA 7
#define PERIPHERAL_I2C_SCL 6
#define KB_INT 12
#define KB_LED 46
#define SD_CS 48
#define GPS_TX 3
#define GPS_RX 2
#define GPS_BAUD 115200
#define GPS_RESET_PIN 5
#define GPS_RTC_INT_PIN 10
#define GPS_ENABLE_PIN 11
#define BAT_ADC_PIN 13
#define EXT_POWER_PIN 1
#define CHARGE_DONE_PIN 8
#define BUZZER_PIN 9

#define TFT_ROTATION 3
#define TFT_BL_INVERT true
#define TFT_PWM_FREQ 44000
