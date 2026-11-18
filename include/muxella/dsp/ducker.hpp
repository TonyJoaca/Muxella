#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace muxella::dsp {

/**
 * @brief Lock-free real-time Sidechain Auto-Ducker.
 *
 * Follows the peak/RMS envelope of an external key strip and smoothly
 * attenuates this strip's stereo buffer while the key is active.
 */
class SidechainDucker {
public:
    explicit SidechainDucker(float sample_rate = 48000.0f) noexcept
        : sample_rate_(std::max(8000.0f, sample_rate)) {
        update_coefficients();
    }

    void set_sample_rate(float sample_rate) noexcept {
        sample_rate_ = std::max(8000.0f, sample_rate);
        update_coefficients();
    }

    /**
     * @brief Configure sidechain ducking.
     * @param trigger_strip Key strip index (e.g. 0 for Mic 1), or -1 to disable.
     * @param amount Knob scale 0.0 (off) .. 10.0 (strong -24 dB ducking).
     */
    void configure(int trigger_strip, float amount) noexcept {
        amount = std::clamp(amount, 0.0f, 10.0f);
        trigger_strip_.store(trigger_strip, std::memory_order_relaxed);
        amount_.store(amount, std::memory_order_relaxed);
        if (trigger_strip < 0 || amount <= 0.01f) {
            enabled_.store(false, std::memory_order_relaxed);
            duck_gain_db_.store(0.0f, std::memory_order_relaxed);
            return;
        }
        // Threshold -30 dB .. -46 dB; max attenuation -3 dB .. -24 dB
        const float thresh_db = -30.0f - (amount / 10.0f) * 16.0f;
        const float max_att_db = -(3.0f + (amount / 10.0f) * 21.0f);
        threshold_linear_ = std::pow(10.0f, thresh_db / 20.0f);
        min_gain_linear_ = std::pow(10.0f, max_att_db / 20.0f);
        enabled_.store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int trigger_strip() const noexcept {
        return trigger_strip_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float amount() const noexcept {
        return amount_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float current_duck_db() const noexcept {
        return duck_gain_db_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        key_envelope_ = 0.0f;
        gain_ = 1.0f;
        hold_counter_ = 0;
        duck_gain_db_.store(0.0f, std::memory_order_relaxed);
    }

    /**
     * @brief Process stereo buffers using optional external key buffers.
     * If key_l/key_r are nullptr (e.g. trigger strip inactive), recovers smoothly to unity.
     */
    void process(const float* key_l, const float* key_r,
                 float* dst_l, float* dst_r, size_t frames) noexcept {
        if (!dst_l || frames == 0) return;
        const bool active = is_enabled() && key_l != nullptr;

        // If disabled and already back at unity, bypass with zero overhead.
        if (!active && gain_ >= 0.9999f) {
            gain_ = 1.0f;
            key_envelope_ = 0.0f;
            hold_counter_ = 0;
            duck_gain_db_.store(0.0f, std::memory_order_relaxed);
            return;
        }

        for (size_t f = 0; f < frames; ++f) {
            float target = 1.0f;
            if (active) {
                const float kl = std::abs(key_l[f]);
                const float kr = key_r ? std::abs(key_r[f]) : kl;
                const float key_peak = std::max(kl, kr);
                const float env_coeff = (key_peak > key_envelope_) ? key_attack_coeff_ : key_release_coeff_;
                key_envelope_ += env_coeff * (key_peak - key_envelope_);

                if (key_envelope_ >= threshold_linear_) {
                    hold_counter_ = hold_samples_;
                } else if (hold_counter_ > 0) {
                    --hold_counter_;
                }

                if (hold_counter_ > 0) {
                    const float ratio = std::clamp((key_envelope_ - threshold_linear_) /
                                                       std::max(1e-5f, threshold_linear_ * 2.0f),
                                                   0.0f, 1.0f);
                    // Apply most ducking as soon as speech passes threshold, up to full depth
                    const float depth = 0.55f + 0.45f * ratio;
                    target = 1.0f - depth * (1.0f - min_gain_linear_);
                }
            } else {
                key_envelope_ *= (1.0f - key_release_coeff_);
                hold_counter_ = 0;
            }

            const float coeff = (target < gain_) ? gain_attack_coeff_ : gain_release_coeff_;
            gain_ += coeff * (target - gain_);
            dst_l[f] *= gain_;
            if (dst_r) dst_r[f] *= gain_;
        }

        const float clamped = std::max(gain_, 1e-4f);
        duck_gain_db_.store(20.0f * std::log10(clamped), std::memory_order_relaxed);
    }

private:
    void update_coefficients() noexcept {
        key_attack_coeff_ = 1.0f - std::exp(-1.0f / (sample_rate_ * 0.005f));
        key_release_coeff_ = 1.0f - std::exp(-1.0f / (sample_rate_ * 0.090f));
        gain_attack_coeff_ = 1.0f - std::exp(-1.0f / (sample_rate_ * 0.010f));
        gain_release_coeff_ = 1.0f - std::exp(-1.0f / (sample_rate_ * 0.240f));
        hold_samples_ = static_cast<size_t>(sample_rate_ * 0.140f);
    }

    float sample_rate_{48000.0f};
    std::atomic<int> trigger_strip_{-1};
    std::atomic<float> amount_{0.0f};
    std::atomic<bool> enabled_{false};
    std::atomic<float> duck_gain_db_{0.0f};

    float threshold_linear_{0.01f};
    float min_gain_linear_{0.25f};
    float key_attack_coeff_{0.0f};
    float key_release_coeff_{0.0f};
    float gain_attack_coeff_{0.0f};
    float gain_release_coeff_{0.0f};
    size_t hold_samples_{6720};

    float key_envelope_{0.0f};
    float gain_{1.0f};
    size_t hold_counter_{0};
};

} // namespace muxella::dsp
