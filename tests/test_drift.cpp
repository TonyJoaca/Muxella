#include "muxella/core/drift_corrector.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>

void test_drift_correction() {
    std::cout << "[Test] Drift Corrector: Closed-loop PI feedback and cubic resampling... ";
    constexpr size_t CHANNELS = 2;
    constexpr size_t TARGET_FILL = 1000;
    muxella::core::ClockDriftCorrector corrector(CHANNELS, TARGET_FILL);

    // Initial ratio is 1.0
    assert(std::abs(corrector.current_ratio() - 1.0) < 1e-4);

    // Simulate buffer over-filling (e.g. 1500 samples in buffer) -> clock is running fast
    for (int i = 0; i < 100; ++i) {
        corrector.update_feedback(1500);
    }

    // Ratio must adapt > 1.0 to consume faster
    assert(corrector.current_ratio() > 1.0001);
    std::cout << "Adapted ratio for fast clock: " << corrector.current_ratio()
              << " (" << corrector.drift_ppm() << " PPM)\n";

    // Simulate actual audio block resampling
    std::vector<float> in_l(512, 0.5f);
    std::vector<float> in_r(512, 0.5f);
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};

    std::vector<float> out_l(512, 0.0f);
    std::vector<float> out_r(512, 0.0f);
    float* out_ptrs[2] = {out_l.data(), out_r.data()};

    size_t consumed = 0;
    size_t generated = corrector.process(in_ptrs, 512, out_ptrs, 500, consumed);

    assert(generated > 0);
    assert(consumed > 0);
    for (size_t i = 0; i < generated; ++i) {
        assert(std::abs(out_l[i] - 0.5f) < 0.01f);
    }

    // Tight virtual-cable max offset clamp (+/- 1000 ppm)
    corrector.reset(TARGET_FILL);
    corrector.set_max_ratio_offset(0.001);
    for (int i = 0; i < 200; ++i) {
        corrector.update_feedback(TARGET_FILL / 4);
    }
    assert(corrector.current_ratio() >= 0.999 - 1e-6);
    assert(corrector.current_ratio() <= 1.001 + 1e-6);

    std::cout << "Drift correction verified PASSED!\n";
}

int main() {
    test_drift_correction();
    return 0;
}
