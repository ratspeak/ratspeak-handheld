#pragma once

#include <cstdint>

namespace handheld::history {

// A projection read failure is not a delivery state. Keep the known message
// facts visible, retry normally, and offer one contextual action only after
// consecutive failed passes span this grace period. Stored in the existing
// page metadata reserve, so neither frontend needs a timer or another cache.
class StatusRefresh {
public:
    static constexpr uint32_t GraceMs = 30000;
    void finish(bool unavailable, uint32_t now) {
        if (!unavailable) { *this = {}; return; }
        if (!_failing) { _since = now; _failing = true; }
        if (uint32_t(now - _since) >= GraceMs) _delayed = true;
    }
    bool delayed() const { return _delayed; }
private:
    uint32_t _since = 0;
    bool _failing = false, _delayed = false;
};

} // namespace handheld::history
