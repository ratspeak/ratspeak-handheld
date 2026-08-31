#include "LoRaInterface.h"
#include "config/BoardConfig.h"
#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <string.h>

// RNode on-air framing constants (from RNode_Firmware Framing.h / Config.h)
// Every LoRa packet has a 1-byte header: upper nibble = random sequence, lower nibble = flags
#define RNODE_HEADER_L      1
#define RNODE_FLAG_SPLIT    0x01
#define RNODE_NIBBLE_SEQ    0xF0
#define RNODE_SINGLE_MTU    (MAX_PACKET_SIZE - RNODE_HEADER_L)  // 254 bytes payload per frame

LoRaInterface::LoRaInterface(SX1262* radio, const char* name)
    : _name(name ? name : "LoRaInterface"), _radio(radio)
{
    refreshRadioTiming(true);
}

LoRaInterface::~LoRaInterface() {
    stop();
}

bool LoRaInterface::start() {
    if (!_radio || !_radio->isRadioOnline()) {
        Serial.println("[LORA_IF] Radio not available");
        _online = false;
        return false;
    }
    _online = true;
    refreshRadioTiming(true);
    _radio->receive();
    Serial.println("[LORA_IF] Interface started (split-packet enabled, MTU=500)");
    return true;
}

void LoRaInterface::stop() {
    _online = false;
    _txQueue.clear();
    _txData.clear();
    _splitTxRemaining.clear();
    _splitRxBuffer.clear();
    _txPending = _splitTxPending = _splitRxPending = false;
    _pacingActive = false;
    Serial.println("[LORA_IF] Interface stopped");
}

bool LoRaInterface::send_outgoing(const uint8_t* rawData, size_t rawLen) {
    if (!_online || !_radio || !rawData || rawLen == 0) return false;
    refreshRadioTiming();

    // Reject packets exceeding Reticulum MTU (500 bytes)
    if (rawLen > RETICULUM_MTU) {
        Serial.printf("[LORA_IF] TX DROPPED: exceeds Reticulum MTU (%d > %d)\n",
            (int)rawLen, (int)RETICULUM_MTU);
        return false;
    }
    // Queue TX when radio is busy OR when we're waiting for split frame 2.
    // Transmitting during split RX would put the radio in TX mode, causing
    // frame 2 to be lost (LoRa is half-duplex).
    if (_txPending || _splitTxPending || _splitRxPending || !pacingReady() || !_txQueue.empty()) {
        if ((int)_txQueue.size() < TX_QUEUE_MAX) {
            _txQueue.emplace_back(rawData, rawData + rawLen);
            if (_splitRxPending) {
                Serial.printf("[LORA_IF] TX deferred (split RX pending, %d in queue)\n", (int)_txQueue.size());
            } else {
                Serial.printf("[LORA_IF] TX queued (%d in queue)\n", (int)_txQueue.size());
            }
        } else {
            // Acceptance is a promise to retain this packet; never evict an earlier one.
            return false;
        }
        return true;
    }

    return transmitNow(std::vector<uint8_t>(rawData, rawData + rawLen));
}

bool LoRaInterface::pacingReady() const {
    return !_pacingActive || (int32_t)((uint32_t)millis() - _nextTxMs) >= 0;
}

float LoRaInterface::packetAirtimeMs(size_t len) const {
    if (!_radio) return 0;
    if (len <= RNODE_SINGLE_MTU) return _radio->getAirtime(len + RNODE_HEADER_L);
    return _radio->getAirtime(MAX_PACKET_SIZE) +
           _radio->getAirtime(len - RNODE_SINGLE_MTU + RNODE_HEADER_L);
}

uint32_t LoRaInterface::txWaitBudgetMs(uint32_t packets) const {
    // Include the in-flight packet and every retained packet. This is congestion pacing,
    // not a regulatory duty-cycle guarantee; both split frames remain contiguous.
    return (uint32_t)ceilf(packetAirtimeMs(RETICULUM_MTU) / AIRTIME_THROTTLE) *
           (TX_QUEUE_MAX + 1 + packets);
}

