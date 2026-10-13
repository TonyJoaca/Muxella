#include "metvoice/dsp/compressor.hpp"

namespace metvoice::dsp {

Compressor::Compressor(float sample_rate)
    : sample_rate_(sample_rate) {
    update_coefficients();
}

void Compressor::set_sample_rate(float sample_rate) {
    sample_rate_ = sample_rate;
    update_coefficients();
}

void Compressor::configure(float threshold_db, float ratio, float attack_ms,
                           float release_ms, float knee_db, float makeup_gain_db) {
    threshold_db_ = threshold_db;
    ratio_ = std::max(1.0f, ratio);
    attack_ms_ = std::max(0.1f, attack_ms);
    release_ms_ = std::max(1.0f, release_ms);
    knee_db_ = std::max(0.0f, knee_db);
    makeup_gain_linear_ = std::pow(10.0f, makeup_gain_db / 20.0f);

    update_coefficients();
}

void Compressor::update_coefficients() {
    attack_coeff_ = std::exp(-1000.0f / (attack_ms_ * sample_rate_));
    release_coeff_ = std::exp(-1000.0f / (release_ms_ * sample_rate_));
}

void Compressor::reset() noexcept {
    envelope_ = 1.0f;
    last_gr_db_.store(0.0f, std::memory_order_relaxed);
}

void Compressor::process(size_t channels, float* const* channel_ptrs, size_t frames) noexcept {
    if (channels == 0 || frames == 0 || channel_ptrs == nullptr) return;

    const float half_knee = knee_db_ * 0.5f;
    const float inv_ratio = 1.0f / ratio_;
    float max_gr_in_block = 0.0f;

    for (size_t f = 0; f < frames; ++f) {
        // Detect sidechain input level across channels
        float peak = 0.0f;
        for (size_t ch = 0; ch < channels; ++ch) {
            peak = std::max(peak, std::abs(channel_ptrs[ch][f]));
        }

        // Convert to dB
        const float in_db = (peak > 1e-6f) ? (20.0f * std::log10(peak)) : -120.0f;
        float gr_target_db = 0.0f;

        if (in_db > threshold_db_ + half_knee) {
            // Above knee: full compression
            const float overshoot = in_db - threshold_db_;
            const float out_db = threshold_db_ + overshoot * inv_ratio;
            gr_target_db = out_db - in_db;
        } else if (in_db > threshold_db_ - half_knee && knee_db_ > 0.0f) {
            // Inside soft knee curve
            const float delta = in_db - threshold_db_ + half_knee;
            gr_target_db = (inv_ratio - 1.0f) * (delta * delta) / (2.0f * knee_db_);
        } else {
            gr_target_db = 0.0f;
        }

        // Target linear gain (always <= 1.0)
        const float target_gain = std::pow(10.0f, gr_target_db / 20.0f);

        // Smooth gain reduction envelope
        if (target_gain < envelope_) {
            envelope_ = attack_coeff_ * envelope_ + (1.0f - attack_coeff_) * target_gain;
        } else {
            envelope_ = release_coeff_ * envelope_ + (1.0f - release_coeff_) * target_gain;
        }

        const float final_gain = envelope_ * makeup_gain_linear_;
        for (size_t ch = 0; ch < channels; ++ch) {
            channel_ptrs[ch][f] *= final_gain;
        }

        const float current_gr_db = -20.0f * std::log10(std::max(envelope_, 1e-5f));
        max_gr_in_block = std::max(max_gr_in_block, current_gr_db);
    }

    last_gr_db_.store(max_gr_in_block, std::memory_order_relaxed);
}

} // namespace metvoice::dsp
