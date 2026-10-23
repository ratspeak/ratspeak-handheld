#pragma once
#include "config/FirmwareVersion.h"
#define BOARD_COMPONENT_ID "crowpanel"
#define BOARD_BOOT_NAMESPACE "crowpanel"
#define DEVICE_NAME "CrowPanel"
#define DEVICE_AP_PREFIX "crowpanel"
#define BOARD_CONFIRM_INPUT_NAME "touch"
#define BOARD_DEFAULT_BRIGHTNESS 80
#define BOARD_DEFAULT_WIFI_MODE RAT_WIFI_OFF
#define BOARD_DEFAULT_SD_STORAGE false
#define BOARD_DEFAULT_RADIO_REGION REGION_EUROPE
#define BOARD_RELEASE_REPO "ratspeak/ratspeak-handheld"
#define HAS_CONTACT_RENAME true
#define HAS_DISPLAY true
#define HAS_KEYBOARD false
#define HAS_TOUCH true
#define HAS_TRACKBALL false
#define HAS_SCROLLWHEEL false
#define HAS_DPAD false
#define HAS_LORA true
#define HAS_WIFI true
#define HAS_SD false
#define HAS_AUDIO false
#define HAS_GPS false
#define HAS_BATTERY_MODEL false
#define HAS_RNODE_MODE false
// --- Persisted Names (NEVER rename — existing user data depends on them) ---
#define NVS_NS_IDENTITY      "crowpanel_id"
#define NVS_NS_MSG           "crowpanel_msg"
// Common persistence APIs require these names even with SD disabled.
#define SD_PATH_ROOT         "/crowpanel"
#define SD_PATH_CONFIG_DIR   "/crowpanel/config"
#define SD_PATH_USER_CONFIG  "/crowpanel/config/user.json"
#define SD_PATH_MESSAGES     "/crowpanel/messages"
#define SD_PATH_CONTACTS     "/crowpanel/contacts"
#define SD_PATH_IDENTITY_DIR "/crowpanel/identity"
#define SD_PATH_IDENTITY     "/crowpanel/identity/identity.key"
#define SD_PATH_IMPORT_IDENTITY "/crowpanel/identity/import.identity"
#define SD_PATH_IMPORT_ID    "/crowpanel/identity/import.key"
#define SD_PATH_TRANSPORT    "/crowpanel/transport"


// Elecrow Advance 5-inch V1.2/V1.3, manufacturer SX1262 module.
// Select wireless expansion on the physical switches. No SD or audio pins driven.
#define SPI_SCK 5
#define SPI_MISO 4
#define SPI_MOSI 6
#define SPI_FREQUENCY 8000000
#define LORA_CS 8
#define LORA_IRQ 20
#define LORA_RST 19
#define LORA_BUSY 2
#define LORA_RXEN -1
#define LORA_TXEN -1
#define LORA_HAS_TCXO true
#define LORA_TCXO_VOLTAGE 0x07 // 3.3 V, as in Elecrow's SX1262 example
#define LORA_DIO2_AS_RF_SWITCH true
#define LORA_USE_DCDC_REGULATOR true
#define LORA_OCP_TUNED 0x38
#define LORA_DEFAULT_FREQ 868000000
#define LORA_DEFAULT_BW 250000
#define LORA_DEFAULT_SF 9
#define LORA_DEFAULT_CR 5
#define LORA_DEFAULT_TX_POWER 17
#define LORA_DEFAULT_PREAMBLE 18
#define MAX_PACKET_SIZE 255
#define I2C_SDA 15
#define I2C_SCL 16
#define I2C_FREQUENCY 400000
#define TOUCH_INT 1
#define TOUCH_I2C_ADDR_1 0x5D
#define TOUCH_I2C_ADDR_2 0x14
#define TFT_WIDTH 800
#define TFT_HEIGHT 480
#define TFT_ROTATION 0
#define SD_CS -1
