#pragma once

#include "HistoryWindow.h"
#include "storage/RrcRecord.h"
#include "ratspeak_protocol.h"
#include "protocol/RrcTypes.h"
#include "protocol/RrcHubText.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace handheld::history::rrc {
namespace disk = storage::rrc;

// A stateless adapter onto the existing reader/window. Conversation keys are
// local storage identifiers; these projected headers never go to LXMF or RNS.
template<class Store>
inline storage::Submission submit(Store& store, const disk::Context& context,
                                  const HistoryWindow::Query& query) {
    if (std::memcmp(context.conversation, query.peer, 16) ||
        (query.kind != HistoryWindow::Kind::Page && query.kind != HistoryWindow::Kind::Record &&
         query.kind != HistoryWindow::Kind::Status)) return {{}, storage::Rejection::Invalid};
    disk::Record record; disk::Record::make(record, context, disk::Kind::Message, nullptr, 0);
    return store.requestRrc(query.kind == HistoryWindow::Kind::Page ? storage::Operation::RrcPage :
        storage::Operation::RrcRead, record, nullptr, query.cursor.counter, 0, query.direction);
}

inline bool fail(storage::Result& result, storage::Error error = storage::Error::InvalidRecord) {
    result.outcome = storage::Outcome::Failed; result.error = error; result.length = 0; return false;
}

inline bool decode(const disk::Record& record, const disk::Context& context, rs_handheld_rrc_view_t& view) {
    if (!record.valid(record.length()) || !record.matches(context) || record.kind() != disk::Kind::Message || record.flags() ||
        rs_handheld_rrc_decode(record.payload(), record.payloadLength(), &view) != RS_HANDHELD_OK) return false;
    const bool incoming = record.status() == disk::Status::Received;
    if (incoming == !std::memcmp(view.meta.source, context.local, 16)) return false;
    if (view.meta.kind == 50) {
        const uint8_t empty[16]{};
        return incoming && view.body_kind == 2 && !view.meta.has_destination &&
            !std::memcmp(context.conversation,empty,16) && handheld::rrc::hubText::source(view.meta.source,context.hub);
    }
    if (view.body_kind != 1 || (view.meta.kind != 20 && view.meta.kind != 21 && view.meta.kind != 22 && view.meta.kind != 40)) return false;
    uint8_t key[16]{};
    if (view.room.length) {
        uint8_t room[64]; size_t length = 0;
        if (view.meta.has_destination || rs_handheld_rrc_normalize(record.payload() + view.room.offset,
            view.room.length, 0, room, sizeof room, &length) != RS_HANDHELD_OK ||
            rs_handheld_rrc_storage_key(0, room, length, key) != RS_HANDHELD_OK) return false;
    } else if (view.meta.has_destination) {
        if (view.meta.kind != 21 || (incoming && std::memcmp(view.meta.destination, context.local, 16)) ||
            rs_handheld_rrc_storage_key(1, incoming ? view.meta.source : view.meta.destination, 16, key) != RS_HANDHELD_OK) return false;
    }
    if(!std::memcmp(key, context.conversation, 16)) return true;
    // Room-scoped hub replies also arrive before membership (invitations and
    // denials). The owner stores these in Hub notices, preserving original CBOR.
    const uint8_t empty[16]{};
    return incoming && view.room.length && (view.meta.kind==21 || view.meta.kind==40) &&
        !std::memcmp(context.conversation,empty,16) && handheld::rrc::hubText::source(view.meta.source,context.hub);
}

// The native renderer stores seconds in 32 bits. Out-of-range wire times are
// displayed as unknown, preserving raw CBOR on disk instead of inventing a date.
inline uint32_t timestamp(uint64_t milliseconds) {
    return milliseconds / 1000 <= UINT32_MAX ? uint32_t(milliseconds / 1000) : 0;
}
inline uint8_t displayedStatus(disk::Status status) {
    // Pending on disk cannot prove whether a pre-restart transmission started.
    return uint8_t(status == disk::Status::Pending ? disk::Status::Unconfirmed : status);
}
inline const char* statusName(uint8_t status) {
    switch (disk::Status(status)) {
    case disk::Status::Received: return "";
    case disk::Status::Pending: return "Saving";
    case disk::Status::Unconfirmed: return "Unconfirmed";
    case disk::Status::Confirmed: return "Confirmed by hub";
    case disk::Status::Transmitted: return "Transmitted";
    case disk::Status::Failed: return "Not sent";
    }
    return "Unknown";
}

inline constexpr char ObservationText[]="Live observation started. Messages while away are unavailable.";
inline bool detail(const disk::Record& record,const disk::Context& context,handheld::rrc::MessageDetail& out) {
    if(!record.valid(record.length()) || !record.matches(context)) return false;
    out={};out.counter=record.counter();out.status=displayedStatus(record.status());
    if(disk::boundary(record)) {
        std::memcpy(out.nickname,"Local history",14);out.length=sizeof ObservationText-1;
        std::memcpy(out.text,ObservationText,sizeof ObservationText);return true;
    }
    rs_handheld_rrc_view_t view{};if(!decode(record,context,view)) return false;
    std::memcpy(out.source,view.meta.source,16);out.timestamp=timestamp(view.meta.timestamp_ms);out.kind=uint8_t(view.meta.kind);
    char invited[65];out.invitation=record.status()==disk::Status::Received &&
        handheld::rrc::hubText::source(view.meta.source,context.hub) && handheld::rrc::hubText::invitation(record.payload(),view,invited);
    std::memcpy(out.nickname,record.payload()+view.nickname.offset,std::min(size_t(32),size_t(view.nickname.length)));
    const bool unsupported=view.meta.kind==50;
    out.length=unsupported?sizeof(handheld::rrc::UnsupportedResourceText)-1:view.text.length;
    std::memcpy(out.text,unsupported?reinterpret_cast<const uint8_t*>(handheld::rrc::UnsupportedResourceText):record.payload()+view.text.offset,out.length);return true;
}

