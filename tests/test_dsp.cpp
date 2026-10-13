#include "metvoice/dsp/biquad.hpp"
#include "metvoice/dsp/compressor.hpp"
#include "metvoice/dsp/limiter.hpp"
#include "metvoice/dsp/meter.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <numbers>

void test_biquad_lowpass() {
    std::cout << "[Test] DSP: Biquad Low-Pass filter frequency response... ";
    metvoice::dsp::BiquadFilter lpf;
    constexpr float SAMPLE_RATE = 48000.0f;
    lpf.configure(metvoice::dsp::FilterType::LowPass, SAMPLE_RATE, 1000.0f, 0.7071f);

    // Feed 100 Hz sine wave (passband)
    float passband_power = 0.0f;
    for (int i = 0; i < 4800; ++i) {
        float in = std::sin(2.0f * std::numbers::pi * 100.0f * i / SAMPLE_RATE);
        float out = lpf.process_sample(in);
        if (i > 2400) passband_power += out * out;
    }

    lpf.reset();

    // Feed 10,000 Hz sine wave (stopband)
    float stopband_power = 0.0f;
    for (int i = 0; i < 4800; ++i) {
        float in = std::sin(2.0f * std::numbers::pi * 10000.0f * i / SAMPLE_RATE);
        float out = lpf.process_sample(in);
        if (i > 2400) stopband_power += out * out;
    }

    // Stopband must be heavily attenuated compared to passband
    assert(passband_power > 0.0f);
    assert(stopband_power < passband_power * 0.01f); // At least 20 dB attenuation
    std::cout << "PASSED (Stopband attenuation: "
              << (10.0f * std::log10(stopband_power / passband_power)) << " dB)\n";
}

void test_compressor_dynamics() {
    std::cout << "[Test] DSP: Dynamic range compressor gain reduction... ";
    constexpr float SAMPLE_RATE = 48000.0f;
    metvoice::dsp::Compressor comp(SAMPLE_RATE);
    // Threshold -20dB, 4:1 ratio, 5ms attack, 50ms release, 0dB makeup
    comp.configure(-20.0f, 4.0f, 5.0f, 50.0f, 0.0f, 0.0f);

    // Feed a loud signal at 0 dBFS (amplitude 1.0)
    std::vector<float> loud_block(4800, 1.0f);
    float* ptr = loud_block.data();
    comp.process(1, &ptr, loud_block.size());

    // With 0dB in and -20dB threshold at 4:1:
    // Output target is -20 + 20/4 = -15dB -> gain reduction should be ~15dB
    float gr = comp.current_gain_reduction_db();
    assert(gr > 12.0f); // Steady state reached near 15dB
    std::cout << "PASSED (Measured GR: " << gr << " dB)\n";
}

void test_limiter_ceiling_guarantee() {
    std::cout << "[Test] DSP: Soft Limiter ceiling enforcement under massive +12dB overload... ";
    metvoice::dsp::SoftLimiter limiter(-0.1f); // Ceiling ~0.9885

    // Feed a heavy overdrive signal of amplitude 4.0 (+12 dBFS)
    std::vector<float> extreme_signal(1024, 4.0f);
    float* ptr = extreme_signal.data();
    limiter.process(&ptr, 1, extreme_signal.size());

    for (size_t i = 0; i < extreme_signal.size(); ++i) {
        // Output must never exceed ceiling linear (0.9886)
        assert(extreme_signal[i] <= 0.99f);
        assert(extreme_signal[i] >= -0.99f);
    }
    assert(limiter.clip_events() > 0);
    std::cout << "PASSED (Limited " << limiter.clip_events() << " samples smoothly)\n";
}

int main() {
    test_biquad_lowpass();
    test_compressor_dynamics();
    test_limiter_ceiling_guarantee();
    std::cout << "All DSP unit tests passed successfully!\n";
    return 0;
}
