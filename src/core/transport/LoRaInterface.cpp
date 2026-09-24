#include "LoRaInterface.h"
#include "runtime/ResourceBudget.h"
#include "config/BoardConfig.h"
#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <string.h>

static_assert(sizeof(LoRaInterface) <= handheld::ResourceBudget::LoRaRetainedBytes,
              "LoRa fixed payload, lease, RX and driver state must fit the adopted budget");

// RNode on-air framing constants (from RNode_Firmware Framing.h / Config.h)
// Every LoRa packet has a 1-byte header: upper nibble = random sequence, lower nibble = flags
#define RNODE_HEADER_L      1
#define RNODE_FLAG_SPLIT    0x01
#define RNODE_NIBBLE_SEQ    0xF0
#define RNODE_SINGLE_MTU    (MAX_PACKET_SIZE - RNODE_HEADER_L)  // 254 bytes payload per frame

LoRaInterface::LoRaInterface(BoardRadio* radio, const char* name)
    : _radio(radio)
{
    snprintf(_name, sizeof(_name), "%s", name ? name : "LoRaInterface");
    refreshRadioTiming(true);
}

LoRaInterface::~LoRaInterface() {
    stop();
}

bool LoRaInterface::start() {
    if (_changingOwner || _notifying) return false;
    if (_online) return !_maintenance;
    if (_generation == UINT32_MAX) return false;
    if (!_radio || !_radio->isRadioOnline()) {
        Serial.println("[LORA_IF] Radio not available");
        _online = false;
        return false;
    }
    _online = true;
    _maintenance = _maintenanceTxFailed = false;
    _reconfigurePending = false;
    ++_generation;
    refreshRadioTiming(true);
    _radio->receive();
    if (!_radio->isRadioOnline()) { stop(); return false; }
    Serial.println("[LORA_IF] Interface started (split-packet enabled, MTU=500)");
    return true;
}

void LoRaInterface::stop() {
    _online = false;
    _reconfigurePending = false;
    const bool changing = _changingOwner;
    _changingOwner = true;
    while (_txCount) removeQueued(0, handheld::TxReceiptEvent::Dropped, "interface stopped");
    _txLength = 0;
    _splitRxLength = 0;
    _txPending = _splitTxPending = _splitRxPending = false;
    _pacingActive = false;
    _changingOwner = changing;
    Serial.println("[LORA_IF] Interface stopped");
}

void LoRaInterface::beginMaintenance() {
    if (_maintenance) return;
    // Set the gate before returning any receipt: callbacks may reenter stop,
    // maintenance or admission. The active payload has no receipt references.
    _maintenance = true;
    _reconfigurePending = false;
    // Count before callbacks can recursively stop and empty the remaining queue.
    for (size_t i = 0; i < _txCount; ++i)
        if (!_txQueue[i].lease.generation && _maintenanceDroppedRaw != UINT32_MAX)
            ++_maintenanceDroppedRaw;
    const bool changing = _changingOwner;
    _changingOwner = true;
    while (_txCount) {
        removeQueued(0, handheld::TxReceiptEvent::Dropped, "maintenance");
    }
    _splitRxPending = false;
    _splitRxLength = 0;
    _changingOwner = changing;
}

void LoRaInterface::pollMaintenance() {
    if (!_maintenance || !_online || !_radio || _changingOwner || _notifying ||
        _maintenanceTxFailed) return;
    pollActiveTx(true);
}

void LoRaInterface::setTxValidator(void* context, TxValidator validator) {
    // Discard old-context queued bytes before replacing a validator. The active
    // radio burst owns a fixed payload copy and no longer references a context.
    const bool changing = _changingOwner;
    _changingOwner = true;
    for (size_t i = 0; i < _txCount;) {
        if (_txQueue[i].lease.generation)
            removeQueued(i, handheld::TxReceiptEvent::Dropped, "validator changed");
        else ++i;
    }
    _validatorContext = context;
    _validator = validator;
    _changingOwner = changing;
}

