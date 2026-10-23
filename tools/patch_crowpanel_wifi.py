"""Arduino 3.0.2 port of the shared bounded Wi-Fi scan ownership repair.

Only checksum-matched SDK sources are rewritten, into the build directory.
The installed Arduino SDK and the Arduino 2.x board builds remain untouched.
"""
import hashlib
from pathlib import Path
from patch_wifi_runtime import SCAN_SUPPORT, SCAN_DONE, replace_once

SOURCE_FILES = {
    "WiFiGeneric.cpp": "a6cd2a09564d74a143947989559b7f9ee1f487c5baa929390376d7527765b083",
    "WiFiScan.cpp": "b32287c1c923dab403759379922057bc3257398561b83273c77b14b60fdf3949",
    "STA.cpp": "4271241ab02b5f85abea6b6faad65474f2103c4b75138c93c681e1ab7f07a214",
}


def function(source, signature, replacement):
    start = source.index(signature)
    # SDK methods use an unindented closing brace. Nested blocks are indented.
    end = source.index("\n}", start) + 2
    return source[:start] + replacement + source[end:]


def patched_source(source):
    raw = Path(source).read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_FILES[Path(source).name]:
        raise ValueError(f"Unreviewed Arduino 3.0.2 WiFi source: {source}")
    data = raw.decode()
    name = Path(source).name
    if name == "STA.cpp":
        return replace_once(data, "      DoReconnect = true;\n      log_d(\"WiFi Reconnect Running\");",
                            "      DoReconnect = _sta_network_if->getAutoReconnect();\n      log_d(\"WiFi Reconnect Running\");")
    if name == "WiFiGeneric.cpp":
        data = replace_once(data, '#include "WiFiGeneric.h"', '#include "WiFiGeneric.h"\n'
            'extern "C" void handheld_wifi_scan_stopping();\n'
            'extern "C" void handheld_wifi_scan_stop_failed();\n'
            'extern "C" void handheld_wifi_scan_done(uint32_t status);')
        data = replace_once(data, "  _esp_wifi_started = false;\n  err = esp_wifi_stop();",
            "  const bool stoppingSta = WiFi.getMode() & WIFI_MODE_STA;\n"
            "  if (stoppingSta) handheld_wifi_scan_stopping();\n"
            "  _esp_wifi_started = false;\n  err = esp_wifi_stop();")
        data = replace_once(data, '    log_e("Could not stop WiFi! %d", err);',
            '    if (stoppingSta) handheld_wifi_scan_stop_failed();\n    log_e("Could not stop WiFi! %d", err);')
        data = replace_once(data, "  err = esp_wifi_set_mode(m);",
            "  const bool stoppingSta = _esp_wifi_started && (cm & WIFI_MODE_STA) && !(m & WIFI_MODE_STA);\n"
            "  if (stoppingSta) handheld_wifi_scan_stopping();\n  err = esp_wifi_set_mode(m);")
        data = replace_once(data, '    log_e("Could not set mode! %d", err);',
            '    if (stoppingSta) handheld_wifi_scan_stop_failed();\n    log_e("Could not set mode! %d", err);')
        return replace_once(data, "    WiFiScanClass::_scanDone();",
            "    handheld_wifi_scan_done(event->event_info.wifi_scan_done.status);")

    support = SCAN_SUPPORT.replace('extern "C" void handheld_wifi_scan_stopping() {', '''extern "C" void handheld_wifi_scan_stopped();
extern "C" void handheld_wifi_scan_stopping() {
    // Arduino 3 removes its Generic/STA callbacks when switching Wi-Fi off.
    // Keep a boot-lifetime observer so STA_STOP still retires the old scan.
    static bool registered = false; // protocol owner only
    if (!registered) {
        WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t) {
            if (event == ARDUINO_EVENT_WIFI_STA_STOP) handheld_wifi_scan_stopped();
        });
        registered = true;
    }''')
    data = replace_once(data, "bool WiFiScanClass::_scanAsync = false;", support + "\nbool WiFiScanClass::_scanAsync = false;")
    data = replace_once(data, "  if (WiFiGenericClass::getStatusBits() & WIFI_SCANNING_BIT) {\n    return WIFI_SCAN_RUNNING;\n  }\n\n  WiFiScanClass::_scanAsync",
                        "  if (handheld_wifi_scan_busy()) return WIFI_SCAN_FAILED;\n\n  WiFiScanClass::_scanAsync")
    data = replace_once(data, "  WiFi.enableSTA(true);", "  if (!WiFi.enableSTA(true)) return WIFI_SCAN_FAILED;")
    data = replace_once(data, "  wifi_scan_config_t config;", "  wifi_scan_config_t config{};")
    start = data.index("  if (esp_wifi_scan_start(&config, false) == ESP_OK) {")
    end = data.index("    if (WiFiScanClass::_scanAsync)", start)
    data = data[:start] + '''  scanError.store(ESP_OK, std::memory_order_relaxed);
  scanCancelled.store(false, std::memory_order_relaxed);
  _scanStarted = millis();
  if (!_scanStarted) ++_scanStarted;
  scanClock.store(_scanStarted, std::memory_order_relaxed);
  WiFiGenericClass::clearStatusBits(WIFI_SCAN_DONE_BIT);
  WiFiGenericClass::setStatusBits(WIFI_SCANNING_BIT);
  scanInFlight.store(true, std::memory_order_release);
  const esp_err_t started = esp_wifi_scan_start(&config, false);
  if (started == ESP_OK) {
''' + data[end:]
    start = data.index("  return WIFI_SCAN_FAILED;\n}", start)
    data = data[:start] + '''  if (started != ESP_OK) {
    _scanStarted = 0;
    scanError.store(started, std::memory_order_relaxed);
    WiFiGenericClass::clearStatusBits(WIFI_SCANNING_BIT);
    scanInFlight.store(false, std::memory_order_release);
  }
''' + data[start:]
    data = function(data, "void WiFiScanClass::_scanDone()", SCAN_DONE.replace("IDF 4.4", "IDF 5.1"))
    data = function(data, "int16_t WiFiScanClass::scanComplete()", '''int16_t WiFiScanClass::scanComplete() {
  if (handheld_wifi_scan_busy())
    return (millis() - scanClock.load(std::memory_order_relaxed)) > _scanTimeout ? WIFI_SCAN_FAILED : WIFI_SCAN_RUNNING;
  if (handheld_wifi_scan_error() != ESP_OK) return WIFI_SCAN_FAILED;
  return (WiFiGenericClass::getStatusBits() & WIFI_SCAN_DONE_BIT) ? _scanCount : WIFI_SCAN_FAILED;
}''')
    return replace_once(data, "void WiFiScanClass::scanDelete() {",
                        "void WiFiScanClass::scanDelete() {\n  if (handheld_wifi_scan_busy()) return;")
