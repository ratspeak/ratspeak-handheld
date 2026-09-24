#pragma once

#if !defined(RSCARDPUTER)
#include "ServicePump.h"
#include "MaintenanceBarrier.h"
#include "ScanResult.h"
#include "config/UserConfig.h"
#include "radio/RadioSettings.h"
#include "protocol/ProtocolBackend.h"
#include "reticulum/AnnounceManager.h"
#include "reticulum/IdentityManager.h"
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace handheld {

class DeviceService : public ServiceHandler {
public:
    DeviceService(ProtocolBackend& backend, MessageStore& messages, UserConfig& config,
                  IdentityManager& identities, FlashStore& flash, SDStore& sd);
    ~DeviceService(); // Runner must no longer be executing before destruction.
    bool begin(AnnounceManager* nodes);
    // One binding attempt per service lifetime. Refusal leaves setup ownership
    // intact and exposes only cached Failed status and explicit Restart recovery.
    void bindOwner();
    void tick();
    void poll() override;
    bool readyForCommand() override;
    void execute(uint8_t slot) override;
    ServiceMailbox& mailbox() { return _mailbox; }
    Status& ownerStatus() { return _status; }
    void notice(const char* text);
    bool stopped() const { return _status.state == ServiceState::Stopped; }
    bool settingsApplyPending() const { return _settingsSlot != ServiceMailbox::NoSlot || _config.settingsPending(); }

    // Installed once before ownership transfer; invoked only by the owner.
    std::function<void()> pollNetwork;
    std::function<void(Status&)> networkStatus;
    // false cancels only the live continuation after maintenance closes admission.
    std::function<RadioApply(const UserSettings&, bool)> applyRadio;
    std::function<void(const UserSettings&)> applyPeripherals;
    std::function<void()> homeReady;
    std::function<void(uint32_t)> diagnostics;
    std::function<void()> closeAdmissions;
    std::function<void()> pollSettlements;
    std::function<bool()> settlementFailed;
    std::function<void()> beginQuiesce;
    std::function<bool()> quiescent;
    std::function<void()> startScan;
    std::function<ScanResult(String&)> finishScan;

private:
    void refreshStatus();
    void complete(uint8_t slot, Outcome outcome, const char* detail = nullptr);
    void jsonResult(uint8_t slot, JsonDocument& doc, Result result = {});
    void runLifecycle(uint8_t slot);
    void history(uint8_t slot);
    void pollHistory();
    void finishHistory(const storage::Result&);
    void pollSends();
    void pollStorageWrites();
    void pollMaintenance();
    void pollSettings();
    void finishSettings(uint8_t slot);
    ProtocolBackend& _backend;
    MessageStore& _messages;
    UserConfig& _config;
    IdentityManager& _identities;
    FlashStore& _flash;
    SDStore& _sd;
    AnnounceManager* _nodes = nullptr;
    ServiceMailbox _mailbox;
    ServicePump _pump;
    Status _status;
    TaskHandle_t _owner = nullptr;
    uint8_t* _arena = nullptr;
    char _scratch[ServiceMailbox::MaxPayload + 1] = {};
    uint32_t _lastStatus = 0, _lastStorageStatus = 0;
    uint32_t _lastIdentityRetry = 0;
    uint32_t _nodeFingerprint = 0;
    // Freeze membership, not string-heavy node records, for one bounded copy.
    // Live announces may update metadata without restarting the UI's transfer.
    uint8_t _nodeCopyKeys[ANNOUNCE_MAX_NODES][16] = {};
    size_t _nodeCopyCount = 0;
    uint32_t _nodeCopyRevision = 0, _nodeCopyGeneration = 0;
    // One retained storage query; normal mailbox work owns its terminal until
    // consumed, including peer/mode changes and lifecycle settlement.
    storage::Ticket _queryTicket;
    uint32_t _queryRetryAt = 0;
    uint8_t _querySlot = ServiceMailbox::NoSlot;
    uint8_t _scanSlot = ServiceMailbox::NoSlot;
    uint8_t _settingsSlot = ServiceMailbox::NoSlot;
    uint8_t _lifecycleSlot = ServiceMailbox::NoSlot;
    MaintenanceBarrier _maintenance;
    bool _storageOwnerBound = false;
    struct PendingSend {
        outgoing::Ticket ticket;
        bool cancelled = false;
    };
    PendingSend _sends[ServiceMailbox::NormalSlots];
    storage::Ticket _storageWrites[ServiceMailbox::NormalSlots];
    bool _outgoingPaused = false;
};

} // namespace handheld
#endif
