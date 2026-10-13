#pragma once

#include "metvoice/core/audio_buffer.hpp"
#include "metvoice/core/ring_buffer.hpp"
#include "metvoice/core/drift_corrector.hpp"
#include "metvoice/dsp/dsp_strip.hpp"
#include "metvoice/dsp/master_bus.hpp"
#include "metvoice/matrix/router.hpp"
#include <vector>
#include <memory>
#include <string>

namespace metvoice::matrix {

/**
 * @brief Complete Real-Time Audio Mixing and Routing Engine.
 * 
 * Manages N strips and M buses, lock-free ring-buffers, clock drift resamplers,
 * summing matrix, and zero-allocation block processing.
 */
class MixerEngine {
public:
    MixerEngine(size_t num_strips, size_t num_buses, size_t max_block_frames = 1024,
                float sample_rate = 48000.0f);
    ~MixerEngine() = default;

    void set_strip_name(size_t idx, const std::string& name);
    void set_bus_name(size_t idx, const std::string& name);

    [[nodiscard]] dsp::DspChannelStrip& strip(size_t idx) { return *strips_[idx]; }
    [[nodiscard]] const dsp::DspChannelStrip& strip(size_t idx) const { return *strips_[idx]; }

    [[nodiscard]] dsp::MasterBus& bus(size_t idx) { return *buses_[idx]; }
    [[nodiscard]] const dsp::MasterBus& bus(size_t idx) const { return *buses_[idx]; }

    [[nodiscard]] RoutingGrid& routing() noexcept { return routing_; }
    [[nodiscard]] const RoutingGrid& routing() const noexcept { return routing_; }

    [[nodiscard]] std::shared_ptr<core::SpscRingBuffer<float>> strip_input_ring_buffer(size_t idx) {
        return strip_input_rings_[idx];
    }

    [[nodiscard]] std::shared_ptr<core::SpscRingBuffer<float>> bus_output_ring_buffer(size_t idx) {
        return bus_output_rings_[idx];
    }

    /**
     * @brief Execute one synchronous mixing cycle for the given frame count.
     * Guaranteed zero-allocations, lock-free real-time audio thread safe.
     */
    void process_block(size_t frames) noexcept;

    [[nodiscard]] size_t num_strips() const noexcept { return num_strips_; }
    [[nodiscard]] size_t num_buses() const noexcept { return num_buses_; }
    [[nodiscard]] float sample_rate() const noexcept { return sample_rate_; }

private:
    size_t num_strips_;
    size_t num_buses_;
    size_t max_block_frames_;
    float sample_rate_;

    RoutingGrid routing_;
    std::vector<std::unique_ptr<dsp::DspChannelStrip>> strips_;
    std::vector<std::unique_ptr<dsp::MasterBus>> buses_;

    // Ring buffers for interfacing with audio endpoints
    std::vector<std::shared_ptr<core::SpscRingBuffer<float>>> strip_input_rings_;
    std::vector<std::shared_ptr<core::SpscRingBuffer<float>>> bus_output_rings_;

    // Preallocated working buffers (2 channels per strip and bus)
    std::vector<core::AudioBuffer> strip_buffers_;
    std::vector<core::AudioBuffer> bus_buffers_;

    // Intermediate interleaved buffers to avoid heap allocs during transfer
    std::vector<float> temp_interleaved_in_;
    std::vector<float> temp_interleaved_out_;

    // Jitter cushion state per strip to prevent partial-block reads & crackling
    std::vector<bool> strip_primed_;

    // Closed-loop PI clock drift correctors per strip
    std::vector<std::unique_ptr<core::ClockDriftCorrector>> drift_correctors_;
    std::vector<float> drift_in_planar_l_;
    std::vector<float> drift_in_planar_r_;
};

} // namespace metvoice::matrix
