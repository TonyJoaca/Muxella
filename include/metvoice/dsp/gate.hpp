#pragma once

#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstddef>

namespace metvoice::dsp {

/**
 * @brief High-performance Noise Gate with smooth attack/hold/release ballistics.
 * Eliminates background hum, fan noise, and keyboard clicks when not speaking.
 */
class NoiseGate {
public:
    explicit NoiseGate(float sample_rate = 48000.0f)
        : sample_rate_(sample_rate) {
        configure(-50.0f, 2.0f, 40.0f, 80.0f, -60.0f);
    }

    void set_sample_rate(float sample_rate) noexcept {
        sample_rate_ = sample_rate;
        update_coefficients();
    }

    void configure(float threshold_db, float attack_ms = 2.0f, float hold_ms = 40.0f,
                   float release_ms = 80.0f, float floor_db = -60.0f) noexcept {
        threshold_db_ = threshold_db;
        threshold_linear_ = std::pow(10.0f, threshold_db / 20.0f);
        attack_ms_ = std::max(0.1f, attack_ms);
        hold_ms_ = std::max(1.0f, hold_ms);
        release_ms_ = std::max(1.0f, release_ms);
        floor_linear_ = std::pow(10.0f, floor_db / 20.0f);
        update_coefficients();
    }

    /**
     * @brief Configure via single 0.0 .. 10.0 knob amount.
     * 0.0 = Bypassed (fully open).
     * 0.1 .. 10.0 = Threshold ranges from -65 dB (gentle) to -25 dB (aggressive).
     */
    void set_amount(float amount) noexcept {
        amount = std::clamp(amount, 0.0f, 10.0f);
        amount_.store(amount, std::memory_order_relaxed);
        if (amount <= 0.01f) {
            enabled_.store(false, std::memory_order_relaxed);
        } else {
            // Map 0.1 .. 10.0 to -65 dB .. -25 dB threshold
            const float thresh = -65.0f + (amount / 10.0f) * 40.0f;
            configure(thresh, 2.0f, 35.0f, 75.0f, -60.0f);
            enabled_.store(true, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool is_enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        envelope_ = 1.0f;
        hold_counter_ = 0;
    }

    void process(size_t channels, float* const* channel_ptrs, size_t frames) noexcept {
        if (!is_enabled() || channels == 0 || frames == 0 || channel_ptrs == nullptr) {
            return;
        }

        for (size_t f = 0; f < frames; ++f) {
            // Detect peak level across channels
            float peak = 0.0f;
            for (size_t ch = 0; ch < channels; ++ch) {
                peak = std::max(peak, std::abs(channel_ptrs[ch][f]));
            }

            // Gate state logic with hold
            if (peak >= threshold_linear_) {
                target_gain_ = 1.0f;
                hold_counter_ = hold_samples_;
            } else {
                if (hold_counter_ > 0) {
                    --hold_counter_;
                    target_gain_ = 1.0f;
                } else {
                    target_gain_ = floor_linear_;
                }
            }

            // Smooth ballistics
            if (target_gain_ > envelope_) {
                envelope_ = attack_coeff_ * envelope_ + (1.0f - attack_coeff_) * target_gain_;
            } else {
                envelope_ = release_coeff_ * envelope_ + (1.0f - release_coeff_) * target_gain_;
            }

            for (size_t ch = 0; ch < channels; ++ch) {
                channel_ptrs[ch][f] *= envelope_;
            }
        }
    }

private:
    float sample_rate_{48000.0f};
    float threshold_db_{-50.0f};
    float threshold_linear_{0.00316f};
    float attack_ms_{2.0f};
    float hold_ms_{40.0f};
    float release_ms_{80.0f};
    float floor_linear_{0.001f};

    float attack_coeff_{0.0f};
    float release_coeff_{0.0f};
    size_t hold_samples_{0};
    size_t hold_counter_{0};

    float target_gain_{1.0f};
    float envelope_{1.0f};

    std::atomic<bool> enabled_{false};
    std::atomic<float> amount_{0.0f};

    void update_coefficients() noexcept {
        attack_coeff_ = std::exp(-1000.0f / (attack_ms_ * sample_rate_));
        release_coeff_ = std::exp(-1000.0f / (release_ms_ * sample_rate_));
        hold_samples_ = static_cast<size_t>((hold_ms_ * sample_rate_) / 1000.0f);
    }
};

} // namespace metvoice::dsp
