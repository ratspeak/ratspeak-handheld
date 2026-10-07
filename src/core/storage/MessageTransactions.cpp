#include "MessageTransactions.h"
#include "PreparedEnvelope.h"
#include "RrcRecord.h"
#include "MemoDraft.h"
#include "protocol/RrcPreferences.h"
#include "PurgeJournal.h"
#include "config/Config.h"
#include <Preferences.h>
#include <limits>
#include <cerrno>
#include "util/DisplayText.h"

namespace handheld::storage {

static bool historyLess(HistoryEntry a, HistoryEntry b) {
    return a.counter < b.counter || (a.counter == b.counter && a.incoming < b.incoming);
}

void MessageTransactions::clearSummaries() {
    for (auto& summary : _summaries) summary.counter = 0;
    _summaryClock = 0;
}

void MessageTransactions::checkSummaryMedia() {
    const uint8_t available = (medium(0).isReady() ? 1 : 0) | (medium(1).isReady() ? 2 : 0);
    if (_summaryMedia != available) { clearSummaries(); _summaryMedia = available; }
}

void MessageTransactions::invalidateSummary(const uint8_t peer[16]) {
    for (auto& summary : _summaries)
        if (summary.counter && !memcmp(summary.row.peer, peer, 16)) summary.counter = 0;
}

MessageTransactions::Summary* MessageTransactions::findSummary(const uint8_t peer[16]) {
    checkSummaryMedia();
    if (_summaryClock == UINT32_MAX) clearSummaries();
    for (auto& summary : _summaries) {
        if (summary.counter && !memcmp(summary.row.peer, peer, 16)) {
            summary.used = ++_summaryClock; return &summary;
        }
    }
    return nullptr;
}

void MessageTransactions::rememberSummary(const ConversationView& row, const ConversationSelector& selector,
                                         uint32_t revision) {
    if (!selector.counter) return;
    checkSummaryMedia();
    if (_summaryClock == UINT32_MAX) clearSummaries();
    auto* selected = &_summaries[0];
    for (auto& summary : _summaries) {
        // A forced reduction (startup hints) can refresh an existing entry.
        // Prefer that entry even if an earlier slot was invalidated.
        if (summary.counter && !memcmp(summary.row.peer, row.peer, 16)) { selected = &summary; break; }
        if (!summary.counter || (selected->counter && summary.used < selected->used)) selected = &summary;
    }
    *selected = {row, selector.counter, revision, ++_summaryClock, bool(selector.incoming)};
}

void MessageTransactions::directory(const uint8_t peer[16], unsigned which, char output[96]) const {
    char hex[33]; encodeHex(peer, 16, hex);
    snprintf(output, 96, "%s/%s", which == 0 ? PATH_MESSAGES : SD_PATH_MESSAGES, hex);
}

void MessageTransactions::path(const RecordKey& key, unsigned which, char output[128], const char* suffix) const {
    char parent[96]; directory(key.peer, which, parent);
    snprintf(output, 128, "%s/%013lu_%c.json%s", parent, (unsigned long)key.counter,
             key.incoming ? 'i' : 'o', suffix);
}

bool MessageTransactions::filename(const char* name, uint32_t& counter, bool& incoming) {
    if (!name) return false;
    const size_t length = strnlen(name, 25);
    if (length != 20 && length != 24) return false;
    if (name[13] != '_' || (name[14] != 'i' && name[14] != 'o') ||
        memcmp(name + 15, ".json", 5) || (length == 24 && memcmp(name + 20, ".bak", 4))) return false;
    uint64_t value = 0;
    for (unsigned i = 0; i < 13; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        value = value * 10 + unsigned(name[i] - '0');
    }
    if (value > UINT32_MAX) return false;
    counter = uint32_t(value); incoming = name[14] == 'i'; return true;
}

#include "DeleteRecord.inc"

uint32_t MessageTransactions::deletedThrough(const uint8_t peer[16]) {
    MessageDocument document; return deletedThrough(peer, document);
}

uint32_t MessageTransactions::deletedThrough(const uint8_t peer[16], MessageDocument& document,
    uint8_t* markerState, Error* markerError) {
    if (markerState) *markerState = 0;
    if (markerError) *markerError = Error::None;
    auto flash = medium(0);
    if (!flash.isReady()) { if (markerError) *markerError = Error::Unavailable; return UINT32_MAX; }
    char parent[96], marker[128]; directory(peer, 0, parent);
    snprintf(marker, sizeof(marker), "%s/.deleted", parent);
    uint32_t cutoff = 0;
    bool exists = false, valid = false, primaryExists = false, primaryValid = false;
    uint32_t primaryCutoff = 0;
    Error uncertain = Error::None;
    for (const char* suffix : {"", ".bak"}) {
        char candidate[128]; snprintf(candidate, sizeof(candidate), "%s%s", marker, suffix);
        if (!flash.exists(candidate)) continue;
        exists = true;
        if (!suffix[0]) primaryExists = true;
        File file = flash.open(candidate);
        if (!file) { uncertain = Error::Read; continue; }
        if (file.size() > 512) continue;
        const auto parsed = document.parse(file);
        if (parsed == Error::Allocation || parsed == Error::Read) uncertain = parsed;
        if (parsed != Error::None || !document.document()["through"].is<uint32_t>()) continue;
        const uint32_t through = document.document()["through"].as<uint32_t>();
        if (!suffix[0]) { primaryValid = true; primaryCutoff = through; }
        else if (markerState && primaryValid && through > primaryCutoff) *markerState |= 1;
        cutoff = std::max(cutoff, through); valid = true;
    }
    if (markerState && primaryExists && !primaryValid) *markerState |= 2;
    const auto error = uncertain != Error::None ? uncertain : exists && !valid ? Error::InvalidRecord : Error::None;
    if (markerError) *markerError = error;
    return error == Error::None ? cutoff : UINT32_MAX;
}

void MessageTransactions::beginRecords(Cursor& cursor, const uint8_t peer[16], MessageDocument* scratch) {
    cursor = {}; memcpy(cursor.peer, peer, 16);
    if (scratch) cursor.cutoff = deletedThrough(peer, *scratch, nullptr, &cursor.error);
    else { MessageDocument document; cursor.cutoff = deletedThrough(peer, document, nullptr, &cursor.error); }
}

Error MessageTransactions::openDirectory(unsigned which, const char* path, bool required, File& directory) {
    auto store = medium(which);
    if (!store.isReady()) return which == 0 ? Error::Unavailable : Error::None;
    errno = 0;
    directory = store.open(path);
    const int failure = errno;
    if (directory) return directory.isDirectory() ? Error::None : Error::Read;
    // A missing peer directory is normal. A required root, a known directory
    // that refused opening, or a reported I/O failure is never successful EOF.
    if (required || store.exists(path) || (failure && failure != ENOENT))
        return failure == ENOMEM ? Error::Allocation : Error::Read;
    return Error::None;
}

bool MessageTransactions::nextRecord(Cursor& cursor, RecordKey& key) {
    if (cursor.error != Error::None) return false;
    for (; cursor.medium < 2;) {
        auto store = medium(cursor.medium);
        if (!cursor.opened) {
            char parent[96]; directory(cursor.peer, cursor.medium, parent);
            cursor.error = openDirectory(cursor.medium, parent, false, cursor.directory);
            if (cursor.error != Error::None) return false;
            cursor.opened = true;
        }
        if (!cursor.directory || !cursor.directory.isDirectory()) {
            cursor.directory.close(); cursor.opened = false; ++cursor.medium; continue;
        }
        errno = 0;
        File file = cursor.directory.openNextFile();
        const int failure = errno;
        if (!file) {
            if (failure) { cursor.error = failure == ENOMEM ? Error::Allocation : Error::Read; return false; }
            cursor.directory.close(); cursor.opened = false; ++cursor.medium; continue;
        }
        uint32_t counter = 0; bool incoming = false;
        if(file.isDirectory()) continue;
        char name[32];snprintf(name,sizeof name,"%s",file.name());
        const size_t length=strlen(name);
        // Retention visits sidecar-bearing records only. Text-only history and
        // already-cleaned audio tombstones need no JSON parse or deletion read.
        const char* audioSuffixes[] = {".audio", ".audio.bak", ".audio.tmp", ".prop", ".prop.bak", ".prop.tmp"};
        size_t audioSuffix = 0;
        if (cursor.audioOnly) {
            while (audioSuffix < 6 && (length <= 20 || strcmp(name + 20, audioSuffixes[audioSuffix]))) ++audioSuffix;
            if (audioSuffix == 6) continue;
            name[20] = 0;
        }
        const bool tombstone=(length==24 && !strcmp(name+20,".del")) || (length==28 && !strcmp(name+20,".del.bak"));
        if(tombstone && cursor.includeDeleted) name[20]=0;
        if(!filename(name,counter,incoming) || counter<=cursor.cutoff) continue;
        key = {}; memcpy(key.peer, cursor.peer, 16); key.counter = counter; key.incoming = incoming;
        if (cursor.audioOnly) {
            bool duplicate = false;
            for (size_t i = 0; i < audioSuffix; ++i) {
                char candidate[128]; path(key, cursor.medium, candidate, audioSuffixes[i]);
                if (store.exists(candidate)) { duplicate = true; break; }
            }
            if (duplicate) continue;
            char record[128]; path(key, cursor.medium, record);
            if (!store.exists(record)) {
                path(key, cursor.medium, record, ".bak");
                if (!store.exists(record)) continue; // Uncommitted orphan, not a retained message.
            }
        }
        if(!cursor.includeDeleted) {
            bool deleted=false;cursor.error=recordDeletion(key,deleted);
            if(cursor.error!=Error::None) return false;
            if(deleted) continue;
        } else if(tombstone) return true;
        // A key is visited once even when it has a backup or an optional mirror.
        char primary[128]; path(key, cursor.medium, primary);
        const bool backup = strlen(file.name()) == 24;
        if (backup && store.exists(primary)) continue;
        if (cursor.medium == 1) {
            auto flash = medium(0); char candidate[128]; path(key, 0, candidate);
            if (flash.exists(candidate)) continue;
            path(key, 0, candidate, ".bak"); if (flash.exists(candidate)) continue;
        }
        return true;
    }
    return false;
}

bool MessageTransactions::nextPeer(Cursor& cursor, char output[33]) {
    output[0] = 0;
    if (cursor.error != Error::None) return false;
    while (cursor.medium < 2) {
        if (!cursor.opened) {
            cursor.error = openDirectory(cursor.medium, cursor.medium == 0 ? PATH_MESSAGES : SD_PATH_MESSAGES,
                                         true, cursor.directory);
            if (cursor.error != Error::None) return false;
            cursor.opened = true;
        }
        if (!cursor.directory) { cursor.opened = false; ++cursor.medium; continue; }
        errno = 0;
        File entry = cursor.directory.openNextFile();
        const int failure = errno;
        if (!entry) {
            if (failure) { cursor.error = failure == ENOMEM ? Error::Allocation : Error::Read; return false; }
            cursor.directory.close(); cursor.opened = false; ++cursor.medium; continue;
        }
        uint8_t peer[16]; const char* name = entry.name();
        if (!entry.isDirectory() || !decodeHex(name, strnlen(name, 33), peer, 16)) continue;
        if (cursor.medium == 1) {
            char parent[96]; directory(peer, 0, parent); File known;
            cursor.error = openDirectory(0, parent, false, known);
            if (cursor.error != Error::None) return false;
            if (known) continue; // Flash peer already visited; summarize spans both media.
        }
        memcpy(output, name, 33); return true;
    }
    return false;
}

bool MessageTransactions::nextPeer(const char* after, char output[33], Error& error) {
    output[0] = 0;
    if (error != Error::None) return false;
    Cursor cursor; char peer[33];
    while (nextPeer(cursor, peer)) {
        if (strcmp(peer, after) <= 0 || (output[0] && strcmp(peer, output) >= 0)) continue;
        memcpy(output, peer, 33);
    }
    error = cursor.error;
    if (error != Error::None) return false;
    return output[0] != 0;
}

Error MessageTransactions::inspect(const RecordKey& key, unsigned which, const char* suffix,
                                   MessageDocument& document, StoredRecordHeader& header, bool* parsed) {
    char filePath[128]; path(key, which, filePath, suffix);
    auto store = medium(which); File file = store.open(filePath);
    if (!file) return Error::Read;
    if (parsed) *parsed = true;
    const auto error = document.parse(file);
    if (error != Error::None) return error;
    header = {};
    if (!recordHeader(document.document(), header) || header.incoming != key.incoming ||
        memcmp(key.peer, key.incoming ? header.source : header.destination, 16)) return Error::InvalidRecord;
    header.counter = key.counter;
    return Error::None;
}

Error MessageTransactions::load(const RecordKey& key, MessageDocument& document, StoredRecordHeader& header,
                                uint8_t* selectedMedium) {
    if (selectedMedium) *selectedMedium = UINT8_MAX;
    Error markerError;
    const auto cutoff = deletedThrough(key.peer, document, nullptr, &markerError);
    if (markerError != Error::None) return markerError;
    if (key.counter <= cutoff) return Error::Stale;
    bool deleted=false;markerError=recordDeletion(key,deleted);
    if(markerError!=Error::None) return markerError;
    if(deleted) return Error::Stale;
    int best = -1, loaded = -1; uint32_t revision = 0; _blockedMedia = _preferBackup = 0;
    uint32_t primaryRevision[2] = {}, backupRevision[2] = {};
    bool primaryValid[2] = {}, backupValid[2] = {};
    Error failure = Error::Read;
    // Inspect every copy, leaving the preferred (flash primary) copy last.
    // The selected document usually remains in the existing scratch owner,
    // avoiding another complete parse for every preview/status/body slice.
    for (unsigned candidate : {3u, 2u, 1u, 0u}) {
        const unsigned which = candidate / 2; const char* suffix = candidate % 2 ? ".bak" : "";
        StoredRecordHeader current;
        bool parsed = false;
        const auto error = inspect(key, which, suffix, document, current, &parsed);
        if (parsed) loaded = int(candidate); // Even an invalid parse replaces scratch.
        if (error != Error::None) {
            // An unreadable candidate may have a newer revision. Do not build
            // a mutation on an older mirror merely because memory/I/O failed.
            if (error == Error::Allocation) return error;
            if (error == Error::Read) {
                char candidatePath[128]; path(key, which, candidatePath, suffix); auto store = medium(which);
                if (store.exists(candidatePath)) return error;
            }
            if (candidate % 2 == 0) {
                char primary[128]; path(key, which, primary); auto store = medium(which);
                if (store.exists(primary)) _blockedMedia |= uint8_t(1u << which);
            }
            if (error == Error::Allocation || error == Error::InvalidRecord) failure = error;
            continue;
        }
        if (candidate % 2 == 0) { primaryValid[which] = true; primaryRevision[which] = current.revision; }
        else { backupValid[which] = true; backupRevision[which] = current.revision; }
        if (best < 0 || current.revision > revision ||
            (current.revision == revision && int(candidate) < best)) {
            best = int(candidate); revision = current.revision; header = current;
        }
    }
    if (best < 0) return failure;
    for (unsigned which = 0; which < 2; ++which)
        if (primaryValid[which] && backupValid[which] && backupRevision[which] > primaryRevision[which])
            _preferBackup |= uint8_t(1u << which);
    const auto error = loaded == best ? Error::None :
        inspect(key, unsigned(best) / 2, best % 2 ? ".bak" : "", document, header);
    if (error == Error::None && selectedMedium) *selectedMedium = uint8_t(best / 2);
    return error;
}

bool MessageTransactions::begin(FlashStore* flash, SDStore* sd, bool external, bool deferred) {
    handheld::assertDeviceOwner();
    _flash = flash; _sd = sd; _external = external; _deferred = deferred;
    clearSummaries(); checkSummaryMedia();
    if (!StorageLease::initialize()) return false;
    StorageLease lease;
    if (!lease.held() || !_flash || !_flash->isReady() || !_flash->ensureDir(PATH_MESSAGES)) return false;
    if (_external && _sd && _sd->isReady()) _sd->ensureDir(SD_PATH_MESSAGES);
    return initializeCounter();
}

bool MessageTransactions::initializeCounter() {
    Preferences preferences;
    if (!preferences.begin(NVS_NS_MSG, false)) return false;
    _nextCounter = std::max(uint32_t(1), preferences.getUInt("msgctr", 0));
    preferences.end(); _reservedThrough = _nextCounter;
    // Always reconcile the persisted ceiling with imported/recovered files.
    char previous[33] = {}, peerHex[33]; Error scanError = Error::None;
    while (nextPeer(previous, peerHex, scanError)) {
        memcpy(previous, peerHex, 33); uint8_t peer[16]; decodeHex(peerHex, 32, peer, 16);
        Cursor cursor; beginRecords(cursor, peer);cursor.includeDeleted=true; RecordKey key;
        if (cursor.cutoff == UINT32_MAX) { _nextCounter = UINT32_MAX; return true; }
        _nextCounter = std::max(_nextCounter, cursor.cutoff + 1);
        while (nextRecord(cursor, key)) {
            if (key.counter == UINT32_MAX) { _nextCounter = UINT32_MAX; return true; }
            _nextCounter = std::max(_nextCounter, key.counter + 1);
        }
        if (cursor.error != Error::None) return false;
    }
    return scanError == Error::None;
}

bool MessageTransactions::reserveCounter(uint32_t& counter) {
    if (_nextCounter == UINT32_MAX) return false;
    counter = _nextCounter;
    if (_nextCounter >= _reservedThrough) {
        const uint32_t ceiling = _nextCounter > UINT32_MAX - 32 ? UINT32_MAX : _nextCounter + 32;
        Preferences preferences;
        if (!preferences.begin(NVS_NS_MSG, false)) return false;
        const bool saved = preferences.putUInt("msgctr", ceiling) == sizeof(uint32_t) &&
            preferences.getUInt("msgctr", 0) == ceiling;
        preferences.end();
        if (!saved) return false;
        _reservedThrough = ceiling;
    }
    ++_nextCounter;
    return true;
}

File MessageTransactions::openAudio(const RecordKey& key, unsigned which, const StoredRecordHeader& header,
                                   uint8_t* output, size_t offset, size_t length) {
    auto store = medium(which);
    if (!store.isReady() || header.audio.state != 1) return {};
    for (const char* suffix : {".audio", ".audio.bak"}) {
        char candidate[128]; path(key, which, candidate, suffix);
        File file = store.open(candidate);
        if (audio::verify(file, key, header, output, offset, length)) return file;
    }
    return {};
}

Error MessageTransactions::ensureAudio(const RecordKey& key, unsigned which,
                                      const StoredRecordHeader& header, const uint8_t* bytes) {
    if (header.audio.state != 1) return Error::None;
    if (openAudio(key, which, header)) return Error::None;
    auto store = medium(which); char target[128]; path(key, which, target, ".audio");
    if (bytes) {
        if (audio::digest(key, header.source, header.destination, header.audio, bytes) != header.audio.checksum)
            return Error::Verify;
        MemorySource source(bytes, header.audio.length);
        return store.write(target, source);
    }
    auto original = openAudio(key, 1 - which, header);
    if (!original) return Error::Read;
    audio::FileSource source(original, header.audio.length,
        audio::seed(key, header.source, header.destination, header.audio), header.audio.checksum);
    return store.write(target, source);
}

bool MessageTransactions::removeAudio(const RecordKey& key, unsigned which) {
    auto store = medium(which); bool removed = true;
    for (const char* suffix : {".audio", ".audio.bak", ".audio.tmp"}) {
        char candidate[128]; path(key, which, candidate, suffix);
        if (store.exists(candidate) && !store.remove(candidate)) removed = false;
    }
    return removed;
}

Error MessageTransactions::commit(const RecordKey& key, MessageDocument& document, bool creating, Result& result,
                                  const uint8_t* media, uint8_t referenceOnlyMedium, bool requireFlash) {
    JsonSource source(document);
    if (source.error() != Error::None) return source.error();
    if (document.document().overflowed() || source.length() > (creating ? Budget::MaxNewJson : Budget::MaxStoredFile))
        return Error::InvalidRecord;
    StoredRecordHeader header;
    if (!recordHeader(document.document(), header)) return Error::InvalidRecord;
    bool committed = false; Error error = Error::Unavailable;
    for (unsigned which = 0; which < 2; ++which) {
        if (which == 1 && requireFlash && !result.flash.committed) return error;
        auto store = medium(which); auto& outcome = which == 0 ? result.flash : result.sd;
        if (which == 1 && !_external) continue;
        outcome.attempted = true;
        if (!store.isReady()) { outcome.error = Error::Unavailable; continue; }
        try {
            char parent[96], filePath[128]; directory(key.peer, which, parent); path(key, which, filePath);
            char backup[128]; path(key, which, backup, ".bak");
            if (!creating && retainedCopy(key, which, document)) outcome.error = Error::None;
            else if (!creating && (_blockedMedia & (1u << which))) outcome.error = Error::InvalidRecord;
            else if (creating && (store.exists(filePath) || store.exists(backup))) outcome.error = Error::Stale;
            else if (!creating && (_preferBackup & (1u << which)) &&
                     (!store.remove(filePath) || !store.rename(backup, filePath))) outcome.error = Error::Rename;
            else if (!store.ensureDir(parent)) outcome.error = Error::Write;
            else {
                // Each medium commits its immutable blob before the referencing
                // JSON. Neither a partial mirror nor an orphan is delivery proof.
                outcome.error = ensureAudio(key, which, header, media);
                // Read/status changes may retain an already committed media
                // reference on the copy load() selected, even if its audio was
                // subsequently lost. They cannot create a new mirror or prove
                // receipt of a new/resubmitted message without durable audio.
                if (!creating && which == referenceOnlyMedium) outcome.error = Error::None;
                if (outcome.error == Error::None) outcome.error = store.write(filePath, source);
            }
        } catch (const std::bad_alloc&) { outcome.error = Error::Allocation; }
        catch (...) { outcome.error = Error::Internal; }
        outcome.committed = outcome.error == Error::None;
        committed |= outcome.committed;
        if (!outcome.committed) error = outcome.error;
    }
    return committed ? Error::None : error;
}

void MessageTransactions::create(const Request& request, uint8_t* bytes, Result& result, uint32_t reservedCounter) {
    const uint32_t cutoff = deletedThrough(request.key.peer);
    if (cutoff == UINT32_MAX) { result.error = Error::CounterExhausted; return; }
    if (reservedCounter) {
        RecordKey reserved=request.key;reserved.counter=reservedCounter;bool deleted=false;
        result.error=recordDeletion(reserved,deleted);
        if(result.error!=Error::None) return;
        if(deleted) {result.error=Error::Stale;return;}
    }
    MessageDocument document;
    bool conversationExists = false;
    {
        Cursor cursor; beginRecords(cursor, request.key.peer, &document); RecordKey key;
        cursor.includeDeleted=request.key.incoming && request.hasMessageId;
        while (nextRecord(cursor, key)) {
            bool deleted=false;StoredRecordHeader deletedHeader;
            result.error=recordDeletion(key,deleted,&deletedHeader);if(result.error!=Error::None) return;
            if(deleted) {
                if(request.key.incoming && request.hasMessageId && deletedHeader.incoming && deletedHeader.hasMessageId &&
                   !memcmp(deletedHeader.destination,request.destination,16) && !memcmp(deletedHeader.messageId,request.messageId,32)) {
                    result.key=key;result.revision=deletedHeader.revision;result.duplicate=true;
                    result.oldStatus=result.newStatus=deletedHeader.status;result.outcome=Outcome::Committed;return;
                }
                continue;
            }
            conversationExists = true;
            if (!request.key.incoming || !request.hasMessageId) break;
            StoredRecordHeader header;
            const auto error = load(key, document, header);
            if (error == Error::Allocation || error == Error::Read) { result.error = error; return; }
            if (error == Error::None && header.incoming && header.hasMessageId &&
                !memcmp(header.messageId, request.messageId, 32)) {
                result.key = key; result.revision = header.revision; result.duplicate = true;
                result.oldStatus = result.newStatus = header.status;
                auto media = request.audio;
                const auto* audioBytes = bytes + request.titleLength + request.contentLength;
                if (media.state == 1) media.checksum = audio::digest(key, request.source, request.destination, media, audioBytes);
                if (!header.audio.state && media.state) {
                    // Older firmware stored authenticated text but discarded the
                    // audio field. A verified resend can enrich that same MID.
                    if (header.revision == UINT32_MAX) { result.error = Error::RevisionExhausted; return; }
                    if (!recordAudio(document.document(), media)) { result.error = Error::Allocation; return; }
                    document.document()["store_revision"] = ++header.revision;
                    result.revision = header.revision;
                    result.enriched = true;
                } else if (header.audio.state==3 && media.state==1 && media.mode==header.audio.mode &&
                           media.length==header.audio.length && media.checksum==header.audio.checksum) {
                    // An authenticated duplicate must not resurrect locally expired audio.
                    media.state=3;
                } else if (memcmp(&media, &header.audio, sizeof(media))) {
                    result.error = Error::Verify; return;
                }
                result.error = commit(key, document, false, result, media.state == 1 ? audioBytes : nullptr);
                if (result.error == Error::None) result.outcome = Outcome::Committed;
                return;
            }
        }
        if (cursor.error != Error::None) { result.error = cursor.error; return; }
    }
    document.normal();
    if (!createRecord(document.document(), request, bytes)) { result.error = Error::Allocation; return; }
    if (reservedCounter) document.document()["memo_draft"] = request.peerGeneration;
    _nextCounter = std::max(_nextCounter, cutoff + 1);
    if (reservedCounter && reservedCounter <= cutoff) { result.error = Error::Stale; return; }
    result.key.counter = reservedCounter;
    if (!reservedCounter && !reserveCounter(result.key.counter)) {
        result.error = _nextCounter == UINT32_MAX ? Error::CounterExhausted : Error::Write; return;
    }
    auto media = request.audio;
    const auto* audioBytes = bytes + request.titleLength + request.contentLength;
    if (media.state == 1) media.checksum = audio::digest(result.key, request.source, request.destination, media, audioBytes);
    if (!recordAudio(document.document(), media)) { result.error = Error::Allocation; return; }
    result.error = commit(result.key, document, true, result, media.state == 1 ? audioBytes : nullptr,
                          UINT8_MAX, reservedCounter != 0);
    if (result.error != Error::None) return;
    result.revision = 1; result.countDelta = 1; result.conversationDelta = conversationExists ? 0 : 1;
    result.unreadDelta = request.key.incoming && !request.read ? 1 : 0;
    result.newStatus = request.status;
    result.outcome = Outcome::Committed;
}

void MessageTransactions::update(const Request& request, Result& result) {
    MessageDocument document; StoredRecordHeader header;
    uint8_t selectedMedium;
    result.error = load(request.key, document, header, &selectedMedium);
    if (result.error != Error::None) return;
    const bool retry=request.operation==Operation::RetryOutgoing;
    uint8_t target=request.status;
    if(retry) {
        if(header.incoming || header.revision!=request.offset || memcmp(header.source,request.source,16) ||
           (!messaging::retryableStatus(header.status) || header.audio.state==3)) {result.error=Error::Stale;return;}
        target=uint8_t(header.deliveryPolicy==messaging::DeliveryPolicy::Always || messaging::relayStatus(header.status)?
            LXMFStatus::PROP_QUEUED:LXMFStatus::QUEUED);
    }
    result.oldStatus = header.status; result.newStatus = target;
    result.audioBecameRetainable = header.audio.state == 1 && messaging::pendingStatus(header.status) &&
        !messaging::pendingStatus(target) && target != uint8_t(LXMFStatus::DRAFT);
    if (!messaging::validDelivery(target, header.deliveryPolicy, header.incoming) ||
        (messaging::relayStatus(header.status) && target < 7 && target != 4 && target != 5)) {
        result.error = Error::Stale; return;
    }
    if (header.status == target) {
        result.revision = header.revision; result.error = commit(request.key, document, false, result, nullptr, selectedMedium);
        if (result.error == Error::None) result.outcome = Outcome::Committed;
        return;
    }
    // Retry applies only to a failed/unconfirmed exact revision. A recipient or
    // propagation receipt remains authoritative and cannot be made pending.
    if (header.status == 4 || (header.status == 9 && target != 4)) {
        result.error = Error::Stale; return;
    }
    if (header.revision == UINT32_MAX) { result.error = Error::RevisionExhausted; return; }
    document.document()["status"] = target;
    document.document()["store_revision"] = header.revision + 1;
    result.error = commit(request.key, document, false, result, nullptr, selectedMedium, retry);
    if (result.error == Error::None) { result.revision = header.revision + 1; result.outcome = Outcome::Committed; }
}

void MessageTransactions::markRead(const Request& request, Result& result) {
    if (deletedThrough(request.key.peer) == UINT32_MAX) { result.error = Error::Stale; return; }
    Cursor cursor; beginRecords(cursor, request.key.peer); RecordKey key;
    MessageDocument document; StoredRecordHeader header; Error failure = Error::None;
    while (nextRecord(cursor, key)) {
        if (!key.incoming || (request.key.counter && key.counter > request.key.counter)) continue;
        uint8_t selectedMedium;
        const auto error = load(key, document, header, &selectedMedium);
        if (error != Error::None) { failure = error; continue; }
        if (header.read) continue;
        if (header.revision == UINT32_MAX) { failure = Error::RevisionExhausted; continue; }
        document.document()["read"] = true; document.document()["store_revision"] = header.revision + 1;
        Result changed;
        const auto saved = commit(key, document, false, changed, nullptr, selectedMedium);
        if (saved != Error::None) { failure = saved; continue; }
        ++result.total; result.outcome = Outcome::Committed;
        if (result.unreadDelta > INT32_MIN) --result.unreadDelta;
        auto merge = [](MediumResult& aggregate, const MediumResult& item) {
            aggregate.attempted |= item.attempted; aggregate.committed |= item.committed;
            if (item.error != Error::None) aggregate.error = item.error;
        };
        merge(result.flash, changed.flash); merge(result.sd, changed.sd);
    }
    if (cursor.error != Error::None) failure = cursor.error;
    result.error = failure;
    if (failure == Error::None || result.total) result.outcome = Outcome::Committed;
}

void MessageTransactions::erase(const Request& request, Result& result) {
    const uint32_t previous = deletedThrough(request.key.peer);
    if (previous == UINT32_MAX) { result.error = Error::InvalidRecord; return; }
    uint32_t cutoff = std::max(previous, _nextCounter - 1);
    Cursor cursor; beginRecords(cursor, request.key.peer); RecordKey key;
    int32_t records = 0, unread = 0;
    MessageDocument previousDocument; StoredRecordHeader previousHeader;
    while (nextRecord(cursor, key)) {
        cutoff = std::max(cutoff, key.counter);
        if (records < INT32_MAX) ++records;
        const Error error = load(key, previousDocument, previousHeader);
        if (error == Error::Allocation || error == Error::Read) { result.error = error; return; }
        if (error == Error::None && previousHeader.incoming && !previousHeader.read && unread < INT32_MAX) ++unread;
    }
    if (cursor.error != Error::None) { result.error = cursor.error; return; }
    uint8_t markerState = 0;
    deletedThrough(request.key.peer, previousDocument, &markerState);
    previousDocument.reset();
    char parent[96], marker[128], backupMarker[128]; directory(request.key.peer, 0, parent);
    snprintf(marker, sizeof(marker), "%s/.deleted", parent);
    snprintf(backupMarker, sizeof(backupMarker), "%s/.deleted.bak", parent);
    MessageDocument document; document.document()["through"] = cutoff;
    JsonSource source(document); auto flash = medium(0);
    if (source.error() != Error::None) { result.error = source.error(); return; }
    result.flash.attempted = true;
    if (markerState & 2) result.flash.error = Error::InvalidRecord;
    else if ((markerState & 1) && (!flash.remove(marker) || !flash.rename(backupMarker, marker)))
        result.flash.error = Error::Rename;
    else result.flash.error = flash.ensureDir(parent) ? flash.write(marker, source) : Error::Write;
    result.flash.committed = result.flash.error == Error::None;
    if (!result.flash.committed) { result.error = result.flash.error; return; }
    result.key.counter = cutoff; result.outcome = Outcome::Committed;
    result.countDelta = -records; result.unreadDelta = -unread; result.conversationDelta = records ? -1 : 0;
    for (unsigned which = 0; which < 2; ++which) {
        auto store = medium(which); if (which == 1 && !_external) continue;
        auto& outcome = which == 0 ? result.flash : result.sd;
        outcome.attempted = true;
        if (!store.isReady()) { outcome.error = Error::Unavailable; continue; }
        directory(request.key.peer, which, parent);
        File dir;
        const Error opened = openDirectory(which, parent, false, dir);
        bool erased = opened == Error::None;
        if (opened != Error::None) outcome.error = opened;
        if (erased && dir) {
            for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
                if (strncmp(entry.name(), ".deleted", 8) == 0) continue;
                char child[128]; snprintf(child, sizeof(child), "%s/%s", parent, entry.name());
                entry.close(); erased = store.remove(child) && erased;
            }
        }
        if (!erased && outcome.error == Error::None) outcome.error = Error::Write;
        if (which == 1) outcome.committed = erased;
    }
}

