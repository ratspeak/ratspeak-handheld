#pragma once

#include <esp_err.h>

// Implemented by the build-local, checksum-verified Arduino WiFi scan backport.
// A cancelled scan retains ownership until its completion/STA-stop event drains.
extern "C" bool handheld_wifi_scan_busy();
extern "C" esp_err_t handheld_wifi_scan_cancel();
extern "C" esp_err_t handheld_wifi_scan_error();
