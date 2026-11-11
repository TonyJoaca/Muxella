#pragma once

#include "metvoice/core/audio_buffer.hpp"
#include "metvoice/dsp/biquad.hpp"
#include "metvoice/dsp/compressor.hpp"
#include "metvoice/dsp/gate.hpp"
#include "metvoice/dsp/denoise.hpp"
#include "metvoice/dsp/reverb.hpp"
#include "metvoice/dsp/meter.hpp"
#include "metvoice/dsp/ramp.hpp"
#include <numbers>
#include <atomic>
#include <string>

namespace metvoice::dsp {

/**
 * @brief Input Channel Strip with Gain, Pan, EQ, Dynamics, Gate, Denoise, Reverb, and Metering.
 */
class DspChannelStrip {
public:
    explicit DspChannelStrip(const std::string& name = "Strip", float sample_rate = 48000.0f)
        : name_(name), sample_rate_(sample_rate), compressor_(sample_rate),
          gate_(sample_rate), denoiser_(sample_rate), reverb_(sample_rate),
          meter_left_(sample_rate), meter_right_(sample_rate) {
        init_dsp();
    }

    void set_name(const std::string& name) { name_ = name; }
    [[nodiscard]] const std::string& name() const { return name_; }

    void set_gain_db(float gain_db) noexcept {
        gain_db_.store(gain_db, std::memory_order_relaxed);
        gain_linear_.store(std::pow(10.0f, gain_db / 20.0f), std::memory_order_relaxed);
    }
    [[nodiscard]] float gain_db() const noexcept { return gain_db_.load(std::memory_order_relaxed); }

