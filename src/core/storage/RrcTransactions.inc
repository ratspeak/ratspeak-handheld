// Included once by MessageTransactions.cpp, inside handheld::storage. These
// operations share its worker, filesystem lease and queue credits. RRC has a
// separate counter, so corruption/exhaustion cannot consume LXMF counter space.
// Flash is authoritative, as for the propagation journal. This version does not
// restore from removable SD, so an old card cannot resurrect cleared records.
namespace {
constexpr size_t RrcPath = 192;
constexpr size_t RrcQuota = WriteQueue::CompactProfile ? 256 * 1024 : 1024 * 1024;
constexpr size_t RrcHistoryLimit = WriteQueue::CompactProfile ? 100 : 300;

void rrcDirectory(const rrc::Context& context, char (&path)[RrcPath]) {
    char local[33], hub[33], room[33];
    encodeHex(context.local, 16, local); encodeHex(context.hub, 16, hub);
    encodeHex(context.conversation, 16, room);
    snprintf(path, sizeof path, "/rrc/%s/%s/%s", local, hub, room);
}
void rrcPath(const rrc::Context& context, rrc::Kind kind, const uint8_t file[16], char (&path)[RrcPath]) {
    rrcDirectory(context, path);
    const size_t at = strlen(path);
    if (kind == rrc::Kind::Message) {
        char key[33]; encodeHex(file, 16, key); snprintf(path + at, sizeof path - at, "/m%s.rrc", key);
    } else snprintf(path + at, sizeof path - at, "/%u.rrc", unsigned(kind));
}
const char* rrcBasename(const char* name) { const auto* slash = strrchr(name, '/'); return slash ? slash + 1 : name; }
bool rrcMessageName(const char* name, uint8_t key[16]) {
    name = rrcBasename(name);
    return (strlen(name) == 37 || (strlen(name) == 41 && !strcmp(name + 37, ".bak"))) &&
        name[0] == 'm' && !strncmp(name + 33, ".rrc", 4) && decodeHex(name + 1, 32, key, 16);
}
template<class Medium>
bool rrcEnumerate(Medium& medium, const rrc::Context& context, const char* entry,
                  uint8_t key[16], char (&path)[RrcPath]) {
    if (!rrcMessageName(entry, key)) return false;
    rrcPath(context, rrc::Kind::Message, key, path);
    // A sole backup is a recoverable record; a pair contributes exactly one
    // selector. Its physical bytes are still both charged to the quota.
    return strlen(rrcBasename(entry)) == 37 || !medium.exists(path);
}
template<class Medium>
bool rrcRemove(Medium& medium, const char* path) {
    char backup[RrcPath + 8]; snprintf(backup, sizeof backup, "%s.bak", path);
    if (medium.exists(backup) && !medium.remove(backup)) return false;
    return !medium.exists(path) || medium.remove(path);
}
template<class Medium>
Error rrcInspect(Medium& medium, const char* path, rrc::Record& record, bool& present) {
    present = medium.exists(path);
    if (!present) return Error::None;
    File file = medium.open(path);
    if (!file) return Error::Read;
    const auto length = file.size();
    if (length < rrc::Header || length > rrc::Maximum) return Error::InvalidRecord;
    size_t received = 0;
    while (received < length) {
        const auto part = file.read(record.bytes + received, length - received);
        if (!part || part > length - received) return Error::Read;
        received += part;
    }
    return record.valid(length) ? Error::None : Error::InvalidRecord;
}
template<class Medium>
Error rrcLoad(Medium& medium, const char* path, const rrc::Context& context,
              rrc::Kind kind, rrc::Record& record, bool& present, bool repair = false) {
    bool primary = false, backup = false;
    const auto first = rrcInspect(medium, path, record, primary);
    const bool primaryValid = primary && first == Error::None && record.matches(context) && record.kind() == kind;
    rrc::Record older; char backupPath[RrcPath + 8]; snprintf(backupPath, sizeof backupPath, "%s.bak", path);
    const auto second = rrcInspect(medium, backupPath, older, backup);
    const bool backupValid = backup && second == Error::None && older.matches(context) && older.kind() == kind;
    present = primary || backup;
    if (!present) return Error::None;
    // A corrupt committed deletion marker must never fall back to an older
    // cutoff and make previously cleared history visible again.
    if (kind == rrc::Kind::Tombstone && primary && !primaryValid) return Error::InvalidRecord;
    if (!primaryValid && !backupValid) return first != Error::None ? first : second != Error::None ? second : Error::InvalidRecord;
    if (backupValid && (!primaryValid || older.revision() > record.revision())) {
        record = older;
        if (repair) {
            // Preserve the sole valid copy before atomicStream can rotate a
            // primary over it. A failed promotion leaves the backup readable.
            if (primary && !medium.remove(path)) return Error::Rename;
            if (!medium.rename(backupPath, path)) return Error::Rename;
        }
    }
    return Error::None;
}
template<class Medium>
bool rrcEnsure(Medium& medium, const rrc::Context& context) {
    char path[RrcPath]; rrcDirectory(context, path);
    for (char* part = path + 1; *part; ++part) {
        if (*part != '/') continue;
        *part = 0; const bool ok = medium.ensureDir(path); *part = '/';
        if (!ok) return false;
    }
    return medium.ensureDir(path);
}

// Fixed-depth walk: local identity / hub / conversation. No filename vector,
// recursive directory allocation, protocol callback or foreign identity scan.
template<class Medium, class Visit>
Error rrcWalk(Medium& medium, const uint8_t local[16], Visit&& visit) {
    char id[33], root[RrcPath]; encodeHex(local, 16, id); snprintf(root, sizeof root, "/rrc/%s", id);
    if (!medium.exists(root)) return Error::None;
    File hubs = medium.open(root); if (!hubs || !hubs.isDirectory()) return Error::Read;
    unsigned visited = 0;
    while (File hub = hubs.openNextFile()) {
        rrc::Context context; memcpy(context.local, local, 16);
        const char* hubName = rrcBasename(hub.name());
        if (!hub.isDirectory() || !decodeHex(hubName, strlen(hubName), context.hub, 16)) continue;
        while (File room = hub.openNextFile()) {
            const char* roomName = rrcBasename(room.name());
            if (!room.isDirectory() || !decodeHex(roomName, strlen(roomName), context.conversation, 16)) continue;
            while (File file = room.openNextFile()) {
                if (++visited > 16384) return Error::InvalidRecord;
                if (file.isDirectory()) return Error::InvalidRecord;
                char path[RrcPath]; rrcDirectory(context, path);
                const auto at = strlen(path); const char* name = rrcBasename(file.name());
                if (strlen(name) >= sizeof path - at - 1) return Error::InvalidRecord;
                snprintf(path + at, sizeof path - at, "/%s", name);
                const auto size = file.size(); file.close();
                const auto error = visit(context, path, size);
                if (error != Error::None) return error;
                yield();
            }
        }
    }
    return Error::None;
}
}