void MessageTransactions::read(const Request& request, uint8_t* bytes, size_t capacity, Result& result) {
    capacity = std::min(capacity, size_t(request.readCapacity));
    if (capacity < sizeof(StoredRecordHeader)) { result.error = Error::InvalidRecord; return; }
    MessageDocument document; StoredRecordHeader header;
    result.error = load(request.key, document, header);
    if (result.error != Error::None) return;
    const size_t total = size_t(header.titleLength) + header.contentLength;
    if (request.offset > total) { result.error = Error::InvalidRecord; return; }
    memcpy(bytes, &header, sizeof(header));
    const size_t count = std::min(total - request.offset, capacity - sizeof(header));
    size_t offset = request.offset, copied = 0;
    for (const char* field : {"title", "content"}) {
        const auto value = document.document()[field].as<JsonString>();
        if (offset >= value.size()) { offset -= value.size(); continue; }
        const size_t take = std::min(value.size() - offset, count - copied);
        memcpy(bytes + sizeof(header) + copied, value.c_str() + offset, take);
        copied += take; offset = 0;
    }
    result.length = uint16_t(sizeof(header) + copied); result.total = uint32_t(total);
    result.nextOffset = request.offset + uint32_t(copied); result.more = result.nextOffset < total;
    result.revision = header.revision; result.outcome = Outcome::Committed;
}

