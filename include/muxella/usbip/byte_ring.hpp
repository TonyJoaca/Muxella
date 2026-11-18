#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace muxella::usbip {

// Bounded thread-safe byte ring buffer matching UAC1 virtual-cable audio forwarding.
// Overflow discards oldest bytes so real-time USB workers never block.
class ByteRing {
public:
    explicit ByteRing(size_t capacity = 1)
        : buf_(std::max<size_t>(1, capacity), 0) {}

    [[nodiscard]] size_t capacity() const noexcept { return buf_.size(); }

    [[nodiscard]] size_t available() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return size_;
    }

    size_t write(const uint8_t* data, size_t len) noexcept {
        if (!data || len == 0) return 0;
        std::lock_guard<std::mutex> lock(mu_);
        const size_t original = len;
        const size_t cap = buf_.size();
        if (len > cap) {
            data += (len - cap);
            len = cap;
        }
        if (size_ + len > cap) {
            const size_t overflow = size_ + len - cap;
            read_pos_ = (read_pos_ + overflow) % cap;
            size_ -= overflow;
            dropped_ += overflow;
        }
        const size_t first = std::min(len, cap - write_pos_);
        std::memcpy(buf_.data() + write_pos_, data, first);
        if (len > first) {
            std::memcpy(buf_.data(), data + first, len - first);
        }
        write_pos_ = (write_pos_ + len) % cap;
        size_ += len;
        return original;
    }

    size_t read_silence(uint8_t* dst, size_t len) noexcept {
        if (!dst || len == 0) return 0;
        std::lock_guard<std::mutex> lock(mu_);
        const size_t cap = buf_.size();
        const size_t to_read = std::min(len, size_);
        const size_t first = std::min(to_read, cap - read_pos_);
        if (first > 0) {
            std::memcpy(dst, buf_.data() + read_pos_, first);
        }
        if (to_read > first) {
            std::memcpy(dst + first, buf_.data(), to_read - first);
        }
        if (to_read < len) {
            std::memset(dst + to_read, 0, len - to_read);
            underruns_ += (len - to_read);
        }
        read_pos_ = (read_pos_ + to_read) % cap;
        size_ -= to_read;
        return to_read;
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        read_pos_ = 0;
        write_pos_ = 0;
        size_ = 0;
    }

private:
    mutable std::mutex mu_;
    std::vector<uint8_t> buf_;
    size_t read_pos_{0};
    size_t write_pos_{0};
    size_t size_{0};
    uint64_t dropped_{0};
    uint64_t underruns_{0};
};

} // namespace muxella::usbip
