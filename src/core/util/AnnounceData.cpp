#include "AnnounceData.h"
#include "DisplayName.h"

// LXMF announce app_data:
//   [display_name(bin), stamp_cost(nil|uint), supported_functionality(array)]
// Always emit fixarray(3) so Python LXMF doesn't default auto_compress=True for
// our destinations. stamp_cost=nil means no inbound stamp is required. Empty
// supported_functionality list = we do NOT support SF_COMPRESSION (bz2).
rs::Bytes encodeAnnounceName(const String& name, const char* destination) {
    char fallback[handheld::DEVICE_NAME_FALLBACK_SIZE];
    const char* label = handheld::deviceDisplayName(name.c_str(), destination, "", fallback);
    const size_t nameLen = handheld::displayNamePrefix(label, strlen(label), 31);
    uint8_t buf[5 + 31];
    size_t i = 0;
    buf[i++] = 0x93;                   // fixarray(3)
    buf[i++] = 0xC4;                   // bin 8
    buf[i++] = (uint8_t)nameLen;
    if (nameLen) { memcpy(buf + i, label, nameLen); i += nameLen; }
    buf[i++] = 0xC0;                   // stamp_cost = nil (no stamp required)
    buf[i++] = 0x90;                   // empty fixarray (no SF_* supported)
    return rs::Bytes(buf, i);
}
