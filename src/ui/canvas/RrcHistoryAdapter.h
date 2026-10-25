#pragma once

#include "history/RrcHistory.h"

namespace handheld::canvas {

// Canvas uses the board's existing MessageStore owner. This adapter retains
// only the context of an accepted query; HistoryWindow owns the sole ticket.
// A mode switch must retire any Direct ticket before admitting an RRC query,
// and must retire an RRC ticket here before the Direct adapter can use it.
class RrcHistoryAdapter {
public:
    bool pending() const { return _pending; }
    template<class Store>
    void poll(history::HistoryWindow& window, Store& store,
              const storage::rrc::Context* visibleContext, uint32_t statusRevision,
              uint32_t now, bool allowAdmission) {
        if (_pending) {
            const auto held = window.ownerTicket();
            if (!held.valid()) return; // Never steal/reconstruct a lost ticket.
            if (!window.responseCopied()) {
                storage::Result result;
                if (!store.peekResult(held, result)) return;
                uint8_t bytes[history::HistoryWindow::ReadCapacity];
                if (result.length > sizeof bytes || !store.readPayload(held, bytes, result.length))
                    history::rrc::fail(result, storage::Error::Internal);
                else history::rrc::project(_context, _query, result, bytes, sizeof bytes, statusRevision);
                window.result(_query.nonce, result, bytes, result.length, now);
            }
            if (!store.releaseResult(held)) return;
            window.released(_query.nonce); _pending = false;
        }
        if (!allowAdmission || !visibleContext || window.ownerTicket().valid()) return;
        const auto query = window.next(now);
        if (query.kind == history::HistoryWindow::Kind::None) return;
        const auto submitted = history::rrc::submit(store, *visibleContext, query);
        if (!submitted.accepted()) { window.rejected(query.nonce, submitted.rejection, now); return; }
        _context = *visibleContext; _query = query; _pending = true;
        window.admitted(query.nonce, submitted.ticket);
    }
private:
    storage::rrc::Context _context;
    history::HistoryWindow::Query _query;
    bool _pending = false;
};
static_assert(sizeof(RrcHistoryAdapter) <= 112, "Canvas adapter stores only one query binding");
}
