#include "runtime/FirmwareReleaseCheck.h"
#if defined(RSDECK) || defined(RATPAGER) || defined(RSM9)
#include "runtime/ReleaseVersion.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <sdkconfig.h>
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "The firmware release check requires the SDK certificate bundle"
#endif
// Arduino ships a different esp_crt_bundle.h with the same include name.
// Bind the SDK API explicitly; its bundle is already embedded in libmbedtls.
extern "C" esp_err_t esp_crt_bundle_attach(void* config);

namespace handheld {
namespace {
struct HttpOwner {
    esp_http_client_handle_t client;
    ~HttpOwner() { if (client) esp_http_client_cleanup(client); }
};
// GitHub's release object can be much larger than its leading tag metadata.
// A complete tag value may be used from an otherwise incomplete bounded prefix.
bool extractTag(const String& payload, char* version, size_t capacity) {
    JsonDocument filter, result;
    filter["tag_name"] = true;
    if (filter.overflowed()) return false;
    auto error = deserializeJson(result, payload.c_str(), payload.length(),
                                 DeserializationOption::Filter(filter));
    if (error && error != DeserializationError::IncompleteInput) return false;
    const char* tag = result["tag_name"].as<const char*>();
    if (!tag) return false;
    if (*tag == 'v' || *tag == 'V') ++tag;
    release_version::Version parsed;
    if (!release_version::parse(tag, parsed) || strlen(tag) >= capacity) return false;
    memcpy(version, tag, strlen(tag) + 1);
    return true;
}
}

ReleaseCheckResult checkFirmwareRelease(const char* repository, const char* installed,
                                       char* version, size_t capacity) {
    if (!version || !capacity) return ReleaseCheckResult::Failed;
    version[0] = '\0';
    release_version::Version current;
    if (!repository || !release_version::parse(installed, current)) return ReleaseCheckResult::Failed;
    char url[192];
    const int length = snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", repository);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(url)) return ReleaseCheckResult::Failed;
    esp_http_client_config_t config = {};
    config.url = url;
    config.user_agent = "ratspeak-handheld";
    config.timeout_ms = 5000;
    config.disable_auto_redirect = true;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 512;
    config.buffer_size_tx = 512;
    HttpOwner http{esp_http_client_init(&config)};
    if (!http.client ||
        esp_http_client_set_header(http.client, "Accept", "application/vnd.github.v3+json") != ESP_OK ||
        esp_http_client_open(http.client, 0) != ESP_OK ||
        esp_http_client_fetch_headers(http.client) < 0 ||
        esp_http_client_get_status_code(http.client) != 200) return ReleaseCheckResult::Failed;

    String payload;
    if (!payload.reserve(2048)) return ReleaseCheckResult::Failed;
    const uint32_t started = millis();
    while (static_cast<uint32_t>(millis() - started) < 5000 && payload.length() < 2048) {
        // The pinned Arduino String::concat copies the trailing byte as well.
        char chunk[129];
        const size_t remaining = 2048 - payload.length();
        const size_t requested = remaining < sizeof(chunk) - 1 ? remaining : sizeof(chunk) - 1;
        const int count = esp_http_client_read(http.client, chunk, requested);
        if (count <= 0 || static_cast<size_t>(count) > requested) break;
        chunk[count] = '\0';
        if (!payload.concat(chunk, static_cast<unsigned>(count))) break;
        if (extractTag(payload, version, capacity)) {
            release_version::Version candidate;
            if (!release_version::parse(version, candidate)) break;
            return release_version::compare(candidate, current) > 0
                ? ReleaseCheckResult::Available : ReleaseCheckResult::Current;
        }
    }
    version[0] = '\0';
    return ReleaseCheckResult::Failed;
}
} // namespace handheld
#endif
