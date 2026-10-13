#pragma once

#include "metvoice/core/audio_buffer.hpp"
#include "metvoice/dsp/biquad.hpp"
#include "metvoice/dsp/compressor.hpp"
#include "metvoice/dsp/meter.hpp"
#include <numbers>
#include <atomic>
#include <string>

namespace metvoice::dsp {

/**
 * @brief Input Channel Strip with Gain, Pan, EQ, Dynamics, and Metering.
 */
class DspChannelStrip {
public:
    explicit DspChannelStrip(const std::string& name = "Strip", float sample_rate = 48000.0f)
        : name_(name), sample_rate_(sample_rate), compressor_(sample_rate),
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
        // Constant power pan law
        const float angle = (pan + 1.0f) * 0.25f * static_cast<float>(std::numbers::pi);
        pan_left_.store(std::cos(angle), std::memory_order_relaxed);
        pan_right_.store(std::sin(angle), std::memory_order_relaxed);
    }
    [[nodiscard]] float pan() const noexcept { return pan_.load(std::memory_order_relaxed); }

    void set_mute(bool mute) noexcept { mute_.store(mute, std::memory_order_relaxed); }
    [[nodiscard]] bool is_muted() const noexcept { return mute_.load(std::memory_order_relaxed); }

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

    [[nodiscard]] float gain_reduction_db() const noexcept { return compressor_.current_gain_reduction_db(); }

    [[nodiscard]] float peak_left_db() const noexcept { return meter_left_.peak_db(); }
    [[nodiscard]] float peak_right_db() const noexcept { return meter_right_.peak_db(); }
    [[nodiscard]] float rms_left_db() const noexcept { return meter_left_.rms_db(); }
    [[nodiscard]] float rms_right_db() const noexcept { return meter_right_.rms_db(); }

    /**
     * @brief Process an in-place stereo or mono audio buffer.
     */
    void process(core::AudioBuffer& buffer) noexcept {
        const size_t frames = buffer.frames();
        if (frames == 0) return;

        float* l = buffer.channel_data(0);
        float* r = (buffer.channels() > 1) ? buffer.channel_data(1) : buffer.channel_data(0);

        // Pre-gain & Pan
        const float gain = is_muted() ? 0.0f : gain_linear_.load(std::memory_order_relaxed);
        const float pan_l = pan_left_.load(std::memory_order_relaxed);
        const float pan_r = pan_right_.load(std::memory_order_relaxed);

        for (size_t f = 0; f < frames; ++f) {
            l[f] *= gain * pan_l;
            r[f] *= gain * pan_r;
        }

        // EQ Stage
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

        // Dynamics Stage
        if (comp_enabled_.load(std::memory_order_relaxed)) {
            float* ptrs[2] = {l, r};
            compressor_.process(buffer.channels() > 1 ? 2 : 1, ptrs, frames);
        }

        // Metering Stage
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
    }

    std::string name_;
    float sample_rate_{48000.0f};

    std::atomic<float> gain_db_{0.0f};
    std::atomic<float> gain_linear_{1.0f};
    std::atomic<float> pan_{0.0f};
    std::atomic<float> pan_left_{0.7071f};
    std::atomic<float> pan_right_{0.7071f};

    std::atomic<bool> mute_{false};
    std::atomic<bool> solo_{false};
    std::atomic<bool> eq_enabled_{false};
    std::atomic<bool> comp_enabled_{false};

    BiquadFilter eq_low_l_, eq_low_r_;
    BiquadFilter eq_mid_l_, eq_mid_r_;
    BiquadFilter eq_high_l_, eq_high_r_;

    Compressor compressor_;
    AudioMeter meter_left_;
    AudioMeter meter_right_;
};

} // namespace metvoice::dsp
