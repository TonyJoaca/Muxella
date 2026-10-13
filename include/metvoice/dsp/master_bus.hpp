#pragma once

#include "metvoice/core/audio_buffer.hpp"
#include "metvoice/dsp/biquad.hpp"
#include "metvoice/dsp/limiter.hpp"
#include "metvoice/dsp/meter.hpp"
#include <atomic>
#include <string>

namespace metvoice::dsp {

/**
 * @brief Master Output Bus with summing buffer, EQ, Limiter, and Metering.
 */
class MasterBus {
public:
    explicit MasterBus(const std::string& name = "Bus", float sample_rate = 48000.0f)
        : name_(name), sample_rate_(sample_rate),
          meter_left_(sample_rate), meter_right_(sample_rate) {
        set_gain_db(0.0f);
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

        const float gain = is_muted() ? 0.0f : gain_linear_.load(std::memory_order_relaxed);

        for (size_t f = 0; f < frames; ++f) {
            l[f] *= gain;
            r[f] *= gain;
        }

        // Apply soft limiter to prevent clipping
        float* ptrs[2] = {l, r};
        limiter_.process(ptrs, buffer.channels() > 1 ? 2 : 1, frames);

        // Metering
        meter_left_.process(l, frames);
        if (buffer.channels() > 1) {
            meter_right_.process(r, frames);
        } else {
            meter_right_.process(l, frames);
        }
    }

private:
    std::string name_;
    float sample_rate_{48000.0f};

    std::atomic<float> gain_db_{0.0f};
    std::atomic<float> gain_linear_{1.0f};
    std::atomic<bool> mute_{false};

    SoftLimiter limiter_{-0.1f};
    AudioMeter meter_left_;
    AudioMeter meter_right_;
};

} // namespace metvoice::dsp
