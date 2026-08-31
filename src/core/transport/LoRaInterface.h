#pragma once

#include <stdint.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "radio/SX1262.h"

// RNode-framed LoRa driver. The raw-frame sink is the RX handoff;
// TX enters via sendRaw from the protocol pump.
class LoRaInterface {
public:
    LoRaInterface(SX1262* radio, const char* name = "LoRaInterface");
    ~LoRaInterface();

    bool start();
    void stop();
    void loop();

    // Raw-frame seam for the backend pump: RX frames go to the sink; TX
    // reuses the existing queue/split/airtime path.
    using RawSink = std::function<void(const uint8_t* data, size_t len)>;
    void setRawSink(RawSink sink) { _rawSink = sink; }
    // True when the new packet was handed to the radio or retained in the bounded TX queue.
    bool sendRaw(const uint8_t* data, size_t len) { return send_outgoing(data, len); }

    float airtimeUtilization() const;

    // Last received packet signal quality
    int lastRxRssi() const { return _lastRxRssi; }
    float lastRxSnr() const { return _lastRxSnr; }
    bool isOnline() const { return _online; }
    unsigned long splitRxTimeoutMs() const { return _splitRxTimeoutMs; }
    float singleFrameAirtimeMs() const { return _singleFrameAirtimeMs; }
    uint32_t bitrate() const { return _bitrate; }
    // Conservative local queue + packet pacing allowance, for protocol retry timers.
    uint32_t txWaitBudgetMs(uint32_t packets) const;

private:
    bool send_outgoing(const uint8_t* data, size_t len);
    bool transmitNow(const std::vector<uint8_t>& data);
    bool drainTx();
    float packetAirtimeMs(size_t len) const;
    bool pacingReady() const;
    void refreshRadioTiming(bool forceLog = false);
    unsigned long computeSplitRxTimeoutMs(float frameAirtimeMs) const;

    // Reticulum clear-packet MTU (split framing carries up to 2x254 = 508 on air).
    static constexpr size_t RETICULUM_MTU = 500;

    RawSink _rawSink;

    std::string _name;
    bool _online = false;
    uint32_t _bitrate = 0;

    SX1262* _radio;
    bool _txPending = false;
    std::vector<uint8_t> _txData;

    // TX queue: buffer packets when radio is busy instead of dropping
    static constexpr int TX_QUEUE_MAX = 4;
    std::deque<std::vector<uint8_t>> _txQueue;
    uint32_t _nextTxMs = 0;
    bool _pacingActive = false;

    // Split-packet TX state: when a packet > 254 bytes, send in two LoRa frames
    bool _splitTxPending = false;
    std::vector<uint8_t> _splitTxRemaining;
    uint8_t _splitTxHeader = 0;

    // Split-packet RX state: reassemble two LoRa frames into one Reticulum packet
    static constexpr unsigned long SPLIT_RX_TIMEOUT_FLOOR_MS = 5000;
    static constexpr unsigned long SPLIT_RX_TIMEOUT_CEIL_MS = 60000;
    static constexpr unsigned long SPLIT_RX_TIMEOUT_MARGIN_MS = 2000;
    static constexpr float SPLIT_RX_TIMEOUT_MULT = 1.5f;
    bool _splitRxPending = false;
    uint8_t _splitRxSeq = 0;
    std::vector<uint8_t> _splitRxBuffer;
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
