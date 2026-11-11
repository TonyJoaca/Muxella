#pragma once

#include "metvoice/core/audio_buffer.hpp"
#include "metvoice/dsp/biquad.hpp"
#include "metvoice/dsp/limiter.hpp"
#include "metvoice/dsp/meter.hpp"
#include "metvoice/dsp/ramp.hpp"
#include <atomic>
#include <string>

namespace metvoice::dsp {

/**
 * @brief Master Output Bus with summing buffer, Mono downmix, EQ, Limiter, and Metering.
 */
class MasterBus {
public:
    explicit MasterBus(const std::string& name = "Bus", float sample_rate = 48000.0f)
        : name_(name), sample_rate_(sample_rate),
          meter_left_(sample_rate), meter_right_(sample_rate) {
        set_gain_db(0.0f);
        set_eq_low(0.0f, 100.0f);
        set_eq_mid(0.0f, 1000.0f, 1.0f);
        set_eq_high(0.0f, 8000.0f);
    }

    void set_name(const std::string& name) { name_ = name; }
    [[nodiscard]] const std::string& name() const { return name_; }

    void set_gain_db(float gain_db) noexcept {
        gain_db_.store(gain_db, std::memory_order_relaxed);
        gain_linear_.store(std::pow(10.0f, gain_db / 20.0f), std::memory_order_relaxed);
    }
    [[nodiscard]] float gain_db() const noexcept { return gain_db_.load(std::memory_order_relaxed); }

    void set_mute(bool mute) noexcept { mute_.store(mute, std::memory_order_relaxed); }
    [[nodiscard]] bool is_muted() const noexcept { return mute_.load(std::memory_order_relaxed); }
    void set_enabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool is_enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void reset_processing() noexcept {
        eq_low_l_.reset(); eq_low_r_.reset(); eq_mid_l_.reset(); eq_mid_r_.reset();
        eq_high_l_.reset(); eq_high_r_.reset(); limiter_.reset();
        meter_left_.reset(); meter_right_.reset();
        gain_ramp_ = GainRamp(gain_linear_.load(std::memory_order_relaxed));
        mute_ramp_ = GainRamp(0.0f);
    }

    void set_mono(bool mono) noexcept { mono_.store(mono, std::memory_order_relaxed); }
    [[nodiscard]] bool is_mono() const noexcept { return mono_.load(std::memory_order_relaxed); }

    void enable_eq(bool enable) noexcept { eq_enabled_.store(enable, std::memory_order_relaxed); }
    [[nodiscard]] bool is_eq_enabled() const noexcept { return eq_enabled_.load(std::memory_order_relaxed); }
    void enable_limiter(bool enable) noexcept { limiter_enabled_.store(enable, std::memory_order_relaxed); }

    void set_eq_low(float gain_db, float freq_hz = 100.0f) {
        eq_low_gain_.store(gain_db, std::memory_order_relaxed);
        eq_low_freq_.store(freq_hz, std::memory_order_relaxed);
        eq_low_l_.configure(FilterType::LowShelf, sample_rate_, freq_hz, 0.7071f, gain_db);
        eq_low_r_.configure(FilterType::LowShelf, sample_rate_, freq_hz, 0.7071f, gain_db);
    }

    void set_eq_mid(float gain_db, float freq_hz = 1000.0f, float q = 1.0f) {
        eq_mid_gain_.store(gain_db, std::memory_order_relaxed);
        eq_mid_freq_.store(freq_hz, std::memory_order_relaxed);
        eq_mid_q_.store(q, std::memory_order_relaxed);
        eq_mid_l_.configure(FilterType::Peaking, sample_rate_, freq_hz, q, gain_db);
        eq_mid_r_.configure(FilterType::Peaking, sample_rate_, freq_hz, q, gain_db);
    }

    void set_eq_high(float gain_db, float freq_hz = 8000.0f) {
        eq_high_gain_.store(gain_db, std::memory_order_relaxed);
        eq_high_freq_.store(freq_hz, std::memory_order_relaxed);
        eq_high_l_.configure(FilterType::HighShelf, sample_rate_, freq_hz, 0.7071f, gain_db);
        eq_high_r_.configure(FilterType::HighShelf, sample_rate_, freq_hz, 0.7071f, gain_db);
    }

