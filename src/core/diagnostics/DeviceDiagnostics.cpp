#include "DeviceDiagnostics.h"
#include "config/BoardConfig.h"
#include "config/Config.h"
#include "runtime/TaskOwner.h"
#include "ratspeak_protocol.h"
#include <cctype>
#include <cstring>
#include <esp_heap_caps.h>

namespace handheld {
static constexpr uint8_t LITE_TRANSPORT_ID[16] = {
    'r', 's', 'l', 'i', 't', 'e', '-', 'h',
    'e', 'l', 't', 'e', 'c', '-', 'v', '3'
};
void DeviceDiagnostics::printDiagnostics() {
    assertDeviceOwner();
    Serial.println("=== DIAGNOSTIC DUMP ===");
    Serial.printf("Device: %s\n", boardName);
    Serial.printf("Protocol: %s\n", backend->backendName());
    Serial.printf("Identity: %s\n", backend->identityHash().c_str());
    Serial.printf("Transport: %s\n", backend->isTransportActive() ? "ACTIVE" : "OFFLINE");
    Serial.printf("Paths: %d  Links: %d\n", (int)backend->pathCount(), (int)backend->linkCount());
    Serial.printf("Delivery: %s  Resources: %u\n", backend->deliveryBackendDetail(),
                  (unsigned)backend->activeResourceTransfers());
    Serial.printf("Radio: %s\n", radioOnline ? "ONLINE" : "OFFLINE");
    if (radioOnline) {
        Serial.printf("Freq: %lu Hz  SF: %d  BW: %lu  CR: 4/%d  TXP: %d dBm\n",
                      (unsigned long)radio.getFrequency(),
                      radio.getSpreadingFactor(),
                      (unsigned long)radio.getSignalBandwidth(),
                      radio.getCodingRate4(),
                      radio.getTxPower());
#if !defined(RSM9)
        Serial.printf("Regulator: %s\n", LORA_USE_DCDC_REGULATOR ? "DC-DC" : "LDO");
#endif
        Serial.printf("Preamble: %ld symbols\n", radio.getPreambleLength());
        Serial.printf("Bitrate: %lu bps  LDRO: %s  frame255: %.0f ms\n",
                      (unsigned long)radio.getBitrate(),
                      radio.lowDataRateEnabled() ? "ON" : "off",
                      radio.getAirtime(MAX_PACKET_SIZE));
        LoRaInterface* loraIf = &rustLoraIface;
        if (loraIf) {
            Serial.printf("LoRaIF: bitrate=%lu bps split_timeout=%lu ms frame=%.0f ms airtime=%.2f%%\n",
                          (unsigned long)loraIf->bitrate(),
                          loraIf->splitRxTimeoutMs(),
                          loraIf->singleFrameAirtimeMs(),
                          loraIf->airtimeUtilization() * 100.0f);
        }
        Serial.printf("IQ invert: %s\n", radio.getInvertIQ() ? "ON" : "off");
#if defined(RSM9)
        radio.printDiagnostics();
#else
        Serial.printf("SyncWord regs: 0x%02X%02X\n",
            radio.readRegister(REG_SYNC_WORD_MSB_6X),
            radio.readRegister(REG_SYNC_WORD_LSB_6X));
        uint16_t devErr = radio.getDeviceErrors();
        uint8_t status = radio.getStatus();
        Serial.printf("DevErrors: 0x%04X  Status: 0x%02X (mode=%d cmd=%d)\n",
            devErr, status, (status >> 4) & 0x07, (status >> 1) & 0x07);
        if (devErr & 0x40) Serial.println("  *** PLL LOCK FAILED ***");
        Serial.printf("IRQ flags: 0x%04X\n", radio.getIrqFlags());
        Serial.printf("Current RSSI: %d dBm\n", radio.currentRssi());
        Serial.printf("SX1262 IQ polarity=0x%02X config=0x%02X LNA=0x%02X OCP=0x%02X clamp=0x%02X\n",
                      radio.readRegister(0x0889), radio.readRegister(0x0736),
                      radio.readRegister(0x08AC), radio.readRegister(0x08E7),
                      radio.readRegister(REG_TX_CLAMP_CONFIG_6X));
        uint8_t packetType = radio.getPacketType();
        const char* packetTypeName =
            (packetType == 0x00) ? "GFSK" :
            (packetType == 0x01) ? "LoRa" :
            (packetType == 0x02) ? "LR-FHSS" : "unknown";
        Serial.printf("Packet type: 0x%02X (%s)%s\n",
                      packetType, packetTypeName,
                      packetType == 0x01 ? "" : " *** NOT LoRa ***");
#endif
    }
    Serial.printf("Free heap: %lu bytes  PSRAM: %lu bytes\n",
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getFreePsram());
    sampleMemory(static_cast<uint32_t>(millis()));
    printMemory();
    if (extraDump) extraDump();
    Serial.printf("Uptime: %lu s\n", millis() / 1000);
    Serial.println("=======================");
}

void DeviceDiagnostics::cycleDiagnosticTxPower() {
    if (!radioOnline || !rustLoraIface.canReconfigure()) {
        Serial.println("[SERIAL] radio unavailable or busy");
        return;
    }
    static constexpr int8_t kPowers[] = {-9, -3, 0, 2, 6, 10, 14, 17, 22};
    int current = radio.getTxPower();
    size_t next = 0;
    for (size_t i = 0; i < sizeof(kPowers) / sizeof(kPowers[0]); i++) {
        if (current == kPowers[i]) {
            next = (i + 1) % (sizeof(kPowers) / sizeof(kPowers[0]));
            break;
        }
    }

    radio.setTxPower(kPowers[next]);
    radio.receive();
    Serial.printf("[SERIAL] transient TX power set to %d dBm\n", (int)kPowers[next]);
}

void DeviceDiagnostics::setDiagnosticMinTxPower() {
    if (!radioOnline || !rustLoraIface.canReconfigure()) {
        Serial.println("[SERIAL] radio unavailable or busy");
        return;
    }
    radio.setTxPower(-9);
    radio.receive();
    Serial.println("[SERIAL] transient TX power set to -9 dBm");
}

bool DeviceDiagnostics::setDiagnosticTxPower(int powerDbm) {
    if (!radioOnline || !rustLoraIface.canReconfigure()) {
        Serial.println("[SERIAL] radio unavailable or busy");
        return false;
    }
    static constexpr int kMaxDiagnosticTxPower = 22;
    if (powerDbm < -9 || powerDbm > kMaxDiagnosticTxPower) {
        Serial.printf("[SERIAL] TX power out of range: %d dBm (allowed -9..%d)\n",
                      powerDbm, kMaxDiagnosticTxPower);
        return false;
    }
    radio.setTxPower((int8_t)powerDbm);
    radio.receive();
    Serial.printf("[SERIAL] transient TX power set to %d dBm\n", powerDbm);
    return true;
}

void DeviceDiagnostics::toggleDiagnosticInvertIQ() {
    if (!radioOnline || !rustLoraIface.canReconfigure()) {
        Serial.println("[SERIAL] radio unavailable or busy");
        return;
    }
    radio.setInvertIQ(!radio.getInvertIQ());
    radio.receive();
    Serial.printf("[SERIAL] IQ inversion %s\n", radio.getInvertIQ() ? "ON" : "off");
}

bool DeviceDiagnostics::setDiagnosticFrequency(uint32_t frequencyHz) {
    if (!radioOnline || !rustLoraIface.canReconfigure()) {
        Serial.println("[SERIAL] radio unavailable or busy");
        return false;
    }
    if (frequencyHz < 150000000UL || frequencyHz > 960000000UL) {
        Serial.printf("[SERIAL] frequency out of range: %lu Hz (allowed 150000000..960000000)\n",
                      (unsigned long)frequencyHz);
        return false;
    }
    radio.setFrequency(frequencyHz);
    radio.receive();
    Serial.printf("[SERIAL] transient frequency set to %lu Hz\n", (unsigned long)frequencyHz);
    return true;
}

bool DeviceDiagnostics::selectDiagnosticPeer(const char* explicitArg, rs::Bytes& destHash, std::string& label) {
    if (diagnostics::hasArgument(explicitArg)) {
        if (!parseSerialDestinationHash(explicitArg, destHash)) {
            Serial.println("[SERIAL] invalid LXMF destination hash; expected 32 hex characters");
            return false;
        }
        label = destHash.toHex();
        return true;
    }

    if (!announceManager) {
        Serial.println("[SERIAL] LXMF test failed: announce manager is not ready");
        return false;
    }

    const std::string localHex = backend->destinationHashHex().c_str();
    for (const auto& node : announceManager->nodes()) {
        if (node.hash.size() != 16) continue;
        const std::string nodeHex = node.hash.toHex();
        if (nodeHex == localHex) continue;
        destHash = node.hash;
        label = node.name.empty() ? nodeHex : (node.name + " " + nodeHex);
        return true;
    }

    Serial.println("[SERIAL] LXMF test failed: no peer known; send/receive announces first or pass a hash");
    return false;
}

void DeviceDiagnostics::makeDiagnosticLxmfPayload(char* output, size_t length) {
    const char* kPrefix = payloadPrefix;
    static constexpr char kPattern[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    size_t written = 0;
    for (; kPrefix[written] && written < length; ++written) {
        output[written] = kPrefix[written];
    }
    for (size_t i = 0; written < length; ++i) {
        output[written++] = kPattern[i % (sizeof(kPattern) - 1)];
    }
    output[written] = '\0';
}

bool DeviceDiagnostics::sendDiagnosticLxmf(size_t length, const char* explicitDest) {
    if (diagnosticSend.valid()) {
        Serial.println("[SERIAL] LXMF test waiting for message save");
        return false;
    }
    if (!backend->protocolReady()) {
        (void)length; (void)explicitDest;
        Serial.println("[SERIAL] LXMF test failed: rust backend protocol not ready");
        return false;
    }
    static constexpr size_t kMaxDiagnosticLxmfChars = 512;
    if (length == 0 || length > kMaxDiagnosticLxmfChars) {
        Serial.printf("[SERIAL] LXMF test length out of range: %u (allowed 1..%u)\n",
                      (unsigned)length, (unsigned)kMaxDiagnosticLxmfChars);
        return false;
    }

    rs::Bytes destHash;
    std::string peerLabel;
    if (!selectDiagnosticPeer(explicitDest, destHash, peerLabel)) return false;

    char payload[kMaxDiagnosticLxmfChars + 1];
    makeDiagnosticLxmfPayload(payload, length);
    const auto submitted = backend->lxmfSubmit(destHash.data(), nullptr, 0,
        reinterpret_cast<const uint8_t*>(payload), length, false);
    const bool ok = submitted.accepted();
    if (ok) diagnosticSend = submitted.ticket;
    Serial.printf("[SERIAL] LXMF test %s: len=%u dest=%s queue=%d rejection=%u\n",
                  ok ? "save pending" : "rejected",
                  (unsigned)length,
                  peerLabel.c_str(),
                  backend->lxmfQueuedCount(), static_cast<unsigned>(submitted.rejection));
    return ok;
}

bool DeviceDiagnostics::buildDiagnosticHeader2(uint8_t packetType, uint8_t context,
                                   const rs::Bytes& destHash, const uint8_t* payload,
                                   size_t payloadLen, rs::Bytes& out) {
    if (destHash.size() != 16) return false;
    uint8_t raw[RNODE_DIAG_SINGLE_MTU];
    size_t rawLen = 0;
    out.clear();
    if (rs_handheld_rns_packet_build(1, packetType, 0, context, LITE_TRANSPORT_ID,
                                     destHash.data(), payload, payloadLen, raw, sizeof(raw),
                                     &rawLen) !=
        RS_HANDHELD_OK)
        return false;
    out.append(raw, rawLen);
    return true;
}

bool DeviceDiagnostics::buildDiagnosticLinkPacket(uint8_t packetType, uint8_t context, const uint8_t* payload,
                                      size_t payloadLen, rs::Bytes& out) {
    if (diagnosticLiteLinkId.size() != 16) {
        Serial.println("[SERIAL] lite link diag failed: send J [dest_hash] first");
        return false;
    }
    uint8_t raw[RNODE_DIAG_SINGLE_MTU];
    size_t rawLen = 0;
    out.clear();
    if (rs_handheld_rns_packet_build(0, packetType, 3, context, nullptr,
                                     diagnosticLiteLinkId.data(), payload, payloadLen, raw,
                                     sizeof(raw), &rawLen) != RS_HANDHELD_OK)
        return false;
    out.append(raw, rawLen);
    return true;
}

void DeviceDiagnostics::fillDiagnosticPayload(uint8_t* payload, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++) {
        payload[i] = (uint8_t)(seed + i);
    }
}

bool DeviceDiagnostics::sendDiagnosticLiteHeader2Data(size_t length, const char* explicitDest) {
    static constexpr size_t kMaxDiagnosticTransportPayload = 160;
    if (length == 0 || length > kMaxDiagnosticTransportPayload) {
        Serial.printf("[SERIAL] usage: H<len> [dest_hash], length 1..%u\n",
                      (unsigned)kMaxDiagnosticTransportPayload);
        return false;
    }

    rs::Bytes destHash;
    std::string peerLabel;
    if (!selectDiagnosticPeer(explicitDest, destHash, peerLabel)) return false;

    uint8_t payload[kMaxDiagnosticTransportPayload];
    fillDiagnosticPayload(payload, length, 0x48);

    rs::Bytes raw;
    if (!buildDiagnosticHeader2(0x00, 0x00, destHash, payload, length, raw)) {
        Serial.println("[SERIAL] lite Header2 data build failed");
        return false;
    }

    Serial.printf("[SERIAL] lite Header2 DATA to Heltec transport, dest=%s payload=%u\n",
                  peerLabel.c_str(), (unsigned)length);
    return sendDiagnosticRawReticulum(raw, "H2-DATA");
}

bool DeviceDiagnostics::sendDiagnosticLiteLinkRequest(const char* explicitDest) {
    rs::Bytes destHash;
    std::string peerLabel;
    if (!selectDiagnosticPeer(explicitDest, destHash, peerLabel)) return false;

    uint8_t payload[64];
    fillDiagnosticPayload(payload, sizeof(payload), 0xA5);

    rs::Bytes raw;
    if (!buildDiagnosticHeader2(0x02, 0x00, destHash, payload, sizeof(payload), raw)) {
        Serial.println("[SERIAL] lite link request build failed");
        return false;
    }

    uint8_t linkId[16];
    if (rs_handheld_rns_link_id(destHash.data(), payload, sizeof(payload), linkId) != RS_HANDHELD_OK)
        return false;
    diagnosticLiteLinkId = rs::Bytes(linkId, sizeof(linkId));
    Serial.printf("[SERIAL] lite LINKREQUEST to Heltec transport, dest=%s link=%s\n",
                  peerLabel.c_str(), diagnosticLiteLinkId.toHex().c_str());
    return sendDiagnosticRawReticulum(raw, "LINKREQUEST");
}

bool DeviceDiagnostics::sendDiagnosticLiteLinkData(const char* contextArg) {
    uint8_t context = 0x0E;  // Channel
    if (!parseSerialContextByte(contextArg, context, context)) return false;

    uint8_t payload[24];
    fillDiagnosticPayload(payload, sizeof(payload), context);

    rs::Bytes raw;
    if (!buildDiagnosticLinkPacket(0x00, context, payload, sizeof(payload), raw)) {
        Serial.println("[SERIAL] lite link data build failed");
        return false;
    }

    Serial.printf("[SERIAL] lite LINK DATA context=0x%02X link=%s\n",
                  context, diagnosticLiteLinkId.toHex().c_str());
    return sendDiagnosticRawReticulum(raw, "LINK-DATA");
}

bool DeviceDiagnostics::sendDiagnosticLiteLinkProof(const char* contextArg) {
    uint8_t context = 0xFD;  // LinkProof
    if (!parseSerialContextByte(contextArg, context, context)) return false;

    uint8_t payload[64];
    fillDiagnosticPayload(payload, sizeof(payload), 0x7A);

    rs::Bytes raw;
    if (!buildDiagnosticLinkPacket(0x03, context, payload, sizeof(payload), raw)) {
        Serial.println("[SERIAL] lite link proof build failed");
        return false;
    }

    Serial.printf("[SERIAL] lite LINK PROOF context=0x%02X link=%s\n",
                  context, diagnosticLiteLinkId.toHex().c_str());
    return sendDiagnosticRawReticulum(raw, "LINK-PROOF");
}

void DeviceDiagnostics::handleSerialLineCommand(const char* line) {
    if (!line || !*line) return;

    switch ((char)std::toupper((unsigned char)line[0])) {
        case 'U': {
            diagnostics::RemoteUiRequest request;
            const char* error = nullptr;
            if (!diagnostics::parseRemoteUi(line, request)) error = "invalid";
            else if (!remoteUi) error = "unsupported";
            else {
                const auto admission = remoteUi->submit(request);
                if (admission == diagnostics::RemoteUiBridge::Admission::Busy) error = "busy";
                if (admission == diagnostics::RemoteUiBridge::Admission::Unavailable) error = "unavailable";
            }
            if (error) Serial.printf("[UICTRL] {\"id\":%lu,\"ok\":false,\"error\":\"%s\"}\n",
                static_cast<unsigned long>(request.id), error);
            break;
        }
        case 'F': {
            int32_t value = 0;
            if (!diagnostics::parseInteger(line + 1, value) || value < 0) {
                Serial.println("[SERIAL] usage: F<frequency_hz>, for example F915000000");
                return;
            }
            setDiagnosticFrequency((uint32_t)value);
            break;
        }
        case 'P': {
            int32_t value = 0;
            if (!diagnostics::parseInteger(line + 1, value)) {
                Serial.println("[SERIAL] usage: P<tx_power_dbm>, for example P1 or P5");
                return;
            }
            setDiagnosticTxPower((int)value);
            break;
        }
        case 'L': {
            int32_t length = 0;
            const char* rest = nullptr;
            if (!diagnostics::parseInteger(line + 1, length, &rest) || length <= 0) {
                Serial.println("[SERIAL] usage: L<payload_chars> [dest_hash], for example L120");
                return;
            }
            sendDiagnosticLxmf((size_t)length, rest);
            break;
        }
        case 'H': {
            int32_t length = 0;
            const char* rest = nullptr;
            if (!diagnostics::parseInteger(line + 1, length, &rest) || length <= 0) {
                Serial.println("[SERIAL] usage: H<len> [dest_hash], for example H32 2db8...");
                return;
            }
            sendDiagnosticLiteHeader2Data((size_t)length, rest);
            break;
        }
        case 'J': {
            sendDiagnosticLiteLinkRequest(line + 1);
            break;
        }
        case 'K': {
            sendDiagnosticLiteLinkData(line + 1);
            break;
        }
        case 'Y': {
            sendDiagnosticLiteLinkProof(line + 1);
            break;
        }
#ifdef PROTOCOL_PACKET_TRACE
        case 'W': case 'C': {
            diagnostics::TraceCommand command;
            if (!command.parse(line)) {
                Serial.println("[BENCH] invalid address, password length or port");
                break;
            }
            if (command.wifi && traceWifi) traceWifi(command.address, command.password);
            if (!command.wifi && traceTcp) traceTcp(command.address, command.port);
            // This stack object is short-lived and is never retained by a hook.
            volatile char* password = command.password;
            for (size_t i = 0; i < sizeof(command.password); ++i) password[i] = 0;
            break;
        }
#endif
        default:
            Serial.printf("[SERIAL] unknown line command '%c'\n", line[0]);
            break;
    }
}

void DeviceDiagnostics::printSerialHelp() {
    Serial.println("[SERIAL] commands: ? help | a announce | t raw-test | d diag | r rssi | i irq | p tx-power-cycle | m min-power | q iq | +/- freq");
    Serial.println("[SERIAL] line commands: F<hz> exact-frequency | P<dBm> exact-tx-power | L<len> [dest_hash] LXMF test");
    Serial.println("[SERIAL] lite relay diag: H<len> [dest] Header2 data | J [dest] linkreq | K<ctx_hex> link-data | Y<ctx_hex> link-proof");
    if (remoteUi) Serial.println("[SERIAL] UI: U <id> view [offset] | U <id> hold | U <id> key <up/down/left/right/enter/backspace/escape/tab> | U <id> char <32..126> [ctrl]");
    if (boardHelp) Serial.println(boardHelp);
}

bool DeviceDiagnostics::parseSerialDestinationHash(const char* p, rs::Bytes& hash) {
    uint8_t bytes[16];
    if (!diagnostics::parseDestination(p, bytes)) return false;
    hash = rs::Bytes(bytes, sizeof(bytes));
    return true;
}
bool DeviceDiagnostics::parseSerialContextByte(const char* p, uint8_t fallback, uint8_t& context) {
    int32_t parsed = fallback;
    if (diagnostics::hasArgument(p) &&
        (!diagnostics::parseInteger(p, parsed, nullptr, 16) || parsed < 0 || parsed > 255)) {
        Serial.println("[SERIAL] invalid context; expected one hex byte");
        return false;
    }
    context = static_cast<uint8_t>(parsed);
    return true;
}
void DeviceDiagnostics::nudgeDiagnosticFrequency(int32_t deltaHz) {
    const int64_t next = static_cast<int64_t>(radio.getFrequency()) + deltaHz;
    if (next < 150000000 || next > 960000000) {
        Serial.println("[SERIAL] frequency adjustment out of range");
        return;
    }
    setDiagnosticFrequency(static_cast<uint32_t>(next));
}
void DeviceDiagnostics::runRadioTest() {
    assertDeviceOwner();
    const bool accepted = radioOnline && rustLoraIface.sendRaw(
        reinterpret_cast<const uint8_t*>(rawPayload), std::strlen(rawPayload));
    Serial.printf("[TEST] raw packet %s (%u bytes)\n", accepted ? "accepted" : "rejected",
                  static_cast<unsigned>(std::strlen(rawPayload)));
}
bool DeviceDiagnostics::sendDiagnosticRawReticulum(const rs::Bytes& raw, const char* label) {
    const bool accepted = radioOnline && !raw.empty() && raw.size() <= RNODE_DIAG_SINGLE_MTU &&
                          rustLoraIface.sendRaw(raw.data(), raw.size());
    Serial.printf("[SERIAL] lite diag %s %s raw=%u\n", label,
                  accepted ? "accepted" : "rejected", static_cast<unsigned>(raw.size()));
    return accepted;
}
void DeviceDiagnostics::startIrqMonitor() {
    assertDeviceOwner();
    if (radioOnline) irqWindow.toggle(static_cast<uint32_t>(millis()));
}
void DeviceDiagnostics::startRssiMonitor() {
    assertDeviceOwner();
    if (!radioOnline) { Serial.println("[RSSI] Radio offline"); return; }
    rssiWindow.toggle(static_cast<uint32_t>(millis()));
    if (rssiWindow.active()) {
        rssiMin = 0; rssiMax = -200; rssiSamples = 0;
        Serial.println("[RSSI] Sampling for 5 seconds...");
    } else {
        Serial.printf("[RSSI] Stopped: %d samples, min=%d max=%d dBm\n", rssiSamples, rssiMin, rssiMax);
    }
}
void DeviceDiagnostics::pollSamples() {
    assertDeviceOwner();
    const uint32_t now = millis();
    sampleMemory(now);
    if (memory.reportDue(now)) printMemory();
    const bool wasActive = rssiWindow.active();
    if (rssiWindow.due(now) && radioOnline) {
        const int rssi = radio.currentRssi();
        if (rssi < rssiMin) rssiMin = rssi;
        if (rssi > rssiMax) rssiMax = rssi;
        ++rssiSamples;
        Serial.printf("[RSSI] %d dBm\n", rssi);
    }
    if (wasActive && !rssiWindow.active())
        Serial.printf("[RSSI] Done: %d samples, min=%d max=%d dBm\n", rssiSamples, rssiMin, rssiMax);
    if (irqWindow.due(now) && radioOnline)
        Serial.printf("[IRQ] flags=0x%04X rssi=%d\n", radio.getIrqFlags(), radio.currentRssi());
}
void DeviceDiagnostics::sampleMemory(uint32_t now) {
    if (!memory.due(now)) return;
    auto sample = [](HeapObservation& value, uint32_t caps) {
        value.add(heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps),
                  heap_caps_get_minimum_free_size(caps));
    };
    sample(memory.internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    sample(memory.psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    memory.sampled(now);
}
void DeviceDiagnostics::printMemory() const {
    auto print = [this](const char* region, const HeapObservation& value) {
        Serial.printf("[MEMORY] region=%s samples=%lu free=%lu largest=%lu sampled_min_free=%lu sampled_min_largest=%lu allocator_min_free=%lu\n",
            region, (unsigned long)memory.samples, (unsigned long)value.free,
            (unsigned long)value.largest, (unsigned long)value.sampledMinimumFree,
            (unsigned long)value.sampledMinimumLargest, (unsigned long)value.allocatorMinimum);
    };
    print("internal8", memory.internal);
    print("psram8", memory.psram);
}
void DeviceDiagnostics::pollResults() {
    assertDeviceOwner();
    const char* readyData = nullptr;
    size_t readyLength = 0;
    if (remoteUi && remoteUi->result(readyData, readyLength)) {
        bool connected = true;
        size_t available = diagnostics::RemoteUiReplyDelivery::TxCapacity;
#if (defined(RSDECK) && ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE) || defined(RSM9)
        // Admit whole replies to the bounded HWCDC/UART transmit queue.
        // On HWCDC this also lets IN_EMPTY establish the connection and
        // avoids its offline FIFO path silently discarding the reply.
        connected = static_cast<bool>(Serial);
        const int writable = connected ? Serial.availableForWrite() : 0;
        available = writable > 0 ? static_cast<size_t>(writable) : 0;
#endif
        remoteUiDelivery.poll(*remoteUi, static_cast<uint32_t>(millis()), connected, available,
            [](const char* data, size_t length) {
                // One mutex-protected write preserves framing beside logs.
                return Serial.write(reinterpret_cast<const uint8_t*>(data), length);
            });
    }
    if (diagnosticSend.valid()) {
        outgoing::InitialResult result;
        const auto state = backend->lxmfPoll(diagnosticSend, result);
        if (state == outgoing::Poll::Ready) {
            const auto ticket = diagnosticSend;
            diagnosticSend = {};
            backend->lxmfAcknowledge(ticket);
            Serial.printf("[SERIAL] LXMF test %s: record=%lu storage=%u sending=%s\n",
                result.outcome == storage::Outcome::Committed ? "saved" : "not saved",
                static_cast<unsigned long>(result.key.counter), static_cast<unsigned>(result.error),
                result.txSuppressed ? "cancelled" :
                    result.outcome == storage::Outcome::Committed ? "eligible" : "not started");
        } else if (state == outgoing::Poll::Invalid) {
            diagnosticSend = {};
            Serial.println("[SERIAL] LXMF test result unavailable");
        }
    }
}
void DeviceDiagnostics::poll() {
    assertDeviceOwner();
    pollResults();
    // Bound both byte consumption and commands. A flood cannot monopolize the
    // service loop by executing dozens of expensive diagnostics in one tick.
    for (unsigned read = 0; read < 96 && Serial.available() > 0; ++read) {
        const char c = static_cast<char>(Serial.read());
        const auto result = commands.feed(c);
        if (result == diagnostics::SerialCommand::Result::None) continue;
        if (result == diagnostics::SerialCommand::Result::Rejected) {
            commands.consume();
            Serial.println("[SERIAL] invalid or overlong command discarded");
        } else if (result == diagnostics::SerialCommand::Result::Line) {
            handleSerialLineCommand(commands.line());
            commands.consume();
        } else {
            switch (c) {
                case '?': printSerialHelp(); break;
                case 'a': case 'A': manualAnnounce(); break;
                case 't': case 'T': runRadioTest(); break;
                case 'd': case 'D': printDiagnostics(); break;
                case 'r': case 'R': startRssiMonitor(); break;
                case 'i': case 'I': startIrqMonitor(); break;
                case 'p': cycleDiagnosticTxPower(); break;
                case 'm': case 'M': setDiagnosticMinTxPower(); break;
                case 'q': case 'Q': toggleDiagnosticInvertIQ(); break;
                case '+': case '=': nudgeDiagnosticFrequency(1000); break;
                case '-': case '_': nudgeDiagnosticFrequency(-1000); break;
                default:
                    if (!boardCommand || !boardCommand(c))
                        Serial.println("[SERIAL] unknown command; use ? for help");
                    break;
            }
        }
        break;
    }
}
} // namespace handheld
