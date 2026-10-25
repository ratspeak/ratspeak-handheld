#pragma once

#include "config/Config.h"
#include "util/Bytes.h"
#include "storage/MirrorWriteState.h"
#include <dirent.h>
#include <functional>
#include <vector>
#include <string>
#include <string_view>
#include <map>

class SDStore;
class FlashStore;
class LoRaInterface;

struct DiscoveredNode {
    rs::Bytes hash;
    std::string name;
    std::string identityHex;
    int rssi = 0;
    float snr = 0;
    uint8_t hops = 0;
    unsigned long lastSeen = 0;
    bool saved = false;
};

class AnnounceManager {
public:
    AnnounceManager(const char* aspectFilter = nullptr);
    virtual ~AnnounceManager();

    // Announce bridge (D13, sole ingest path since micro retirement 2026-08-13):
    // the FFI validates signature + binding; this applies the contact/name-cache/
    // rate-limit policy from raw event fields. KeyMap continuity is enforced by
    // ProtocolRuntime before this is called.
    void receivedAnnounceEvent(const uint8_t destHash[16], const uint8_t identityHash[16],
                               const uint8_t* appData, size_t appLen, int rssi, float snr,
                               uint8_t hops);

    void setStorage(SDStore* sd, FlashStore* flash);
    void setLoRaInterface(LoRaInterface* li) { _loraIf = li; }
    void setLocalDestHash(const rs::Bytes& hash) { _localDestHash = hash; }
    bool saveContacts();
    bool flushPending();
    void loadContacts();
    bool deleteContact(int nodeIdx);
    // Hash-addressed contact ops: safe across node-table compaction (UI must not hold indices)
    bool deleteContactByHex(const std::string& hexHash);
    bool setContactName(const std::string& hexHash, const std::string& name);
    void loop();  // Call from main loop — handles deferred saves

    // Name cache: persists hash→name mappings so names survive reboots
    std::string lookupName(const std::string& hexHash) const;
    bool saveNameCache();
    void loadNameCache();

    const std::vector<DiscoveredNode>& nodes() const { return _nodes; }
    int nodeCount() const { return _nodes.size(); }
    int nodesOnlineSince(unsigned long maxAgeMs) const;
    const DiscoveredNode* findNode(const rs::Bytes& hash) const;
    const DiscoveredNode* findNodeByHex(const std::string& hexHash) const;
    bool addManualContact(const std::string& hexHash, const std::string& name);
    // Save/unsave a discovered node as contact by hex hash (ui/lite callbacks)
    bool saveNode(const std::string& hexHash);
    void unsaveNode(const std::string& hexHash);
    void evictStale(unsigned long maxAgeMs = 3600000);
    void clearTransientNodes();
    void clearAll();

private:
    bool saveContact(const DiscoveredNode& node);
    bool removeContact(const std::string& hexHash);
    bool commitContact(const std::string& hexHash, const String& json);
    void flushContactMirrors();
    void closeContactMirrorDirectory();
    bool mirrorContact(const char* filename);
    bool cacheName(std::string_view hash, std::string_view name);
    int findNodeIndex(const uint8_t* hash, size_t length) const;
    int findNodeIndexHex(const std::string& hash) const;

    std::vector<DiscoveredNode> _nodes;
    SDStore* _sd = nullptr;
    FlashStore* _flash = nullptr;
    LoRaInterface* _loraIf = nullptr;
    rs::Bytes _localDestHash;
    bool _contactsDirty = false;
    // The canonical directory is the retry ledger, including contacts outside
    // the in-memory node limit and deletion markers. No filename backlog.
    DIR* _contactMirrorDirectory = nullptr;
    bool _contactMirrorsPending = false;
    bool _contactMirrorRetry = false;
    bool _contactMirrorWaiting = false;
    unsigned long _lastContactMirrorAttempt = 0;
    MirrorWriteState _nameCacheWrites;
    bool _nameCachePersistenceBlocked = false;
    unsigned long _lastContactSave = 0;
    unsigned long _lastNameCacheSave = 0;
    // Transparent comparison: lookups by a stack hex view allocate nothing.
    std::map<std::string, std::string, std::less<>> _nameCache;  // hexHash → displayName
    unsigned long _globalAnnounceWindowStart = 0;
    unsigned int _globalAnnounceCount = 0;
    static constexpr unsigned int MAX_GLOBAL_ANNOUNCES_PER_SEC = ANNOUNCE_MAX_GLOBAL_PER_SEC;
    static constexpr int MAX_NODES = ANNOUNCE_MAX_NODES;
    static constexpr int MAX_NAME_CACHE = ANNOUNCE_MAX_NAME_CACHE;
    static constexpr unsigned long CONTACT_SAVE_INTERVAL_MS = 30000;
    static constexpr unsigned long ANNOUNCE_MIN_INTERVAL_MS = 200;  // Rate-limit announce processing

};
