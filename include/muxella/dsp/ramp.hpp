#pragma once
#include <cstddef>

namespace muxella::dsp {
// Audio-thread state only. Control threads publish targets through existing atomics.
class GainRamp {
public:
    explicit GainRamp(float initial = 1.0f) : current_(initial), target_(initial) {}
    void target(float value, size_t samples) noexcept {
        if (value == target_) return;
        target_ = value;
        remaining_ = samples > 0 ? samples : 1;
        step_ = (target_ - current_) / static_cast<float>(remaining_);
    }
    float next() noexcept {
        if (remaining_ && --remaining_ == 0) current_ = target_;
        else if (remaining_) current_ += step_;
        return current_;
    }
    bool silent() const noexcept { return current_ == 0.0f && target_ == 0.0f; }
private:
    float current_, target_, step_{0};
    size_t remaining_{0};
};
}
