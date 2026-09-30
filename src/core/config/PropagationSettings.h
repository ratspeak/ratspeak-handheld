#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace handheld::propagation {

enum class Selection : uint8_t { Auto = 0, Manual = 1 };
enum class Delivery : uint8_t { Auto = 0, Always = 1 };

enum class SyncStatus : uint8_t { Off, Idle, Waiting, Connecting, Listing, Receiving, Saving, Purging,
    Complete, Unavailable, Unsupported, Invalid, StorageError, SourceUnknown, StampCostHigh };
struct SyncView {
    uint8_t node[16]{};
    SyncStatus status = SyncStatus::Off;
    uint8_t received = 0;
    bool busy = false;
};

struct NodeView {
    uint8_t address[16]{};
    char name[32]{};
    uint8_t cost = 0, hops = 0, interface = UINT8_MAX;
    bool active = false, usable = false;
};

// A value record: copying settings never allocates another address String.
// The manual pin survives AUTO/OFF and is cleared only by an explicit edit.
struct Settings {
    bool enabled = false;
    Selection selection = Selection::Auto;
    Delivery delivery = Delivery::Auto;
    bool hasManual = false;
    uint8_t manual[16]{};

    bool valid() const {
        return selection <= Selection::Manual && delivery <= Delivery::Always;
    }

    bool setManual(const char* hex, size_t length) {
        if (length != 0 && length != 32) return false;
        if (!hex && length) return false;
        uint8_t prepared[16]{};
        for (size_t i = 0; i < length; ++i) {
            const char c = hex[i];
            const int nibble = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (nibble < 0) return false;
            prepared[i / 2] |= uint8_t(nibble << (i % 2 ? 0 : 4));
        }
        std::memcpy(manual, prepared, sizeof manual);
        hasManual = length != 0;
        return true;
    }

    void manualHex(char (&out)[33]) const {
        static constexpr char digits[] = "0123456789abcdef";
        std::memset(out, 0, sizeof out);
        if (!hasManual) return;
        for (size_t i = 0; i < sizeof manual; ++i) {
            out[2*i] = digits[manual[i] >> 4];
            out[2*i+1] = digits[manual[i] & 15];
        }
    }
};
static_assert(sizeof(Settings) == 20, "Propagation settings must remain a bounded value");

} // namespace handheld::propagation
