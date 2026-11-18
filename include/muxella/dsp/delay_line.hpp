#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

namespace muxella::dsp {

/**
 * @brief Pre-allocated zero-allocation stereo delay line (0 .. 200 ms)
 * for audio/video lip-sync alignment on output buses.
 */
class StereoDelayLine {
public:
    static constexpr float kMaxDelayMs = 200.0f;

    explicit StereoDelayLine(float sample_rate = 48000.0f)
        : sample_rate_(std::max(8000.0f, sample_rate)) {
        max_delay_frames_ = static_cast<size_t>(std::ceil(sample_rate_ * (kMaxDelayMs / 1000.0f))) + 8;
        buffer_len_ = max_delay_frames_ + 16;
        buf_l_.assign(buffer_len_, 0.0f);
        buf_r_.assign(buffer_len_, 0.0f);
    }

    void set_delay_ms(float delay_ms) noexcept {
        delay_ms = std::clamp(delay_ms, 0.0f, kMaxDelayMs);
        delay_ms_.store(delay_ms, std::memory_order_relaxed);
    }

    [[nodiscard]] float delay_ms() const noexcept {
        return delay_ms_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        std::fill(buf_l_.begin(), buf_l_.end(), 0.0f);
        std::fill(buf_r_.begin(), buf_r_.end(), 0.0f);
        write_pos_ = 0;
        current_delay_samples_ = (delay_ms_.load(std::memory_order_relaxed) * 0.001f) * sample_rate_;
    }

    void process(float* left, float* right, size_t frames) noexcept {
        if (!left || frames == 0) return;
        const float target_samples = std::clamp((delay_ms_.load(std::memory_order_relaxed) * 0.001f) * sample_rate_,
                                                0.0f, static_cast<float>(max_delay_frames_));
        if (target_samples < 0.05f && current_delay_samples_ < 0.05f) {
            current_delay_samples_ = 0.0f;
            // Keep ring buffer primed silently/cleanly or bypass directly
            for (size_t f = 0; f < frames; ++f) {
                buf_l_[write_pos_] = left[f];
                buf_r_[write_pos_] = right ? right[f] : left[f];
                if (++write_pos_ >= buffer_len_) write_pos_ = 0;
            }
            return;
        }

        // Smooth slew up to ~0.04 samples per audio frame (~2 ms per 480-frame block) so slider moves don't click.
        constexpr float kMaxSlewPerSample = 0.04f;
        for (size_t f = 0; f < frames; ++f) {
            const float in_l = left[f];
            const float in_r = right ? right[f] : in_l;
            buf_l_[write_pos_] = in_l;
            buf_r_[write_pos_] = in_r;

            const float diff = target_samples - current_delay_samples_;
            if (std::abs(diff) <= kMaxSlewPerSample) {
                current_delay_samples_ = target_samples;
            } else {
                current_delay_samples_ += (diff > 0.0f) ? kMaxSlewPerSample : -kMaxSlewPerSample;
            }

            const float d = std::clamp(current_delay_samples_, 0.0f, static_cast<float>(max_delay_frames_));
            const size_t d0 = static_cast<size_t>(d);
            const size_t d1 = d0 + 1;
            const float frac = d - static_cast<float>(d0);

            const size_t idx0 = (write_pos_ + buffer_len_ - d0) % buffer_len_;
            const size_t idx1 = (write_pos_ + buffer_len_ - d1) % buffer_len_;

            left[f] = buf_l_[idx0] * (1.0f - frac) + buf_l_[idx1] * frac;
            if (right) {
                right[f] = buf_r_[idx0] * (1.0f - frac) + buf_r_[idx1] * frac;
            }

            if (++write_pos_ >= buffer_len_) write_pos_ = 0;
        }
    }

private:
    float sample_rate_{48000.0f};
    size_t max_delay_frames_{9608};
    size_t buffer_len_{9624};
    std::vector<float> buf_l_;
    std::vector<float> buf_r_;
    size_t write_pos_{0};
    float current_delay_samples_{0.0f};
    std::atomic<float> delay_ms_{0.0f};
};

} // namespace muxella::dsp