bool LoRaInterface::drainTx() {
    if (_txPending || _splitTxPending || _splitRxPending || !pacingReady() || _txQueue.empty())
        return false;
    if (transmitNow(_txQueue.front())) {
        _txQueue.pop_front();
        return true;
    }
    // A driver failure must not spin or remove an accepted packet. Retry on the next second.
    _nextTxMs = (uint32_t)millis() + 1000;
    _pacingActive = true;
    _radio->receive();
    return false;
}

bool LoRaInterface::transmitNow(const std::vector<uint8_t>& data) {
    refreshRadioTiming();
    uint8_t header = (uint8_t)(random(256)) & RNODE_NIBBLE_SEQ;
    bool needsSplit = (data.size() > RNODE_SINGLE_MTU);

    if (needsSplit) {
        header |= RNODE_FLAG_SPLIT;
        // First frame: header + first 254 bytes of payload
        size_t firstLen = RNODE_SINGLE_MTU;

        Serial.printf("[LORA_IF] TX SPLIT: %d bytes in 2 frames (seq=0x%02X)\n",
            (int)data.size(), header & RNODE_NIBBLE_SEQ);

        if (!_radio->beginPacket()) return false;
        _radio->write(header);
        _radio->write(data.data(), firstLen);
        if (!_radio->endPacket(true)) { _radio->receive(); return false; }

        // Save remaining data for second frame
        _splitTxPending = true;
        _splitTxRemaining.assign(data.data() + firstLen, data.data() + data.size());
        _splitTxHeader = header;

        Serial.printf("[LORA_IF] TX SPLIT frame 1: %d+1 bytes (remaining: %d)\n",
            (int)firstLen, (int)_splitTxRemaining.size());
    } else {
        // Single frame: fits in one LoRa packet
        if (!_radio->beginPacket()) return false;
        _radio->write(header);
        _radio->write(data.data(), data.size());
        if (!_radio->endPacket(true)) { _radio->receive(); return false; }

        Serial.printf("[LORA_IF] TX %d+1 bytes (hdr=0x%02X)\n", (int)data.size(), header);
    }

    _txPending = true;
    _txData = data;
    _nextTxMs = (uint32_t)millis() +
        (uint32_t)ceilf(packetAirtimeMs(data.size()) / AIRTIME_THROTTLE);
    _pacingActive = true;

    // Track airtime
    size_t airBytes = needsSplit ? (RNODE_SINGLE_MTU + RNODE_HEADER_L) : (data.size() + RNODE_HEADER_L);
    float airtimeMs = _radio->getAirtime(airBytes);
    unsigned long txNow = millis();
    if (txNow - _airtimeWindowStart >= AIRTIME_WINDOW_MS) {
        _airtimeAccumMs = 0;
        _airtimeWindowStart = txNow;
    } else {
        float elapsed = (float)(txNow - _airtimeWindowStart);
        float remaining = 1.0f - (elapsed / AIRTIME_WINDOW_MS);
        if (remaining < 0) remaining = 0;
        _airtimeAccumMs *= remaining;
        _airtimeWindowStart = txNow;
    }
    _airtimeAccumMs += airtimeMs;
    return true;
}

