#!/usr/bin/env python3
"""Build-local scan ownership and reconnect fixes for the pinned Arduino core."""
import hashlib
import json
from pathlib import Path

SOURCE_FILES = {
    "WiFiGeneric.cpp": "e37ce5edb3e0ab2f9d6a2879b2c32162c78c7b76249bc0ff114b3283bcaa2e51",
    "WiFiScan.cpp": "e96ac4be873860dc2b441d6d01d6613eb4d06bb21e3f225c3f428619496e6c21",
}

SCAN_SUPPORT = r'''
#include <atomic>
#include <new>

namespace {
// Only the protocol owner starts/deletes scans. The Arduino event task retires
// them. Release/acquire keeps result storage private until retirement finishes.
std::atomic<bool> scanInFlight{false};
std::atomic<bool> scanCancelled{false};
std::atomic<esp_err_t> scanError{ESP_OK};
std::atomic<uint32_t> scanClock{0};
std::atomic<unsigned> pendingStaStops{0};
uint32_t scanEventStatus = 0; // Arduino event task only
constexpr uint16_t scanCapacity = 32;
struct ScanStatusBits : WiFiGenericClass {
    static void clear() { clearStatusBits(WIFI_SCANNING_BIT | WIFI_SCAN_DONE_BIT); }
};
}
extern "C" bool handheld_wifi_scan_busy() {
    return scanInFlight.load(std::memory_order_acquire) ||
        pendingStaStops.load(std::memory_order_acquire) != 0;
}
extern "C" esp_err_t handheld_wifi_scan_error() {
    return scanError.load(std::memory_order_acquire);
}
extern "C" esp_err_t handheld_wifi_scan_cancel() {
    if (!scanInFlight.load(std::memory_order_acquire)) return ESP_OK;
    scanCancelled.store(true, std::memory_order_release);
    return esp_wifi_scan_stop();
}
extern "C" void handheld_wifi_scan_done(uint32_t status) {
    scanEventStatus = status;
    WiFiScanClass::_scanDone();
}
extern "C" void handheld_wifi_scan_stopping() {
    pendingStaStops.fetch_add(1, std::memory_order_release);
}
extern "C" void handheld_wifi_scan_stop_failed() {
    pendingStaStops.fetch_sub(1, std::memory_order_release);
}
extern "C" void handheld_wifi_scan_stopped() {
    // STA_STOP follows earlier scan events on the same SDK event queue. It is
    // the recovery barrier when a failed stop did not produce SCAN_DONE.
    if (!pendingStaStops.load(std::memory_order_acquire)) return;
    if (scanInFlight.load(std::memory_order_acquire)) {
        scanError.store(ESP_ERR_WIFI_NOT_STARTED, std::memory_order_relaxed);
        ScanStatusBits::clear();
        scanInFlight.store(false, std::memory_order_release);
    }
    pendingStaStops.fetch_sub(1, std::memory_order_release);
}
'''

SCAN_DONE = r'''void WiFiScanClass::_scanDone()
{
    if (!scanInFlight.load(std::memory_order_acquire)) return;
    esp_err_t error = ESP_OK;
    uint16_t count = 0;
    _scanCount = 0;
    if (scanCancelled.load(std::memory_order_acquire) || scanEventStatus != 0) {
        error = ESP_FAIL;
    } else {
        error = esp_wifi_scan_get_ap_num(&count);
    }
    wifi_ap_record_t* rows = nullptr;
    if (error == ESP_OK && count) {
        // IDF 4.4 releases its whole list through this bulk API. Bound the
        // duplicate SDK copy to 32 APs; the application deduplicates up to 15
        // names. Do not allocate one SDK record per AP in a dense environment.
        const uint16_t retained = count < scanCapacity ? count : scanCapacity;
        rows = new (std::nothrow) wifi_ap_record_t[retained];
        if (!rows) error = ESP_ERR_NO_MEM;
        else {
            _scanCount = retained;
            error = esp_wifi_scan_get_ap_records(&_scanCount, rows);
            if (count > retained) log_d("Scan list limited to %u of %u APs", retained, count);
        }
    }
    if (error != ESP_OK) {
        esp_wifi_clear_ap_list();
        delete[] rows;
        rows = nullptr;
        _scanCount = 0;
    }
    _scanResult = rows;
    _scanStarted = 0;
    scanError.store(error, std::memory_order_relaxed);
    WiFiGenericClass::clearStatusBits(WIFI_SCANNING_BIT);
    WiFiGenericClass::setStatusBits(WIFI_SCAN_DONE_BIT);
    // No result/state writes after this publication: a new scan may now start.
    scanInFlight.store(false, std::memory_order_release);
}'''


def replace_once(source: str, before: str, after: str) -> str:
    if source.count(before) != 1:
        raise ValueError("WiFi runtime patch must select exactly one reviewed boundary")
    return source.replace(before, after)