void MessageTransactions::readAudio(const Request& request, uint8_t* bytes, size_t capacity, Result& result) {
    capacity = std::min(capacity, size_t(request.readCapacity));
    if (capacity <= sizeof(StoredRecordHeader)) { result.error = Error::InvalidRecord; return; }
    MessageDocument document; StoredRecordHeader header;
    result.error = load(request.key, document, header);
    if (result.error != Error::None) return;
    if (header.audio.state != 1 || request.offset > header.audio.length) {
        result.error = Error::InvalidRecord; return;
    }
    const size_t count = std::min(size_t(header.audio.length) - request.offset, capacity - sizeof(header));
    File file = openAudio(request.key, 0, header, bytes + sizeof(header), request.offset, count);
    if (!file) file = openAudio(request.key, 1, header, bytes + sizeof(header), request.offset, count);
    if (!file) { result.error = Error::Read; return; }
    memcpy(bytes, &header, sizeof(header));
    result.length = uint16_t(sizeof(header) + count); result.total = header.audio.length;
    result.nextOffset = request.offset + uint32_t(count); result.more = result.nextOffset < result.total;
    result.revision = header.revision; result.outcome = Outcome::Committed;
}

bool MessageTransactions::retainedCopy(const RecordKey& key, unsigned which, MessageDocument& document) {
    auto store = medium(which); JsonSource source(document);
    if (source.error() != Error::None) return false;
    StoredRecordHeader header;
    if (!recordHeader(document.document(), header) ||
        (header.audio.state == 1 && !openAudio(key, which, header))) return false;
    uint8_t scratch[Budget::IoScratch];
    for (const char* suffix : {"", ".bak"}) {
        char candidate[128]; path(key, which, candidate, suffix); File file = store.open(candidate);
        if (!file || file.size() != source.length()) continue;
        FileSink sink(file, scratch, source.length(), true);
        if (source.emit(sink) && sink.finish()) return true;
    }
    return false;
}