void LoRaInterface::loop() {
    if (!_online || !_radio) return;
    refreshRadioTiming();

    // Handle async TX completion
    if (_txPending) {
        if (!_radio->isTxBusy()) {
            _txPending = false;

            // If split TX pending, send the second frame immediately
            if (_splitTxPending) {
                size_t frame2Size = _splitTxRemaining.size();
                Serial.printf("[LORA_IF] TX SPLIT frame 2: %d+1 bytes\n", (int)frame2Size);

                if (!_radio->beginPacket()) { stop(); return; }
                _radio->write(_splitTxHeader);
                _radio->write(_splitTxRemaining.data(), frame2Size);
                if (!_radio->endPacket(true)) { stop(); return; }

                _splitTxPending = false;
                _txPending = true;
                _splitTxRemaining.clear();

                // Track airtime for second frame (must use saved size before clear)
                float airtimeMs = _radio->getAirtime(frame2Size + RNODE_HEADER_L);
                _airtimeAccumMs += airtimeMs;
                return;
            }

            _txData.clear();

            if (!drainTx()) _radio->receive();
        }
        return;
    }

    // Split RX timeout: discard stale partial packets and drain deferred TX
    if (_splitRxPending && (millis() - _splitRxTimestamp > _splitRxTimeoutMs)) {
        unsigned long age = millis() - _splitRxTimestamp;
        Serial.printf("[LORA_IF] RX SPLIT timeout after %lums (limit=%lums frame=%.0fms), discarding partial\n",
                      age, _splitRxTimeoutMs, _singleFrameAirtimeMs);
        _splitRxPending = false;
        _splitRxBuffer.clear();
    }

    // Periodic RX debug
    static unsigned long lastRxDebug = 0;
    if (millis() - lastRxDebug > 30000) {
        lastRxDebug = millis();
        int rssi = _radio->currentRssi();
        uint8_t status = _radio->getStatus();
        uint8_t chipMode = (status >> 4) & 0x07;
        Serial.printf("[LORA_IF] RX: RSSI=%d dBm, status=0x%02X(mode=%d)\n",
            rssi, status, chipMode);
    }

    if (!_radio->packetAvailable) { drainTx(); return; }
    _radio->packetAvailable = false;

    int packetSize = _radio->parsePacket();
    if (packetSize <= RNODE_HEADER_L) {
        if (packetSize > 0) {
            Serial.printf("[LORA_IF] RX runt packet (%d bytes), discarding\n", packetSize);
        }
        _radio->receive();
        return;
    }

    uint8_t raw[MAX_PACKET_SIZE];
    memcpy(raw, _radio->packetBuffer(), packetSize);

    // Capture signal quality before any further processing
    _lastRxRssi = _radio->packetRssi();
    _lastRxSnr = _radio->packetSnr();

    uint8_t header = raw[0];
    int payloadSize = packetSize - RNODE_HEADER_L;
    uint8_t seq = header & RNODE_NIBBLE_SEQ;
    bool isSplit = (header & RNODE_FLAG_SPLIT) != 0;

    if (isSplit) {
        // Split packet handling
        if (!_splitRxPending) {
            // First frame of a split packet
            _splitRxPending = true;
            _splitRxSeq = seq;
            _splitRxBuffer.assign(raw + RNODE_HEADER_L, raw + RNODE_HEADER_L + payloadSize);
            _splitRxTimestamp = millis();

            Serial.printf("[LORA_IF] RX SPLIT frame 1: %d bytes (seq=0x%02X), RSSI=%d, SNR=%.1f, timeout=%lums\n",
                payloadSize, seq, _lastRxRssi, _lastRxSnr, _splitRxTimeoutMs);
            _radio->receive();
            return;
        } else if (seq == _splitRxSeq) {
            // Second frame matches — reassemble
            Serial.printf("[LORA_IF] RX SPLIT frame 2: %d bytes (seq=0x%02X), RSSI=%d, SNR=%.1f, age=%lums\n",
                payloadSize, seq, _lastRxRssi, _lastRxSnr, millis() - _splitRxTimestamp);

            _splitRxBuffer.insert(_splitRxBuffer.end(), raw + RNODE_HEADER_L, raw + RNODE_HEADER_L + payloadSize);
            int totalSize = _splitRxBuffer.size();
            _splitRxPending = false;

            Serial.printf("[LORA_IF] RX SPLIT reassembled: %d bytes total\n", totalSize);

            // RX-side MTU cap (mirrors the TX-side check): two adversarial 255-byte
            // frames can reassemble past the Reticulum MTU — drop, don't hand up.
            if (totalSize > (int)RETICULUM_MTU) {
                Serial.printf("[LORA_IF] RX SPLIT over MTU (%d) — dropped\n", totalSize);
                _splitRxBuffer.clear();
                if (!drainTx() && !_txPending) _radio->receive();
                return;
            }

            if (_rawSink) {
                _rawSink(_splitRxBuffer.data(), _splitRxBuffer.size());
            }
            _splitRxBuffer.clear();

            // Drain any TX that was deferred during split RX hold
            if (!drainTx() && !_txPending) _radio->receive();
            return;
        } else {
            // Different split packet's frame 1 arrived — the previous split is lost.
            // This happens when frame 2 was missed (radio was busy, collision, etc.)
            Serial.printf("[LORA_IF] RX SPLIT new seq (had 0x%02X, got 0x%02X), previous frame 2 lost\n",
                _splitRxSeq, seq);
            _splitRxSeq = seq;
            _splitRxBuffer.assign(raw + RNODE_HEADER_L, raw + RNODE_HEADER_L + payloadSize);
            _splitRxTimestamp = millis();
            _radio->receive();
            return;
        }
    }

    // Non-split packet while waiting for split frame 2:
    // Process the non-split packet normally but KEEP the split buffer.
    // Frame 2 may still arrive after this interleaving packet.
    if (_splitRxPending) {
        Serial.printf("[LORA_IF] RX non-split %d bytes while awaiting split frame 2 (kept)\n", payloadSize);
    }

    Serial.printf("[LORA_IF] RX %d bytes (hdr=0x%02X, payload=%d), RSSI=%d, SNR=%.1f\n",
                  packetSize, header, payloadSize,
                  _lastRxRssi, _lastRxSnr);

    if (_rawSink) {
        _rawSink(raw + RNODE_HEADER_L, payloadSize);
    }

    if (!_txPending) {
        _radio->receive();
    }
}

