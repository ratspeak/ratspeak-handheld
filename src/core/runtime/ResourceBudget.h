#pragma once

#include "storage/StorageContract.h"
#include <cstddef>

namespace handheld {
// Approved ceilings, shared by admission and qualification. This is not a heap
// availability estimate: SDK allocations and fragmentation require live floors.
// Each owner must enforce its row before integrated release qualification.
struct ResourceBudget {
    static constexpr size_t MicroNode = 32768;
    static constexpr size_t ProtocolContext = 9216;
    static constexpr size_t ResourceBuffer = 4096;
    static constexpr size_t ResourceDirections = 2;
    static constexpr size_t DirectCodecScratch = 4096;
    static constexpr size_t OutgoingEntries = 20;
    static constexpr size_t OutgoingDescriptor = 320;
    static constexpr size_t OutgoingDescriptors = OutgoingEntries * OutgoingDescriptor;
    static constexpr size_t OutgoingBody = 4096;
    static constexpr size_t CanvasHistoryText = 4096;
    static constexpr size_t CanvasHistoryRows = 1024;
    // Two independent 400-byte drafts, peer/identity bindings and editor state.
    static constexpr size_t CanvasDraft = 1280;
    static constexpr size_t SummaryEntry = 160;
    static constexpr size_t CardSummaryEntries = 16;
    static constexpr size_t LargeSummaryEntries = 64;
    static constexpr size_t DirectoryBatch = 8 * 128;
    static constexpr size_t TcpOwners = 4; // active plus not-yet-retired objects
    static constexpr size_t TcpDescriptor = 1024;
    static constexpr size_t TcpCodec = 8194;
    static constexpr size_t TcpConnectStack = 4096;
    static constexpr size_t AutoPeers = 16; // preserves configured maximum
    static constexpr size_t LoRaRetainedBytes = 4096;
    static constexpr size_t PendingFrame = 640;
    // Seven signed packets with original lifetimes, pacing and bounded replay followups.
    // Their bytes also serve as the bounded exact-tag replay cache (no duplicate pool).
    static constexpr size_t PathResponses = 7 * 720;
    static constexpr size_t InputEvents = 8;
    static constexpr size_t InputEventBytes = 16;
    static constexpr size_t InputRetention = InputEvents * InputEventBytes;
    static constexpr size_t LauncherSharedState = 4096;
    static constexpr size_t DiagnosticsState = 320;
    static constexpr size_t DiagnosticsPayload = 513;
    static constexpr size_t MainStack = 16384;
    static constexpr size_t ServiceStack = 24576;
    static constexpr size_t ServiceStackReserve = ServiceStack / 4;
    static constexpr size_t WorkerStackReserve = storage::Budget::WorkerStack / 4;
    static constexpr size_t CardInternalFree = 12288;
    static constexpr size_t CardLargestBlock = 8192;
    static constexpr size_t LargeInternalFree = 32768;
    static constexpr size_t LargeLargestBlock = 16384;
    static constexpr size_t LvglAllocations = 2 * 1024 * 1024;
    static constexpr size_t PsramFree = 256 * 1024;

    static constexpr size_t summaryBytes(bool cardputer) {
        return (cardputer ? CardSummaryEntries : LargeSummaryEntries) * SummaryEntry;
    }
    static constexpr size_t tcpBytes(bool cardputer) {
        return TcpOwners * (TcpDescriptor + TcpConnectStack) +
               (cardputer ? 1 : TcpOwners) * TcpCodec;
    }
};
static_assert(ResourceBudget::OutgoingDescriptors == 6400, "Review outgoing retention changes");
static_assert(ResourceBudget::InputRetention == 128, "Review input retention changes");
static_assert(ResourceBudget::CardInternalFree == storage::Budget::CardHeapFloor,
              "Storage and global internal-heap floor must agree");
static_assert(ResourceBudget::WorkerStackReserve >= 2048 &&
              ResourceBudget::ServiceStackReserve >= 4096, "Preserve stack reserves");
}
