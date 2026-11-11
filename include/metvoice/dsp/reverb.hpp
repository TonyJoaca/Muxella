#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstddef>

namespace metvoice::dsp {

/**
 * @brief High-quality Schroeder/Freeverb Stereo Algorithmic Reverb.
 * 8 parallel low-pass feedback comb filters + 4 series allpass filters with stereo spread.
 * Zero memory allocation in audio processing thread.
 */
class Reverb {
private:
    struct Comb {
        std::vector<float> buffer;
        size_t buf_size{0};
        size_t buf_idx{0};
        float filter_store{0.0f};

        void init(size_t size) {
            buf_size = size;
            buffer.assign(size, 0.0f);
            buf_idx = 0;
            filter_store = 0.0f;
        }

        inline float process(float input, float feedback, float damp) noexcept {
            const float output = buffer[buf_idx];
            filter_store = (output * (1.0f - damp)) + (filter_store * damp);
            buffer[buf_idx] = input + (filter_store * feedback);
            if (++buf_idx >= buf_size) buf_idx = 0;
            return output;
        }

        void reset() noexcept {
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            buf_idx = 0;
            filter_store = 0.0f;
        }
    };

    struct Allpass {
        std::vector<float> buffer;
        size_t buf_size{0};
        size_t buf_idx{0};

        void init(size_t size) {
            buf_size = size;
            buffer.assign(size, 0.0f);
            buf_idx = 0;
        }

        inline float process(float input) noexcept {
            const float buf_out = buffer[buf_idx];
            const float output = -input + buf_out;
            buffer[buf_idx] = input + (buf_out * 0.5f);
            if (++buf_idx >= buf_size) buf_idx = 0;
            return output;
        }

        void reset() noexcept {
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            buf_idx = 0;
        }
    };

public:
    explicit Reverb(float sample_rate = 48000.0f)
        : sample_rate_(sample_rate) {
        init_buffers();
    }

    void set_sample_rate(float sample_rate) {
        sample_rate_ = sample_rate;
        init_buffers();
    }

    /**
     * @brief Configure via single 0.0 .. 10.0 knob amount.
     * 0.0 = Bypassed (100% dry).
     * 0.1 .. 10.0 = Wet mix from 4% to 45% with lush room reflection.
     */
    void set_amount(float amount) noexcept {
        amount = std::clamp(amount, 0.0f, 10.0f);
        amount_.store(amount, std::memory_order_relaxed);
        if (amount <= 0.01f) {
            enabled_.store(false, std::memory_order_relaxed);
            wet_gain_.store(0.0f, std::memory_order_relaxed);
        } else {
            // Wet gain scales from 0.04 to 0.45
            const float wet = 0.04f + (amount / 10.0f) * 0.41f;
            wet_gain_.store(wet, std::memory_order_relaxed);
            room_size_.store(0.65f + (amount / 10.0f) * 0.25f, std::memory_order_relaxed);
            enabled_.store(true, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool is_enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        for (auto& c : comb_l_) c.reset();
        for (auto& c : comb_r_) c.reset();
        for (auto& a : allpass_l_) a.reset();
        for (auto& a : allpass_r_) a.reset();
    }

    void process(size_t channels, float* const* channel_ptrs, size_t frames) noexcept {
        if (!is_enabled() || channels == 0 || frames == 0 || channel_ptrs == nullptr) {
            return;
        }

        const float wet = wet_gain_.load(std::memory_order_relaxed);
        const float dry = 1.0f - (wet * 0.4f);
        const float room = room_size_.load(std::memory_order_relaxed);
        const float damp = 0.35f;

        float* l = channel_ptrs[0];
        float* r = (channels > 1) ? channel_ptrs[1] : channel_ptrs[0];

        for (size_t f = 0; f < frames; ++f) {
            const float in_sample = 0.5f * (l[f] + r[f]);
            float out_l = 0.0f;
            float out_r = 0.0f;

            // 8 parallel combs
            for (size_t i = 0; i < 8; ++i) {
                out_l += comb_l_[i].process(in_sample, room, damp);
                out_r += comb_r_[i].process(in_sample, room, damp);
            }

            // 4 series allpasses
            for (size_t i = 0; i < 4; ++i) {
                out_l = allpass_l_[i].process(out_l);
                out_r = allpass_r_[i].process(out_r);
            }

            l[f] = (l[f] * dry) + (out_l * wet);
            if (channels > 1) {
                r[f] = (r[f] * dry) + (out_r * wet);
            }
        }
    }

private:
    float sample_rate_{48000.0f};
    std::atomic<bool> enabled_{false};
    std::atomic<float> amount_{0.0f};
    std::atomic<float> wet_gain_{0.0f};
    std::atomic<float> room_size_{0.75f};

    Comb comb_l_[8];
    Comb comb_r_[8];
    Allpass allpass_l_[4];
    Allpass allpass_r_[4];

    void init_buffers() {
        // Standard Freeverb prime tuning delays scaled to sample rate
        const float scale = sample_rate_ / 44100.0f;
        const size_t comb_tunings[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        constexpr size_t stereo_spread = 23;

        for (size_t i = 0; i < 8; ++i) {
            const size_t size_l = static_cast<size_t>(comb_tunings[i] * scale);
            const size_t size_r = static_cast<size_t>((comb_tunings[i] + stereo_spread) * scale);
            comb_l_[i].init(size_l);
            comb_r_[i].init(size_r);
        }

        const size_t allpass_tunings[4] = {556, 441, 341, 225};
        for (size_t i = 0; i < 4; ++i) {
            const size_t size_l = static_cast<size_t>(allpass_tunings[i] * scale);
            const size_t size_r = static_cast<size_t>((allpass_tunings[i] + stereo_spread) * scale);
            allpass_l_[i].init(size_l);
            allpass_r_[i].init(size_r);
        }
    }
};

} // namespace metvoice::dsp