    void set_pan(float pan) noexcept {
        pan = std::clamp(pan, -1.0f, 1.0f);
        pan_.store(pan, std::memory_order_relaxed);
        // Stereo balance control (0 dB Center / unity gain at center)
        if (pan <= 0.0f) {
            pan_left_.store(1.0f, std::memory_order_relaxed);
            pan_right_.store(1.0f + pan, std::memory_order_relaxed);
        } else {
            pan_left_.store(1.0f - pan, std::memory_order_relaxed);
            pan_right_.store(1.0f, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] float pan() const noexcept { return pan_.load(std::memory_order_relaxed); }

    void set_mute(bool mute) noexcept { mute_.store(mute, std::memory_order_relaxed); }
    [[nodiscard]] bool is_muted() const noexcept { return mute_.load(std::memory_order_relaxed); }
    void set_enabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void reset_processing() noexcept {
        eq_low_l_.reset(); eq_low_r_.reset(); eq_mid_l_.reset(); eq_mid_r_.reset();
        eq_high_l_.reset(); eq_high_r_.reset(); compressor_.reset(); gate_.reset();
        denoiser_.reset(); reverb_.reset(); meter_left_.reset(); meter_right_.reset();
        gain_ramp_ = GainRamp(gain_linear_.load(std::memory_order_relaxed));
        pan_l_ramp_ = GainRamp(pan_left_.load(std::memory_order_relaxed));
        pan_r_ramp_ = GainRamp(pan_right_.load(std::memory_order_relaxed));
        mute_ramp_ = GainRamp(0.0f);
    }

    void set_mono(bool mono) noexcept { mono_.store(mono, std::memory_order_relaxed); }
    [[nodiscard]] bool is_mono() const noexcept { return mono_.load(std::memory_order_relaxed); }

    void set_solo(bool solo) noexcept { solo_.store(solo, std::memory_order_relaxed); }
    [[nodiscard]] bool is_solo() const noexcept { return solo_.load(std::memory_order_relaxed); }

    void set_eq_low(float gain_db, float freq_hz = 100.0f) {
        eq_low_l_.configure(FilterType::LowShelf, sample_rate_, freq_hz, 0.7071f, gain_db);
        eq_low_r_.configure(FilterType::LowShelf, sample_rate_, freq_hz, 0.7071f, gain_db);
    }

    void set_eq_mid(float gain_db, float freq_hz = 1000.0f, float q = 1.0f) {
        eq_mid_l_.configure(FilterType::Peaking, sample_rate_, freq_hz, q, gain_db);
        eq_mid_r_.configure(FilterType::Peaking, sample_rate_, freq_hz, q, gain_db);
    }

    void set_eq_high(float gain_db, float freq_hz = 8000.0f) {
        eq_high_l_.configure(FilterType::HighShelf, sample_rate_, freq_hz, 0.7071f, gain_db);
        eq_high_r_.configure(FilterType::HighShelf, sample_rate_, freq_hz, 0.7071f, gain_db);
    }

    void enable_eq(bool enable) noexcept { eq_enabled_.store(enable, std::memory_order_relaxed); }
    [[nodiscard]] bool is_eq_enabled() const noexcept { return eq_enabled_.load(std::memory_order_relaxed); }

    void configure_compressor(float threshold_db, float ratio, float attack_ms,
                              float release_ms, float knee_db, float makeup_gain_db) {
        compressor_.configure(threshold_db, ratio, attack_ms, release_ms, knee_db, makeup_gain_db);
    }

    void enable_compressor(bool enable) noexcept { comp_enabled_.store(enable, std::memory_order_relaxed); }
    [[nodiscard]] bool is_compressor_enabled() const noexcept { return comp_enabled_.load(std::memory_order_relaxed); }

    void set_comp(float amount) noexcept {
        amount = std::clamp(amount, 0.0f, 10.0f);
        comp_amount_.store(amount, std::memory_order_relaxed);
        if (amount <= 0.01f) {
            enable_compressor(false);
        } else {
            // Punchy voice compressor:
            // Threshold smoothly scales from -14 dB down to -36 dB
            // Ratio from 2.0:1 to 7.0:1
            // Automatic makeup gain from +1 dB to +6 dB
            const float thresh = -14.0f - (amount * 2.2f);
            const float ratio = 2.0f + (amount * 0.5f);
            const float makeup = 0.5f + (amount * 0.6f);
            configure_compressor(thresh, ratio, 5.0f, 60.0f, 4.0f, makeup);
            enable_compressor(true);
        }
    }
    [[nodiscard]] float comp_amount() const noexcept { return comp_amount_.load(std::memory_order_relaxed); }

    void set_gate(float amount) noexcept { gate_.set_amount(amount); }
    [[nodiscard]] float gate_amount() const noexcept { return gate_.is_enabled() ? 1.0f : 0.0f; }

    void set_denoise(float amount) noexcept { denoiser_.set_amount(amount); }
    [[nodiscard]] float denoise_amount() const noexcept { return denoiser_.is_enabled() ? 1.0f : 0.0f; }

    void set_reverb(float amount) noexcept { reverb_.set_amount(amount); }
    [[nodiscard]] float reverb_amount() const noexcept { return reverb_.is_enabled() ? 1.0f : 0.0f; }

    [[nodiscard]] float gain_reduction_db() const noexcept { return compressor_.current_gain_reduction_db(); }

    [[nodiscard]] float peak_left_db() const noexcept { return meter_left_.peak_db(); }
    [[nodiscard]] float peak_right_db() const noexcept { return meter_right_.peak_db(); }
    [[nodiscard]] float rms_left_db() const noexcept { return meter_left_.rms_db(); }
    [[nodiscard]] float rms_right_db() const noexcept { return meter_right_.rms_db(); }

    /**
     * @brief Process an in-place stereo or mono audio buffer.
     */
    void process(core::AudioBuffer& buffer, bool audible = true) noexcept {
        const size_t frames = buffer.frames();
        if (frames == 0) return;

        float* l = buffer.channel_data(0);
        float* r = (buffer.channels() > 1) ? buffer.channel_data(1) : buffer.channel_data(0);
        float* ptrs[2] = {l, r};
        const size_t ch_count = (buffer.channels() > 1) ? 2 : 1;

        // 1. Pre-gain & Pan
        const auto gain_frames = static_cast<size_t>(sample_rate_ * 0.020f);
        gain_ramp_.target(gain_linear_.load(std::memory_order_relaxed), gain_frames);
        pan_l_ramp_.target(pan_left_.load(std::memory_order_relaxed), gain_frames);
        pan_r_ramp_.target(pan_right_.load(std::memory_order_relaxed), gain_frames);
        mute_ramp_.target(is_muted() || !audible ? 0.0f : 1.0f, static_cast<size_t>(sample_rate_ * 0.010f));

        for (size_t f = 0; f < frames; ++f) {
            const float gain = gain_ramp_.next();
            l[f] *= gain * pan_l_ramp_.next();
            if (ch_count > 1) r[f] *= gain * pan_r_ramp_.next();
            else pan_r_ramp_.next();
        }

        // 2. Mono downmix
        if (mono_.load(std::memory_order_relaxed)) {
            for (size_t f = 0; f < frames; ++f) {
                const float m = 0.5f * (l[f] + r[f]);
                l[f] = m;
                r[f] = m;
            }
        }

        // 3. Denoiser Stage (Low-rumble high-pass & dynamic background hiss expansion)
        denoiser_.process(ch_count, ptrs, frames);

        // 4. Noise Gate Stage (mutes silence & background clicks)
        gate_.process(ch_count, ptrs, frames);

        // 5. EQ Stage
        if (eq_enabled_.load(std::memory_order_relaxed)) {
            eq_low_l_.process(l, frames);
            eq_mid_l_.process(l, frames);
            eq_high_l_.process(l, frames);

            if (buffer.channels() > 1) {
                eq_low_r_.process(r, frames);
                eq_mid_r_.process(r, frames);
                eq_high_r_.process(r, frames);
            }
        }

        // 6. Compressor Dynamics Stage
        if (comp_enabled_.load(std::memory_order_relaxed)) {
            compressor_.process(ch_count, ptrs, frames);
        }

        // 7. Studio Reverb Stage
        reverb_.process(ch_count, ptrs, frames);

        // Fade mute after all effects, including reverb tails.
        for (size_t f = 0; f < frames; ++f) {
            const float gain = mute_ramp_.next();
            l[f] *= gain;
            if (ch_count > 1) r[f] *= gain;
        }

        // 8. Metering Stage
        meter_left_.process(l, frames);
        if (buffer.channels() > 1) {
            meter_right_.process(r, frames);
        } else {
            meter_right_.process(l, frames);
        }
    }

private:
    void init_dsp() {
        set_gain_db(0.0f);
        set_pan(0.0f);
        set_eq_low(0.0f, 100.0f);
        set_eq_mid(0.0f, 1000.0f, 1.0f);
        set_eq_high(0.0f, 8000.0f);
        set_comp(0.0f);
        set_gate(0.0f);
        set_denoise(0.0f);
        set_reverb(0.0f);
    }

    GainRamp gain_ramp_, pan_l_ramp_, pan_r_ramp_, mute_ramp_;
    std::string name_;
    float sample_rate_{48000.0f};

    std::atomic<float> gain_db_{0.0f};
    std::atomic<float> gain_linear_{1.0f};
    std::atomic<float> pan_{0.0f};
    std::atomic<float> pan_left_{1.0f};
    std::atomic<float> pan_right_{1.0f};

    std::atomic<bool> mute_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> solo_{false};
    std::atomic<bool> mono_{false};
    std::atomic<bool> eq_enabled_{false};
    std::atomic<bool> comp_enabled_{false};
    std::atomic<float> comp_amount_{0.0f};

    BiquadFilter eq_low_l_, eq_low_r_;
    BiquadFilter eq_mid_l_, eq_mid_r_;
    BiquadFilter eq_high_l_, eq_high_r_;

    Compressor compressor_;
    NoiseGate gate_;
    Denoiser denoiser_;
    Reverb reverb_;

    AudioMeter meter_left_;
    AudioMeter meter_right_;
};

} // namespace metvoice::dsp
