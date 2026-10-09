#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Network packets are not disk blocks. Keep SD writes sector-aligned and large,
// and handle legal short writes without silently dropping their unwritten tail.
template<class Sink> class DownloadWriteBuffer {
public:
    DownloadWriteBuffer(Sink &sink, uint8_t *buffer, size_t capacity)
        : sink_(sink), buffer_(buffer), capacity_(capacity) {}
    bool append(const uint8_t *data, size_t size) {
        if (!buffer_ || !capacity_ || error_) return false;
        while (size) {
            const size_t count = size < capacity_ - used_ ? size : capacity_ - used_;
            memcpy(buffer_ + used_, data, count);
            used_ += count; data += count; size -= count;
            if (used_ == capacity_ && !flush()) return false;
        }
        return true;
    }
    bool finish() { return !error_ && flush() && sink_.sync(); }
    int error() const { return error_; }
private:
    bool flush() {
        size_t offset = 0;
        unsigned interrupted = 0;
        while (offset < used_) {
            errno = 0;
            const auto count = sink_.write(buffer_ + offset, used_ - offset);
            if (count < 0 && errno == EINTR && ++interrupted < 8) continue;
            if (count <= 0 || static_cast<size_t>(count) > used_ - offset) {
                error_ = errno ? errno : EIO;
                return false;
            }
            offset += static_cast<size_t>(count);
        }
        used_ = 0;
        return true;
    }
    Sink &sink_;
    uint8_t *buffer_;
    size_t capacity_, used_ = 0;
    int error_ = 0;
};
