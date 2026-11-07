#pragma once

#include <string>

namespace rs {
// Ratspeak RSCP1 public contact card. Returns empty for incomplete identity data.
std::string contactCardPayload(const std::string& name, const std::string& destination,
                               const std::string& identity, const std::string& publicKeyHex);
// `lxma://<address>:<public key>`: the Columba contact format, also scanned by
// MeshChatX, Retichat, and rettui. Carries no name. Empty for incomplete data.
std::string legacyContactPayload(const std::string& destination, const std::string& publicKeyHex);
}
