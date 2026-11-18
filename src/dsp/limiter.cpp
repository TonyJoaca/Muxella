#include "muxella/dsp/limiter.hpp"

namespace muxella::dsp {

SoftLimiter::SoftLimiter(float ceiling_db, float knee_threshold)
    : knee_threshold_(knee_threshold) {
    set_ceiling_db(ceiling_db);
}

void SoftLimiter::set_ceiling_db(float ceiling_db) {
    ceiling_linear_ = std::pow(10.0f, ceiling_db / 20.0f);
    knee_threshold_ = std::min(knee_threshold_, ceiling_linear_ * 0.95f);
}

void SoftLimiter::reset() noexcept {
    clip_events_.store(0, std::memory_order_relaxed);
}

float SoftLimiter::limit_sample(float x) noexcept {
    const float sign = (x < 0.0f) ? -1.0f : 1.0f;
    const float abs_x = std::abs(x);

    if (abs_x <= knee_threshold_) {
        return x;
    }

    clip_events_.fetch_add(1, std::memory_order_relaxed);

    // Smooth rational transition above knee up to ceiling
    const float diff = abs_x - knee_threshold_;
    const float range = ceiling_linear_ - knee_threshold_;
    const float saturated = knee_threshold_ + range * std::tanh(diff / range);
    return sign * saturated;
}

void SoftLimiter::process(float* const* channel_ptrs, size_t channels, size_t frames) noexcept {
    if (channels == 0 || frames == 0 || channel_ptrs == nullptr) return;

    for (size_t ch = 0; ch < channels; ++ch) {
        float* data = channel_ptrs[ch];
        for (size_t f = 0; f < frames; ++f) {
            data[f] = limit_sample(data[f]);
        }
    }
}

} // namespace muxella::dsp
