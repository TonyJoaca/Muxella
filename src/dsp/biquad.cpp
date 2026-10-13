#include "metvoice/dsp/biquad.hpp"
#include <algorithm>

namespace metvoice::dsp {

BiquadFilter::BiquadFilter() {
    reset();
}

void BiquadFilter::reset() noexcept {
    s1_ = 0.0;
    s2_ = 0.0;
}

void BiquadFilter::configure(FilterType type, float sample_rate, float frequency_hz,
                             float q, float gain_db) {
    type_ = type;
    freq_hz_ = std::clamp(frequency_hz, 10.0f, sample_rate * 0.49f);
    q_ = std::max(0.01f, q);
    gain_db_ = gain_db;

    const double w0 = 2.0 * std::numbers::pi * (freq_hz_ / sample_rate);
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha = sin_w0 / (2.0 * q_);
    const double A = std::pow(10.0, gain_db_ / 40.0); // sqrt(10^(gain/20))

    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (type) {
        case FilterType::LowPass:
            b0 = (1.0 - cos_w0) / 2.0;
            b1 = 1.0 - cos_w0;
            b2 = (1.0 - cos_w0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;

        case FilterType::HighPass:
            b0 = (1.0 + cos_w0) / 2.0;
            b1 = -(1.0 + cos_w0);
            b2 = (1.0 + cos_w0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;

        case FilterType::BandPass:
            b0 = alpha;
            b1 = 0.0;
            b2 = -alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;

        case FilterType::Notch:
            b0 = 1.0;
            b1 = -2.0 * cos_w0;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;

        case FilterType::Peaking:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cos_w0;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha / A;
            break;

        case FilterType::LowShelf: {
            const double sqrt_A_2 = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + sqrt_A_2);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
            b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - sqrt_A_2);
            a0 = (A + 1.0) + (A - 1.0) * cos_w0 + sqrt_A_2;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
            a2 = (A + 1.0) + (A - 1.0) * cos_w0 - sqrt_A_2;
            break;
        }

        case FilterType::HighShelf: {
            const double sqrt_A_2 = 2.0 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + sqrt_A_2);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
            b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - sqrt_A_2);
            a0 = (A + 1.0) - (A - 1.0) * cos_w0 + sqrt_A_2;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
            a2 = (A + 1.0) - (A - 1.0) * cos_w0 - sqrt_A_2;
            break;
        }
    }

    const double inv_a0 = 1.0 / a0;
    b0_ = b0 * inv_a0;
    b1_ = b1 * inv_a0;
    b2_ = b2 * inv_a0;
    a1_ = a1 * inv_a0;
    a2_ = a2 * inv_a0;
}

} // namespace metvoice::dsp
