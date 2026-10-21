// Included by DeviceService.cpp inside handheld. NomadNet raw-page browse (D1L
// Peers-menu feature): one command admits the browse, one poll loop watches it settle
// and delivers the page text through the mailbox block arena -- mirrors RrcService.inc's
// executeRrc/pollRrcCommands shape (a backend action that outlives one mailbox tick).

void DeviceService::executeNomad(uint8_t slot) {
    const auto& request = _mailbox.request(slot);
    Serial.printf("[NOMAD] executeNomad slot=%u peer=%s\n", slot, request.peer);
    if (_nomadSlot != ServiceMailbox::NoSlot) {
        Serial.println("[NOMAD] rejected: a browse is already in progress");
        complete(slot, Outcome::Failed, "A browse is already in progress"); return;
    }
    if (!_nodes) { Serial.println("[NOMAD] rejected: _nodes not ready"); complete(slot, Outcome::NotReady); return; }
    const auto* node = _nodes->findNodeByHex(request.peer);
    if (!node) { Serial.println("[NOMAD] rejected: unknown peer (findNodeByHex failed)"); complete(slot, Outcome::Invalid, "Unknown peer"); return; }
    // A contact can still be keyless (never announced since being saved, or saved
    // before key persistence existed) -- hashing an all-zero key would silently
    // compute a destination for nobody and burn the full path-discovery timeout
    // before failing uninformatively. Fail fast instead.
    bool hasKey = false;
    for (uint8_t b : node->publicKey) if (b) { hasKey = true; break; }
    if (!hasKey) {
        Serial.println("[NOMAD] rejected: no public key on file for this peer yet");
        complete(slot, Outcome::Invalid, "No key on file yet for this peer -- wait for a fresh announce");
        return;
    }
    // Mailbox body layout (same block arena every other variable-length request/
    // result uses -- see ServiceClient::requestNomadBrowse()'s doc comment):
    // [1-byte flags][1-byte path length][path bytes][msgpack form field data bytes
    // -- implied length = body length - 2 - path length]. flags bit 0 (0x01) is
    // "identify on this request" -- off by default, see NomadBrowseClient::
    // start()'s doc comment. An entirely empty body means "no path given, no form
    // data, no identify", which defaults to the index page (same as every browse
    // before link navigation existed) -- defensive only, requestNomadBrowse()
    // itself never actually sends a 0-byte body (the flags byte alone makes 2 the
    // real minimum).
    char path[RS_HANDHELD_NOMAD_PATH_MAX + 1] = {};
    uint8_t fieldData[RS_HANDHELD_NOMAD_FIELD_DATA_MAX] = {};
    size_t fieldDataLen = 0;
    bool identify = false;
    const size_t bodyLen = _mailbox.length(slot);
    if (bodyLen) {
        uint8_t body[1 + 1 + RS_HANDHELD_NOMAD_PATH_MAX + RS_HANDHELD_NOMAD_FIELD_DATA_MAX];
        if (bodyLen > sizeof body || !_mailbox.read(slot, body, bodyLen)) {
            Serial.println("[NOMAD] rejected: bad body");
            complete(slot, Outcome::Invalid, "Bad path"); return;
        }
        identify = (body[0] & 0x01) != 0;
        const size_t pathLen = body[1];
        if (pathLen > RS_HANDHELD_NOMAD_PATH_MAX || 2 + pathLen > bodyLen) {
            Serial.println("[NOMAD] rejected: bad path length in body");
            complete(slot, Outcome::Invalid, "Bad path"); return;
        }
        memcpy(path, body + 2, pathLen); path[pathLen] = 0;
        fieldDataLen = bodyLen - 2 - pathLen;
        if (fieldDataLen > RS_HANDHELD_NOMAD_FIELD_DATA_MAX) {
            Serial.println("[NOMAD] rejected: form field data too large");
            complete(slot, Outcome::Invalid, "Bad form data"); return;
        }
        if (fieldDataLen) memcpy(fieldData, body + 2 + pathLen, fieldDataLen);
    }
    if (!path[0]) strlcpy(path, "/page/index.mu", sizeof path);
    if (!_backend.nomadBrowseStart(node->publicKey, path, request.id,
                                    fieldDataLen ? fieldData : nullptr, fieldDataLen, identify)) {
        Serial.println("[NOMAD] rejected: nomadBrowseStart() returned false");
        complete(slot, Outcome::Failed, "Could not start the browse"); return;
    }
    _nomadSlot = slot;
}

void DeviceService::pollNomadBrowse() {
    if (_nomadSlot == ServiceMailbox::NoSlot) return;
    const auto st = NomadBrowseClient::Status(_backend.nomadBrowseStatus());
    if (st != NomadBrowseClient::Status::Done && st != NomadBrowseClient::Status::Failed) return;
    if (_backend.nomadBrowseRequestId() != _mailbox.request(_nomadSlot).id) return;
    const auto slot = _nomadSlot; _nomadSlot = ServiceMailbox::NoSlot;
    Result result;
    if (st == NomadBrowseClient::Status::Done) {
        const size_t capacity = _mailbox.capacity(slot);
        const size_t cap = capacity < sizeof _scratch ? capacity : sizeof _scratch;
        const size_t full = _backend.nomadBrowsePageText(_scratch, cap);
        const size_t written = full < cap ? full : cap;
        if (!_mailbox.write(slot, _scratch, written)) { complete(slot, Outcome::Invalid); return; }
        result.outcome = Outcome::Ok; result.length = uint16_t(written); result.total = uint32_t(full);
    } else {
        result.outcome = Outcome::Failed;
        const char* reason = _backend.nomadBrowseFailureReason();
        strlcpy(result.detail, reason ? reason : "Browse failed", sizeof result.detail);
    }
    _mailbox.complete(slot, result);
}
