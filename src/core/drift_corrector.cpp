#include "metvoice/core/drift_corrector.hpp"

namespace metvoice::core {

// 4-point Catmull-Rom cubic interpolation
// p0: y[n-1], p1: y[n], p2: y[n+1], p3: y[n+2], t: fractional offset in [0, 1)
static inline float cubic_hermite_interpolate(float p0, float p1, float p2, float p3, float t) noexcept {
    const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float c = -0.5f * p0 + 0.5f * p2;
    const float d = p1;
    return ((a * t + b) * t + c) * t + d;
}

ClockDriftCorrector::ClockDriftCorrector(size_t channels, size_t target_fill_level,
                                         double kp, double ki)
    : channels_(channels),
      target_fill_level_(target_fill_level),
      kp_(kp),
      ki_(ki),
      integral_error_(0.0),
      current_ratio_(1.0),
      sub_sample_pos_(0.0),
      history_(channels, std::vector<float>(4, 0.0f)) {
}

void ClockDriftCorrector::reset(size_t target_fill_level) {
    target_fill_level_ = target_fill_level;
    integral_error_ = 0.0;
    current_ratio_ = 1.0;
    sub_sample_pos_ = 0.0;
    for (auto& ch_hist : history_) {
        std::fill(ch_hist.begin(), ch_hist.end(), 0.0f);
    }
}

void ClockDriftCorrector::update_feedback(size_t current_fill) noexcept {
    const double error = static_cast<double>(current_fill) - static_cast<double>(target_fill_level_);

    // Accumulate integral with anti-windup
    integral_error_ += error;
    if (integral_error_ > 50000.0) integral_error_ = 50000.0;
    if (integral_error_ < -50000.0) integral_error_ = -50000.0;

    // PI control output: if current_fill > target, we want ratio > 1.0 to consume faster
    double target_ratio = 1.0 + (kp_ * error) + (ki_ * integral_error_);

    // Clamp adjustment range to +/- 0.5% (5000 PPM) to preserve audio pitch transparency
    target_ratio = std::clamp(target_ratio, 0.995, 1.005);

    // Smooth transition
    current_ratio_ = current_ratio_ * 0.95 + target_ratio * 0.05;
}

size_t ClockDriftCorrector::process(const float* const* in_channel_ptrs, size_t in_frames,
                                    float* const* out_channel_ptrs, size_t out_frames,
                                    size_t& frames_consumed) noexcept {
    if (in_frames == 0 || out_frames == 0) {
        frames_consumed = 0;
        return 0;
    }

    size_t out_idx = 0;
    double in_pos = sub_sample_pos_;

    while (out_idx < out_frames) {
        const size_t base_idx = static_cast<size_t>(std::floor(in_pos));
        const float frac = static_cast<float>(in_pos - base_idx);

        // If we need points beyond what's available in this chunk, stop
        // We need base_idx + 2 < in_frames to interpolate safely
        if (base_idx + 2 >= in_frames) {
            break;
        }

        for (size_t ch = 0; ch < channels_; ++ch) {
            const float* in = in_channel_ptrs[ch];
            // Samples: p0 = in[base_idx - 1] (or history), p1 = in[base_idx], p2 = in[base_idx + 1], p3 = in[base_idx + 2]
            float p0 = (base_idx > 0) ? in[base_idx - 1] : history_[ch][2];
            float p1 = in[base_idx];
            float p2 = in[base_idx + 1];
            float p3 = in[base_idx + 2];

            out_channel_ptrs[ch][out_idx] = cubic_hermite_interpolate(p0, p1, p2, p3, frac);
        }

        out_idx++;
        in_pos += current_ratio_;
    }

    const size_t consumed = static_cast<size_t>(std::floor(in_pos));
    frames_consumed = std::min(consumed, in_frames);

    // Save history from the end of the consumed chunk
    if (frames_consumed >= 3) {
        for (size_t ch = 0; ch < channels_; ++ch) {
            const float* in = in_channel_ptrs[ch];
            history_[ch][0] = in[frames_consumed - 3];
            history_[ch][1] = in[frames_consumed - 2];
            history_[ch][2] = in[frames_consumed - 1];
        }
    }

    // Retain fractional leftover phase
    sub_sample_pos_ = in_pos - frames_consumed;
    return out_idx;
}

} // namespace metvoice::core
