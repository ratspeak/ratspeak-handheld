#pragma once
#include "diagnostics/SerialCommand.h"
#include "diagnostics/TraceCommand.h"
#include "diagnostics/RemoteUi.h"
#include "protocol/ProtocolBackend.h"
#include "reticulum/AnnounceManager.h"
#include "transport/LoRaInterface.h"
#include "runtime/RuntimeMetrics.h"

namespace handheld {
// Called only by the protocol owner. Board-specific rails, GPIO and UI remain
// in the board composition; diagnostic packets use the ordinary LoRa queue.
class DeviceDiagnostics {
public:
    DeviceDiagnostics(BoardRadio& radio, LoRaInterface& lora, ProtocolBackend& backend,
                      AnnounceManager*& nodes, bool& online, const char* boardName,
                      const char* payloadPrefix, const char* rawPayload, void (*announce)())
        : radio(radio), rustLoraIface(lora), backend(&backend), announceManager(nodes),
          radioOnline(online), boardName(boardName), payloadPrefix(payloadPrefix),
          rawPayload(rawPayload), manualAnnounce(announce) {}
    void (*extraDump)() = nullptr;
    bool (*boardCommand)(char) = nullptr;
    const char* boardHelp = nullptr;
    diagnostics::RemoteUiBridge* remoteUi = nullptr;
#ifdef PROTOCOL_PACKET_TRACE
    void (*traceWifi)(const char*, const char*) = nullptr;
    void (*traceTcp)(const char*, uint16_t) = nullptr;
#endif
    void poll();
    // Settlement only: never reads serial bytes or admits another command.
    void pollResults();
    bool resultsDrained() const { return !diagnosticSend.valid() && (!remoteUi || remoteUi->drained()); }
    void pollSamples();
    void printDiagnostics();
    void runRadioTest();
    void startRssiMonitor();
    void startIrqMonitor();
private:
    void sampleMemory(uint32_t now);
    void printMemory() const;
    HeapObservations memory;
    BoardRadio& radio;
    LoRaInterface& rustLoraIface;
    ProtocolBackend* backend;
    AnnounceManager*& announceManager;
    bool& radioOnline;
    const char* boardName;
    const char* payloadPrefix;
    const char* rawPayload;
    void (*manualAnnounce)();
    diagnostics::SerialCommand commands;
    diagnostics::RemoteUiReplyDelivery remoteUiDelivery;
    diagnostics::SampleWindow rssiWindow, irqWindow;
    int rssiMin = 0, rssiMax = -200, rssiSamples = 0;
    rs::Bytes diagnosticLiteLinkId;
    outgoing::Ticket diagnosticSend;
    static constexpr size_t RNODE_DIAG_SINGLE_MTU = 254;
    bool parseSerialDestinationHash(const char*, rs::Bytes&);
    bool parseSerialContextByte(const char*, uint8_t, uint8_t&);
    bool sendDiagnosticRawReticulum(const rs::Bytes&, const char*);
    void nudgeDiagnosticFrequency(int32_t);
    void cycleDiagnosticTxPower();
    void setDiagnosticMinTxPower();
    bool setDiagnosticTxPower(int powerDbm);
    void toggleDiagnosticInvertIQ();
    bool setDiagnosticFrequency(uint32_t frequencyHz);
    bool selectDiagnosticPeer(const char* explicitArg, rs::Bytes& destHash, std::string& label);
    void makeDiagnosticLxmfPayload(char* output, size_t length);
    bool sendDiagnosticLxmf(size_t length, const char* explicitDest);
    bool buildDiagnosticHeader2(uint8_t packetType, uint8_t context,
                                   const rs::Bytes& destHash, const uint8_t* payload,
                                   size_t payloadLen, rs::Bytes& out);
    bool buildDiagnosticLinkPacket(uint8_t packetType, uint8_t context, const uint8_t* payload,
                                      size_t payloadLen, rs::Bytes& out);
    void fillDiagnosticPayload(uint8_t* payload, size_t len, uint8_t seed);
    bool sendDiagnosticLiteHeader2Data(size_t length, const char* explicitDest);
    bool sendDiagnosticLiteLinkRequest(const char* explicitDest);
    bool sendDiagnosticLiteLinkData(const char* contextArg);
    bool sendDiagnosticLiteLinkProof(const char* contextArg);
    void handleSerialLineCommand(const char* line);
    void printSerialHelp();
};
} // namespace handheld
