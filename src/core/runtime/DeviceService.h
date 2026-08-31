#pragma once

#if !defined(RSCARDPUTER)
#include "ServicePump.h"
#include "config/UserConfig.h"
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
    void bindOwner();
    void tick();
    void poll() override;
    void execute(uint8_t slot) override;
    ServiceMailbox& mailbox() { return _mailbox; }
    Status& ownerStatus() { return _status; }
    void notice(const char* text);
    bool stopped() const { return _status.state == ServiceState::Stopped; }

    // Installed once before ownership transfer; invoked only by the owner.
    std::function<void()> pollNetwork;
    std::function<void(Status&)> networkStatus;
    std::function<void(const UserSettings&)> applyRadio;
    std::function<void(const UserSettings&)> applyPeripherals;
    std::function<void()> homeReady;
    std::function<void(uint32_t)> diagnostics;
    std::function<void()> beginQuiesce;
    std::function<bool()> quiescent;
    std::function<void()> startScan;
    std::function<bool(String&)> finishScan;

private:
    void refreshStatus();
    void complete(uint8_t slot, Outcome outcome, const char* detail = nullptr);
    void jsonResult(uint8_t slot, JsonDocument& doc, Result result = {});
    void runLifecycle(uint8_t slot);
    void history(uint8_t slot);
    void historyChunk(uint8_t slot);
    void closeHistory();
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
    uint32_t _lastHistoryAccess = 0, _lastIdentityRetry = 0;
    uint32_t _nodeFingerprint = 0;
    uint32_t _historyQuery = 0;
    uint32_t _historyRevision = 0;
    std::string _historyPeer;
    MessageStore::HistoryCursor _history;
    LXMFMessage _historyMessage;
    uint32_t _historyMessageIndex = UINT32_MAX;
    uint8_t _historySlot = ServiceMailbox::NoSlot;
    uint8_t _scanSlot = ServiceMailbox::NoSlot;
    uint8_t _lifecycleSlot = ServiceMailbox::NoSlot;
    bool _quiesceStarted = false;
};

} // namespace handheld
#endif
