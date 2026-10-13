#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <algorithm>
#include <cmath>

namespace metvoice::core {

/**
 * @brief Multi-channel planar 32-bit float audio buffer for real-time DSP.
 * Pre-allocated to avoid dynamic memory allocation on the audio callback path.
 */
class AudioBuffer {
public:
    AudioBuffer() = default;

    AudioBuffer(size_t channels, size_t max_frames)
        : channels_(channels), max_frames_(max_frames), frames_(0) {
        data_.resize(channels * max_frames, 0.0f);
        channel_ptrs_.resize(channels);
        update_pointers();
    }

    void resize_storage(size_t channels, size_t max_frames) {
        channels_ = channels;
        max_frames_ = max_frames;
        frames_ = 0;
        data_.resize(channels * max_frames, 0.0f);
        channel_ptrs_.resize(channels);
        update_pointers();
    }

    void set_frame_count(size_t frames) noexcept {
        frames_ = std::min(frames, max_frames_);
    }

    [[nodiscard]] size_t channels() const noexcept { return channels_; }
    [[nodiscard]] size_t frames() const noexcept { return frames_; }
    [[nodiscard]] size_t max_frames() const noexcept { return max_frames_; }

    [[nodiscard]] float* channel_data(size_t ch) noexcept {
        return channel_ptrs_[ch];
    }

    [[nodiscard]] const float* channel_data(size_t ch) const noexcept {
        return channel_ptrs_[ch];
    }

    void clear() noexcept {
        std::fill(data_.begin(), data_.end(), 0.0f);
    }

    void clear_frames() noexcept {
        for (size_t ch = 0; ch < channels_; ++ch) {
            std::fill_n(channel_ptrs_[ch], frames_, 0.0f);
        }
    }

    /**
     * @brief De-interleave from an interleaved 32-bit float source.
     */
    void deinterleave_from(const float* interleaved, size_t num_frames, size_t source_channels) noexcept {
        const size_t to_process = std::min(num_frames, max_frames_);
        const size_t ch_limit = std::min(channels_, source_channels);
        frames_ = to_process;

        for (size_t ch = 0; ch < ch_limit; ++ch) {
            float* dst = channel_ptrs_[ch];
            for (size_t f = 0; f < to_process; ++f) {
                dst[f] = interleaved[f * source_channels + ch];
            }
        }

        // Zero out unused channels if any
        for (size_t ch = ch_limit; ch < channels_; ++ch) {
            std::fill_n(channel_ptrs_[ch], to_process, 0.0f);
        }
    }

    /**
     * @brief Interleave planar channels to an interleaved 32-bit float destination.
     */
    void interleave_to(float* interleaved, size_t num_frames, size_t dest_channels) const noexcept {
        const size_t to_process = std::min(num_frames, frames_);
        const size_t ch_limit = std::min(channels_, dest_channels);

        for (size_t f = 0; f < to_process; ++f) {
            for (size_t ch = 0; ch < ch_limit; ++ch) {
                interleaved[f * dest_channels + ch] = channel_ptrs_[ch][f];
            }
            for (size_t ch = ch_limit; ch < dest_channels; ++ch) {
                interleaved[f * dest_channels + ch] = 0.0f;
            }
        }
    }

    /**
     * @brief Convert 16-bit signed integer interleaved PCM to 32-bit planar float.
     */
    void from_int16_interleaved(const int16_t* src, size_t num_frames, size_t source_channels) noexcept {
        constexpr float SCALE = 1.0f / 32768.0f;
        const size_t to_process = std::min(num_frames, max_frames_);
        const size_t ch_limit = std::min(channels_, source_channels);
        frames_ = to_process;

        for (size_t ch = 0; ch < ch_limit; ++ch) {
            float* dst = channel_ptrs_[ch];
            for (size_t f = 0; f < to_process; ++f) {
                dst[f] = static_cast<float>(src[f * source_channels + ch]) * SCALE;
            }
        }
    }

    /**
     * @brief Convert 24-bit in 32-bit container signed integer interleaved PCM to 32-bit planar float.
     */
    void from_int24_in_32_interleaved(const int32_t* src, size_t num_frames, size_t source_channels) noexcept {
        constexpr float SCALE = 1.0f / 8388608.0f;
        const size_t to_process = std::min(num_frames, max_frames_);
        const size_t ch_limit = std::min(channels_, source_channels);
        frames_ = to_process;

        for (size_t ch = 0; ch < ch_limit; ++ch) {
            float* dst = channel_ptrs_[ch];
            for (size_t f = 0; f < to_process; ++f) {
                // Arithmetic shift 8 bits to extract signed 24-bit value
                int32_t val24 = src[f * source_channels + ch] >> 8;
                dst[f] = static_cast<float>(val24) * SCALE;
            }
        }
    }

private:
    void update_pointers() noexcept {
        for (size_t ch = 0; ch < channels_; ++ch) {
            channel_ptrs_[ch] = data_.data() + (ch * max_frames_);
        }
    }

    size_t channels_{0};
    size_t max_frames_{0};
    size_t frames_{0};
    std::vector<float> data_;
    std::vector<float*> channel_ptrs_;
};

} // namespace metvoice::core
