#pragma once

#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstddef>

namespace metvoice::dsp {

/**
 * @brief Lock-Free Audio Meter (Peak + RMS + Decay Ballistics).
 * 
 * Written by the real-time audio thread; read asynchronously by UI or IPC.
 */
class AudioMeter {
public:
    explicit AudioMeter(float sample_rate = 48000.0f)
        : sample_rate_(sample_rate) {
        // -20 dB per second decay
        decay_factor_ = std::pow(10.0f, -20.0f / (20.0f * (sample_rate_ / 256.0f)));
        reset();
    }

    void reset() noexcept {
        peak_linear_.store(0.0f, std::memory_order_relaxed);
        rms_linear_.store(0.0f, std::memory_order_relaxed);
        peak_hold_.store(0.0f, std::memory_order_relaxed);
    }

    void process(const float* data, size_t count) noexcept {
        if (count == 0 || data == nullptr) return;

        float max_sample = 0.0f;
        double sum_squares = 0.0;

        for (size_t i = 0; i < count; ++i) {
            const float s = std::abs(data[i]);
            if (s > max_sample) max_sample = s;
            sum_squares += static_cast<double>(s) * static_cast<double>(s);
        }

        const float block_rms = static_cast<float>(std::sqrt(sum_squares / count));

        // Update peak with decay
        float current_peak = peak_linear_.load(std::memory_order_relaxed);
        current_peak = std::max(max_sample, current_peak * decay_factor_);
        peak_linear_.store(current_peak, std::memory_order_relaxed);

        // Update RMS
        rms_linear_.store(block_rms, std::memory_order_relaxed);

        // Update peak hold
        float current_hold = peak_hold_.load(std::memory_order_relaxed);
        if (max_sample > current_hold) {
            peak_hold_.store(max_sample, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] float peak_db() const noexcept {
        const float p = peak_linear_.load(std::memory_order_relaxed);
        return (p > 1e-6f) ? (20.0f * std::log10(p)) : -120.0f;
    }

    [[nodiscard]] float rms_db() const noexcept {
        const float r = rms_linear_.load(std::memory_order_relaxed);
        return (r > 1e-6f) ? (20.0f * std::log10(r)) : -120.0f;
    }

    [[nodiscard]] float peak_hold_db() const noexcept {
        const float h = peak_hold_.load(std::memory_order_relaxed);
        return (h > 1e-6f) ? (20.0f * std::log10(h)) : -120.0f;
    }

    void reset_peak_hold() noexcept {
        peak_hold_.store(0.0f, std::memory_order_relaxed);
    }

private:
    float sample_rate_{48000.0f};
    float decay_factor_{0.95f};

    std::atomic<float> peak_linear_{0.0f};
    std::atomic<float> rms_linear_{0.0f};
    std::atomic<float> peak_hold_{0.0f};
};

} // namespace metvoice::dsp
