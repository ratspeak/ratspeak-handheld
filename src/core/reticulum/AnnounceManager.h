#pragma once

#include "config/Config.h"
#include "util/Bytes.h"
#include "storage/MirrorWriteState.h"
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <set>

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
    virtual ~AnnounceManager() = default;

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
    void rebuildIndex();

private:
    bool saveContact(const DiscoveredNode& node);
    bool removeContact(const std::string& hexHash);
    bool commitContact(const std::string& hexHash, const String& json);
    void flushContactMirrors();

    std::vector<DiscoveredNode> _nodes;
    SDStore* _sd = nullptr;
    FlashStore* _flash = nullptr;
    LoRaInterface* _loraIf = nullptr;
    rs::Bytes _localDestHash;
    bool _contactsDirty = false;
    std::set<std::string> _contactMirrorsPending;
    MirrorWriteState _nameCacheWrites;
    unsigned long _lastContactSave = 0;
    unsigned long _lastNameCacheSave = 0;
    std::map<std::string, std::string> _nameCache;  // hexHash → displayName
    unsigned long _globalAnnounceWindowStart = 0;
    unsigned int _globalAnnounceCount = 0;
    static constexpr unsigned int MAX_GLOBAL_ANNOUNCES_PER_SEC = ANNOUNCE_MAX_GLOBAL_PER_SEC;
    static constexpr int MAX_NODES = ANNOUNCE_MAX_NODES;
    static constexpr int MAX_NAME_CACHE = ANNOUNCE_MAX_NAME_CACHE;
    static constexpr unsigned long CONTACT_SAVE_INTERVAL_MS = 30000;
    static constexpr unsigned long ANNOUNCE_MIN_INTERVAL_MS = 200;  // Rate-limit announce processing

    std::unordered_map<std::string, int> _hashIndex;  // raw hash bytes → _nodes index

    static std::string makeKey(const rs::Bytes& hash) {
        return std::string((const char*)hash.data(), hash.size());
    }
};
