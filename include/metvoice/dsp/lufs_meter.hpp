#pragma once

#include "metvoice/dsp/biquad.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace metvoice::dsp {

/**
 * @brief Stereo ITU-R BS.1770 K-weighted Momentary (400 ms) & Short-Term (3 s) LUFS meter.
 */
class LufsMeter {
public:
    static constexpr size_t kSubBlocks3s = 30;  // 30 * 100 ms = 3.0 s
    static constexpr size_t kSubBlocks400ms = 4; // 4 * 100 ms = 400 ms
    static constexpr float kMinLufs = -70.0f;

    explicit LufsMeter(float sample_rate = 48000.0f)
        : sample_rate_(std::max(8000.0f, sample_rate)) {
        configure_filters();
        reset();
    }

    void set_sample_rate(float sample_rate) noexcept {
        sample_rate_ = std::max(8000.0f, sample_rate);
        configure_filters();
        reset();
    }

    void reset() noexcept {
        pre_l_.reset(); pre_r_.reset();
        rlb_l_.reset(); rlb_r_.reset();
        sub_energy_.fill(0.0);
        sub_index_ = 0;
        accum_energy_ = 0.0;
        accum_frames_ = 0;
        momentary_lufs_.store(kMinLufs, std::memory_order_relaxed);
        short_term_lufs_.store(kMinLufs, std::memory_order_relaxed);
    }

    void process(const float* left, const float* right, size_t frames) noexcept {
        if (!left || frames == 0) return;
        for (size_t f = 0; f < frames; ++f) {
            const float yl = rlb_l_.process_sample(pre_l_.process_sample(left[f]));
            const float yr = rlb_r_.process_sample(pre_r_.process_sample(right ? right[f] : left[f]));
            accum_energy_ += static_cast<double>(yl) * yl + static_cast<double>(yr) * yr;
            if (++accum_frames_ >= sub_block_frames_) {
                const double mean_sq = accum_energy_ / static_cast<double>(accum_frames_);
                sub_energy_[sub_index_] = mean_sq;
                sub_index_ = (sub_index_ + 1) % kSubBlocks3s;
                accum_energy_ = 0.0;
                accum_frames_ = 0;
                update_readouts();
            }
        }
    }

    [[nodiscard]] float momentary_lufs() const noexcept {
        return momentary_lufs_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float short_term_lufs() const noexcept {
        return short_term_lufs_.load(std::memory_order_relaxed);
    }

private:
    void configure_filters() noexcept {
        sub_block_frames_ = std::max<size_t>(1, static_cast<size_t>(std::round(sample_rate_ * 0.1)));
        // BS.1770 K-weighting: Stage 1 +4.0 dB high-shelf @ 1682 Hz; Stage 2 RLB high-pass @ 38 Hz
        pre_l_.configure(FilterType::HighShelf, sample_rate_, 1681.97f, 0.7071f, 4.0f);
        pre_r_.configure(FilterType::HighShelf, sample_rate_, 1681.97f, 0.7071f, 4.0f);
        rlb_l_.configure(FilterType::HighPass, sample_rate_, 38.14f, 0.5003f, 0.0f);
        rlb_r_.configure(FilterType::HighPass, sample_rate_, 38.14f, 0.5003f, 0.0f);
    }

    void update_readouts() noexcept {
        double sum_m = 0.0;
        for (size_t k = 0; k < kSubBlocks400ms; ++k) {
            const size_t idx = (sub_index_ + kSubBlocks3s - 1 - k) % kSubBlocks3s;
            sum_m += sub_energy_[idx];
        }
        double sum_s = 0.0;
        for (double e : sub_energy_) sum_s += e;

        auto to_lufs = [](double avg_power) noexcept -> float {
            if (avg_power <= 1e-10) return kMinLufs;
            return std::max(kMinLufs, static_cast<float>(-0.691 + 10.0 * std::log10(avg_power)));
        };
        momentary_lufs_.store(to_lufs(sum_m / static_cast<double>(kSubBlocks400ms)), std::memory_order_relaxed);
        short_term_lufs_.store(to_lufs(sum_s / static_cast<double>(kSubBlocks3s)), std::memory_order_relaxed);
    }

    float sample_rate_{48000.0f};
    size_t sub_block_frames_{4800};
    BiquadFilter pre_l_, pre_r_;
    BiquadFilter rlb_l_, rlb_r_;
    std::array<double, kSubBlocks3s> sub_energy_{};
    size_t sub_index_{0};
    double accum_energy_{0.0};
    size_t accum_frames_{0};
    std::atomic<float> momentary_lufs_{kMinLufs};
    std::atomic<float> short_term_lufs_{kMinLufs};
};

} // namespace metvoice::dsp