static void conversationPreview(ConversationView& row, const char* content, size_t length) {
    static constexpr char hex[] = "0123456789ABCDEF";
    size_t input = 0, output = 0;
    row.flags &= ~ConversationView::PreviewTruncated;
    while (input < length) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(content) + input;
        bool escape = false;
        const size_t count = handheld::display::codepoint(bytes, length - input, true, escape);
        escape |= bytes[0] < 0x20; // Summary rows always stay on one line.
        const size_t needed = escape ? 4 : count;
        if (output + needed >= sizeof(row.preview)) break;
        if (escape) {
            row.preview[output++] = '\\'; row.preview[output++] = 'x';
            row.preview[output++] = hex[bytes[0] >> 4]; row.preview[output++] = hex[bytes[0] & 15];
        } else { memcpy(row.preview + output, bytes, count); output += count; }
        input += count;
    }
    row.preview[output] = 0; row.previewLength = uint8_t(output);
    if (input < length) row.flags |= ConversationView::PreviewTruncated;
}

Error MessageTransactions::summarize(const uint8_t peer[16], ConversationView& row, ConversationSelector& selector,
    RecentIds* recent, size_t recentCapacity, uint32_t* latestRevision) {
    row = {}; selector = {};
    if (latestRevision) *latestRevision = 0;
    // Boot still reads all records to collect recent-ID hints, then retains
    // this same reducer's bounded result for the first list read. Runtime
    // queries reuse it only until a write, media change or eviction.
    if (!recent) if (const auto* summary = findSummary(peer)) {
        row = summary->row;
        selector.cursor.timestamp = row.timestamp; memcpy(selector.cursor.peer, peer, 16);
        selector.counter = summary->counter; selector.incoming = summary->incoming;
        if (latestRevision) *latestRevision = summary->revision;
        return Error::None;
    }
    memcpy(row.peer, peer, 16); memcpy(selector.cursor.peer, peer, 16);
    Cursor cursor; beginRecords(cursor, peer);
    MessageDocument document; StoredRecordHeader header; RecordKey key;
    uint32_t newestRevision = 0;
    while (nextRecord(cursor, key)) {
        const auto error = load(key, document, header);
        if (error != Error::None) return error;
        if (row.totalCount < UINT32_MAX) ++row.totalCount;
        if (!selector.counter || historyLess({selector.counter, bool(selector.incoming)}, {key.counter, key.incoming})) {
            selector.counter = key.counter; selector.incoming = key.incoming;
            if (latestRevision) *latestRevision = header.revision;
            newestRevision = header.revision;
            selector.cursor.timestamp = row.timestamp = header.timestamp;
            row.flags = (row.flags & ~ConversationView::LastIncoming) | (header.incoming ? ConversationView::LastIncoming : 0);
            const auto content = document.document()["content"].as<JsonString>();
            conversationPreview(row, content.c_str(), content.size());
        }
        if (header.incoming && !header.read && row.unreadCount < UINT32_MAX) ++row.unreadCount;
        if (!header.incoming) {
            if (!(row.flags & ConversationView::HasOutgoing) || key.counter >= row.lastOutgoingCounter) {
                row.flags |= ConversationView::HasOutgoing;
                row.lastOutgoingCounter = key.counter; row.outgoingRevision = header.revision;
                row.status = row.durableStatus = header.status;
            }
            if (messaging::pendingStatus(header.status) && row.pendingCount < UINT16_MAX) ++row.pendingCount;
            if (messaging::failedStatus(header.status) && row.failedCount < UINT16_MAX) ++row.failedCount;
        }
        // Retain the existing bounded counter-only boot hint policy exactly.
        if (recent && recentCapacity && header.hasMessageId) {
            const auto oldest = std::min_element(recent->begin(), recent->end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            if (recent->size() < recentCapacity || key.counter > oldest->first) {
                char id[65]; encodeHex(header.messageId, 32, id);
                if (recent->size() < recentCapacity) recent->push_back({key.counter, id});
                else *oldest = {key.counter, id};
            }
        }
    }
    if (cursor.error == Error::None) rememberSummary(row, selector, newestRevision);
    return cursor.error;
}

void MessageTransactions::conversationPage(const Request& request, uint8_t* bytes, size_t capacity, Result& result) {
    if (!validConversationOrder(request.conversationOrder) || !validConversationDirection(request.conversationDirection) ||
        !std::isfinite(request.timestamp)) { result.error = Error::InvalidRecord; return; }
    capacity = std::min(capacity, size_t(request.readCapacity));
    const size_t limit = std::min(size_t(64), capacity / sizeof(ConversationSelector));
    if (!limit) { result.error = Error::InvalidRecord; return; }
    ConversationCursor boundary; boundary.timestamp = request.timestamp; memcpy(boundary.peer, request.key.peer, 16);
    const bool before = request.conversationDirection == ConversationDirection::Before;
    auto less = [&](const ConversationSelector& a, const ConversationSelector& b) {
        return conversationLess(a.cursor, b.cursor, request.conversationOrder);
    };
    // Select directly in the ticket's owned output buffer, with memcpy loads
    // instead of alignment/object-lifetime assumptions about the byte slab.
    auto at = [&](size_t index) { ConversationSelector value; memcpy(&value, bytes + index * sizeof(value), sizeof(value)); return value; };
    size_t count = 0; char peerHex[33]; Cursor peers;
    while (nextPeer(peers, peerHex)) {
        uint8_t peer[16]; decodeHex(peerHex, 32, peer, 16);
        // Ordering needs only the latest record, not every message body and
        // unread/status aggregate in every off-screen conversation. Enumerate
        // keys on a cache miss, then validate the newest record through the
        // same revision/mirror-aware loader used by details. Cold summaries
        // still validate all records, including earlier corrupt bodies.
        ConversationSelector selector;
        memcpy(selector.cursor.peer, peer, 16);
        RecordKey latest;
        Error error = Error::None;
        // A cached summary supplies the newest key without opening every
        // directory entry. Still validate that record against all copies and
        // tombstones, so a failed read is never certified as a current page.
        if (const auto* summary = findSummary(peer)) {
            memcpy(latest.peer, peer, 16); latest.counter = summary->counter; latest.incoming = summary->incoming;
        } else {
            Cursor records; beginRecords(records, peer);
            RecordKey key;
            while (nextRecord(records, key)) {
                if (!latest.counter || historyLess({latest.counter, latest.incoming}, {key.counter, key.incoming})) latest = key;
            }
            error = records.error;
        }
        if (error == Error::None && latest.counter) {
            MessageDocument document; StoredRecordHeader header;
            error = load(latest, document, header);
            if (error == Error::None) {
                if (const auto* summary = findSummary(peer))
                    if (summary->revision != header.revision || summary->row.timestamp != header.timestamp)
                        invalidateSummary(peer);
                selector.counter = latest.counter; selector.incoming = latest.incoming;
                selector.cursor.timestamp = header.timestamp;
            }
        }
        if (error != Error::None) invalidateSummary(peer);
        if (error == Error::InvalidRecord) {
            // A permanent corrupt peer remains navigable beside healthy peers.
            // Its unknown visibility cannot be certified as an empty directory.
            selector = {}; memcpy(selector.cursor.peer, peer, 16); selector.error = error;
        } else if (error != Error::None) { result.error = error; result.total = 0; return; }
        else if (!selector.counter) continue;
        if (result.total < UINT32_MAX) ++result.total;
        if (request.hasConversationCursor && !(before ?
            conversationLess(selector.cursor, boundary, request.conversationOrder) :
            conversationLess(boundary, selector.cursor, request.conversationOrder))) continue;
        if (count == limit) {
            result.more = true;
            if (before ? !less(at(0), selector) : !less(selector, at(count - 1))) continue;
            if (before) memmove(bytes, bytes + sizeof(selector), (count - 1) * sizeof(selector));
            --count;
        }
        size_t position = 0;
        while (position < count && less(at(position), selector)) ++position;
        memmove(bytes + (position + 1) * sizeof(selector), bytes + position * sizeof(selector),
                (count - position) * sizeof(selector));
        memcpy(bytes + position * sizeof(selector), &selector, sizeof(selector)); ++count;
    }
    if (peers.error != Error::None) { result.error = peers.error; result.total = 0; return; }
    result.length = uint16_t(count * sizeof(ConversationSelector)); result.outcome = Outcome::Committed;
}

void MessageTransactions::conversation(const Request& request, uint8_t* bytes, size_t capacity, Result& result) {
    if (!request.key.counter || !std::isfinite(request.timestamp) ||
        std::min(capacity, size_t(request.readCapacity)) < sizeof(ConversationView)) {
        result.error = Error::InvalidRecord; return;
    }
    ConversationView row; ConversationSelector latest;
    uint32_t latestRevision = 0;
    result.error = summarize(request.key.peer, row, latest, nullptr, 0, &latestRevision);
    if (result.error != Error::None) return;
    // The normal selector is still the newest record. Its validated preview
    // and revision are already in the summary; don't open and parse it again.
    if (latest.counter == request.key.counter && bool(latest.incoming) == request.key.incoming) {
        if (row.timestamp != request.timestamp) { result.error = Error::Stale; return; }
        memcpy(bytes, &row, sizeof(row)); result.length = sizeof(row);
        result.revision = latestRevision; result.outcome = Outcome::Committed; return;
    }
    MessageDocument document; StoredRecordHeader anchor;
    result.error = load(request.key, document, anchor);
    if (result.error != Error::None) {
        if (result.error == Error::Read) {
            bool retained = false;
            for (unsigned which = 0; which < 2; ++which) {
                auto store = medium(which);
                for (const char* suffix : {"", ".bak"}) {
                    char candidate[128]; path(request.key, which, candidate, suffix);
                    retained |= store.exists(candidate);
                }
            }
            if (!retained) result.error = Error::Stale;
        }
        return;
    }
    if (anchor.timestamp != request.timestamp) { result.error = Error::Stale; return; }
    row.timestamp = anchor.timestamp;
    row.flags = (row.flags & ~ConversationView::LastIncoming) | (anchor.incoming ? ConversationView::LastIncoming : 0);
    const auto content = document.document()["content"].as<JsonString>();
    conversationPreview(row, content.c_str(), content.size());
    memcpy(bytes, &row, sizeof(row)); result.length = sizeof(row);
    result.revision = anchor.revision; result.outcome = Outcome::Committed;
}

void MessageTransactions::history(const Request& request, uint8_t* bytes, size_t capacity, Result& result) {
    if (request.historyDirection != HistoryDirection::Before && request.historyDirection != HistoryDirection::After) {
        result.error = Error::InvalidRecord; return;
    }
    const bool before = request.historyDirection == HistoryDirection::Before;
    capacity = std::min(capacity, size_t(request.readCapacity));
    // Fixed page memory, independent of persisted conversation length. The
    // Complete record selectors keep equal-counter incoming/outgoing records
    // distinct, including at a page boundary after legacy migration.
    HistoryEntry entries[48] = {};
    const HistoryEntry boundary{request.key.counter, request.key.incoming};
    const size_t limit = std::min(size_t(48), capacity / sizeof(HistoryEntry));
    if (!limit) { result.error = Error::InvalidRecord; return; }
    Cursor cursor; beginRecords(cursor, request.key.peer); RecordKey key; size_t count = 0;
    while (nextRecord(cursor, key)) {
        if (result.total < UINT32_MAX) ++result.total;
        const HistoryEntry entry{key.counter, key.incoming};
        if (boundary.counter && !(before ? historyLess(entry, boundary) : historyLess(boundary, entry))) continue;
        if (count == limit) {
            result.more = true;
            // Keep the nearest eligible tuples in either direction, using the
            // same bounded sorted page even if filesystem iteration is unordered.
            if (before ? !historyLess(entries[0], entry) : !historyLess(entry, entries[count - 1])) continue;
            if (before) std::move(entries + 1, entries + count, entries);
            --count;
        }
        const auto at = std::lower_bound(entries, entries + count, entry, historyLess);
        std::move_backward(at, entries + count, entries + count + 1);
        *at = entry; ++count;
    }
    if (cursor.error != Error::None) { result.error = cursor.error; result.total = 0; return; }
    result.length = uint16_t(count * sizeof(HistoryEntry));
    const auto continuation = count ? entries[before ? 0 : count - 1] : HistoryEntry{};
    result.key.counter = continuation.counter;
    result.key.incoming = continuation.incoming;
    if (result.length) memcpy(bytes, entries, result.length);
    result.outcome = Outcome::Committed;
}

void MessageTransactions::pending(const Request& request, uint8_t* bytes, size_t capacity, Result& result) {
    capacity = std::min(capacity, size_t(request.readCapacity));
    if (capacity < sizeof(StoredRecordHeader)) { result.error = Error::InvalidRecord; return; }
    if (request.key.incoming) { result.error = Error::InvalidRecord; return; }
    const auto less = [](const RecordKey& a, const RecordKey& b) {
        return a.counter < b.counter || (a.counter == b.counter && memcmp(a.peer, b.peer, 16) < 0);
    };
    MessageDocument document; StoredRecordHeader selected; bool found = false;
    result.key = {};
    char afterPeer[33] = {}, peerHex[33]; Error scanError = Error::None;
    while (nextPeer(afterPeer, peerHex, scanError)) {
        memcpy(afterPeer, peerHex, 33); uint8_t peer[16]; decodeHex(peerHex, 32, peer, 16);
        Cursor cursor; beginRecords(cursor, peer, &document); RecordKey key;
        while (nextRecord(cursor, key)) {
            if (key.incoming || (request.key.counter && !less(request.key, key))) continue;
            StoredRecordHeader header; const auto error = load(key, document, header);
            if (error != Error::None) { result.error = error; return; }
            if (!messaging::pendingStatus(header.status)) continue;
            if (found) result.more = true;
            if (found && !less(key, result.key)) continue;
            found = true; selected = header; result.key = key;
        }
        if (cursor.error != Error::None) { result.error = cursor.error; return; }
    }
    if (scanError != Error::None) { result.error = scanError; return; }
    if (found) {
        memcpy(bytes, &selected, sizeof(selected)); result.length = sizeof(selected);
        result.revision = selected.revision;
    }
    result.outcome = Outcome::Committed;
}

void MessageTransactions::prepared(const Request& request, uint8_t* bytes, size_t length,
                                   size_t capacity, Result& result) {
    namespace prep = handheld::storage::prepared;
    capacity = std::min(capacity, size_t(request.readCapacity));
    const bool writing = request.operation == Operation::WritePrepared;
    if (!bytes || capacity < prep::Max || !request.key.counter || request.key.incoming ||
        (writing && (!request.hasMessageId || !prep::valid(bytes, length, request.key,
                                                         request.source, request.messageId)))) {
        result.error = Error::InvalidRecord; return;
    }
    auto flash = medium(0);
    if (!flash.isReady()) { result.error = Error::Unavailable; return; }
    MessageDocument document; StoredRecordHeader header;
    result.error = load(request.key, document, header);
    if (result.error != Error::None) return;
    if (header.incoming || memcmp(header.source, request.source, 16) ||
        memcmp(header.destination, request.key.peer, 16) ||
        (header.status != 7 && header.status != 8) ||
        (writing && header.hasMessageId && memcmp(header.messageId, request.messageId, 32))) {
        result.error = Error::Stale; return;
    }
    result.revision = header.revision;
    char primary[128], backup[128];
    path(request.key, 0, primary, ".prop"); path(request.key, 0, backup, ".prop.bak");
    auto markReady = [&]() {
        if (header.prepared) return true;
        // The binding write may have promoted a newer backup. Reload before a
        // second commit so its medium-selection flags cannot describe the old
        // primary/backup topology.
        result.error = load(request.key, document, header);
        if (result.error != Error::None) return false;
        if (!header.hasMessageId || memcmp(header.messageId, bytes + 40, 32) ||
            memcmp(header.source, request.source, 16) || (header.status != 7 && header.status != 8)) {
            result.error = Error::Stale; return false;
        }
        if (header.prepared) return true;
        if (header.revision == UINT32_MAX) { result.error = Error::RevisionExhausted; return false; }
        document.document()["prop_prepared"] = true;
        document.document()["store_revision"] = header.revision + 1;
        result.error = commit(request.key, document, false, result);
        result.sd = {};
        if (result.error != Error::None || !result.flash.committed) {
            if (result.error == Error::None) result.error = Error::Write;
            return false;
        }
        result.revision = ++header.revision; header.prepared = true;
        return true;
    };
    // Never replace an existing encrypted entry, even when a writer retried
    // after losing its completion. A corrupt primary cannot roll back to a
    // different backup or silently regenerate ciphertext.
    const char* existing = flash.exists(primary) ? primary : flash.exists(backup) ? backup : nullptr;
    if (existing) {
        if (!retainedCopy(request.key, 0, document)) {
            result.error = commit(request.key, document, false, result);
            result.sd = {};
            if (result.error != Error::None || !result.flash.committed) {
                if (result.error == Error::None) result.error = Error::Write;
                return;
            }
        }
        File file = flash.open(existing);
        if (!file) { result.error = Error::Read; return; }
        const size_t size = file.size();
        if (size > capacity || size < prep::Header + 112) { result.error = Error::InvalidRecord; return; }
        size_t received = 0;
        while (received < size) {
            const size_t part = file.read(bytes + received, size - received);
            if (!part || part > size - received) { result.error = Error::Read; return; }
            received += part;
        }
        if (!header.hasMessageId || !prep::valid(bytes, size, request.key, request.source, header.messageId)) {
            result.error = Error::InvalidRecord; return;
        }
        const bool alreadyReady = header.prepared;
        if (!markReady()) return;
        result.length = uint16_t(size); result.duplicate = alreadyReady;
        result.outcome = Outcome::Committed; return;
    }
    if (header.prepared) { result.error = Error::InvalidRecord; return; }
    if (!writing) { result.duplicate = true; result.outcome = Outcome::Committed; return; }
    if (!header.hasMessageId) {
        if (header.revision == UINT32_MAX) { result.error = Error::RevisionExhausted; return; }
        char id[65]; encodeHex(request.messageId, 32, id);
        document.document()["msgid"] = JsonString(id, size_t(64), false);
        document.document()["store_revision"] = header.revision + 1;
        if (document.document().overflowed()) { result.error = Error::Allocation; return; }
        ++header.revision;
    }
    // The record binding must be durable on the same authoritative medium
    // before an envelope can authorize any transmission.
    result.error = commit(request.key, document, false, result);
    result.sd = {}; // This operation's envelope is flash-authoritative.
    if (result.error != Error::None || !result.flash.committed) {
        if (result.error == Error::None) result.error = Error::Write;
        return;
    }
    result.revision = header.revision;
    const MemorySource source(bytes, length);
    result.flash.committed = false;
    result.flash.error = result.error = flash.write(primary, source);
    if (result.error != Error::None) return;
    result.flash.committed = true;
    if (!markReady()) return;
    result.length = uint16_t(length); result.outcome = Outcome::Committed;
}

void MessageTransactions::trim(const Request& request, Result& result) {
    if (deletedThrough(request.key.peer) == UINT32_MAX) { result.error = Error::Stale; return; }
    MessageDocument document;
    // Recover only known atomic artifacts. A valid backup newer than its
    // primary, or the only valid copy, must remain available to load().
    for (unsigned which = 0; which < 2; ++which) {
        auto store = medium(which); char parent[96]; directory(request.key.peer, which, parent);
        File dir = store.open(parent);
        if (!dir || !dir.isDirectory()) continue;
        for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
            if (entry.isDirectory()) continue;
            const char* name = entry.name(); const size_t length = strnlen(name, 31);
            uint32_t counter = 0; bool incoming = false;
            if ((length == 26 || length == 30) && memcmp(name + 20, ".audio", 6) == 0) {
                char canonical[21]; memcpy(canonical, name, 20); canonical[20] = 0;
                if (!filename(canonical, counter, incoming) ||
                    (length == 30 && memcmp(name + 26, ".bak", 4) && memcmp(name + 26, ".tmp", 4))) continue;
                RecordKey key; memcpy(key.peer, request.key.peer, 16); key.counter = counter; key.incoming = incoming;
                char artifact[128]; snprintf(artifact, sizeof(artifact), "%s/%s", parent, name);
                const bool temporary = length == 30 && memcmp(name + 26, ".tmp", 4) == 0;
                entry.close();
                // An unavailable configured mirror may own the only reference.
                bool referenced = !medium(0).isReady() || (_external && !medium(1).isReady());
                for (unsigned copy = 0; copy < 2 && !referenced; ++copy) {
                    for (const char* suffix : {"", ".bak"}) {
                        char record[128]; path(key, copy, record, suffix);
                        referenced |= medium(copy).exists(record);
                    }
                }
                if (temporary || !referenced) store.remove(artifact);
                continue;
            }
            if (length == 24 && memcmp(name + 20, ".tmp", 4) == 0) {
                char canonical[21]; memcpy(canonical, name, 20); canonical[20] = 0;
                if (filename(canonical, counter, incoming)) {
                    char artifact[128]; snprintf(artifact, sizeof(artifact), "%s/%s", parent, name);
                    entry.close(); store.remove(artifact);
                }
                continue;
            }
            if (length != 24 || !filename(name, counter, incoming)) continue;
            RecordKey key; memcpy(key.peer, request.key.peer, 16); key.counter = counter; key.incoming = incoming;
            char primary[128], backup[128]; path(key, which, primary); path(key, which, backup, ".bak");
            entry.close();
            if (!store.exists(primary)) { store.rename(backup, primary); continue; }
            StoredRecordHeader primaryHeader, backupHeader;
            if (inspect(key, which, "", document, primaryHeader) == Error::None &&
                inspect(key, which, ".bak", document, backupHeader) == Error::None &&
                primaryHeader.revision >= backupHeader.revision) store.remove(backup);
        }
    }
    // Space is bounded while persistent history is not. Each pass selects at
    // most eight oldest removable keys; queued/sending/unconfirmed bodies stay
    // pinned. A flash cache eviction requires an exact verified SD mirror.
    for (int target = 1; target >= 0; --target) {
        auto store = medium(unsigned(target));
        if (!store.isReady()) continue;
        const size_t limit = target == 0 && _external && _sd && _sd->isReady() ?
            FLASH_MSG_CACHE_LIMIT : RSDECK_MAX_MESSAGES_PER_CONV;
        HistoryEntry after;
        for (;;) {
            RecordKey oldest[8]; size_t selected = 0, count = 0;
            Cursor cursor; beginRecords(cursor, request.key.peer, &document); RecordKey key;
            while (nextRecord(cursor, key)) {
                char primary[128], backup[128]; path(key, unsigned(target), primary); path(key, unsigned(target), backup, ".bak");
                if (!store.exists(primary) && !store.exists(backup)) continue;
                ++count;
                const HistoryEntry entry{key.counter, key.incoming};
                if (after.counter && !historyLess(after, entry)) continue;
                StoredRecordHeader header; const auto error = load(key, document, header);
                if (error != Error::None) { result.error = error; continue; }
                if (!header.incoming && header.status != 4 && header.status != 9 && !messaging::failedStatus(header.status)) continue;
                if (memoRetains(key, header, document.document()["memo_draft"] | uint32_t(0))) continue;
                size_t at = 0;
                while (at < selected && historyLess({oldest[at].counter, oldest[at].incoming}, entry)) ++at;
                if (at == 8) continue;
                if (selected < 8) ++selected;
                for (size_t move = selected - 1; move > at; --move) oldest[move] = oldest[move - 1];
                oldest[at] = key;
            }
            if (cursor.error != Error::None) { result.error = cursor.error; return; }
            if (count <= limit || !selected) break;
            const size_t needed = std::min(selected, count - limit); bool progress = false;
            for (size_t index = 0; index < needed; ++index) {
                key = oldest[index]; StoredRecordHeader header;
                if (load(key, document, header) != Error::None) continue;
                if (target == 0 && _external && _sd && _sd->isReady() && !retainedCopy(key, 1, document)) {
                    // Repair a missing/stale optional mirror from the committed
                    // chosen record before dropping the cache's retained copy.
                    if (_blockedMedia & 2) continue;
                    char parent[96], candidate[128]; directory(key.peer, 1, parent); path(key, 1, candidate);
                    auto mirror = medium(1); JsonSource source(document);
                    if (source.error() != Error::None) { result.error = source.error(); continue; }
                    if (!mirror.ensureDir(parent) || ensureAudio(key, 1, header, nullptr) != Error::None ||
                        mirror.write(candidate, source) != Error::None ||
                        !retainedCopy(key, 1, document)) continue;
                }
                char primary[128], backup[128]; path(key, unsigned(target), primary); path(key, unsigned(target), backup, ".bak");
                if (target == 0) {
                    bool removed = true;
                    for (const char* suffix : {".prop", ".prop.bak", ".prop.tmp"}) {
                        char preparedPath[128]; path(key, 0, preparedPath, suffix);
                        if (store.exists(preparedPath) && !store.remove(preparedPath)) removed = false;
                    }
                    if (!removed) { result.error = Error::Write; continue; }
                }
                const bool primaryRemoved = !store.exists(primary) || store.remove(primary);
                const bool backupRemoved = !store.exists(backup) || store.remove(backup);
                if (primaryRemoved && backupRemoved && !removeAudio(key, unsigned(target))) result.error = Error::Write;
                if (primaryRemoved && backupRemoved) { progress = true; ++result.total; result.outcome = Outcome::Committed; }
                else result.error = Error::Write;
                if (target == 1 && primaryRemoved && backupRemoved) {
                    // Retention expiry removes an old completed logical record,
                    // including its flash cache copy. Pending records were pinned.
                    auto flash = medium(0); path(key, 0, primary); path(key, 0, backup, ".bak");
                    const bool removedPrimary = !flash.exists(primary) || flash.remove(primary);
                    const bool removedBackup = !flash.exists(backup) || flash.remove(backup);
                    if (!removedPrimary || !removedBackup) { result.error = Error::Write; continue; }
                    if (!removeAudio(key, 0)) result.error = Error::Write;
                }
                const bool cacheOnly = target == 0 && _external && _sd && _sd->isReady();
                if (primaryRemoved && backupRemoved && !cacheOnly) {
                    if (result.countDelta > INT32_MIN) --result.countDelta;
                    if (header.incoming && !header.read && result.unreadDelta > INT32_MIN) --result.unreadDelta;
                }
            }
            if (progress) after = {};
            else {
                // Unavailable mirrors pin these keys, but later verified keys
                // may still be evicted. Advance monotonically instead of
                // retrying the same failed oldest batch forever.
                after = {oldest[needed - 1].counter, oldest[needed - 1].incoming};
            }
        }
    }
    if (result.error == Error::None || result.total) result.outcome = Outcome::Committed;
}