void LoRaInterface::setReceiptHook(void* context, handheld::TxReceiptHook hook) {
    const bool changing = _changingOwner;
    _changingOwner = true;
    for (size_t i = 0; i < _txCount;) {
        if (_txQueue[i].lease.receipt().valid())
            removeQueued(i, handheld::TxReceiptEvent::Dropped, "receipt owner changed");
        else ++i;
    }
    _receiptContext = context;
    _receiptHook = hook;
    _changingOwner = changing;
}

void LoRaInterface::notifyReceipt(handheld::TxReceipt receipt, handheld::TxReceiptEvent event) {
    if (!receipt.valid() || !_receiptHook) return;
    const bool notifying = _notifying;
    _notifying = true;
    _receiptHook(_receiptContext, receipt, event);
    _notifying = notifying;
}

bool LoRaInterface::queueLive(const QueuedFrame& frame) const {
    if (frame.lease.generation) {
        return _validator && _validator(_validatorContext, frame.lease);
    }
    // Diagnostic raw packets have no protocol state, but still have a finite
    // immutable age. Unsigned elapsed time handles the 32-bit millis rollover.
    const uint32_t age = static_cast<uint32_t>(millis()) - frame.queuedAt;
    return age < 120000;
}

void LoRaInterface::removeQueued(size_t index, handheld::TxReceiptEvent event, const char* cause) {
    const auto receipt = _txQueue[index].lease.receipt();
    for (size_t i = index + 1; i < _txCount; ++i) _txQueue[i - 1] = _txQueue[i];
    --_txCount;
    _txQueue[_txCount].length = 0;
    if (event == handheld::TxReceiptEvent::Dropped) {
        if (_queuedDropped != UINT32_MAX) ++_queuedDropped;
        Serial.printf("[LORA_IF] queued TX dropped: %s (total=%lu)\n",
                      cause, static_cast<unsigned long>(_queuedDropped));
    }
    // No queue reference survives this callback, which may retire/reuse the
    // receipt or quiesce the pump. Admissions during callbacks are backpressured.
    notifyReceipt(receipt, event);
}

void LoRaInterface::discardExpired() {
    for (size_t i = 0; i < _txCount;) {
        if (!queueLive(_txQueue[i]))
            removeQueued(i, handheld::TxReceiptEvent::Dropped,
                         _txQueue[i].lease.generation ? "lease expired or retired" : "raw queue deadline");
        else ++i;
    }
}

