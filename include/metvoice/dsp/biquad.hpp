#pragma once

#include <cmath>
#include <cstddef>
#include <numbers>

namespace metvoice::dsp {

enum class FilterType {
    LowPass,
    HighPass,
    BandPass,
    Notch,
    Peaking,
    LowShelf,
    HighShelf
};

/**
 * @brief High-precision 64-bit state Biquad Filter (Direct Form II Transposed).
 * Implements Robert Bristow-Johnson's Audio EQ Cookbook coefficients.
 */
class BiquadFilter {
public:
    BiquadFilter();

    void configure(FilterType type, float sample_rate, float frequency_hz,
                   float q = 0.7071f, float gain_db = 0.0f);

    void reset() noexcept;

    [[nodiscard]] float process_sample(float in) noexcept {
        // Direct Form II Transposed:
        // y[n] = b0*x[n] + s1
        // s1   = b1*x[n] - a1*y[n] + s2
        // s2   = b2*x[n] - a2*y[n]
        const double x = static_cast<double>(in);
        const double y = b0_ * x + s1_;
        s1_ = b1_ * x - a1_ * y + s2_;
        s2_ = b2_ * x - a2_ * y;
        return static_cast<float>(y);
    }

    void process(float* data, size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            data[i] = process_sample(data[i]);
        }
    }

    [[nodiscard]] FilterType type() const noexcept { return type_; }
    [[nodiscard]] float frequency() const noexcept { return freq_hz_; }
    [[nodiscard]] float q() const noexcept { return q_; }
    [[nodiscard]] float gain_db() const noexcept { return gain_db_; }

private:
    FilterType type_{FilterType::Peaking};
    float freq_hz_{1000.0f};
    float q_{0.7071f};
    float gain_db_{0.0f};

    // Normalized coefficients
    double b0_{1.0};
    double b1_{0.0};
    double b2_{0.0};
    double a1_{0.0};
    double a2_{0.0};

    // Direct Form II Transposed delay states
    double s1_{0.0};
    double s2_{0.0};
};

} // namespace metvoice::dsp