bool MessageTransactions::reserveRrcCounter(uint32_t& counter, uint32_t minimum) {
    Preferences preferences;
    if (!preferences.begin(NVS_NS_MSG, false)) return false;
    if (!_rrcReservedThrough) _rrcNextCounter = std::max(uint32_t(1), preferences.getUInt("rrcctr", 1));
    _rrcNextCounter = std::max(_rrcNextCounter, minimum);
    if (_rrcNextCounter == UINT32_MAX) { preferences.end(); return false; }
    if (_rrcNextCounter >= _rrcReservedThrough) {
        const uint32_t ceiling = _rrcNextCounter > UINT32_MAX - 32 ? UINT32_MAX : _rrcNextCounter + 32;
        if (preferences.putUInt("rrcctr", ceiling) != sizeof(uint32_t) || preferences.getUInt("rrcctr", 0) != ceiling) {
            preferences.end(); return false;
        }
        _rrcReservedThrough = ceiling;
    }
    preferences.end(); counter = _rrcNextCounter++; return true;
}

void MessageTransactions::rrcTransaction(const Request& request, uint8_t* bytes,
                                        size_t length, size_t capacity, Result& result) {
    if (!bytes || capacity < Budget::SmallPayload || length < rrc::Header || length > rrc::Maximum) {
        result.error = Error::InvalidRecord; return;
    }
    rrc::Record input; memcpy(input.bytes, bytes, length);
    if (!input.valid(length)) { result.error = Error::InvalidRecord; return; }
    const auto context = input.context(); const auto kind = input.kind();
    auto flash = medium(0);
    if (!flash.isReady()) { result.error = Error::Unavailable; return; }
    char path[RrcPath]; rrcPath(context, kind, request.messageId, path);
    auto finish = [&](const rrc::Record& record, bool duplicate = false) {
        memcpy(bytes, record.bytes, record.length()); result.length = uint16_t(record.length());
        result.key.counter = record.counter(); result.revision = record.revision();
        result.outcome = Outcome::Committed; result.error = Error::None; result.duplicate = duplicate;
    };
    auto marker = [&](rrc::Kind markerKind, uint32_t& counter) {
        char name[RrcPath]; rrcPath(context, markerKind, request.messageId, name);
        rrc::Record record; bool present = false;
        const auto error = rrcLoad(flash, name, context, markerKind, record, present);
        counter = present && error == Error::None ? record.counter() : 0;
        return error;
    };
    uint32_t deleted = 0;
    result.error = marker(rrc::Kind::Tombstone, deleted); if (result.error != Error::None) return;

    if (request.operation == Operation::RrcPage) {
        if (kind != rrc::Kind::Message || (request.historyDirection != HistoryDirection::Before &&
            request.historyDirection != HistoryDirection::After)) { result.error = Error::InvalidRecord; return; }
        const bool before = request.historyDirection == HistoryDirection::Before;
        uint32_t readThrough = 0; result.error = marker(rrc::Kind::ReadMarker, readThrough);
        if (result.error != Error::None) return;
        constexpr size_t Count = Budget::SmallPayload / sizeof(rrc::Selector);
        rrc::Selector rows[Count]{}; size_t used = 0, eligible = 0;
        char parent[RrcPath]; rrcDirectory(context, parent);
        if (flash.exists(parent)) {
            File directory = flash.open(parent);
            if (!directory || !directory.isDirectory()) { result.error = Error::Read; return; }
            unsigned visited = 0;
            while (File file = directory.openNextFile()) {
                if (++visited > 4096) { result.error = Error::InvalidRecord; return; }
                rrc::Selector row; char name[RrcPath];
                if (file.isDirectory() || !rrcEnumerate(flash, context, file.name(), row.file, name)) continue;
                file.close();
                rrc::Record record; bool present = false;
                result.error = rrcLoad(flash, name, context, kind, record, present);
                if (result.error != Error::None) return;
                if (!present || !record.counter() || record.counter() <= deleted) continue;
                ++result.total;
                if (record.status() == rrc::Status::Received && record.counter() > readThrough) ++result.unreadDelta;
                if (request.key.counter && (before ? record.counter() >= request.key.counter :
                    record.counter() <= request.key.counter)) continue;
                ++eligible; row.counter = record.counter(); row.revision = record.revision();
                row.status = record.status(); row.flags = record.flags();
                // Retain the nearest bounded page on the requested side.
                size_t at = 0; while (at < used && (before ? rows[at].counter > row.counter :
                    rows[at].counter < row.counter)) ++at;
                if (at < used && rows[at].counter == row.counter) { result.error = Error::InvalidRecord; return; }
                if (at >= Count) continue;
                if (used < Count) ++used;
                for (size_t n = used - 1; n > at; --n) rows[n] = rows[n - 1];
                rows[at] = row;
            }
        }
        if (before) std::reverse(rows, rows + used);
        memcpy(bytes, rows, used * sizeof(rows[0]));
        result.length = uint16_t(used * sizeof(rows[0])); result.more = eligible > used;
        result.key.counter = used ? rows[before ? 0 : used - 1].counter : 0; result.outcome = Outcome::Committed; return;
    }

    if (request.operation == Operation::RrcRead || request.operation == Operation::RrcStatus) {
        rrc::Record record; bool present = false;
        if (request.operation == Operation::RrcRead && kind == rrc::Kind::Message && !request.hasMessageId) {
            if (!request.key.counter) { result.error = Error::InvalidRecord; return; }
            char parent[RrcPath]; rrcDirectory(context, parent);
            if (request.key.counter > deleted && flash.exists(parent)) {
                File directory = flash.open(parent);
                if (!directory || !directory.isDirectory()) { result.error = Error::Read; return; }
                unsigned visited = 0;
                while (File file = directory.openNextFile()) {
                    if (++visited > 4096) { result.error = Error::InvalidRecord; return; }
                    uint8_t key[16]; char name[RrcPath];
                    if (file.isDirectory() || !rrcEnumerate(flash, context, file.name(), key, name)) continue;
                    file.close(); rrc::Record candidate; bool found = false;
                    result.error = rrcLoad(flash, name, context, kind, candidate, found);
                    if (result.error != Error::None) return;
                    if (found && candidate.counter() == request.key.counter) {
                        if (present) { result.error = Error::InvalidRecord; return; }
                        present = true; record = candidate;
                    }
                    yield();
                }
            }
        } else result.error = rrcLoad(flash, path, context, kind, record, present, request.operation == Operation::RrcStatus);
        if (result.error != Error::None) return;
        if (!present || (kind == rrc::Kind::Message && record.counter() <= deleted)) {
            if (request.operation == Operation::RrcStatus) { result.error = Error::Stale; return; }
            result.outcome = Outcome::Committed; result.duplicate = true; return;
        }
        if ((request.key.counter && record.counter() != request.key.counter) ||
            (request.offset && record.revision() != request.offset)) { result.error = Error::Stale; return; }
        if (request.operation == Operation::RrcRead) { finish(record); return; }
        if (kind != rrc::Kind::Message || record.revision() == UINT32_MAX) { result.error = Error::RevisionExhausted; return; }
        if (!rrc::transition(record.status(), input.status())) { result.error = Error::Stale; return; }
        if (record.status() == input.status()) { finish(record, true); return; }
        record.status(input.status()); record.revision(record.revision() + 1); record.seal();
        const MemorySource source(record.bytes, record.length());
        result.error = flash.write(path, source); if (result.error != Error::None) return;
        result.flash = {true, true, Error::None}; finish(record); return;
    }

    if (request.operation == Operation::RrcMarkRead || request.operation == Operation::RrcClear) {
        const bool clearing = request.operation == Operation::RrcClear;
        const auto markerKind = clearing ? rrc::Kind::Tombstone : rrc::Kind::ReadMarker;
        if (!rrcEnsure(flash, context)) { result.error = Error::Write; return; }
        char markerPath[RrcPath]; rrcPath(context, markerKind, request.messageId, markerPath);
        rrc::Record oldMarker; bool hadMarker = false;
        result.error = rrcLoad(flash, markerPath, context, markerKind, oldMarker, hadMarker, true);
        if (result.error != Error::None) return;
        if (hadMarker && oldMarker.revision() == UINT32_MAX) { result.error = Error::RevisionExhausted; return; }
        const uint32_t previous = hadMarker ? oldMarker.counter() : 0;
        uint32_t through = request.key.counter;
        uint32_t highest = std::max(previous, deleted);
        char parent[RrcPath]; rrcDirectory(context, parent); File records = flash.open(parent);
        if (!records || !records.isDirectory()) { result.error = Error::Read; return; }
        unsigned scanned = 0;
        while (File file = records.openNextFile()) {
            if (++scanned > 4096) { result.error = Error::InvalidRecord; return; }
            uint8_t key[16]; char name[RrcPath];
            if (file.isDirectory() || !rrcEnumerate(flash, context, file.name(), key, name)) continue;
            file.close();
            rrc::Record old; bool found = false;
            const auto error = rrcLoad(flash, name, context, rrc::Kind::Message, old, found);
            if (error != Error::None && !clearing) { result.error = error; return; }
            if (error == Error::None && found) highest = std::max(highest, old.counter());
        }
        records.close();
        if (clearing && (highest == UINT32_MAX || !reserveRrcCounter(through, highest + 1))) {
            result.error = Error::CounterExhausted; return;
        }
        if (!clearing && through > highest) { result.error = Error::Stale; return; }
        rrc::Record record; rrc::Record::make(record, context, markerKind, nullptr, 0);
        record.counter(std::max(previous, through)); record.revision(hadMarker ? oldMarker.revision() + 1 : 1); record.seal();
        rrcPath(context, markerKind, request.messageId, path);
        const MemorySource source(record.bytes, record.length());
        result.error = flash.write(path, source); if (result.error != Error::None) return;
        if (clearing) {
            // The marker already hides these records, including after a reset
            // during cleanup. Never touch preferences or drafts.
            char parent[RrcPath]; rrcDirectory(context, parent); File directory = flash.open(parent);
            unsigned visited = 0;
            while (File file = directory.openNextFile()) {
                if (++visited > 4096) { result.more = true; break; }
                uint8_t key[16]; char name[RrcPath];
                if (file.isDirectory() || !rrcEnumerate(flash, context, file.name(), key, name)) continue;
                file.close();
                // No append can interleave this executor operation. Explicit
                // clear also removes unreadable records in this exact room.
                if (!rrcRemove(flash, name)) result.more = true;
            }
        }
        result.flash = {true, true, Error::None}; finish(record); return;
    }

    const bool append = request.operation == Operation::RrcAppend;
    if ((!append && request.operation != Operation::RrcWrite) ||
        (append && (kind != rrc::Kind::Message || !input.payloadLength())) ||
        (!append && kind != rrc::Kind::Preferences && kind != rrc::Kind::Draft)) {
        result.error = Error::InvalidRecord; return;
    }
    rrc::Record previous; bool present = false;
    result.error = rrcLoad(flash, path, context, kind, previous, present, true);
    if (result.error != Error::None) return;
    if (append && present) { finish(previous, true); return; } // exact local file identity is immutable
    if ((request.offset && (!present || request.offset != previous.revision())) ||
        (present && previous.revision() == UINT32_MAX)) { result.error = Error::Stale; return; }
    // Quota and retention are worker work. No directory IO reaches the protocol
    // or UI owner; one extra atomic record is the maximum temporary overage.
    uint32_t highest = 0;
    auto usage = [&](size_t& total, size_t& roomCount, char (&oldest)[RrcPath], bool roomOnly) {
        total = roomCount = 0; uint32_t oldestCounter = UINT32_MAX; oldest[0] = 0;
        return rrcWalk(flash, context.local, [&](const rrc::Context& c, const char* name, size_t size) {
            if (size > RrcQuota || total > RrcQuota * 2 - size) return Error::InvalidRecord;
            total += size; uint8_t fileKey[16]; char canonical[RrcPath];
            if (!rrcEnumerate(flash, c, name, fileKey, canonical)) return Error::None;
            rrc::Record record; bool exists = false;
            const auto error = rrcLoad(flash, canonical, c, rrc::Kind::Message, record, exists);
            if (error != Error::None) return error;
            if (!exists) return Error::None;
            highest = std::max(highest, record.counter());
            const bool sameRoom = !memcmp(&c, &context, sizeof c);
            if (sameRoom) ++roomCount;
            if ((roomOnly && !sameRoom) || !strcmp(canonical, path) || record.status() == rrc::Status::Pending ||
                record.status() == rrc::Status::Unconfirmed || !record.counter() || record.counter() >= oldestCounter) return Error::None;
            oldestCounter = record.counter(); snprintf(oldest, sizeof oldest, "%s", canonical); return Error::None;
        });
    };
    size_t total = 0, count = 0; char oldest[RrcPath];
    result.error = usage(total, count, oldest, false); if (result.error != Error::None) return;
    const size_t previousBytes = present ? previous.length() : 0;
    if (append && count >= RrcHistoryLimit) {
        result.error = usage(total, count, oldest, true); if (result.error != Error::None) return;
        if (!oldest[0]) { result.error = Error::Unavailable; return; }
    }
    if (total > RrcQuota || ((total - std::min(total, previousBytes) + input.length() > RrcQuota ||
         (append && count >= RrcHistoryLimit)) && !oldest[0])) { result.error = Error::Unavailable; return; }
    if (append) {
        if (highest == UINT32_MAX) { result.error = Error::CounterExhausted; return; }
        // Reconcile recovered RRC files after an NVS reset without making
        // direct-message boot depend on the health of this separate namespace.
        highest = std::max(highest, deleted);
        uint32_t counter;
        if (highest == UINT32_MAX || !reserveRrcCounter(counter, highest + 1)) { result.error = Error::CounterExhausted; return; }
        input.counter(counter);
    }
    input.revision(present ? previous.revision() + 1 : 1); input.seal();
    if (!rrcEnsure(flash, context)) { result.error = Error::Write; return; }
    const MemorySource source(input.bytes, input.length());
    result.error = flash.write(path, source); if (result.error != Error::None) return;
    result.flash = {true, true, Error::None};
    if (append) ++count;
    // A committed append remains committed if cleanup fails. Future new writes
    // refuse an over-quota store until cleanup/recovery; no false message loss.
    for (unsigned n = 0; n < 8; ++n) {
        const auto error = usage(total, count, oldest, append && count > RrcHistoryLimit);
        if (error != Error::None) { result.more = true; break; }
        if (total <= RrcQuota && (!append || count <= RrcHistoryLimit)) break;
        if (!oldest[0] || !rrcRemove(flash, oldest)) { result.more = true; break; }
        if (n == 7) result.more = true;
    }
    finish(input);
}
