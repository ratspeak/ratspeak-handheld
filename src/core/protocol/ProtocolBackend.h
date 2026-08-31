#pragma once

// Narrow protocol facade shared by the device frontends. Conversation reads
// remain store-backed; protocol sends, status, and lifecycle pass through here.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include "reticulum/LXMFManager.h"

class ProtocolBackend {
public:
    enum class AnnounceResult : uint8_t { Sent, Deferred, Failed };

    virtual ~ProtocolBackend() = default;

    // Implementation identity for diagnostics.
    virtual const char* backendName() const = 0;

    // Lifecycle / tick.
    virtual void loop() = 0;
    virtual bool persistData() = 0;

    // Local endpoint identity + lxmf.delivery destination hashes.
    // identityHash() is the shortened display label, never a wire identifier.
    virtual String identityHash() const = 0;
    virtual String identityHashHex() const = 0; // Full 16-byte identity as 32 hex chars.
    virtual String destinationHashHex() const = 0;
    virtual String destinationHashStr() const = 0;

    // Endpoint / transport state.
    virtual bool isTransportActive() const = 0;
    virtual size_t pathCount() const = 0;
    virtual size_t linkCount() const = 0;

    // Announce: neutral byte buffer; the adapter owns wire-type conversion.
    // Empty buffer (nullptr/0) means "use the backend's default app data".
    virtual AnnounceResult announce(const uint8_t* appData, size_t len) = 0;
    virtual unsigned long lastAnnounceTime() const = 0;
    virtual uint32_t announceFilterCount() const = 0;

    // LXMF send/queue surface. dest is the 16-byte lxmf.delivery hash.
    virtual bool lxmfSendMessage(const uint8_t dest[16], const char* content,
                                 const char* title, bool preferLink) = 0;
    virtual void lxmfDropPeer(const std::string& peerHex) = 0;
    virtual int lxmfQueuedCount() const = 0;
    virtual void setMessageCallback(LXMFManager::MessageCallback cb) = 0;
    virtual void setStatusCallback(LXMFManager::StatusCallback cb) = 0;

    // Identity public key (lxma:// QR payload). Empty when no identity.
    virtual String publicKeyHex() const = 0;

    // Honesty driver: UI protocol gates flip on THIS, never on backend labels.
    virtual bool protocolReady() const = 0;

    // Link/resource status for diagnostics.
    virtual uint8_t activeResourceTransfers() const = 0;
    virtual const char* deliveryBackendDetail() const = 0;
};
