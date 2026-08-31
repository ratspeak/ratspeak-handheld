#pragma once
#include <cstdint>

// Single-owner retry bookkeeping. A successful mirror is not rewritten until
// the value changes; an attempted, failed mirror remains an obligation even if
// its medium temporarily disappears. A never-present optional mirror is not.
class MirrorWriteState {
public:
    enum Medium : uint8_t { Flash = 1, SD = 2 };
    void changed() {
        // A settled generation has no outstanding obligations. Optional media
        // may be absent for the next write; only failed obligations carry over.
        if (!_dirty) _required = 0;
        ++_generation; _done = 0; _dirty = true;
    }
    void require(uint8_t media) { _required |= media; }
    bool needs(Medium medium) const { return _dirty && (_required & medium) && !(_done & medium); }
    void completed(Medium medium, bool ok) { if (ok) _done |= medium; }
    bool settle() {
        if (_required && _done == _required) _dirty = false;
        return !_dirty;
    }
    bool dirty() const { return _dirty; }
    uint32_t generation() const { return _generation; }
    void reset() { *this = {}; }
private:
    uint32_t _generation = 0;
    uint8_t _required = 0, _done = 0;
    bool _dirty = false;
};
