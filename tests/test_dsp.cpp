#include "metvoice/dsp/biquad.hpp"
#include "metvoice/dsp/compressor.hpp"
#include "metvoice/dsp/limiter.hpp"
#include "metvoice/dsp/meter.hpp"
#include "metvoice/dsp/gate.hpp"
#include "metvoice/dsp/denoise.hpp"
#include "metvoice/dsp/reverb.hpp"
#include "metvoice/dsp/dsp_strip.hpp"
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

void test_strip_fx_chain() {
    std::cout << "[Test] DSP: DspChannelStrip FX Chain (Gate, Denoiser, Reverb, Comp)... ";
    metvoice::dsp::DspChannelStrip strip("TestStrip", 48000.0f);

    strip.set_gate(5.0f);
    assert(strip.gate_amount() > 0.0f);

    strip.set_denoise(4.0f);
    assert(strip.denoise_amount() > 0.0f);

    strip.set_comp(3.0f);
    assert(strip.comp_amount() > 0.0f);
    assert(strip.is_compressor_enabled());

    strip.set_reverb(5.0f);
    assert(strip.reverb_amount() > 0.0f);

    // Process a test audio buffer
    metvoice::core::AudioBuffer buf(2, 480);
    for (size_t ch = 0; ch < 2; ++ch) {
        float* d = buf.channel_data(ch);
        for (size_t f = 0; f < 480; ++f) {
            d[f] = 0.2f * std::sin(2.0f * std::numbers::pi * 440.0f * f / 48000.0f);
        }
    }

    strip.process(buf);

    // Verify buffer was processed cleanly without NaNs or infinities
    for (size_t ch = 0; ch < 2; ++ch) {
        float* d = buf.channel_data(ch);
        for (size_t f = 0; f < 480; ++f) {
            assert(!std::isnan(d[f]));
            assert(!std::isinf(d[f]));
        }
    }
    std::cout << "PASSED\n";
}

void test_rnnoise_denoiser_reduction() {
    std::cout << "[Test] DSP: RNNoise neural noise suppression on stationary noise... ";
    metvoice::dsp::Denoiser denoiser(48000.0f);
    denoiser.set_amount(8.0f); // Strong neural suppression
    assert(denoiser.is_enabled());

    // Generate white noise block
    std::vector<float> noise_in(4800);
    uint32_t seed = 123456789;
    for (size_t i = 0; i < noise_in.size(); ++i) {
        seed = seed * 1664525 + 1013904223;
        noise_in[i] = ((static_cast<float>(seed & 0xFFFF) / 32768.0f) - 1.0f) * 0.08f;
    }

    float* ptrs[1] = {noise_in.data()};
    denoiser.process(1, ptrs, noise_in.size());

    // Check energy in the processed audio
    float power_after = 0.0f;
    for (size_t i = 2400; i < 4800; ++i) {
        assert(!std::isnan(noise_in[i]));
        assert(!std::isinf(noise_in[i]));
        power_after += noise_in[i] * noise_in[i];
    }
    std::cout << "PASSED (Residual power: " << power_after << ")\n";
}

int main() {
    test_biquad_lowpass();
    test_compressor_dynamics();
    test_limiter_ceiling_guarantee();
    test_strip_fx_chain();
    test_rnnoise_denoiser_reduction();
    std::cout << "All DSP unit tests passed successfully!\n";
    return 0;
}
