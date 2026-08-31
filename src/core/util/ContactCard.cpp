#include "ContactCard.h"
#include <algorithm>
#include <cstdint>

namespace {
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lowerHex(const std::string& input, size_t length) {
    if (input.size() != length) return {};
    std::string out;
    out.reserve(length);
    for (char c : input) {
        const int digit = hexDigit(c);
        if (digit < 0) return {};
        out += "0123456789abcdef"[digit];
    }
    return out;
}

std::string base64Url(const std::string& input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((input.size() * 8 + 5) / 6);
    uint32_t bits = 0;
    unsigned count = 0;
    for (unsigned char c : input) {
        bits = (bits << 8) | c;
        count += 8;
        while (count >= 6) {
            count -= 6;
            out += alphabet[(bits >> count) & 63];
        }
    }
    if (count) out += alphabet[(bits << (6 - count)) & 63];
    return out; // URL-safe, without padding, matching the desktop contact card.
}

std::string cardName(const std::string& input) {
    const auto first = input.find_first_not_of(" \t\r\n\v\f");
    if (first == std::string::npos) return {};
    size_t end = std::min(input.find_last_not_of(" \t\r\n\v\f") + 1, first + 40);
    // Never split a UTF-8 code point at Ratspeak's 40-byte name boundary.
    while (end < input.size() && end > first &&
           (static_cast<unsigned char>(input[end]) & 0xC0) == 0x80) --end;
    if (end <= first) return {};
    const auto last = input.find_last_not_of(" \t\r\n\v\f", end - 1);
    return input.substr(first, last + 1 - first);
}
}

std::string rs::contactCardPayload(const std::string& name, const std::string& destination,
                                   const std::string& identity, const std::string& publicKeyHex) {
    const auto dest = lowerHex(destination, 32);
    const auto id = lowerHex(identity, 32);
    const auto key = lowerHex(publicKeyHex, 128);
    if (dest.empty() || id.empty() || key.empty()) return {};
    std::string bytes;
    bytes.reserve(64);
    for (size_t i = 0; i < key.size(); i += 2)
        bytes += static_cast<char>((hexDigit(key[i]) << 4) | hexDigit(key[i + 1]));
    return "RSCP1:" + base64Url(cardName(name)) + ":" + dest + ":" + id + ":" + base64Url(bytes);
}
