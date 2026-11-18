#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace muxella::core {

enum class GlitchStage : uint8_t {
    Capture = 0,
    StripInput = 1,
    StripDsp = 2,
    BusOut = 3,
    Render = 4,
    EngineTimer = 5
};

enum class GlitchKind : uint8_t {
    WasapiDiscontinuity = 0,
    StripStarve = 1,
    StripUnprime = 2,
    StripTruncate = 3,
    RenderUnderrun = 4,
    RenderDiscard = 5,
    RenderRebuffer = 6,
    StepJump = 7,
    ZeroDrop = 8,
    LateTick = 9,
    TimerResync = 10
};

struct AudioDebugEvent {
    uint64_t timestamp_ms{0};
    GlitchStage stage{GlitchStage::Capture};
    uint8_t index{0};
    GlitchKind kind{GlitchKind::StepJump};
    float value{0.0f};
};

inline uint64_t debug_now_ms() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/**
 * @brief Lightweight lock-free waveform discontinuity detector.
 *
 * Flags two unmistakable digital glitch patterns without heap allocation:
 *  - ZeroDrop: non-trivial signal (active_peak > 0.025f) dropping to exact silence for 8+ frames.
 *  - StepJump: extreme single-sample discontinuity (|x[n] - x[n-1]| > 0.65f).
 */
struct SignalGlitchDetector {
    float prev_l{0.0f};
    float prev_r{0.0f};
    float active_peak{0.0f};
    uint32_t zero_run{0};
    bool initialized{false};

    void reset() noexcept {
        prev_l = prev_r = active_peak = 0.0f;
        zero_run = 0;
        initialized = false;
    }

    inline bool check_frame(float l, float r, GlitchKind& out_kind, float& out_val) noexcept {
        if (!initialized) {
            prev_l = l;
            prev_r = r;
            active_peak = std::max(std::abs(l), std::abs(r));
            initialized = true;
            return false;
        }
        const float dl = std::abs(l - prev_l);
        const float dr = std::abs(r - prev_r);
        const float max_diff = std::max(dl, dr);
        const float mag = std::max(std::abs(l), std::abs(r));

        prev_l = l;
        prev_r = r;

        active_peak = std::max(mag, active_peak * 0.995f);

        if (mag < 1e-6f) {
            if (active_peak > 0.025f) {
                ++zero_run;
                if (zero_run == 8) {
                    out_kind = GlitchKind::ZeroDrop;
                    out_val = active_peak;
                    active_peak = 0.0f;
                    return true;
                }
            }
        } else {
            zero_run = 0;
        }

        if (max_diff > 0.65f) {
            out_kind = GlitchKind::StepJump;
            out_val = max_diff;
            return true;
        }
        return false;
    }
};

} // namespace muxella::core
