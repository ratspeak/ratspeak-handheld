#pragma once

#include "reticulum/LXMFManager.h"
#include "protocol/ProtocolBackend.h"

namespace handheld::canvas {

// The Canvas history and conversation windows use the same storage/status
// execution rules. Their models retain the sole nonce/ticket and copied flag;
// this adapter retains nothing between cooperative-loop calls. Its512-byte
// scratch fits a Cardputer selector page, detail, or history slice.
template<class Window, class KeyForQuery, class Submit>
void pollStorageWindow(Window& window, LXMFManager& manager, ProtocolBackend* backend,
                       bool visible, bool allowAdmission, KeyForQuery keyForQuery, Submit submit) {
    using namespace storage;
    using Projection = typename Window::StatusProjection;
    auto held = window.ownerTicket();
    if (held.valid()) {
        const auto nonce = window.pendingNonce();
        if (!window.responseCopied()) {
            Result result;
            if (!manager.pollStorageResult(held, result)) return;
            uint8_t bytes[512];
            if (result.length > sizeof(bytes) || !manager.readStoragePayload(held, bytes, result.length)) {
                result.outcome = Outcome::Failed; result.error = Error::Internal; result.length = 0;
            }
            if (window.pendingKind() == Window::Kind::Status && result.outcome == Outcome::Committed) {
                Projection projection;
                if (result.length != sizeof(StoredRecordHeader) || bytes[offsetof(StoredRecordHeader, incoming)] != 0 ||
                    !backend || !allowAdmission || backend->lxmfStatusRevision() != window.pendingStatusRevision()) {
                    result.outcome = Outcome::Failed; result.error = Error::Stale; result.length = 0;
                } else {
                    StoredRecordHeader header; memcpy(&header, bytes, sizeof(header));
                    if (header.counter != result.key.counter || memcmp(header.destination, result.key.peer, 16) ||
                        header.revision != result.revision || header.status > handheld::messaging::LastStatus) {
                        result.outcome = Outcome::Failed; result.error = Error::InvalidRecord; result.length = 0;
                    } else {
                        projection.counter = header.counter; projection.desired = projection.durable = header.status;
                        projection.flags = Projection::Available;
                        result.revision = window.pendingStatusRevision(); result.length = sizeof(projection);
                        memcpy(bytes, &projection, sizeof(projection));
                    }
                }
            }
            window.result(nonce, result, bytes, result.length, millis());
        }
        // Retain the copied terminal until the provider releases its credit.
        if (!manager.releaseStorageResult(held)) return;
        window.released(nonce);
    }
    if (allowAdmission && visible) {
        const auto query = window.next(millis());
        if (query.kind != Window::Kind::None) {
            const RecordKey key = keyForQuery(query);
            outgoing::StatusView status;
            if (query.kind == Window::Kind::Status && backend && backend->lxmfStatus(key, status)) {
                Projection projection;
                projection.counter = key.counter; projection.desired = uint8_t(status.desired);
                projection.durable = uint8_t(status.durable); projection.error = status.error;
                projection.flags = Projection::Available | (status.pending ? Projection::Pending : 0) |
                    (status.txSuppressed ? Projection::TxSuppressed : 0);
                Result result; result.key = key; result.outcome = Outcome::Committed;
                result.revision = backend->lxmfStatusRevision(); result.length = sizeof(projection);
                // Immediate owner-local projection: no storage credit exists.
                window.admitted(query.nonce, {query.nonce, UINT8_MAX - 1});
                window.result(query.nonce, result, &projection, sizeof(projection), millis());
                window.released(query.nonce);
            } else {
                const auto submitted = query.kind == Window::Kind::Status ?
                    manager.requestRecord(key, 0, sizeof(StoredRecordHeader)) : submit(query, key);
                if (submitted.accepted()) window.admitted(query.nonce, submitted.ticket);
                else window.rejected(query.nonce, submitted.rejection, millis());
            }
        }
    }
}

} // namespace handheld::canvas
