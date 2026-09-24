#pragma once

#include <stdint.h>

#include <functional>
#include <array>

#include "radio/BoardRadio.h"
#include "radio/SX1262Timing.h"
#include "radio/RadioTimingPolicy.h"
#include "transport/TxLease.h"

// RNode-framed LoRa driver. The raw-frame sink is the RX handoff;
// TX enters via sendRaw from the protocol pump.
class LoRaInterface {
public:
    LoRaInterface(BoardRadio* radio, const char* name = "LoRaInterface");
    ~LoRaInterface();

    bool start();
    void stop();
    void loop();
    // Service only an already-started burst before the owner enters blocking
    // storage work. Never start a queued packet here; completion restores RX.
    bool pollBeforeBlockingWork();

    // Close admission without aborting an already-started burst. Only its
    // remaining physical frame/completion may progress until explicit stop.
    void beginMaintenance();
    void pollMaintenance();
    bool maintenanceDrained() const {
        return _maintenance && !_maintenanceTxFailed && !_txPending && !_splitTxPending && _txCount == 0;
    }
    bool maintenanceFailed() const { return _maintenanceTxFailed; }
    uint32_t maintenanceDroppedRaw() const { return _maintenanceDroppedRaw; }
    // Terminal retirement after queue admission, distinct from refused offers.
    // Includes leased frames without application receipts. Saturates across restarts.
    uint32_t queuedDropped() const { return _queuedDropped; }

    // Raw-frame seam for the backend pump: RX frames go to the sink; TX
    // reuses the existing queue/split/airtime path.
    using RawSink = std::function<void(const uint8_t* data, size_t len)>;
    void setRawSink(RawSink sink) { _rawSink = sink; }
    // True when the new packet was handed to the radio or retained in the bounded TX queue.
    bool sendRaw(const uint8_t* data, size_t len) { return accepted(send_outgoing(data, len)); }
    bool sendLeased(const uint8_t* data, size_t len, const handheld::TxLease& lease) {
        return accepted(offerLeased(data, len, lease));
    }
    handheld::TxOffer offerLeased(const uint8_t* data, size_t len, const handheld::TxLease& lease) {
        return send_outgoing(data, len, &lease);
    }
    using TxValidator = bool (*)(void*, const handheld::TxLease&);
    // Owner-only; clearing the validator discards queued protocol frames before
    // its context can be destroyed. An already-started split burst finishes.
    void setTxValidator(void* context, TxValidator validator);
    // Drops old receipts through the old hook before changing ownership.
    void setReceiptHook(void* context, handheld::TxReceiptHook hook);
    uint32_t generation() const { return _generation; }

    float airtimeUtilization() const;

    // Last received packet signal quality
    int lastRxRssi() const { return _lastRxRssi; }
    float lastRxSnr() const { return _lastRxSnr; }
    bool isOnline() const { return _online && _radio && _radio->isRadioOnline(); }
    // Owner-only settings continuation. Existing packets retain their payloads
    // and receipts; new admission returns Blocked until retune or cancellation.
    void pauseForReconfigure(bool pause) { _reconfigurePending = pause; }
    // Owner-side transient diagnostic changes must not interrupt a retained
    // packet or either half of an active split exchange.
    bool canReconfigure() const {
        return isOnline() && !_maintenance && !_changingOwner && !_notifying &&
               !_txPending && !_splitTxPending && !_splitRxPending && _txCount == 0;
    }
    unsigned long splitRxTimeoutMs() const { return _splitRxTimeoutMs; }
    float singleFrameAirtimeMs() const { return _singleFrameAirtimeMs; }
    uint32_t bitrate() const { return _bitrate; }
    // Conservative local queue + packet pacing allowance, for protocol retry timers.
    uint32_t txWaitBudgetMs(uint32_t packets) const;

private:
    static bool accepted(handheld::TxOffer offer) {
        return offer == handheld::TxOffer::Queued || offer == handheld::TxOffer::Started;
    }
    handheld::TxOffer send_outgoing(const uint8_t* data, size_t len,
                                   const handheld::TxLease* lease = nullptr);
    bool transmitNow(const uint8_t* data, size_t len);
    void pollActiveTx(bool completionOnly, bool drainQueued = true);
    bool drainTx();
    void discardExpired();
    void removeQueued(size_t index, handheld::TxReceiptEvent event = handheld::TxReceiptEvent::Dropped,
                      const char* cause = "owner retired");
    void notifyReceipt(handheld::TxReceipt, handheld::TxReceiptEvent);
    float packetAirtimeMs(size_t len) const;
    bool pacingReady() const;
    void refreshRadioTiming(bool forceLog = false);
    unsigned long computeSplitRxTimeoutMs(float frameAirtimeMs) const;

    // Reticulum clear-packet MTU (split framing carries up to 2x254 = 508 on air).
    static constexpr size_t RETICULUM_MTU = 500;

    RawSink _rawSink;

    char _name[32] = {};
    bool _online = false;
    bool _maintenance = false, _maintenanceTxFailed = false;
    bool _reconfigurePending = false;
    uint32_t _maintenanceDroppedRaw = 0;
    uint32_t _queuedDropped = 0;
    uint32_t _generation = 0;
    uint32_t _bitrate = 0;

    BoardRadio* _radio;
    bool _txPending = false;
    std::array<uint8_t, RETICULUM_MTU> _txData{};
    size_t _txLength = 0;

    // TX queue: buffer packets when radio is busy instead of dropping
    static constexpr int TX_QUEUE_MAX = 4;
    struct QueuedFrame {
        std::array<uint8_t, RETICULUM_MTU> data{};
        handheld::TxLease lease;
        uint32_t queuedAt = 0;
        uint16_t length = 0;
    };
    std::array<QueuedFrame, TX_QUEUE_MAX> _txQueue{};
    size_t _txCount = 0;
    void* _validatorContext = nullptr;
    TxValidator _validator = nullptr;
    void* _receiptContext = nullptr;
    handheld::TxReceiptHook _receiptHook = nullptr;
    bool _notifying = false, _changingOwner = false;
    bool queueLive(const QueuedFrame& frame) const;
    uint32_t _nextTxMs = 0;
    bool _pacingActive = false;

    // Split-packet TX state: when a packet > 254 bytes, send in two LoRa frames
    bool _splitTxPending = false;
    uint8_t _splitTxHeader = 0;

    // Split-packet RX state: reassemble two LoRa frames into one Reticulum packet
    static constexpr unsigned long SPLIT_RX_TIMEOUT_FLOOR_MS = 5000;
    static constexpr unsigned long SPLIT_RX_TIMEOUT_CEIL_MS =
        handheld::radio_timing::MAX_SPLIT_RX_TIMEOUT_MS;
    bool _splitRxPending = false;
    uint8_t _splitRxSeq = 0;
    std::array<uint8_t, RETICULUM_MTU> _splitRxBuffer{};
    size_t _splitRxLength = 0;
    unsigned long _splitRxTimestamp = 0;
    unsigned long _splitRxTimeoutMs = SPLIT_RX_TIMEOUT_FLOOR_MS;
    float _singleFrameAirtimeMs = 0;
    unsigned long _lastTimingRefreshMs = 0;

    int _lastRxRssi = 0;
    float _lastRxSnr = 0;

    unsigned long _airtimeWindowStart = 0;
    float _airtimeAccumMs = 0;
    static constexpr unsigned long AIRTIME_WINDOW_MS = 60000;
public:
    static constexpr float AIRTIME_THROTTLE = 0.25f;
};
