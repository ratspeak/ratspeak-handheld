#pragma once

// =============================================================================
// Shared compile-time configuration (all boards)
// Board identity, feature flags, and persisted SD/NVS names live in
// boards/<board>/config/BoardConfig.h (resolved via the per-env include root).
// =============================================================================

#include <Arduino.h>
#include "config/BoardConfig.h"

// --- WiFi Defaults ---
#define WIFI_AP_PORT        4242
#define WIFI_AP_PASSWORD    "ratspeak"

// --- Storage Paths (flash-local, no device root) ---
#define PATH_IDENTITY       "/identity/identity.key"
#define PATH_IDENTITY_BAK   "/identity/identity.key.bak"
#define PATH_USER_CONFIG    "/config/user.json"
// Directory paths intentionally have NO trailing slash — some FATFS/VFS
// readdir paths fail to enumerate when given a path ending in '/'.
// Concat sites must add their own '/' before the basename.
#define PATH_CONTACTS       "/contacts"
#define PATH_MESSAGES       "/messages"

// --- TCP Client ---
#define MAX_TCP_CONNECTIONS         4
#define TCP_DEFAULT_PORT            4242
#define TCP_CONNECT_TIMEOUT_MS      500

// --- Announce Flood Defense ---
// Enforced inside the Rust lite transport before signature verification:
// handheld preset 5/s steady, 3/s for the first 60s. C++ may lower the Rust
// budget under heap pressure; it does not inspect or rate-limit announces.

// --- Limits ---
#define RSDECK_MAX_NODES             100   // Endpoint device, not transport node
#define FLASH_MSG_CACHE_LIMIT         20
#define RSDECK_MAX_OUTQUEUE          20
#define RSDECK_RNODE_SINGLE_FRAME_RAW_MAX 254  // Raw Reticulum bytes per RNode LoRa RF frame

// --- Board-overridable knobs (BoardConfig may predefine; defaults = deck/tpager values) ---
#ifndef RSDECK_MAX_MESSAGES_PER_CONV
#define RSDECK_MAX_MESSAGES_PER_CONV 100
#endif
#ifndef ANNOUNCE_MAX_NODES
#define ANNOUNCE_MAX_NODES           RSDECK_MAX_NODES
#endif
#ifndef ANNOUNCE_MAX_NAME_CACHE
#define ANNOUNCE_MAX_NAME_CACHE      300
#endif
#ifndef ANNOUNCE_MAX_GLOBAL_PER_SEC
#define ANNOUNCE_MAX_GLOBAL_PER_SEC  10
#endif
#ifndef STORAGE_ASYNC_WRITES
#define STORAGE_ASYNC_WRITES         0     // 1 = MessageStore writes via WriteQueue task
#endif
#ifndef USERCONFIG_NVS_BACKUP
#define USERCONFIG_NVS_BACKUP        0     // 1 = mirror full user config JSON to NVS (ns NVS_NS_CFG)
#endif
#ifndef TCP_SHARED_BUFFERS
#define TCP_SHARED_BUFFERS           0     // 1 = one static rx/tx/wrap buffer set for all connections
#endif
#ifndef BOARD_DEFAULT_SD_STORAGE
#define BOARD_DEFAULT_SD_STORAGE     0     // UserSettings.sdStorageEnabled default
#endif
#ifndef BOARD_DEFAULT_WIFI_MODE
#define BOARD_DEFAULT_WIFI_MODE      RAT_WIFI_STA
#endif
#ifndef BOARD_DEFAULT_AUTOIFACE_MAX_PEERS
#define BOARD_DEFAULT_AUTOIFACE_MAX_PEERS 8
#endif
#ifndef HAS_CONTACT_RENAME
#define HAS_CONTACT_RENAME           true
#endif
#ifndef LEGACY_GPS_LOC_MIGRATION
#define LEGACY_GPS_LOC_MIGRATION     0     // 1 = accept donor gps_loc when canonical key is absent
#endif
#ifndef LEGACY_READ_CTR_MIGRATION
#define LEGACY_READ_CTR_MIGRATION    0     // 1 = one-time .read_ctr -> read:true rewrite at boot
#endif
#ifndef LEGACY_MSG_FILENAME_MIGRATION
#define LEGACY_MSG_FILENAME_MIGRATION 0    // 1 = pre-2.0 filename rename pass, gated on !fs_migrated
#endif

// --- Power Management ---
#define SCREEN_DIM_TIMEOUT_MS   30000
#define SCREEN_OFF_TIMEOUT_MS   60000
#define SCREEN_DIM_BRIGHTNESS   64

// --- Radio Region Presets (defaults, not limits on manual tuning) ---
enum RadioRegion : uint8_t {
    REGION_AMERICAS  = 0,  // 915 MHz (902-928 ISM)
    REGION_EUROPE    = 1,  // 868 MHz (863-870)
    REGION_AUSTRALIA = 2,  // 915 MHz (915-928)
    REGION_ASIA      = 3,  // 923 MHz (AS923)
    REGION_COUNT     = 4
};

static constexpr uint32_t REGION_FREQ[REGION_COUNT] = {
    915000000, 868000000, 915000000, 923000000
};

static const char* const REGION_LABELS[REGION_COUNT] = {
    "Americas (915)", "Europe (868)", "Australia (915)", "Asia (923)"
};

// --- Serial Debug ---
#define SERIAL_BAUD  115200

// --- Shared Utilities (defined in main.cpp) ---
#include <Arduino.h>
#include "util/Bytes.h"
rs::Bytes encodeAnnounceName(const String& name);
