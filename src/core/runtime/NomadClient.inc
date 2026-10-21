// Included by ServiceClient.cpp inside handheld. One mailbox round trip: start a
// NomadNet raw-page browse for an already-saved peer (identified by hex hash, same as
// requestPeerName) and deliver the fetched page's raw decoded text on completion --
// mirrors requestPeerName's shape exactly, same reason: bulk variable-length text
// through the mailbox block arena, not the small Result struct.
//
// Body layout: [1-byte flags][1-byte path length][path bytes][msgpack form field
// data bytes -- implied length = body length - 2 - path length]. flags bit 0
// (0x01) is "identify on this request" (off by default -- see
// NomadBrowseClient::start()'s doc comment for why Nomad browsing doesn't
// identify unconditionally the way LXMF propagation does). An entirely empty
// body still means "index page, no form data, no identify" as a defensive
// fallback, though this function itself never actually sends one (the flags
// byte alone makes the minimum real body 2 bytes). fieldData is OPTIONAL,
// ALREADY-msgpack-encoded (a fixmap of name->value strings) by the caller -- see
// LvNomadBrowserScreen's encodeFieldDataMsgpack().
uint32_t ServiceClient::requestNomadBrowse(const std::string& peerHex, const std::string& path,
                                           TextCompletion completion,
                                           const uint8_t* fieldData, size_t fieldDataLen,
                                           bool identify) {
    uint8_t decoded[16];
    if (!storage::decodeHex(peerHex.data(), peerHex.size(), decoded, sizeof(decoded))) return 0;
    if (path.size() > 255) return 0; // 1-byte path-length prefix
    // No local field-data ceiling here -- this layer doesn't include
    // ratspeak_protocol.h (RS_HANDHELD_NOMAD_FIELD_DATA_MAX lives there), and the
    // device side (NomadService.inc) is the authoritative bound-check anyway, same
    // component that actually defines and enforces that constant.
    Request request; request.operation = Operation::NomadBrowse;
    strlcpy(request.peer, peerHex.c_str(), sizeof(request.peer));
    std::string body;
    body.reserve(2 + path.size() + fieldDataLen);
    body += char(identify ? 0x01 : 0x00);
    body += char(uint8_t(path.size()));
    body += path;
    if (fieldData && fieldDataLen) body.append(reinterpret_cast<const char*>(fieldData), fieldDataLen);
    return submit(request, body, ServiceMailbox::MaxPayload, std::move(completion));
}
