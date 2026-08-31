#pragma once

#include <FS.h>
#include <FSImpl.h>
#include "hal/SharedSPIBus.h"

// A directory/file can outlive SDStore::openDir(). Protect each later driver
// operation too, without keeping the SPI bus locked while a caller parses JSON
// or walks an entire directory. Descendants retain the same locking contract.
class SPIFileImpl final : public fs::FileImpl {
public:
    explicit SPIFileImpl(File file) : _file(std::move(file)) {}
    ~SPIFileImpl() override { close(); }
    size_t write(const uint8_t* data, size_t size) override {
        SharedSPILock lock; return lock.locked() ? _file.write(data, size) : 0;
    }
    size_t read(uint8_t* data, size_t size) override {
        SharedSPILock lock; return lock.locked() ? _file.read(data, size) : 0;
    }
    void flush() override { SharedSPILock lock; if (lock.locked()) _file.flush(); }
    bool seek(uint32_t pos, SeekMode mode) override {
        SharedSPILock lock; return lock.locked() && _file.seek(pos, mode);
    }
    size_t position() const override { SharedSPILock lock; return lock.locked() ? _file.position() : 0; }
    size_t size() const override { SharedSPILock lock; return lock.locked() ? _file.size() : 0; }
    bool setBufferSize(size_t size) override { SharedSPILock lock; return lock.locked() && _file.setBufferSize(size); }
    void close() override { SharedSPILock lock; if (lock.locked()) _file.close(); }
    time_t getLastWrite() override { SharedSPILock lock; return lock.locked() ? _file.getLastWrite() : 0; }
    const char* path() const override { return _file.path(); }
    const char* name() const override { return _file.name(); }
    boolean isDirectory() override { SharedSPILock lock; return lock.locked() && _file.isDirectory(); }
    fs::FileImplPtr openNextFile(const char* mode) override {
        SharedSPILock lock;
        if (!lock.locked()) return {};
        File child = _file.openNextFile(mode);
        return child ? std::make_shared<SPIFileImpl>(std::move(child)) : fs::FileImplPtr();
    }
    boolean seekDir(long position) override { SharedSPILock lock; return lock.locked() && _file.seekDir(position); }
    String getNextFileName() override { SharedSPILock lock; return lock.locked() ? _file.getNextFileName() : String(); }
    String getNextFileName(bool* isDir) override {
        SharedSPILock lock; return lock.locked() ? _file.getNextFileName(isDir) : String();
    }
    void rewindDirectory() override { SharedSPILock lock; if (lock.locked()) _file.rewindDirectory(); }
    operator bool() override { return bool(_file); }
private:
    File _file;
};

inline File sharedSPIFile(File file) {
#if defined(RSCARDPUTER)
    return file;
#else
    return file ? File(std::make_shared<SPIFileImpl>(std::move(file))) : File();
#endif
}