float LoRaInterface::airtimeUtilization() const {
    if (_airtimeAccumMs <= 0) return 0;
    unsigned long elapsed = millis() - _airtimeWindowStart;
    if (elapsed == 0) elapsed = 1;
    float windowMs = std::min((float)elapsed, (float)AIRTIME_WINDOW_MS);
    return _airtimeAccumMs / windowMs;
}

unsigned long LoRaInterface::computeSplitRxTimeoutMs(float frameAirtimeMs) const {
    if (frameAirtimeMs <= 0) return SPLIT_RX_TIMEOUT_FLOOR_MS;
    float raw = frameAirtimeMs * SPLIT_RX_TIMEOUT_MULT + SPLIT_RX_TIMEOUT_MARGIN_MS;
    unsigned long rounded = (unsigned long)(ceil(raw / 500.0f) * 500.0f);
    return std::min(SPLIT_RX_TIMEOUT_CEIL_MS,
                    std::max(SPLIT_RX_TIMEOUT_FLOOR_MS, rounded));
}

void LoRaInterface::refreshRadioTiming(bool forceLog) {
    if (!_radio || !_radio->isRadioOnline()) {
        if (_bitrate == 0) _bitrate = 1;
        return;
    }

    unsigned long now = millis();
    if (!forceLog && _lastTimingRefreshMs != 0 && (now - _lastTimingRefreshMs) < 1000) return;
    _lastTimingRefreshMs = now;

    uint32_t newBitrate = _radio->getBitrate();
    if (newBitrate == 0) newBitrate = (_bitrate > 0) ? _bitrate : 1;
    float newFrameAirtime = _radio->getAirtime(MAX_PACKET_SIZE);
    unsigned long newTimeout = computeSplitRxTimeoutMs(newFrameAirtime);

    bool changed = forceLog || newBitrate != _bitrate ||
                   newTimeout != _splitRxTimeoutMs ||
                   fabsf(newFrameAirtime - _singleFrameAirtimeMs) >= 1.0f;

    _bitrate = newBitrate;
    _singleFrameAirtimeMs = newFrameAirtime;
    _splitRxTimeoutMs = newTimeout;

    if (changed) {
        Serial.printf("[LORA_IF] timing: bitrate=%lu bps frame=%.0fms split_timeout=%lums ldro=%s\n",
                      (unsigned long)_bitrate,
                      _singleFrameAirtimeMs,
                      _splitRxTimeoutMs,
                      _radio->lowDataRateEnabled() ? "on" : "off");
    }
}
