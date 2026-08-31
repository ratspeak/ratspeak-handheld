#pragma once

#include <Arduino.h>
#include <vector>
#include <string>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"

struct IdentitySlot {
    std::string hash;       // hex hash (first 32 chars)
    String displayName;     // per-identity display name
    String keyPath;         // storage path to .key file
    bool active;            // currently loaded identity
};

class IdentityManager {
public:
    bool begin(FlashStore* flash, SDStore* sd = nullptr);

    // List all stored identities
    const std::vector<IdentitySlot>& identities() const { return _slots; }
    int count() const { return (int)_slots.size(); }

    // Current active identity index (-1 if none)
    int activeIndex() const { return _activeIdx; }

    // Import identity from SD card, returns index
    int importIdentity(const String& displayName = "");

    // Create a new identity, returns index
    int createIdentity(const String& displayName = "");

    // Delete identity at index (cannot delete last one)
    bool deleteIdentity(int index);

    // Switch to identity at index (validates the key, mirrors it to
    // PATH_IDENTITY). Caller reboots/reinitializes the backend after switching.
    bool switchTo(int index);

    // Save display name for identity at index
    bool flushPending();
    bool setDisplayName(int index, const String& name);

    // Get the display name for identity at index
    String getDisplayName(int index) const;

    // Sync active identity's display name from/to config
    // Call on boot: loads active slot's name into outName
    // Returns true if outName was set
    bool syncNameFromActive(String& outName) const;

    // Refresh slot list from storage
    void refresh();

    // Raw-key access: the FFI consumes the raw 64-byte private key
    // (X25519 priv 32 || Ed25519 seed 32) — same bytes the .key slot files
    // already store.
    bool readActiveIdentityKey(uint8_t out[64]) const;
    // NVS is the final backup tier. ProtocolRuntime validates before adopting a
    // recovered key; these methods only perform platform storage I/O.
    bool readNvsIdentityKey(uint8_t out[64]) const;
    bool hasIdentityData() const;
    bool mirrorActiveIdentityToNvs();
    bool mirrorIdentityToNvs(const uint8_t key[64]) const;
    // Adopt an FFI-created identity as a new active slot (fresh boot with no
    // identity anywhere). Writes the slot .key file + PATH_IDENTITY copy +
    // slot meta, mirroring createIdentity+switchTo. Returns slot index or -1.
    int createIdentityFromRaw(const uint8_t key[64], const char* hashHex);

private:
    void loadSlotMeta();
    bool saveSlotMeta();
    bool _metadataDirty = false;
    bool _metadataReadable = true;
    mutable bool _nvsMirrorPending = false;
    String slotKeyPath(int slotNum) const;
    int nextFreeSlotNum() const;
    String importIdentityPath() const;
    bool adoptPathIdentityWhenNoActive();

    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    std::vector<IdentitySlot> _slots;
    int _activeIdx = -1;

    static constexpr int MAX_IDENTITIES = 8;
    static constexpr const char* META_PATH = "/identity/slots.json";
    static constexpr const char* ID_DIR = "/identity";
};