    [[nodiscard]] float eq_low_gain() const noexcept { return eq_low_gain_.load(std::memory_order_relaxed); }
    [[nodiscard]] float eq_mid_gain() const noexcept { return eq_mid_gain_.load(std::memory_order_relaxed); }
    [[nodiscard]] float eq_high_gain() const noexcept { return eq_high_gain_.load(std::memory_order_relaxed); }
    [[nodiscard]] float eq_low_freq() const noexcept { return eq_low_freq_.load(std::memory_order_relaxed); }
    [[nodiscard]] float eq_mid_freq() const noexcept { return eq_mid_freq_.load(std::memory_order_relaxed); }
    [[nodiscard]] float eq_mid_q() const noexcept { return eq_mid_q_.load(std::memory_order_relaxed); }
    [[nodiscard]] float eq_high_freq() const noexcept { return eq_high_freq_.load(std::memory_order_relaxed); }

    [[nodiscard]] float peak_left_db() const noexcept { return meter_left_.peak_db(); }
    [[nodiscard]] float peak_right_db() const noexcept { return meter_right_.peak_db(); }
    [[nodiscard]] float rms_left_db() const noexcept { return meter_left_.rms_db(); }
    [[nodiscard]] float rms_right_db() const noexcept { return meter_right_.rms_db(); }
    [[nodiscard]] uint64_t clip_events() const noexcept { return limiter_.clip_events(); }

    void process(core::AudioBuffer& buffer) noexcept {
        const size_t frames = buffer.frames();
        if (frames == 0) return;

        float* l = buffer.channel_data(0);
        float* r = (buffer.channels() > 1) ? buffer.channel_data(1) : buffer.channel_data(0);

        // 1. Gain & Mute
        gain_ramp_.target(gain_linear_.load(std::memory_order_relaxed), static_cast<size_t>(sample_rate_ * 0.020f));
        mute_ramp_.target(is_muted() ? 0.0f : 1.0f, static_cast<size_t>(sample_rate_ * 0.010f));
        for (size_t f = 0; f < frames; ++f) {
            const float gain = gain_ramp_.next();
            l[f] *= gain;
            if (buffer.channels() > 1) r[f] *= gain;
        }

        // 2. Mono Summing (downmix L & R to mono center)
        if (mono_.load(std::memory_order_relaxed)) {
            for (size_t f = 0; f < frames; ++f) {
                const float m = 0.5f * (l[f] + r[f]);
                l[f] = m;
                r[f] = m;
            }
        }

        // 3. Master EQ
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

        // 4. Soft Limiter ceiling enforcement
        float* ptrs[2] = {l, r};
        if (limiter_enabled_.load(std::memory_order_relaxed))
            limiter_.process(ptrs, buffer.channels() > 1 ? 2 : 1, frames);

        for (size_t f = 0; f < frames; ++f) {
            const float gain = mute_ramp_.next();
            l[f] *= gain;
            if (buffer.channels() > 1) r[f] *= gain;
        }

        // 5. Peak Metering
        meter_left_.process(l, frames);
        if (buffer.channels() > 1) {
            meter_right_.process(r, frames);
        } else {
            meter_right_.process(l, frames);
        }
    }

private:
    GainRamp gain_ramp_, mute_ramp_;
    std::string name_;
    float sample_rate_{48000.0f};

    std::atomic<float> gain_db_{0.0f};
    std::atomic<float> gain_linear_{1.0f};
    std::atomic<bool> mute_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> mono_{false};
    std::atomic<bool> eq_enabled_{false};
    std::atomic<bool> limiter_enabled_{false};
    std::atomic<float> eq_low_gain_{0.0f};
    std::atomic<float> eq_low_freq_{100.0f};
    std::atomic<float> eq_mid_gain_{0.0f};
    std::atomic<float> eq_mid_freq_{1000.0f};
    std::atomic<float> eq_mid_q_{1.0f};
    std::atomic<float> eq_high_gain_{0.0f};
    std::atomic<float> eq_high_freq_{8000.0f};

    BiquadFilter eq_low_l_, eq_low_r_;
    BiquadFilter eq_mid_l_, eq_mid_r_;
    BiquadFilter eq_high_l_, eq_high_r_;

    SoftLimiter limiter_{-0.1f};
    AudioMeter meter_left_;
    AudioMeter meter_right_;
};

} // namespace metvoice::dsp
