#pragma once

#include "PreparedEnvelope.h"

namespace handheld::storage::rrc {
// Private, versioned storage namespace. The payload of Message is the original
// RRC CBOR envelope (at most one Link MDU), never an LXMF record or message ID.
// Explicit little-endian fields avoid compiler padding and board ABI changes.
constexpr size_t Header = 80, Payload = 431, Maximum = Header + Payload;
enum class Kind : uint8_t { Message = 1, Preferences, Draft, ReadMarker, Tombstone };
enum class Status : uint8_t { Received, Pending, Unconfirmed, Confirmed, Transmitted, Failed };
inline bool transition(Status from, Status to) {
    if (from == to) return true;
    switch (from) {
    case Status::Pending:
        return to == Status::Unconfirmed || to == Status::Confirmed ||
               to == Status::Transmitted || to == Status::Failed;
    case Status::Unconfirmed: return to == Status::Confirmed || to == Status::Transmitted;
    case Status::Failed: return to == Status::Confirmed; // authenticated late echo
    default: return false; // incoming and terminal records cannot become sends
    }
}
struct Context { uint8_t local[16]{}, hub[16]{}, conversation[16]{}; };
struct Selector {
    uint8_t file[16]{};
    uint32_t counter = 0, revision = 0;
    Status status = Status::Received;
    uint8_t flags = 0;
};
struct Record {
    uint8_t bytes[Maximum]{};
    Kind kind() const { return Kind(bytes[4]); }
    Status status() const { return Status(bytes[5]); }
    uint8_t flags() const { return bytes[6]; }
    uint32_t counter() const { return prepared::read32(bytes + 8); }
    uint32_t revision() const { return prepared::read32(bytes + 12); }
    size_t payloadLength() const { return size_t(bytes[72]) | size_t(bytes[73]) << 8; }
    size_t length() const { return Header + payloadLength(); }
    const uint8_t* payload() const { return bytes + Header; }
    uint8_t* payload() { return bytes + Header; }
    Context context() const { Context c; std::memcpy(&c, bytes + 16, sizeof c); return c; }
    void seal() { prepared::write32(bytes + 76,
        ~prepared::checksum(prepared::checksum(UINT32_MAX, bytes, 76), payload(), payloadLength())); }
    void counter(uint32_t value) { prepared::write32(bytes + 8, value); }
    void revision(uint32_t value) { prepared::write32(bytes + 12, value); }
    void status(Status value) { bytes[5] = uint8_t(value); }
    bool valid(size_t size) const {
        return size >= Header && size <= Maximum && !std::memcmp(bytes, "HRC1", 4) &&
            bytes[4] >= uint8_t(Kind::Message) && bytes[4] <= uint8_t(Kind::Tombstone) &&
            bytes[5] <= uint8_t(Status::Failed) && !(bytes[6] & ~1u) && !bytes[7] &&
            !bytes[74] && !bytes[75] && length() == size && prepared::read32(bytes + 76) ==
            ~prepared::checksum(prepared::checksum(UINT32_MAX, bytes, 76), payload(), payloadLength());
    }
    bool matches(const Context& c) const { return !std::memcmp(bytes + 16, &c, sizeof c); }
    static bool make(Record& out, const Context& context, Kind kind, const uint8_t* payload,
                     size_t length, Status status = Status::Received, uint64_t receivedMs = 0) {
        if (length > Payload || (length && !payload)) return false;
        out = {}; std::memcpy(out.bytes, "HRC1", 4); out.bytes[4] = uint8_t(kind);
        out.bytes[5] = uint8_t(status); std::memcpy(out.bytes + 16, &context, sizeof context);
        for (unsigned n=0;n<8;++n) out.bytes[64+n] = uint8_t(receivedMs >> (8*n));
        out.bytes[72] = uint8_t(length); out.bytes[73] = uint8_t(length >> 8);
        if (length) std::memcpy(out.payload(), payload, length);
        out.seal(); return true;
    }
};
inline bool operation(Operation op) { return op >= Operation::RrcRead && op <= Operation::RrcClear; }
static_assert(sizeof(Context) == 48 && sizeof(Record) <= Budget::SmallPayload && sizeof(Selector) <= 28,
              "RRC records and pages must fit existing small storage credits");
}