handheld::TxOffer LoRaInterface::send_outgoing(const uint8_t* rawData, size_t rawLen,
                                              const handheld::TxLease* lease) {
    using handheld::TxOffer;
    const auto receipt = lease ? lease->receipt() : handheld::TxReceipt{};
    if (_changingOwner || _notifying || _reconfigurePending) return TxOffer::Blocked;
    const auto reject = [&]() {
        notifyReceipt(receipt, handheld::TxReceiptEvent::Dropped);
        return TxOffer::Rejected;
    };
    if (!isOnline() || _maintenance || !rawData || rawLen == 0) return reject();
    refreshRadioTiming();
    discardExpired();

    // Reject packets exceeding Reticulum MTU (500 bytes)
    if (rawLen > RETICULUM_MTU) {
        Serial.printf("[LORA_IF] TX DROPPED: exceeds Reticulum MTU (%d > %d)\n",
            (int)rawLen, (int)RETICULUM_MTU);
        return reject();
    }
    // Queue TX when radio is busy OR when we're waiting for split frame 2.
    // Transmitting during split RX would put the radio in TX mode, causing
    // frame 2 to be lost (LoRa is half-duplex).
    if (!_online || _maintenance || (lease && (!lease->generation || !_validator ||
                  !_validator(_validatorContext, *lease)))) return reject();
    if (_txPending || _splitTxPending || _splitRxPending || !pacingReady() || _txCount) {
        if (_txCount < TX_QUEUE_MAX) {
            auto& frame = _txQueue[_txCount++];
            memcpy(frame.data.data(), rawData, rawLen);
            frame.length = rawLen;
            frame.queuedAt = static_cast<uint32_t>(millis());
            frame.lease = lease ? *lease : handheld::TxLease{};
            if (_splitRxPending) {
                Serial.printf("[LORA_IF] TX deferred (split RX pending, %d in queue)\n", (int)_txCount);
            } else {
                Serial.printf("[LORA_IF] TX queued (%d in queue)\n", (int)_txCount);
            }
        } else {
            // Acceptance is a promise to retain this packet; never evict an earlier one.
            return TxOffer::Blocked;
        }
        return TxOffer::Queued;
    }

    // Validity is checked before the first physical frame. Once begun, a split
    // exchange is atomic even when its on-air duration exceeds the wait ceiling.
    if (lease && !_validator(_validatorContext, *lease)) return reject();
    if (!transmitNow(rawData, rawLen)) return TxOffer::Blocked;
    notifyReceipt(receipt, handheld::TxReceiptEvent::Started);
    return TxOffer::Started;
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
    if (_changingOwner || _notifying || !_online || _maintenance) return false;
    discardExpired();
    if (!_online || _txPending || _splitTxPending || _splitRxPending || !pacingReady() || !_txCount)
        return false;
    while (_txCount && !queueLive(_txQueue[0]))
        removeQueued(0, handheld::TxReceiptEvent::Dropped,
                     _txQueue[0].lease.generation ? "lease expired or retired" : "raw queue deadline");
    if (!_txCount) return false;
    if (transmitNow(_txQueue[0].data.data(), _txQueue[0].length)) {
        removeQueued(0, handheld::TxReceiptEvent::Started);
        return true;
    }
    // A driver failure must not spin or remove an accepted packet. Retry on the next second.
    _nextTxMs = (uint32_t)millis() + 1000;
    _pacingActive = true;
    _radio->receive();
    return false;
}