void MessageTransactions::purgeJournal(const Request& request, uint8_t* bytes, size_t length,
                                       size_t capacity, Result& result) {
    const bool loading = request.operation == Operation::LoadPurge;
    const bool clearing = request.operation == Operation::ClearPurge;
    purge::Journal supplied;
    if (!bytes || std::min(capacity, size_t(request.readCapacity)) < purge::Size ||
        (!loading && length != purge::Size)) { result.error = Error::InvalidRecord; return; }
    if (!loading) {
        memcpy(supplied.bytes, bytes, purge::Size);
        if (!supplied.valid() || memcmp(supplied.local(), request.destination, 16) ||
            !request.key.incoming || supplied.key().counter != request.key.counter ||
            memcmp(supplied.key().peer, request.key.peer, 16)) {
            result.error = Error::InvalidRecord; return;
        }
    }
    auto flash = medium(0);
    if (!flash.isReady()) { result.error = Error::Unavailable; return; }
    char identity[33], primary[96], backup[100], temporary[100];
    encodeHex(request.destination, 16, identity);
    snprintf(primary, sizeof(primary), "/transport/propagation/%s.purge", identity);
    snprintf(backup, sizeof(backup), "%s.bak", primary);
    snprintf(temporary, sizeof(temporary), "%s.tmp", primary);
    purge::Journal journal;
    const char* existing = flash.exists(primary) ? primary : flash.exists(backup) ? backup : nullptr;
    if (existing) {
        File file = flash.open(existing);
        if (!file) { result.error = Error::Read; return; }
        if (file.size() != purge::Size) { result.error = Error::InvalidRecord; return; }
        size_t received = 0;
        while (received < purge::Size) {
            const size_t part = file.read(journal.bytes + received, purge::Size - received);
            if (!part || part > purge::Size - received) { result.error = Error::Read; return; }
            received += part;
        }
        file.close();
        if (!journal.valid() || memcmp(journal.local(), request.destination, 16)) {
            result.error = Error::InvalidRecord; return;
        }
        // A lost completion may replay the same journal. It cannot replace an
        // outstanding item or clear a later item after the owner has moved on.
        if (!loading && memcmp(supplied.bytes, journal.bytes, purge::Size)) {
            result.error = Error::Stale; return;
        }
    } else if (loading || clearing) {
        result.outcome = Outcome::Committed; result.duplicate = true; return;
    } else journal = supplied;

    if (clearing) {
        // Delete possible resurrecting copies before the selected primary.
        // A partial cleanup can only leave the same item for an idempotent retry.
        result.flash.attempted = true;
        if ((flash.exists(temporary) && !flash.remove(temporary)) ||
            (flash.exists(backup) && !flash.remove(backup)) ||
            (flash.exists(primary) && !flash.remove(primary))) {
            result.flash.error = result.error = Error::Write; return;
        }
        result.flash.committed = true; result.outcome = Outcome::Committed; return;
    }
    // Return a structurally valid journal even when its local message is gone;
    // callers may abandon it locally, but a failed result never permits purge.
    memcpy(bytes, journal.bytes, purge::Size); result.length = purge::Size;
    result.key = journal.key();
    MessageDocument document; StoredRecordHeader header;
    result.error = load(result.key, document, header);
    if (result.error != Error::None) return;
    if (!header.incoming || !header.hasMessageId || header.revision < journal.revision() ||
        memcmp(header.source, result.key.peer, 16) || memcmp(header.destination, journal.local(), 16) ||
        memcmp(header.messageId, journal.messageId(), 32)) { result.error = Error::Stale; return; }
    // Purge permission survives SD removal: require the matching message on
    // authoritative flash before publishing or replaying the flash journal.
    result.error = commit(result.key, document, false, result);
    result.sd = {};
    if (result.error != Error::None || !result.flash.committed) {
        if (result.error == Error::None) result.error = Error::Write;
        return;
    }
    result.revision = header.revision;
    if (!existing) {
        result.flash.committed = false;
        if (!flash.ensureDir("/transport/propagation")) {
            result.flash.error = result.error = Error::Write; return;
        }
        const MemorySource source(bytes, purge::Size);
        result.flash.error = result.error = flash.write(primary, source);
        if (result.error != Error::None) return;
        result.flash.committed = true;
    } else result.duplicate = true;
    result.outcome = Outcome::Committed;
}

