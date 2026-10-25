#pragma once

#include "StorageContract.h"
#include <FS.h>
#include <algorithm>
#include <cstring>

namespace handheld::storage {

class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual size_t write(const uint8_t*, size_t) = 0;
    size_t write(uint8_t byte) { return write(&byte, 1); }
};

// A source emits the same bytes on both passes: write and readback comparison.
// Its lifetime is bounded by the synchronous storage transaction, never queued.
class AtomicSource {
public:
    virtual ~AtomicSource() = default;
    virtual size_t length() const = 0;
    virtual bool emit(ByteSink&) const = 0;
    // After both passes, release handles that would block atomic promotion on
    // ESP LittleFS. No input is read again after this point, even on rollback.
    virtual void releaseInputs() const {}
};

class MemorySource final : public AtomicSource {
public:
    MemorySource(const uint8_t* bytes, size_t length) : _bytes(bytes), _length(length) {}
    size_t length() const override { return _length; }
    bool emit(ByteSink& sink) const override {
        return (!_length || _bytes) && sink.write(_bytes, _length) == _length;
    }
private:
    const uint8_t* _bytes;
    size_t _length;
};

// Buffer the serializer's single-byte writes into actual file operations. The
// comparison pass uses the same buffer plus a second 512-byte readback buffer.
class FileSink final : public ByteSink {
public:
    FileSink(File& file, uint8_t* scratch, size_t expected, bool compare)
        : _file(file), _scratch(scratch), _expected(expected), _compare(compare) {}
    size_t write(const uint8_t* bytes, size_t length) override {
        if (!_ok || (!bytes && length) || length > _expected - _total) return 0;
        const size_t original = length;
        while (length) {
            const size_t count = std::min(length, size_t(512) - _buffered);
            memcpy(_scratch + _buffered, bytes, count);
            _buffered += count; _total += count; bytes += count; length -= count;
            if (_buffered == 512 && !flush()) return 0;
        }
        return original;
    }
    bool finish() { return _ok && _total == _expected && flush(); }
private:
    bool flush() {
        if (!_buffered) return _ok;
        if (_compare) {
            _ok = _file.read(_scratch + 512, _buffered) == _buffered &&
                memcmp(_scratch, _scratch + 512, _buffered) == 0;
        } else _ok = _file.write(_scratch, _buffered) == _buffered;
        _buffered = 0;
        yield();
        return _ok;
    }
    File& _file;
    uint8_t* _scratch;
    size_t _expected, _total = 0, _buffered = 0;
    bool _compare, _ok = true;
};

// Both media use this one existing tmp/verify/backup/promote contract. The
// caller holds its filesystem transaction lease; driver calls retain their own
// short bus locks. Failure never removes the sole previous committed primary
// or backup. A leftover tmp is never treated as a committed record.
template<class Store>
Error atomicStream(Store& store, const char* path, const AtomicSource& source) {
    if (!path || path[0] != '/' || !store.isReady()) return Error::Unavailable;
    const size_t length = source.length();
    const String temporary = String(path) + ".tmp", backup = String(path) + ".bak";
    uint8_t scratch[Budget::IoScratch];
    {
        File file = store.openFile(temporary.c_str(), "w");
        if (!file) return Error::Write;
        FileSink sink(file, scratch, length, false);
        const bool written = source.emit(sink) && sink.finish();
        file.flush(); file.close();
        if (!written) { store.remove(temporary.c_str()); return Error::Write; }
    }
    {
        File file = store.openFile(temporary.c_str(), "r");
        if (!file || file.size() != length) {
            file.close(); store.remove(temporary.c_str()); return Error::Verify;
        }
        FileSink sink(file, scratch, length, true);
        const bool verified = source.emit(sink) && sink.finish();
        file.close();
        if (!verified) { store.remove(temporary.c_str()); return Error::Verify; }
    }
    source.releaseInputs();
    if (store.exists(path)) {
        if (store.exists(backup.c_str()) && !store.remove(backup.c_str())) return Error::Rename;
        if (!store.rename(path, backup.c_str())) {
            store.remove(temporary.c_str()); return Error::Rename;
        }
    }
    if (!store.rename(temporary.c_str(), path)) {
        if (store.exists(backup.c_str())) store.rename(backup.c_str(), path);
        store.remove(temporary.c_str());
        return Error::Rename;
    }
    store.remove(backup.c_str());
    return Error::None;
}

} // namespace handheld::storage