// Input/output share the caller's existing 512-byte read scratch. Only one
// transient record copy is needed; no body or selector survives this call.
inline bool project(const disk::Context& context, const HistoryWindow::Query& query,
                    storage::Result& result, uint8_t* bytes, size_t capacity, uint32_t statusRevision) {
    if (result.outcome != storage::Outcome::Committed) { result.length = 0; return false; }
    if (!bytes || capacity < HistoryWindow::ReadCapacity || result.length > capacity ||
        std::memcmp(query.peer, context.conversation, 16)) return fail(result);
    std::memcpy(result.key.peer, context.conversation, 16);
    if (query.kind == HistoryWindow::Kind::Page) {
        const size_t count = result.length / sizeof(disk::Selector);
        if (result.length % sizeof(disk::Selector) || count > storage::Budget::SmallPayload / sizeof(disk::Selector)) return fail(result);
        uint32_t previous = 0; bool boundaryIncoming = false;
        for (size_t n = 0; n < count; ++n) {
            disk::Selector row; std::memcpy(&row, bytes + n * sizeof(row), sizeof(row));
            if (row.counter <= previous || row.status > disk::Status::Failed) return fail(result);
            previous = row.counter;
            storage::HistoryEntry entry{row.counter, row.status == disk::Status::Received};
            if (row.counter == result.key.counter) boundaryIncoming = entry.incoming;
            std::memcpy(bytes + n * sizeof(entry), &entry, sizeof(entry));
        }
        result.key.incoming = boundaryIncoming; result.length = uint16_t(count * sizeof(storage::HistoryEntry));
        return true;
    }
    disk::Record record;
    if (result.length < disk::Header || result.length > sizeof record) return fail(result, storage::Error::Stale);
    std::memcpy(record.bytes, bytes, result.length);
    rs_handheld_rrc_view_t view{};
    const bool boundary=disk::boundary(record);
    if (!record.valid(result.length) || !record.matches(context) || (!boundary && !decode(record, context, view)) || record.counter() != query.cursor.counter ||
        (record.status() == disk::Status::Received) != query.cursor.incoming) return fail(result);
    result.key.counter = record.counter(); result.key.incoming = query.cursor.incoming;
    if (query.kind == HistoryWindow::Kind::Status) {
        if (query.cursor.incoming || query.statusRevision != statusRevision) return fail(result, storage::Error::Stale);
        HistoryWindow::StatusProjection row;
        row.counter = record.counter(); row.desired = row.durable = displayedStatus(record.status());
        row.flags = HistoryWindow::StatusProjection::Available;
        std::memcpy(bytes, &row, sizeof(row)); result.length = sizeof(row); result.revision = statusRevision;
        result.total = result.nextOffset = 0; result.more = false; return true;
    }
    if (query.kind != HistoryWindow::Kind::Record || query.capacity < sizeof(storage::StoredRecordHeader)) return fail(result);
    char title[64]{}; char id[13]{};
    constexpr char hex[] = "0123456789abcdef";
    for (size_t n=0;n<6;++n) { id[2*n]=hex[view.meta.source[n]>>4]; id[2*n+1]=hex[view.meta.source[n]&15]; }
    const size_t nickLength = std::min(size_t(view.nickname.length), size_t(32));
    // Nicknames remain untrusted text. The full identity is available through
    // message details; a short suffix distinguishes equal visible nicknames.
    std::snprintf(title, sizeof title, "%s%.*s%s%s", view.meta.kind == 22 ? "* " : "",
        int(nickLength), reinterpret_cast<const char*>(record.payload() + view.nickname.offset), nickLength ? " / " : "", id);
    if(boundary) std::snprintf(title,sizeof title,"Local history");
    const bool unsupported=view.meta.kind==50;
    const auto* body=boundary?reinterpret_cast<const uint8_t*>(ObservationText):unsupported?
        reinterpret_cast<const uint8_t*>(handheld::rrc::UnsupportedResourceText):record.payload()+view.text.offset;
    storage::StoredRecordHeader header;
    std::memcpy(header.source, context.conversation, 16); std::memcpy(header.destination, context.conversation, 16);
    header.counter = record.counter(); header.revision = record.revision(); header.timestamp = timestamp(view.meta.timestamp_ms);
    header.incoming = query.cursor.incoming; header.status = displayedStatus(record.status());
    header.titleLength = std::strlen(title); header.contentLength = boundary?sizeof ObservationText-1:
        unsupported?sizeof(handheld::rrc::UnsupportedResourceText)-1:view.text.length;
    const size_t total = header.titleLength + header.contentLength;
    if (query.offset > total) return fail(result, storage::Error::Stale);
    const size_t copied = std::min(total - query.offset, std::min(capacity, size_t(query.capacity)) - sizeof header);
    std::memcpy(bytes, &header, sizeof header);
    for (size_t n = 0; n < copied; ++n) {
        const auto offset = query.offset + n;
        bytes[sizeof header + n] = offset < header.titleLength ? uint8_t(title[offset]) :
            body[offset - header.titleLength];
    }
    result.revision = header.revision; result.total = total; result.nextOffset = query.offset + copied;
    result.more = result.nextOffset < total; result.length = uint16_t(sizeof header + copied); return true;
}
}
