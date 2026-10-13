#pragma once

#include <atomic>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <new>

namespace metvoice::core {

// Standard x86_64 cache line alignment (64 bytes) to avoid false sharing
constexpr size_t CACHE_LINE_SIZE = 64;


/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer.
 * 
 * Specifically optimized for real-time audio threads:
 * - Cache-line aligned read/write heads to prevent false sharing.
 * - Power-of-two capacity for fast modulo operations via bitwise AND.
 * - Acquire-Release memory ordering semantics.
 * - Overrun and underrun atomic telemetry counters.
 * - Zero dynamic allocations during read/write operations.
 */
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t minimum_capacity = 4096)
        : capacity_(round_up_power_of_two(std::max<size_t>(minimum_capacity, 64))),
          mask_(capacity_ - 1),
          storage_(capacity_) {
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
        underrun_count_.store(0, std::memory_order_relaxed);
        overrun_count_.store(0, std::memory_order_relaxed);
    }

    ~SpscRingBuffer() = default;

    // Non-copyable, non-movable for thread safety
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    [[nodiscard]] size_t capacity() const noexcept {
        return capacity_;
    }

    /**
     * @brief Number of elements currently available to read.
     * Safe to call from any thread, but strictly accurate on consumer.
     */
    [[nodiscard]] size_t available_read() const noexcept {
        const size_t w = write_pos_.load(std::memory_order_acquire);
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        return w - r;
    }

    /**
     * @brief Number of elements space currently available to write.
     * Safe to call from any thread, but strictly accurate on producer.
     */
    [[nodiscard]] size_t available_write() const noexcept {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        const size_t r = read_pos_.load(std::memory_order_acquire);
        const size_t occupied = w - r;
        return (occupied < capacity_) ? (capacity_ - occupied) : 0;
    }

    /**
     * @brief Write elements into the ring buffer (Producer thread only).
     * @param src Pointer to input array.
     * @param count Number of elements to write.
     * @return Number of elements actually written. If less than count, an overrun is counted.
     */
    size_t write(const T* src, size_t count) noexcept {
        if (count == 0 || src == nullptr) return 0;

        const size_t w = write_pos_.load(std::memory_order_relaxed);
        const size_t r = read_pos_.load(std::memory_order_acquire);
        const size_t occupied = w - r;
        const size_t available = (occupied < capacity_) ? (capacity_ - occupied) : 0;

        const size_t to_write = std::min(count, available);
        if (to_write < count) {
            overrun_count_.fetch_add(count - to_write, std::memory_order_relaxed);
        }

        if (to_write == 0) return 0;

        const size_t start_idx = w & mask_;
        const size_t first_chunk = std::min(to_write, capacity_ - start_idx);
        const size_t second_chunk = to_write - first_chunk;

        std::memcpy(&storage_[start_idx], src, first_chunk * sizeof(T));
        if (second_chunk > 0) {
            std::memcpy(&storage_[0], src + first_chunk, second_chunk * sizeof(T));
        }

        write_pos_.store(w + to_write, std::memory_order_release);
        return to_write;
    }

    /**
     * @brief Read elements from the ring buffer (Consumer thread only).
     * @param dst Pointer to destination array.
     * @param count Number of elements to read.
     * @return Number of elements actually read. If less than count, an underrun is counted.
     */
    size_t read(T* dst, size_t count) noexcept {
        if (count == 0 || dst == nullptr) return 0;

        const size_t w = write_pos_.load(std::memory_order_acquire);
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        const size_t available = w - r;

        const size_t to_read = std::min(count, available);
        if (to_read < count) {
            underrun_count_.fetch_add(count - to_read, std::memory_order_relaxed);
        }

        if (to_read == 0) return 0;

        const size_t start_idx = r & mask_;
        const size_t first_chunk = std::min(to_read, capacity_ - start_idx);
        const size_t second_chunk = to_read - first_chunk;

        std::memcpy(dst, &storage_[start_idx], first_chunk * sizeof(T));
        if (second_chunk > 0) {
            std::memcpy(dst + first_chunk, &storage_[0], second_chunk * sizeof(T));
        }

        read_pos_.store(r + to_read, std::memory_order_release);
        return to_read;
    }

    /**
     * @brief Peek elements without advancing the read position (Consumer thread only).
     */
    size_t peek(T* dst, size_t count) const noexcept {
        if (count == 0 || dst == nullptr) return 0;

        const size_t w = write_pos_.load(std::memory_order_acquire);
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        const size_t available = w - r;

        const size_t to_read = std::min(count, available);
        if (to_read == 0) return 0;

        const size_t start_idx = r & mask_;
        const size_t first_chunk = std::min(to_read, capacity_ - start_idx);
        const size_t second_chunk = to_read - first_chunk;

        std::memcpy(dst, &storage_[start_idx], first_chunk * sizeof(T));
        if (second_chunk > 0) {
            std::memcpy(dst + first_chunk, &storage_[0], second_chunk * sizeof(T));
        }
        return to_read;
    }

    /**
     * @brief Discard unread samples to catch up if latency grows too high.
     */
    size_t discard(size_t count) noexcept {
        const size_t w = write_pos_.load(std::memory_order_acquire);
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        const size_t available = w - r;
        const size_t to_discard = std::min(count, available);
        if (to_discard > 0) {
            read_pos_.store(r + to_discard, std::memory_order_release);
        }
        return to_discard;
    }

    /**
     * @brief Clear the buffer completely. Safe when synchronized or before starting streams.
     */
    void reset() noexcept {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        read_pos_.store(w, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t underrun_count() const noexcept {
        return underrun_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t overrun_count() const noexcept {
        return overrun_count_.load(std::memory_order_relaxed);
    }

    void reset_metrics() noexcept {
        underrun_count_.store(0, std::memory_order_relaxed);
        overrun_count_.store(0, std::memory_order_relaxed);
    }

private:
    static size_t round_up_power_of_two(size_t n) noexcept {
        size_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    const size_t capacity_;
    const size_t mask_;
    std::vector<T> storage_;

    // Separate read and write atomics onto different cache lines
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> underrun_count_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> overrun_count_{0};
};

} // namespace metvoice::core
