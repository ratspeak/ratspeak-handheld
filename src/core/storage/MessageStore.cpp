#include "MessageStore.h"
#include "MessageTransactions.h"
#include "MessageMigration.h"
#include "runtime/TaskOwner.h"
#include "config/Config.h"
#include "util/PerfTrace.h"
#include <ArduinoJson.h>
#include <Preferences.h>

using namespace handheld::storage;

bool MessageStore::begin(FlashStore* flash, SDStore* sd, bool externalStorageEnabled) {
    handheld::assertDeviceOwner();
    if (_writeQueue.accepting() || !_writeQueue.stopped() || _writeQueue.drainCount()) return false;
    _flash = flash; _sd = sd; _externalStorageEnabled = externalStorageEnabled;
    if (!_flash || !_flash->isReady()) return false;
    if (!handheld::storage::StorageLease::initialize()) return false;
    handheld::storage::StorageLease lease;
    if (!lease.held() || !_flash->ensureDir(PATH_MESSAGES)) return false;
    if (_externalStorageEnabled && _sd && _sd->isReady()) {
        _sd->ensureDir(SD_PATH_ROOT); _sd->ensureDir(SD_PATH_MESSAGES);
    }
    try {
        MessageMigration migration(_flash, _sd, _externalStorageEnabled);
        if (!migration.run()) {
            Serial.println("[MSGSTORE] Initialization deferred: legacy storage needs recovery");
            return false;
        }
#if LEGACY_READ_CTR_MIGRATION
        // Boot owns the executor until migration has settled. Deferred worker
        // registration begins only at the ordinary initialization below.
        if (!transactions().begin(_flash, _sd, _externalStorageEnabled, false) ||
            !migration.foldReadMarkers(transactions())) {
            Serial.println("[MSGSTORE] Initialization deferred: legacy read state needs recovery");
            return false;
        }
#endif
    } catch (const std::bad_alloc&) {
        Serial.println("[MSGSTORE] Initialization deferred: migration memory unavailable");
        return false;
    }
#if LEGACY_MSG_FILENAME_MIGRATION
    // Retain the donor marker for compatibility, but never use a global flag
    // to skip an absent/reinserted medium or an interrupted migration.
    Preferences marker;
    if (marker.begin(NVS_NS_MSG, false)) {
        marker.putBool("fs_migrated", true);
        marker.end();
    }
#endif
    if (!transactions().begin(_flash, _sd, _externalStorageEnabled, STORAGE_DEFERRED_IO != 0)) return false;
    if (!transactions().expireTemporaryMemos()) {
        Serial.println("[MSGSTORE] Temporary voice cleanup pending; retry initialization");return false;
    }
    try {
        if (!loadStartupMetadata()) {
            Serial.println("[MSGSTORE] Initialization deferred: incomplete startup message metadata"); return false;
        }
    } catch (const std::bad_alloc&) {
        releaseStartupSeeds();
        Serial.println("[MSGSTORE] Initialization deferred: insufficient memory"); return false;
    }
    return _writeQueue.begin(transactions(), STORAGE_DEFERRED_IO ? WriteQueue::Execution::Deferred : WriteQueue::Execution::Immediate);
}

int MessageStore::messageCount(const std::string& peer) const {
    handheld::assertDeviceOwner();
    if (_writeQueue.drainCount()) return 0;
    StorageLease lease; uint8_t peerBytes[16];
    if (!lease.held() || !decodeHex(peer.data(), peer.size(), peerBytes, 16)) return 0;
    MessageTransactions::Cursor cursor; transactions().beginRecords(cursor, peerBytes);
    RecordKey key; int count = 0;
    while (transactions().nextRecord(cursor, key)) if (count < INT_MAX) ++count;
    return cursor.error == Error::None ? count : 0;
}

bool MessageStore::updateMessageStatus(const std::string& peer, double timestamp, bool incoming, LXMFStatus status) {
    handheld::assertDeviceOwner();
#if STORAGE_DEFERRED_IO
    (void)peer; (void)timestamp; (void)incoming; (void)status;
    Serial.println("[MSGSTORE] Deferred status requires a stable record ticket"); return false;
#else
    if (_writeQueue.drainCount()) return false;
    uint32_t selected = 0;
    {
        StorageLease lease; uint8_t peerBytes[16];
        if (!lease.held() || !decodeHex(peer.data(), peer.size(), peerBytes, 16)) return false;
        MessageTransactions::Cursor cursor; transactions().beginRecords(cursor, peerBytes);
        MessageDocument document; StoredRecordHeader header; RecordKey key;
        while (transactions().nextRecord(cursor, key)) {
            if (key.incoming != incoming || key.counter <= selected) continue;
            if (transactions().load(key, document, header) == Error::None && header.timestamp == timestamp) selected = key.counter;
        }
        if (cursor.error != Error::None) return false;
    }
    return selected && updateMessageStatusByCounter(peer, selected, incoming, status);
#endif
}

std::vector<std::string> MessageStore::startupRecentMessageIds(size_t maxIds) const {
    handheld::assertDeviceOwner();
    if (maxIds == 0 || _startupRecentMessageIds.size() <= maxIds) {
        return _startupRecentMessageIds;
    }
    return std::vector<std::string>(
        _startupRecentMessageIds.end() - maxIds,
        _startupRecentMessageIds.end());
}

int MessageStore::totalUnreadCount() const {
    handheld::assertDeviceOwner(); return _totalUnread;
}

void MessageStore::bumpRevision() {
    handheld::assertDeviceOwner();
    _revision++;
    if (_revision == 0) _revision = 1;
}
