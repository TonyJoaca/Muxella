#pragma once

#include <cmath>
#include <algorithm>
#include <atomic>

namespace metvoice::dsp {

/**
 * @brief Dynamic Range Compressor with Soft Knee and Ballistics.
 * 
 * Supports stereo linked or independent channel compression.
 * Zero dynamic memory allocations during processing.
 */
class Compressor {
public:
    Compressor(float sample_rate = 48000.0f);

    void configure(float threshold_db, float ratio, float attack_ms,
                   float release_ms, float knee_db, float makeup_gain_db);

    void set_sample_rate(float sample_rate);

    void reset() noexcept;

    /**
     * @brief Process a stereo or multichannel frame.
     * @param channels Number of channels (typically 1 or 2).
     * @param channel_ptrs Pointers to planar audio data.
     * @param frames Number of frames to process.
     */
    void process(size_t channels, float* const* channel_ptrs, size_t frames) noexcept;

    [[nodiscard]] float current_gain_reduction_db() const noexcept {
        return last_gr_db_.load(std::memory_order_relaxed);
    }

private:
    float sample_rate_{48000.0f};
    float threshold_db_{-20.0f};
    float ratio_{4.0f};
    float attack_ms_{10.0f};
    float release_ms_{100.0f};
    float knee_db_{4.0f};
    float makeup_gain_linear_{1.0f};

    float attack_coeff_{0.0f};
    float release_coeff_{0.0f};

    float envelope_{1.0f};
    std::atomic<float> last_gr_db_{0.0f};

    void update_coefficients();
};

} // namespace metvoice::dsp
