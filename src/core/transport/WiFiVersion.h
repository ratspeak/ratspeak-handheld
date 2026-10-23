#pragma once
#include <WiFi.h>
#include <esp_arduino_version.h>
namespace handheld::wifi_version {
inline void enableIPv6() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
 WiFi.enableIPv6();
#else
 WiFi.enableIpV6();
#endif
}
inline auto localIPv6() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
 return WiFi.linkLocalIPv6();
#else
 return WiFi.localIPv6();
#endif
}
template<class Address> inline void bytes(const Address& address,uint8_t (&out)[16]) {
 for(unsigned i=0;i<16;++i) out[i]=address[i];
}
}
