// Included once inside handheld::storage. Drafts share the ordinary storage
// executor/credits and flash deletion fence. Removable media is never authority
// for a draft: reinserting an old card cannot resurrect a discarded recording.
namespace {
template<class Medium> [[gnu::noinline]]
Error memoCutoff(Medium& store, const char* parent, uint32_t& cutoff) {
    // Recording must not borrow the 7 KiB message JSON arena alongside the
    // codec. The deletion marker has one integer and a 512-byte file ceiling;
    // parse it with the same SDK and a bounded stack arena instead. Keep this
    // frame out of the later promotion/JSON-write call chain.
    cutoff = 0;
    for (const char* suffix : {"", ".bak"}) {
        char path[144]; snprintf(path, sizeof path, "%s/.deleted%s", parent, suffix);
        if (!store.exists(path)) continue;
        File file = store.open(path);
        if (!file) return Error::Read;
        if (!file.size() || file.size() > 512) return Error::InvalidRecord;
        alignas(std::max_align_t) uint8_t memory[1280];
        JsonArena arena(memory, sizeof memory); JsonDocument document(&arena);
        BufferedFileReader reader(file);
        const auto parsed = deserializeJson(document, reader);
        const bool ended = reader.ended();
        if (reader.failed()) return Error::Read;
        if (parsed || !ended || !document["through"].is<uint32_t>()) return Error::InvalidRecord;
        cutoff = std::max(cutoff, document["through"].as<uint32_t>());
    }
    return Error::None;
}

struct MemoFile {
    static constexpr size_t Size = 68;
    uint8_t bytes[Size] = {'H', 'M', 'D', 1};
    uint32_t get(size_t at) const {
        return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 |
            uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
    }
    void put(size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
    }
    uint32_t revision() const { return get(4); }
    uint32_t base() const { return get(8); }
    uint32_t counter() const { return get(12); }
    size_t length() const { return get(56); }
    memo::State state() const { return memo::State(bytes[60]); }
    messaging::DeliveryPolicy policy() const { return messaging::DeliveryPolicy(bytes[61]); }
    void time(double value) {
        static_assert(sizeof(double) == 8, "Draft timestamps require binary64");
        uint64_t bits; memcpy(&bits, &value, 8); put(16, uint32_t(bits)); put(20, uint32_t(bits >> 32));
    }
    double time() const {
        const uint64_t bits = uint64_t(get(16)) | uint64_t(get(20)) << 32;
        double value; memcpy(&value, &bits, 8); return value;
    }
    uint32_t seed() const { return audio::checksum(UINT32_MAX, bytes, Size - 4); }
    bool valid(const Request& request) const {
        return !memcmp(bytes, "HMD\1", 4) && revision() &&
            !memcmp(bytes + 24, request.source, 16) && !memcmp(bytes + 40, request.key.peer, 16) &&
            length() <= memo::RecordBytes && !(length() % memo::FrameBytes) &&
            state() <= memo::State::Sent && policy() <= messaging::DeliveryPolicy::Always &&
            !bytes[62] && !bytes[63] && std::isfinite(time()) &&
            (state() != memo::State::Empty || !length()) &&
            (state() < memo::State::Ready || length()) &&
            ((state() >= memo::State::Promoting) == bool(counter()));
    }
};

// Verify and copy in the same pass, including the exact header used to bind
// the requested slice. A corrupt newer primary never falls back to old audio.
bool memoVerify(File& file, const MemoFile& header, uint8_t* output = nullptr,
                size_t offset = 0, size_t count = 0) {
    if (!file || file.size() != MemoFile::Size + header.length() ||
        offset > header.length() || count > header.length() - offset || (count && !output) || !file.seek(0)) return false;
    uint8_t chunk[memo::ChunkBytes];
    if (file.read(chunk, MemoFile::Size) != MemoFile::Size || memcmp(chunk, header.bytes, MemoFile::Size)) return false;
    uint32_t crc = header.seed();
    for (size_t at = 0; at < header.length();) {
        const size_t n = std::min(sizeof chunk, header.length() - at);
        if (file.read(chunk, n) != n) return false;
        crc = audio::checksum(crc, chunk, n);
        const size_t first = std::max(at, offset), last = std::min(at + n, offset + count);
        if (first < last) memcpy(output + (first - offset), chunk + (first - at), last - first);
        at += n; yield();
    }
    return ~crc == header.get(64);
}

template<class Medium>
Error memoLoad(Medium& store, const char* path, const Request& request, MemoFile& header, File& file) {
    char backup[192]; snprintf(backup, sizeof backup, "%s.bak", path);
    const bool primary = store.exists(path), older = store.exists(backup);
    if (!primary && !older) { file = {}; return Error::None; }
    file = store.open(primary ? path : backup);
    if (!file || file.read(header.bytes, MemoFile::Size) != MemoFile::Size) return Error::Read;
    if (!header.valid(request) || !memoVerify(file, header)) return Error::InvalidRecord;
    if (!primary) {
        file = {};
        if (!store.rename(backup, path)) return Error::Rename;
        file = store.open(path);
        if (!memoVerify(file, header)) return Error::Read;
    }
    return Error::None;
}

class MemoSource final : public AtomicSource {
public:
    MemoSource(MemoFile& next, File& previous, const MemoFile& old, const uint8_t* tail, size_t length)
        : _next(next), _file(previous), _old(old), _tail(tail), _tailLength(length) {}
    size_t length() const override { return MemoFile::Size + _next.length(); }
    bool prepare() {
        uint32_t crc = _next.seed();
        if (!body(nullptr, crc)) return false;
        _next.put(64, ~crc); return true;
    }
    bool emit(ByteSink& sink) const override {
        uint32_t crc = _next.seed();
        return sink.write(_next.bytes, MemoFile::Size) == MemoFile::Size && body(&sink, crc) && ~crc == _next.get(64);
    }
private:
    bool body(ByteSink* sink, uint32_t& crc) const {
        const size_t previousLength = _file ? _old.length() : 0;
        if (previousLength + _tailLength != _next.length() || (_tailLength && !_tail)) return false;
        uint8_t chunk[memo::ChunkBytes]; uint32_t oldCrc = _old.seed();
        if (_file && (_file.size() != MemoFile::Size + previousLength || !_file.seek(0) ||
            _file.read(chunk, MemoFile::Size) != MemoFile::Size || memcmp(chunk, _old.bytes, MemoFile::Size))) return false;
        for (size_t at = 0; at < previousLength;) {
            const size_t n = std::min(sizeof chunk, previousLength - at);
            if (_file.read(chunk, n) != n || (sink && sink->write(chunk, n) != n)) return false;
            oldCrc = audio::checksum(oldCrc, chunk, n); crc = audio::checksum(crc, chunk, n); at += n; yield();
        }
        if (_file && ~oldCrc != _old.get(64)) return false;
        if (_tailLength && sink && sink->write(_tail, _tailLength) != _tailLength) return false;
        crc = audio::checksum(crc, _tail, _tailLength); return true;
    }
    MemoFile& _next;
    File& _file;
    const MemoFile& _old;
    const uint8_t* _tail;
    size_t _tailLength;
};

template<class Medium>
Error memoWrite(Medium& store, const char* path, MemoFile& next, File& previous,
                const MemoFile& old, const uint8_t* tail = nullptr, size_t length = 0) {
    MemoSource source(next, previous, old, tail, length);
    return source.prepare() ? store.write(path, source) : Error::Verify;
}
template<class Medium>
void memoRemoveWork(Medium& store, const char* path) {
    // Best-effort cleanup only after a newer main revision has invalidated the
    // work. Failed removal cannot resurrect it, even if both copies survive.
    char other[192];
    for (const char* suffix : {".bak", ".tmp", ""}) {
        snprintf(other, sizeof other, "%s%s", path, suffix);
        if (store.exists(other)) store.remove(other);
    }
}
}

