#pragma once

#include <string>

namespace rs {
// Ratspeak RSCP1 public contact card. Returns empty for incomplete identity data.
std::string contactCardPayload(const std::string& name, const std::string& destination,
                               const std::string& identity, const std::string& publicKeyHex);
}