#include "RrcTransactions.inc"
#include "MemoTransactions.inc"
#include "AudioRetention.inc"

void MessageTransactions::execute(const Request& request, uint8_t* bytes, size_t length, size_t capacity, Result& result) {
    if (_deferred && !bindWorker()) {
        result.key = request.key; result.error = Error::Unavailable; return;
    }
    StorageLease lease;
    result.key = request.key;
    if (!lease.held()) { result.error = Error::Unavailable; return; }
    if (rrc::operation(request.operation)) {
        rrcTransaction(request, bytes, length, capacity, result); return;
    }
    if (memo::operation(request.operation) && request.operation != Operation::MemoPromote) {
        memoTransaction(request, bytes, length, capacity, result); return;
    }
    checkSummaryMedia();
    // An unchanged, fully read conversation needs no directory scan or cache
    // invalidation. Every real mutation already invalidates its peer summary.
    if (request.operation == Operation::MarkRead) {
        if (const auto* summary = findSummary(request.key.peer)) if (!summary->row.unreadCount) {
            result.outcome = Outcome::Committed; return;
        }
    }
    if (request.operation != Operation::ReadRecord && request.operation != Operation::ReadAudio && request.operation != Operation::ReadPending &&
        request.operation != Operation::ReadHistoryPage && request.operation != Operation::ReadConversationPage &&
        request.operation != Operation::ReadConversation && request.operation != Operation::LoadPurge &&
        request.operation != Operation::WritePurge && request.operation != Operation::ClearPurge) {
        if (_mutationEpoch == UINT64_MAX) { result.error = Error::RevisionExhausted; return; }
        ++_mutationEpoch;
        // Invalidate before attempting a write: partial mirrors, failed cleanup
        // and exceptions can all change persistent state even on failure.
        if(request.operation!=Operation::RetainAudio) invalidateSummary(request.key.peer);
    }
    switch (request.operation) {
    case Operation::MemoPromote: memoTransaction(request, bytes, length, capacity, result); break;
    case Operation::CreateIncoming: case Operation::CreateOutgoing:
        if (request.audio.state==3 || !audio::validBody(request.titleLength, request.contentLength, request.audio) || request.audio.checksum ||
            length != size_t(request.titleLength) + request.contentLength + request.audio.length ||
            !messaging::validDelivery(request.status, request.deliveryPolicy, request.key.incoming) ||
            !std::isfinite(request.timestamp) ||
            request.key.incoming != (request.operation == Operation::CreateIncoming) ||
            memcmp(request.key.peer, request.key.incoming ? request.source : request.destination, 16)) {
            result.error = Error::InvalidRecord; return;
        }
        create(request, bytes, result); break;
    case Operation::UpdateStatus: case Operation::RetryOutgoing:
        if (request.status > messaging::LastStatus) { result.error = Error::InvalidRecord; return; }
        update(request, result); break;
    case Operation::MarkRead: markRead(request, result); break;
    case Operation::DeleteConversation: erase(request, result); break;
    case Operation::DeleteRecord: eraseRecord(request,result);break;
    case Operation::ReadRecord: read(request, bytes, capacity, result); break;
    case Operation::ReadAudio: readAudio(request, bytes, capacity, result); break;
    case Operation::ReadPending: pending(request, bytes, capacity, result); break;
    case Operation::ReadHistoryPage: history(request, bytes, capacity, result); break;
    case Operation::ReadConversationPage: conversationPage(request, bytes, capacity, result); break;
    case Operation::ReadConversation: conversation(request, bytes, capacity, result); break;
    case Operation::Trim: trim(request, result); break;
    case Operation::RetainAudio: retainAudio(request,result);break;
    case Operation::LoadPrepared: case Operation::WritePrepared:
        prepared(request, bytes, length, capacity, result); break;
    case Operation::LoadPurge: case Operation::WritePurge: case Operation::ClearPurge:
        purgeJournal(request, bytes, length, capacity, result); break;
    default: result.error = Error::InvalidRecord;
    }
}

} // namespace handheld::storage
