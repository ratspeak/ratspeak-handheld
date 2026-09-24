// The two LVGL boards use TLS for the informational release check. Compile
// the corrected SDK translation unit before the SDK archive is searched.
#if defined(RSDECK) || defined(RATPAGER) || defined(RSM9)
#include "../../../vendor/esp_idf_compat/mbedtls/ssl_tls.c"
#endif
