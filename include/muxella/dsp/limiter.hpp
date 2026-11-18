#pragma once

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <atomic>

namespace muxella::dsp {

/**
 * @brief Zero-latency Soft-Knee Saturation Limiter.
 * 
 * Prevents digital clipping (exceeding 0 dBFS / ceiling) on bus summing nodes.
 * Smoothly compresses peaks via polynomial/tanh rational transfer curve.
 */
class SoftLimiter {
public:
    explicit SoftLimiter(float ceiling_db = -0.1f, float knee_threshold = 0.85f);

    void set_ceiling_db(float ceiling_db);
    void reset() noexcept;

    void process(float* const* channel_ptrs, size_t channels, size_t frames) noexcept;

    [[nodiscard]] uint64_t clip_events() const noexcept {
        return clip_events_.load(std::memory_order_relaxed);
    }

private:
    float ceiling_linear_{0.9885f}; // -0.1 dBFS
    float knee_threshold_{0.85f};
    std::atomic<uint64_t> clip_events_{0};

    [[nodiscard]] float limit_sample(float x) noexcept;
};

} // namespace muxella::dsp