def patched_source(source: Path) -> bytes:
    original = source.read_bytes()
    if hashlib.sha256(original).hexdigest() != SOURCE_FILES.get(source.name):
        raise ValueError(f"unreviewed selected SDK WiFi runtime source: {source}")
    data = original.decode()
    if source.name == "WiFiGeneric.cpp":
        data = replace_once(data, '#include "WiFiGeneric.h"', '#include "WiFiGeneric.h"\n'
            'extern "C" void handheld_wifi_scan_stopping();\n'
            'extern "C" void handheld_wifi_scan_stop_failed();')
        data = replace_once(data, '    _esp_wifi_started = false;\n    err = esp_wifi_stop();',
            '    const bool stoppingSta = WiFi.getMode() & WIFI_MODE_STA;\n'
            '    if (stoppingSta) handheld_wifi_scan_stopping();\n'
            '    _esp_wifi_started = false;\n    err = esp_wifi_stop();')
        data = replace_once(data, '        log_e("Could not stop WiFi! %d", err);',
            '        if (stoppingSta) handheld_wifi_scan_stop_failed();\n'
            '        log_e("Could not stop WiFi! %d", err);')
        data = replace_once(data, '    err = esp_wifi_set_mode(m);',
            '    const bool stoppingSta = _esp_wifi_started && (cm & WIFI_MODE_STA) && !(m & WIFI_MODE_STA);\n'
            '    if (stoppingSta) handheld_wifi_scan_stopping();\n    err = esp_wifi_set_mode(m);')
        data = replace_once(data, '        log_e("Could not set mode! %d", err);',
            '        if (stoppingSta) handheld_wifi_scan_stop_failed();\n'
            '        log_e("Could not set mode! %d", err);')
        data = replace_once(data, 'esp_err_t WiFiGenericClass::_eventCallback(arduino_event_t *event)',
            'extern "C" void handheld_wifi_scan_done(uint32_t status);\n'
            'extern "C" void handheld_wifi_scan_stopped();\n\n'
            'esp_err_t WiFiGenericClass::_eventCallback(arduino_event_t *event)')
        data = replace_once(data, '        WiFiScanClass::_scanDone();',
                            '        handheld_wifi_scan_done(event->event_info.wifi_scan_done.status);')
        data = replace_once(data, '        WiFiSTAClass::_setStatus(WL_NO_SHIELD);',
                            '        handheld_wifi_scan_stopped();\n        WiFiSTAClass::_setStatus(WL_NO_SHIELD);')
        data = replace_once(data, '            first_connect = false;\n            DoReconnect = true;',
                            '            first_connect = false;\n            DoReconnect = WiFi.getAutoReconnect();')
    else:
        data = replace_once(data, 'bool WiFiScanClass::_scanAsync = false;',
                            SCAN_SUPPORT + '\nbool WiFiScanClass::_scanAsync = false;')
        data = replace_once(data, '    if(WiFiGenericClass::getStatusBits() & WIFI_SCANNING_BIT) {\n        return WIFI_SCAN_RUNNING;\n    }\n\n    WiFiScanClass::_scanTimeout',
                            '    if (handheld_wifi_scan_busy()) return WIFI_SCAN_FAILED;\n\n    WiFiScanClass::_scanTimeout')
        data = replace_once(data, '    WiFi.enableSTA(true);',
                            '    if (!WiFi.enableSTA(true)) return WIFI_SCAN_FAILED;')
        data = replace_once(data, '    wifi_scan_config_t config;', '    wifi_scan_config_t config{};')
        start = data.index('    if(esp_wifi_scan_start(&config, false) == ESP_OK) {')
        end = data.index('        if(WiFiScanClass::_scanAsync)', start)
        data = data[:start] + '''    scanError.store(ESP_OK, std::memory_order_relaxed);
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
        data = replace_once(data, '    return WIFI_SCAN_FAILED;\n}\n\n\n/**', '''    if (started != ESP_OK) {
        _scanStarted = 0;
        scanError.store(started, std::memory_order_relaxed);
        WiFiGenericClass::clearStatusBits(WIFI_SCANNING_BIT);
        scanInFlight.store(false, std::memory_order_release);
    }
    return WIFI_SCAN_FAILED;
}


/**''')
        start = data.index('void WiFiScanClass::_scanDone()')
        end = data.index('\n}', start) + 2
        data = data[:start] + SCAN_DONE + data[end:]
        start = data.index('int16_t WiFiScanClass::scanComplete()')
        end = data.index('\n}', start) + 2
        data = data[:start] + '''int16_t WiFiScanClass::scanComplete()
{
    if (handheld_wifi_scan_busy()) {
        // Timeout does not release ownership. The owner must stop and drain it.
        return (millis() - scanClock.load(std::memory_order_relaxed)) > _scanTimeout ? WIFI_SCAN_FAILED : WIFI_SCAN_RUNNING;
    }
    if (handheld_wifi_scan_error() != ESP_OK) return WIFI_SCAN_FAILED;
    return (WiFiGenericClass::getStatusBits() & WIFI_SCAN_DONE_BIT) ? _scanCount : WIFI_SCAN_FAILED;
}''' + data[end:]
        data = replace_once(data, 'void WiFiScanClass::scanDelete()\n{',
                            'void WiFiScanClass::scanDelete()\n{\n    if (handheld_wifi_scan_busy()) return;')
    return data.encode()


def write_patched(source: Path, output: Path) -> None:
    source, output = source.resolve(), output.resolve()
    if output.is_relative_to(source.parent):
        raise ValueError("patched output must be outside the installed SDK")
    data = patched_source(source)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != data:
        output.write_bytes(data)
    output.with_suffix('.cpp.json').write_text(json.dumps({
        "patch": "wifi-scan-owner-v1", "source": str(source),
        "source_sha256": SOURCE_FILES[source.name],
        "output_sha256": hashlib.sha256(data).hexdigest(),
    }, indent=2) + '\n')
