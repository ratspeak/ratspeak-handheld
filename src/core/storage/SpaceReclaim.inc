// Space-driven retention. Below the reserve, the oldest unpinned records across
// all conversations go until reserve + 32 KiB is free. With the card in use only
// internal copies with a verified card copy go; without a card history rolls off.
namespace {
constexpr size_t ReclaimHysteresis = 32 * 1024;
size_t reclaimReserve(size_t total) { return std::min<size_t>(512 * 1024, std::max<size_t>(96 * 1024, total / 4)); }
bool sameRecord(const RecordKey& a, const RecordKey& b) {
    return a.counter == b.counter && a.incoming == b.incoming && !memcmp(a.peer, b.peer, 16);
}
}

bool MessageTransactions::removable(const RecordKey& key, const StoredRecordHeader& header, MessageDocument& document) {
    if (!header.incoming && header.status != 4 && header.status != 9 && !messaging::failedStatus(header.status)) return false;
    return !memoRetains(key, header, document.document()["memo_draft"] | uint32_t(0));
}

// Repair a missing/stale optional mirror from the committed chosen record before
// an internal copy may go.
bool MessageTransactions::mirrorVerified(const RecordKey& key, MessageDocument& document,
                                         const StoredRecordHeader& header, Result& result) {
    if (retainedCopy(key, 1, document)) return true;
    if (_blockedMedia & 2) return false;
    char parent[96], candidate[128]; directory(key.peer, 1, parent); path(key, 1, candidate);
    auto mirror = medium(1); JsonSource source(document);
    if (source.error() != Error::None) { result.error = source.error(); return false; }
    return mirror.ensureDir(parent) && ensureAudio(key, 1, header, nullptr) == Error::None &&
           mirror.write(candidate, source) == Error::None && retainedCopy(key, 1, document);
}

// Backup before primary, so an interruption never resurrects a removed record.
bool MessageTransactions::evictInternal(const RecordKey& key, MessageDocument& document,
                                        const StoredRecordHeader& header, bool cached, Result& result) {
    if (cached && !mirrorVerified(key, document, header, result)) return false;
    auto flash = medium(0);
    for (const char* suffix : {".prop", ".prop.bak", ".prop.tmp", ".bak", ""}) {
        char candidate[128]; path(key, 0, candidate, suffix);
        if (flash.exists(candidate) && !flash.remove(candidate)) { result.error = Error::Write; return false; }
    }
    if (!removeAudio(key, 0)) result.error = Error::Write;
    return true;
}

void MessageTransactions::reclaim(const Request& request, Result& result) {
    // The result key carries the continuation cursor, not the protected record.
    result.key = {};
    if (!_flash || !_flash->isReady()) { result.error = Error::Unavailable; return; }
    // An absent card cannot verify a mirror, so history rolls off as without one.
    const bool cached = _external && _sd && _sd->isReady();
    const size_t total = _flash->totalBytes(), reserve = reclaimReserve(total);
    auto freeBytes = [&] { const size_t used = _flash->usedBytes(); return used < total ? total - used : 0; };
    if (freeBytes() >= reserve + (request.offset ? ReclaimHysteresis : 0)) { result.outcome = Outcome::Committed; return; }
    HistoryEntry after{request.peerGeneration, request.read};
    MessageDocument document;
    // Filenames order every candidate; only the oldest eight are parsed.
    RecordKey oldest[8]; size_t selected = 0;
    Cursor peers; peers.flashOnly = true; char peerHex[33];
    while (nextPeer(peers, peerHex)) {
        uint8_t peer[16]; decodeHex(peerHex, 32, peer, 16);
        Cursor records; beginRecords(records, peer, &document); records.flashOnly = true; RecordKey key;
        while (nextRecord(records, key)) {
            const HistoryEntry entry{key.counter, key.incoming};
            if ((after.counter || after.incoming) && !historyLess(after, entry)) continue;
            if (sameRecord(key, request.key)) continue;
            size_t at = 0;
            while (at < selected && !historyLess(entry, {oldest[at].counter, oldest[at].incoming})) ++at;
            if (at == 8) continue;
            if (selected < 8) ++selected;
            for (size_t move = selected - 1; move > at; --move) oldest[move] = oldest[move - 1];
            oldest[at] = key;
            yield();
        }
        if (records.error != Error::None) { result.error = records.error; return; }
    }
    if (peers.error != Error::None) { result.error = peers.error; return; }
    uint8_t touched[8][16]; size_t peersTouched = 0;
    bool reached = false;
    for (size_t index = 0; index < selected && !reached; ++index) {
        const RecordKey& key = oldest[index];
        after = {key.counter, key.incoming};
        StoredRecordHeader header;
        // Unreadable and pinned records stay; the cursor moves past them.
        if (load(key, document, header) != Error::None || !removable(key, header, document)) continue;
        if (!evictInternal(key, document, header, cached, result)) continue;
        ++result.total; invalidateSummary(key.peer);
        if (!cached) {
            if (result.countDelta > INT32_MIN) --result.countDelta;
            if (header.incoming && !header.read && result.unreadDelta > INT32_MIN) --result.unreadDelta;
            size_t at = 0;
            while (at < peersTouched && memcmp(touched[at], key.peer, 16)) ++at;
            if (at == peersTouched) memcpy(touched[peersTouched++], key.peer, 16);
        }
        reached = freeBytes() >= reserve + ReclaimHysteresis;
    }
    // A conversation whose last record rolled off is no longer listed.
    for (size_t at = 0; at < peersTouched; ++at) {
        Cursor cursor; beginRecords(cursor, touched[at], &document); RecordKey key;
        const bool remaining = nextRecord(cursor, key);
        if (cursor.error != Error::None) { result.error = cursor.error; continue; }
        if (!remaining && result.conversationDelta > INT8_MIN) --result.conversationDelta;
    }
    result.key.counter = after.counter; result.key.incoming = after.incoming;
    result.more = !reached && selected == 8;
    if (result.error == Error::None || result.total) result.outcome = Outcome::Committed;
}
