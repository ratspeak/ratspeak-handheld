#pragma once

// Narrow protocol facade shared by the device frontends. Conversation reads
// remain store-backed; protocol sends, status, and lifecycle pass through here.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include "reticulum/LXMFManager.h"
#include "protocol/OutgoingContract.h"
#include "config/PropagationSettings.h"
#include "protocol/RrcTypes.h"
#include "storage/RrcRecord.h"

class ProtocolBackend {
public:
    enum class AnnounceResult : uint8_t { Sent, Deferred, Failed };

    virtual ~ProtocolBackend() = default;

    // Implementation identity for diagnostics.
    virtual const char* backendName() const = 0;

    // Lifecycle / tick.
    virtual void loop() = 0;
    // Owner-only, bounded TX completion service. False defers blocking work
    // until the active radio burst finishes and receive mode is restored.
    virtual bool pollRadioBeforeBlockingWork() { return true; }
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
    // Empty buffer (nullptr/0) explicitly announces empty wire app data; it does
    // not clear the separately seeded name/capability cache for path responses.
    virtual AnnounceResult announce(const uint8_t* appData, size_t len) = 0;
    virtual unsigned long lastAnnounceTime() const = 0;
    virtual uint32_t announceFilterCount() const = 0;

    virtual void configurePropagation(const handheld::propagation::Settings&) {}
    virtual size_t propagationNodes(handheld::propagation::NodeView*, size_t) const { return 0; }
    virtual handheld::propagation::SyncView propagationStatus() const { return {}; }
    virtual bool propagationSync() { return false; }

    virtual handheld::rrc::Status rrcStatus() const { return {}; }
    virtual void rrcStopAdmissions() {}
    virtual bool rrcDrained() const { return true; }
    virtual size_t rrcHubs(handheld::rrc::HubView*, size_t) const { return 0; }
    virtual size_t rrcRooms(handheld::rrc::RoomView*, size_t) const { return 0; }
    virtual size_t rrcPeople(const char*, handheld::rrc::PersonView*, size_t) const { return 0; }
    virtual handheld::rrc::Code rrcCommand(const handheld::rrc::Command&, const uint8_t*, size_t) { return handheld::rrc::Code::Offline; }
    virtual bool rrcContext(const uint8_t[16], const char*, const uint8_t*, handheld::storage::rrc::Context&) const { return false; }

    // Admission copies spans and reserves a terminal persistence result. Only a
    // Ready/Committed result means saved; each accepted ticket must be acknowledged.
    virtual handheld::outgoing::Submission lxmfSubmit(const uint8_t dest[16],
        const uint8_t* title, size_t titleLength, const uint8_t* content,
        size_t contentLength, bool preferLink = false) = 0;
    virtual handheld::outgoing::Poll lxmfPoll(handheld::outgoing::Ticket,
        handheld::outgoing::InitialResult&) const = 0;
    virtual bool lxmfAcknowledge(handheld::outgoing::Ticket) = 0;
    virtual bool lxmfCancel(handheld::outgoing::Ticket) = 0;
    virtual bool lxmfStatus(const handheld::storage::RecordKey&,
        handheld::outgoing::StatusView&) const = 0;
    virtual uint32_t lxmfStatusRevision() const = 0;
    virtual void lxmfStopAdmissions() = 0;
    virtual bool lxmfDrained() const = 0;
    virtual handheld::storage::Error lxmfDrainError() const = 0;
    virtual bool lxmfBeginPeerDelete(const uint8_t peer[16]) = 0;
    virtual void lxmfFinishPeerDelete(const uint8_t peer[16],
        const handheld::storage::Result&) = 0;
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
