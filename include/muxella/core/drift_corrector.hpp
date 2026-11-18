#pragma once

#include <cstddef>
#include <vector>
#include <cmath>
#include <algorithm>

namespace muxella::core {

/**
 * @brief Dynamic Clock Drift Corrector and Fractional Resampler.
 * 
 * Synchronizes unsynchronized physical or virtual audio endpoints running
 * on slightly different hardware clock domains (e.g. USB mic vs DAC output).
 * 
 * Employs a closed-loop PI controller tracking ring-buffer fill level
 * coupled with a 4-point Catmull-Rom cubic Hermite spline interpolation.
 */
class ClockDriftCorrector {
public:
    ClockDriftCorrector(size_t channels = 2, size_t target_fill_level = 1024,
                        double kp = 0.000002, double ki = 0.00000005);

    void reset(size_t target_fill_level);

    /**
     * @brief Update the internal resample ratio based on observed ring buffer fill.
     * @param current_fill Current number of samples in the intermediate buffer.
     */
    void update_feedback(size_t current_fill) noexcept;

    /**
     * @brief Process an audio block, resampling to adjust for clock drift.
     * @param in_channel_ptrs Planar input pointers.
     * @param in_frames Number of available input frames.
     * @param out_channel_ptrs Planar output pointers.
     * @param out_frames Requested number of output frames.
     * @param frames_consumed Output parameter: number of input frames consumed.
     * @return Number of output frames generated.
     */
    size_t process(const float* const* in_channel_ptrs, size_t in_frames,
                   float* const* out_channel_ptrs, size_t out_frames,
                   size_t& frames_consumed) noexcept;

    [[nodiscard]] double current_ratio() const noexcept { return current_ratio_; }
    [[nodiscard]] double drift_ppm() const noexcept { return (current_ratio_ - 1.0) * 1e6; }
    void set_max_ratio_offset(double max_offset) noexcept {
        max_ratio_offset_ = std::clamp(max_offset, 0.0001, 0.01);
    }

private:
    size_t channels_{2};
    size_t target_fill_level_{1024};
    double kp_{0.000002};
    double ki_{0.00000005};
    double max_ratio_offset_{0.005};

    double integral_error_{0.0};
    double filtered_fill_{0.0};
    double current_ratio_{1.0};
    double sub_sample_pos_{0.0};

    // History for 4-point interpolation per channel (x[-1], x[-2])
    std::vector<std::vector<float>> history_;
};

} // namespace muxella::core
