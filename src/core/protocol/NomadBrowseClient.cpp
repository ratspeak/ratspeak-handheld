#include "NomadBrowseClient.h"
#include "protocol/RustClock.h"
#include "protocol/RustInterfacePump.h"
#include "protocol/RustEntropy.h"
#include <Arduino.h>
#include <cstring>

namespace handheld {

bool NomadBrowseClient::start(const uint8_t publicKey[64], const char* path, uint32_t requestId,
                               const uint8_t* fieldData, size_t fieldDataLen, bool identify) {
    if (!_d.ctx || !_d.links || !_d.pump || !publicKey || !path) {
        Serial.printf("[NOMAD] start() rejected: ctx=%d links=%d pump=%d publicKey=%d path=%d\n",
                      _d.ctx != nullptr, _d.links != nullptr, _d.pump != nullptr, publicKey != nullptr, path != nullptr);
        return false;
    }
    const size_t len = strlen(path);
    if (!len || len > RS_HANDHELD_NOMAD_PATH_MAX) {
        Serial.printf("[NOMAD] start() rejected: bad path length %u\n", (unsigned)len);
        return false;
    }
    if (fieldDataLen > RS_HANDHELD_NOMAD_FIELD_DATA_MAX) {
        Serial.printf("[NOMAD] start() rejected: field data too large (%u)\n", (unsigned)fieldDataLen);
        return false;
    }
    if (rs_handheld_rns_nomad_node_hash(publicKey, _destHash) != RS_HANDHELD_OK) {
        Serial.println("[NOMAD] start() rejected: rs_handheld_rns_nomad_node_hash failed");
        return false;
    }
    {
        char hex[33];
        for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", _destHash[i]);
        Serial.printf("[NOMAD] browsing dest=%s path=%s fields=%uB (derived from a 64-byte public key, not the lxmf.delivery hash)\n",
                      hex, path, (unsigned)fieldDataLen);
    }
    memcpy(_publicKey, publicKey, sizeof _publicKey);
    memcpy(_path, path, len); _path[len] = 0; _pathLen = uint8_t(len);
    if (fieldData && fieldDataLen) memcpy(_fieldData, fieldData, fieldDataLen);
    _fieldDataLen = uint16_t(fieldDataLen);
    _identify = identify;
    // Identify is per-Link on both ends, not per-request -- toggling identify off
    // can't un-identify a Link the destination already has us tagged on (see
    // closeNomadLinkIfIdentified()'s doc comment). Close it now so this browse
    // (and ensureNomadLink() in loop()) establishes a genuinely fresh one instead
    // of silently reusing the already-identified link.
    if (!identify) _d.links->closeNomadLinkIfIdentified(_destHash);
    _pageText.clear(); _failureReason = nullptr; _requestId = requestId; _attempt = 1;
    const uint64_t now = _d.clock ? _d.clock->nowMs() : 0;
    uint8_t tag[16]; RustEntropy::fill(tag, sizeof tag);
    rs_handheld_rns_request_path(_d.ctx, _destHash, tag, 0, now);
    _requestAt = now + 10000; _stepSince = now;
    _status = Status::ResolvingPath;
    return true;
}

void NomadBrowseClient::cancel() {
    if (_status != Status::Idle && _d.links) _d.links->cancelRequest(*this);
    _status = Status::Idle;
}

void NomadBrowseClient::fail(const char* reason) {
    _failureReason = reason;
    _status = Status::Failed;
}

void NomadBrowseClient::loop() {
    if (_status != Status::ResolvingPath && _status != Status::Connecting) return;
    if (!_d.ctx || !_d.links || !_d.pump) { fail("internal error"); return; }
    const uint64_t now = _d.clock ? _d.clock->nowMs() : 0;
    if (now - _stepSince >= 60000) { fail("timed out finding a path"); return; }
    rs_handheld_route_t route{};
    if (rs_handheld_rns_route(_d.ctx, _destHash, now, &route) != RS_HANDHELD_OK ||
        route.kind != RS_HANDHELD_ROUTE_DIRECT || !_d.pump->interfaceGeneration(route.interface_id)) {
        if (now >= _requestAt) {
            uint8_t tag[16]; RustEntropy::fill(tag, sizeof tag);
            rs_handheld_rns_request_path(_d.ctx, _destHash, tag, 0, now);
            _requestAt = now + 10000;
        }
        _status = Status::ResolvingPath;
        return;
    }
    const bool active = _d.links->ensureNomadLink(_destHash, _publicKey, route);
    if (!active) {
        if (_d.links->nomadLinkEstablishing(_destHash)) _status = Status::Connecting;
        return;
    }
    _status = Status::Requesting;
    // 30s, not 20s, for every attempt but the first: meshpoint's own NomadNet
    // browser found 20s too short for a real page round trip in practice
    // (confirmed empirically, not a guess) -- a slow LoRa link plus
    // split-frame reassembly on both legs eats into this fast. Attempt 1
    // gets a shorter leash instead (see requestTimeoutMs()'s doc comment).
    if (!_d.links->startNomadRequest(_destHash, reinterpret_cast<const uint8_t*>(_path), _pathLen,
                                     *this, requestTimeoutMs(), _fieldData, _fieldDataLen, _identify)) {
        fail("could not send the page request");
    }
}

void NomadBrowseClient::onLinkResponse(const uint8_t* value, size_t length) {
    // `value` is the still-msgpack-encoded response "data" element (the generic
    // [request_id, data] envelope is already validated by the caller). A NomadNet page
    // response is the raw page bytes, encoded as msgpack bin/str/nil -- confirmed from
    // Python RNS's RequestReceipt behavior (reticulum-meshchat decodes
    // request_receipt.response directly as UTF-8), NOT yet confirmed against a real
    // NomadNet node's actual wire bytes on this firmware. Only the common short-form
    // bin/str headers are handled; anything else fails cleanly instead of guessing.
    _pageText.clear();
    if (!length) { fail("empty response"); return; }
    const uint8_t tag = value[0];
    size_t headerLen = 0, dataLen = 0;
    if (tag == 0xc0) {
        _status = Status::Done; return; // nil: an intentionally empty page
    } else if (tag == 0xc4 || tag == 0xd9) { // bin8 / str8
        if (length < 2) { fail("malformed response"); return; }
        headerLen = 2; dataLen = value[1];
    } else if (tag == 0xc5 || tag == 0xda) { // bin16 / str16
        if (length < 3) { fail("malformed response"); return; }
        headerLen = 3; dataLen = (size_t(value[1]) << 8) | value[2];
    } else if (tag >= 0xa0 && tag <= 0xbf) { // fixstr
        headerLen = 1; dataLen = tag & 0x1f;
    } else {
        fail("unexpected response format"); return;
    }
    if (length < headerLen + dataLen) { fail("malformed response"); return; }
    try {
        _pageText.assign(reinterpret_cast<const char*>(value + headerLen), dataLen);
    } catch (const std::bad_alloc&) {
        fail("out of memory"); return;
    }
    _status = Status::Done;
}

void NomadBrowseClient::onLinkRequestFailed(RustLinkManager::RequestError error) {
    // Only a plain timeout is worth retrying: the link is still known-good (we were
    // actively waiting on it), so the lost response is most likely one radio-level
    // split frame dropped to channel noise/collision, not a dead peer or protocol
    // mismatch. LinkClosed/Unsupported mean retrying the same way won't help.
    if (error == RustLinkManager::RequestError::Timeout && _attempt < kMaxAttempts &&
        _d.links) {
        _attempt++;
        Serial.printf("[NOMAD] attempt %u timed out, retrying (%u/%u)\n",
                      _attempt - 1, _attempt, kMaxAttempts);
        if (_d.links->startNomadRequest(_destHash, reinterpret_cast<const uint8_t*>(_path), _pathLen,
                                         *this, requestTimeoutMs(), _fieldData, _fieldDataLen, _identify)) {
            return;
        }
        Serial.println("[NOMAD] retry: could not resend the page request");
    }
    switch (error) {
        case RustLinkManager::RequestError::Timeout: fail("timed out waiting for a response"); break;
        case RustLinkManager::RequestError::LinkClosed: fail("link closed"); break;
        case RustLinkManager::RequestError::Unsupported: fail("unsupported"); break;
        default: fail("request failed"); break;
    }
}

size_t NomadBrowseClient::pageText(char* out, size_t capacity) const {
    if (!out || !capacity) return _pageText.size();
    const size_t n = _pageText.size() < capacity ? _pageText.size() : capacity;
    memcpy(out, _pageText.data(), n);
    return _pageText.size();
}

} // namespace handheld