bool MessageTransactions::memoRetains(const RecordKey& key, const StoredRecordHeader& header, uint32_t revision) {
    if (!revision || header.incoming) return false;
    auto store = medium(0);
    if (!store.isReady()) return true;
    Request request; request.key = key; memcpy(request.source, header.source, 16);
    char parent[96], local[33], name[176]; directory(key.peer, 0, parent); encodeHex(header.source, 16, local);
    snprintf(name, sizeof name, "%s/.memo-%s.saved", parent, local);
    MemoFile saved; File file;
    if (memoLoad(store, name, request, saved, file) != Error::None || !file) return true;
    // A newer main revision proves explicit replacement/discard after retirement.
    // Until then a failed retirement must pin the exact outbox record, even if
    // delivery has already completed and ordinary retention would remove it.
    return saved.revision() < revision ||
        (saved.revision() == revision && (saved.state() != memo::State::Sent || saved.counter() != key.counter));
}

void MessageTransactions::memoTransaction(const Request& request, uint8_t* bytes, size_t length,
                                          size_t capacity, Result& result) {
    using memo::State;
    capacity = std::min(capacity, size_t(request.readCapacity));
    if (capacity < sizeof(memo::Snapshot) || request.key.incoming || request.key.counter ||
        length > memo::ChunkBytes || length % memo::FrameBytes ||
        (length && request.operation != Operation::MemoAppend) ||
        (request.operation == Operation::MemoAppend && !length) || !std::isfinite(request.timestamp) ||
        request.deliveryPolicy > messaging::DeliveryPolicy::Always) { result.error = Error::InvalidRecord; return; }
    auto store = medium(0);
    if (!store.isReady()) { result.error = Error::Unavailable; return; }
    char parent[96], local[33], savedPath[176], workPath[176];
    directory(request.key.peer, 0, parent); encodeHex(request.source, 16, local);
    uint32_t cutoff;
    result.error = memoCutoff(store, parent, cutoff);
    if (result.error != Error::None) return;
    if (cutoff == UINT32_MAX) { result.error = Error::CounterExhausted; return; }
    snprintf(savedPath, sizeof savedPath, "%s/.memo-%s.saved", parent, local);
    snprintf(workPath, sizeof workPath, "%s/.memo-%s.work", parent, local);
    MemoFile saved, work; File savedFile, workFile;
    result.error = memoLoad(store, savedPath, request, saved, savedFile);
    if (result.error != Error::None) return;
    if (savedFile && (saved.state() == State::Recording || saved.base())) { result.error = Error::InvalidRecord; return; }
    if (!savedFile || saved.revision() <= cutoff) {
        savedFile = {}; saved = {}; saved.put(4, cutoff);
        memcpy(saved.bytes + 24, request.source, 16); memcpy(saved.bytes + 40, request.key.peer, 16);
    }
    // Clear can recover from an interrupted/corrupt working copy. The new main
    // CAS token invalidates it before cleanup, without touching saved audio.
    if (request.operation != Operation::MemoClear && request.operation != Operation::MemoPromote) {
        result.error = memoLoad(store, workPath, request, work, workFile);
        if (result.error != Error::None) return;
        if (workFile && work.state() > State::Recording) { result.error = Error::InvalidRecord; return; }
        if (workFile && (work.revision() <= cutoff || work.revision() <= saved.revision() || work.base() != saved.revision())) workFile = {};
    }
    auto publish = [&] {
        memo::Snapshot snapshot;
        snapshot.revision = saved.revision(); snapshot.length = uint16_t(saved.length());
        snapshot.state = saved.state(); snapshot.policy = saved.policy(); snapshot.outboxCounter = saved.counter();
        if (workFile) {
            snapshot.workingRevision = work.revision(); snapshot.workingLength = uint16_t(work.length());
            snapshot.interrupted = work.state() == State::Recording;
        }
        memcpy(bytes, &snapshot, sizeof snapshot); result.length = sizeof snapshot;
        result.revision = snapshot.revision; result.outcome = Outcome::Committed;
    };
    auto reserve = [&](uint32_t& counter) {
        const uint32_t maximum = std::max({cutoff, saved.revision(), saved.counter(), workFile ? work.revision() : 0u});
        if (maximum == UINT32_MAX) { result.error = Error::CounterExhausted; return false; }
        _nextCounter = std::max(_nextCounter, maximum + 1);
        if (!reserveCounter(counter)) { result.error = _nextCounter == UINT32_MAX ? Error::CounterExhausted : Error::Write; return false; }
        return true;
    };
    auto write = [&](const char* path, MemoFile& next, File& file, const MemoFile& old,
                     const uint8_t* tail = nullptr, size_t n = 0) {
        result.flash.attempted = true;
        result.error = result.flash.error = store.ensureDir(parent) ? memoWrite(store, path, next, file, old, tail, n) : Error::Write;
        result.flash.committed = result.error == Error::None;
        return result.flash.committed;
    };
    const auto op = request.operation;
    if (op == Operation::MemoRead) {
        if (request.offset && request.offset != saved.revision()) { result.error = Error::Stale; return; }
        const size_t at = request.peerGeneration;
        if (at > saved.length()) { result.error = Error::InvalidRecord; return; }
        const size_t n = std::min(saved.length() - at, capacity - sizeof(memo::Snapshot));
        if (n && !memoVerify(savedFile, saved, bytes + sizeof(memo::Snapshot), at, n)) { result.error = Error::Verify; return; }
        publish(); result.length += n; result.nextOffset = at + n; result.more = result.nextOffset < saved.length(); return;
    }
    if (op == Operation::MemoBegin || op == Operation::MemoClear || op == Operation::MemoPromote) {
        if (request.offset != saved.revision()) { result.error = Error::Stale; return; }
    }
    if (op == Operation::MemoBegin || op == Operation::MemoClear) {
        if (saved.state() == State::Promoting) { result.error = Error::Stale; return; }
        if (op == Operation::MemoBegin && request.peerGeneration != (workFile ? work.revision() : 0)) { result.error = Error::Stale; return; }
        uint32_t token; if (!reserve(token)) return;
        MemoFile next; next.put(4, token); next.put(8, op == Operation::MemoBegin ? saved.revision() : 0);
        memcpy(next.bytes + 24, request.source, 16); memcpy(next.bytes + 40, request.key.peer, 16);
        next.bytes[60] = uint8_t(op == Operation::MemoBegin ? State::Recording : State::Empty);
        File none;
        if (!write(op == Operation::MemoBegin ? workPath : savedPath, next, none, next)) return;
        if (op == Operation::MemoBegin) {
            work = next; workFile = store.open(workPath);
            if (!workFile) { result.error = Error::Read; return; }
        } else { saved = next; workFile = {}; memoRemoveWork(store, workPath); }
        publish(); return;
    }
    if (op == Operation::MemoSeal && saved.revision() == request.offset && saved.state() == State::Ready) {
        result.duplicate = true; publish(); return;
    }
    if (op == Operation::MemoAppend || op == Operation::MemoSeal || op == Operation::MemoCancel) {
        if (!workFile || work.revision() != request.offset || work.state() != State::Recording || saved.state() == State::Promoting) {
            result.error = Error::Stale; return;
        }
        MemoFile next = work;
        if (op == Operation::MemoAppend) {
            const size_t at = request.peerGeneration;
            if (at < work.length() && length <= work.length() - at) {
                uint8_t previous[memo::ChunkBytes];
                if (!memoVerify(workFile, work, previous, at, length)) { result.error = Error::Verify; return; }
                if (memcmp(previous, bytes, length)) { result.error = Error::Stale; return; }
                result.duplicate = true; publish(); return;
            }
            if (at != work.length() || length > memo::RecordBytes - at) { result.error = Error::InvalidRecord; return; }
            next.put(56, at + length);
            if (!write(workPath, next, workFile, work, bytes, length)) return;
            work = next; publish(); return;
        }
        if (op == Operation::MemoSeal) {
            if (!work.length()) { result.error = Error::InvalidRecord; return; }
            next.bytes[60] = uint8_t(State::Ready); next.put(8, 0);
            if (!write(savedPath, next, workFile, work)) return;
            saved = next; workFile = {}; memoRemoveWork(store, workPath); publish(); return;
        }
        // Persist cancellation, rather than deleting the only record of the
        // session. Delayed append/seal requests then fail their state check.
        next.bytes[60] = uint8_t(State::Empty); next.put(56, 0);
        File none;
        if (!write(workPath, next, none, next)) return;
        work = next; publish(); return;
    }
    if (op != Operation::MemoPromote || saved.state() < State::Ready || !saved.length()) { result.error = Error::Stale; return; }
    if (saved.state() == State::Sent) {
        result.key.counter = saved.counter(); result.duplicate = true; publish(); return;
    }
    if (capacity < memo::RecordBytes + 64) { result.error = Error::InvalidRecord; return; }
    if (saved.state() == State::Ready) {
        uint32_t counter; if (!reserve(counter)) return;
        MemoFile next = saved; next.put(12, counter); next.time(request.timestamp);
        next.bytes[60] = uint8_t(State::Promoting); next.bytes[61] = uint8_t(request.deliveryPolicy);
        if (!write(savedPath, next, savedFile, saved)) return;
        result.error = memoLoad(store, savedPath, request, saved, savedFile);
        if (result.error != Error::None) return;
    }
    Request createRequest; createRequest.operation = Operation::CreateOutgoing;
    createRequest.peerGeneration = saved.revision();
    memcpy(createRequest.key.peer, request.key.peer, 16); memcpy(createRequest.source, request.source, 16);
    memcpy(createRequest.destination, request.key.peer, 16); createRequest.timestamp = saved.time();
    createRequest.deliveryPolicy = saved.policy(); createRequest.read = true;
    createRequest.status = uint8_t(saved.policy() == messaging::DeliveryPolicy::Always ? LXMFStatus::PROP_QUEUED : LXMFStatus::QUEUED);
    createRequest.contentLength = uint16_t(snprintf(reinterpret_cast<char*>(bytes), capacity, "Voice message (%us)",
        unsigned((saved.length() / memo::FrameBytes * memo::FrameMs + 999) / 1000)));
    createRequest.audio = {0, uint16_t(saved.length()), memo::Mode, 1};
    if (!memoVerify(savedFile, saved, bytes + createRequest.contentLength, 0, saved.length())) { result.error = Error::Verify; return; }
    RecordKey key = request.key; key.counter = saved.counter();
    bool exists = false;
    for (unsigned which = 0; which < 2; ++which) {
        auto candidate = medium(which); char name[128];
        for (const char* suffix : {"", ".bak"}) { path(key, which, name, suffix); exists |= candidate.exists(name); }
    }
    if (exists) {
        MessageDocument document; StoredRecordHeader header;
        result.error = load(key, document, header);
        const auto& audio = createRequest.audio;
        if (result.error != Error::None) return;
        if (memcmp(header.source, request.source, 16) || memcmp(header.destination, request.key.peer, 16) ||
            header.incoming || header.timestamp != saved.time() || header.deliveryPolicy != saved.policy() ||
            header.audio.mode != memo::Mode || header.audio.state != 1 || header.audio.length != saved.length() ||
            header.audio.checksum != audio::digest(key, request.source, request.key.peer, audio, bytes + createRequest.contentLength)) {
            result.error = Error::Verify; return;
        }
        result.error = ensureAudio(key, 0, header, bytes + createRequest.contentLength);
        if (result.error != Error::None) return;
        result.key = key; result.duplicate = true; result.newStatus = header.status;
        result.outcome = Outcome::Committed;
    } else {
        create(createRequest, bytes, result, key.counter);
        if (result.outcome != Outcome::Committed) return;
    }
    // A committed outbox record is already a successful Send, even if retiring
    // the draft fails. Recovery checks that exact counter before attempting any
    // creation, preserving status/receipts and the frozen timestamp/policy.
    MemoFile sent = saved; sent.bytes[60] = uint8_t(State::Sent);
    try {
        if (memoWrite(store, savedPath, sent, savedFile, saved) == Error::None) saved = sent;
    } catch (...) {
        // Optional retirement cannot hide an already committed Send. Its
        // reservation remains durable and the record stays pinned for recovery.
    }
    publish(); result.error = Error::None;
}