bool LoRaInterface::transmitNow(const uint8_t* data, size_t len) {
    refreshRadioTiming();
    // Copy before touching the radio; split frame two never allocates or depends
    // on a queue slot that can be compacted during the active burst.
    memcpy(_txData.data(), data, len);
    _txLength = len;
    uint8_t header = (uint8_t)(random(256)) & RNODE_NIBBLE_SEQ;
    bool needsSplit = (len > RNODE_SINGLE_MTU);

    if (needsSplit) {
        header |= RNODE_FLAG_SPLIT;
        // First frame: header + first 254 bytes of payload
        size_t firstLen = RNODE_SINGLE_MTU;

        Serial.printf("[LORA_IF] TX SPLIT: %d bytes in 2 frames (seq=0x%02X)\n",
            (int)len, header & RNODE_NIBBLE_SEQ);

        if (!_radio->beginPacket()) return false;
        if (_radio->write(header) != 1 || _radio->write(_txData.data(), firstLen) != firstLen)
            return false;
        if (!_radio->endPacket(true)) { _radio->receive(); return false; }

        // Save remaining data for second frame
        _splitTxPending = true;
        _splitTxHeader = header;

        Serial.printf("[LORA_IF] TX SPLIT frame 1: %d+1 bytes (remaining: %d)\n",
            (int)firstLen, (int)(len - firstLen));
    } else {
        // Single frame: fits in one LoRa packet
        if (!_radio->beginPacket()) return false;
        if (_radio->write(header) != 1 || _radio->write(_txData.data(), len) != len)
            return false;
        if (!_radio->endPacket(true)) { _radio->receive(); return false; }

        Serial.printf("[LORA_IF] TX %d+1 bytes (hdr=0x%02X)\n", (int)len, header);
    }

    _txPending = true;
    _nextTxMs = (uint32_t)millis() +
        (uint32_t)ceilf(packetAirtimeMs(len) / AIRTIME_THROTTLE);
    _pacingActive = true;

    // Track airtime
    size_t airBytes = needsSplit ? (RNODE_SINGLE_MTU + RNODE_HEADER_L) : (len + RNODE_HEADER_L);
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

bool LoRaInterface::pollBeforeBlockingWork() {
    if (_maintenanceTxFailed) return false;
    if (_txPending && !_changingOwner && !_notifying) {
        if (!_online || !_radio || !_radio->isRadioOnline()) {
            // Maintenance must retain an incomplete burst for its failure/reset
            // policy; losing the radio does not make that burst drained.
            if (_maintenance) _maintenanceTxFailed = true;
            else stop();
        } else pollActiveTx(_maintenance, false);
    }
    return !_txPending && !_splitTxPending;
}

void LoRaInterface::pollActiveTx(bool completionOnly, bool drainQueued) {
    if (_txPending) {
        if (!_radio->isTxBusy()) {
            if (_radio->txFailed()) {
                Serial.println("[LORA_IF] TX failed before complete burst");
                if (completionOnly) _maintenanceTxFailed = true;
                else stop();
                return;
            }
            _txPending = false;

            // If split TX pending, send the second frame immediately
            if (_splitTxPending) {
                size_t frame2Size = _txLength - RNODE_SINGLE_MTU;
                Serial.printf("[LORA_IF] TX SPLIT frame 2: %d+1 bytes\n", (int)frame2Size);

                const auto failed = [&] {
                    // Preserve the unsent second half for the maintenance
                    // controller's explicit failure/reset policy. It is not a
                    // drained burst and must not be silently retried or freed.
                    if (completionOnly) _maintenanceTxFailed = true;
                    else stop();
                };
                if (!_radio->beginPacket()) { failed(); return; }
                if (_radio->write(_splitTxHeader) != 1 ||
                    _radio->write(_txData.data() + RNODE_SINGLE_MTU, frame2Size) != frame2Size) {
                    failed(); return;
                }
                if (!_radio->endPacket(true)) { failed(); return; }

                _splitTxPending = false;
                _txPending = true;

                // Track airtime for second frame (must use saved size before clear)
                float airtimeMs = _radio->getAirtime(frame2Size + RNODE_HEADER_L);
                _airtimeAccumMs += airtimeMs;
                return;
            }

            _txLength = 0;

            if (!completionOnly && !(drainQueued && drainTx())) _radio->receive();
        }
        return;
    }
}

void LoRaInterface::loop() {
    if (_maintenance) { pollMaintenance(); return; }
    if (!_online || !_radio || _changingOwner || _notifying) return;
    if (!_radio->isRadioOnline()) { stop(); return; }
    refreshRadioTiming();
    discardExpired();
    if (!_online || _maintenance) return;
    if (_txPending) { pollActiveTx(false); return; }

    // Split RX timeout: discard stale partial packets and drain deferred TX
    if (_splitRxPending && (millis() - _splitRxTimestamp > _splitRxTimeoutMs)) {
        unsigned long age = millis() - _splitRxTimestamp;
        Serial.printf("[LORA_IF] RX SPLIT timeout after %lums (limit=%lums frame=%.0fms), discarding partial\n",
                      age, _splitRxTimeoutMs, _singleFrameAirtimeMs);
        _splitRxPending = false;
        _splitRxLength = 0;
    }

    // Periodic RX debug
    static unsigned long lastRxDebug = 0;
    if (millis() - lastRxDebug > 30000) {
        lastRxDebug = millis();
        int rssi = _radio->currentRssi();
        uint8_t status = _radio->getStatus();
        Serial.printf("[LORA_IF] RX: RSSI=%d dBm, status=0x%02X\n", rssi, status);
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
    if (packetSize > static_cast<int>(sizeof(raw))) { _radio->receive(); return; }
    memcpy(raw, _radio->packetBuffer(), packetSize);

    // Capture signal quality before any further processing
    _lastRxRssi = _radio->packetRssi();
    _lastRxSnr = _radio->packetSnr();

    uint8_t header = raw[0];
    int payloadSize = packetSize - RNODE_HEADER_L;
    uint8_t seq = header & RNODE_NIBBLE_SEQ;
    bool isSplit = (header & RNODE_FLAG_SPLIT) != 0;

    // A saved retune waits for the existing split, not an endless stream of
    // replacement first halves. Preserve its original deadline and bytes.
    if (_reconfigurePending && isSplit && (!_splitRxPending || seq != _splitRxSeq)) {
        if (!drainTx() && !_txPending) _radio->receive();
        return;
    }

    if (isSplit) {
        // Split packet handling
        if (!_splitRxPending) {
            // First frame of a split packet
            _splitRxPending = true;
            _splitRxSeq = seq;
            memcpy(_splitRxBuffer.data(), raw + RNODE_HEADER_L, payloadSize);
            _splitRxLength = payloadSize;
            _splitRxTimestamp = millis();

            Serial.printf("[LORA_IF] RX SPLIT frame 1: %d bytes (seq=0x%02X), RSSI=%d, SNR=%.1f, timeout=%lums\n",
                payloadSize, seq, _lastRxRssi, _lastRxSnr, _splitRxTimeoutMs);
            _radio->receive();
            return;
        } else if (seq == _splitRxSeq) {
            // Second frame matches — reassemble
            Serial.printf("[LORA_IF] RX SPLIT frame 2: %d bytes (seq=0x%02X), RSSI=%d, SNR=%.1f, age=%lums\n",
                payloadSize, seq, _lastRxRssi, _lastRxSnr, millis() - _splitRxTimestamp);

            const size_t totalSize = _splitRxLength + payloadSize;
            _splitRxPending = false;

            Serial.printf("[LORA_IF] RX SPLIT reassembled: %d bytes total\n", (int)totalSize);

            // RX-side MTU cap (mirrors the TX-side check): two adversarial 255-byte
            // frames can reassemble past the Reticulum MTU — drop, don't hand up.
            if (totalSize > RETICULUM_MTU) {
                Serial.printf("[LORA_IF] RX SPLIT over MTU (%d) — dropped\n", (int)totalSize);
                _splitRxLength = 0;
                if (!drainTx() && !_txPending) _radio->receive();
                return;
            }
            memcpy(_splitRxBuffer.data() + _splitRxLength, raw + RNODE_HEADER_L, payloadSize);

            if (_rawSink) {
                _rawSink(_splitRxBuffer.data(), totalSize);
            }
            _splitRxLength = 0;

            // Drain any TX that was deferred during split RX hold
            if (!drainTx() && !_txPending) _radio->receive();
            return;
        } else {
            // Different split packet's frame 1 arrived — the previous split is lost.
            // This happens when frame 2 was missed (radio was busy, collision, etc.)
            Serial.printf("[LORA_IF] RX SPLIT new seq (had 0x%02X, got 0x%02X), previous frame 2 lost\n",
                _splitRxSeq, seq);
            _splitRxSeq = seq;
            memcpy(_splitRxBuffer.data(), raw + RNODE_HEADER_L, payloadSize);
            _splitRxLength = payloadSize;
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
    // Bound before converting to integer, including NaN/infinity from a
    // malformed timing provider. The ceiling covers every supported tuple.
    if (!std::isfinite(frameAirtimeMs) || frameAirtimeMs >= SPLIT_RX_TIMEOUT_CEIL_MS)
        return SPLIT_RX_TIMEOUT_CEIL_MS;
    return handheld::radio_timing::splitReceiveTimeoutMs(
        static_cast<uint32_t>(ceilf(frameAirtimeMs)));
}

void LoRaInterface::refreshRadioTiming(bool forceLog) {
    if (!_radio || !_radio->isRadioOnline()) {
        if (_bitrate == 0) _bitrate = 1;
        return;
    }

    unsigned long now = millis();
    // Configuration setters are cheap to observe and may run just before TX.
    // Do not retain the previous modem's timeout/bitrate for another second.
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
