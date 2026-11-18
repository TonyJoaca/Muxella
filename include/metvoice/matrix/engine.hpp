#pragma once

#include "metvoice/core/audio_buffer.hpp"
#include "metvoice/core/ring_buffer.hpp"
#include "metvoice/core/drift_corrector.hpp"
#include "metvoice/core/debug_telemetry.hpp"
#include "metvoice/core/wav_recorder.hpp"
#include "metvoice/dsp/dsp_strip.hpp"
#include "metvoice/dsp/master_bus.hpp"
#include "metvoice/matrix/router.hpp"
#include <vector>
#include <memory>
#include <string>
#include <array>

namespace metvoice::matrix {

/**
 * @brief Complete Real-Time Audio Mixing and Routing Engine.
 * 
 * Manages N strips and M buses, lock-free ring-buffers, clock drift resamplers,
 * summing matrix, and zero-allocation block processing.
 */
class MixerEngine {
public:
    enum class DspKind { StripEq, Compressor, Gate, Denoise, Reverb, Duck, BusEq, BusDelay };
    struct DspUpdate {
        DspKind kind{};
        size_t index{};
        std::array<float, 7> values{};
    };
    // Single producer: the IPC worker. Only the audio thread applies DSP data.
    bool enqueue_dsp(const DspUpdate& update) noexcept {
        const bool is_bus = (update.kind == DspKind::BusEq || update.kind == DspKind::BusDelay);
        if (update.index >= (is_bus ? num_buses_ : num_strips_)) return false;
        return dsp_updates_.write(&update, 1) == 1;
    }
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
    [[nodiscard]] size_t block_frames() const noexcept { return max_block_frames_; }
    [[nodiscard]] core::WavRecorder& recorder() noexcept { return recorder_; }
    std::atomic<bool>* input_connection_state(size_t index) noexcept { return &input_connected_[index]; }
    std::atomic<bool>* input_virtual_state(size_t index) noexcept { return &input_virtual_cable_[index]; }
    std::atomic<bool>* output_connection_state(size_t index) noexcept { return &output_connected_[index]; }

    struct StripTelemetry {
        std::atomic<uint64_t> starve_events{0};
        std::atomic<uint64_t> unprime_events{0};
        std::atomic<uint64_t> truncate_events{0};
        std::atomic<uint64_t> dsp_glitches{0};
        std::atomic<double> drift_ppm{0.0};
    };
    struct BusTelemetry {
        std::atomic<uint64_t> bus_glitches{0};
    };
    struct SchedulerTelemetry {
        std::atomic<uint64_t> total_blocks{0};
        std::atomic<uint64_t> late_ticks{0};
        std::atomic<uint64_t> timer_resyncs{0};
        std::atomic<double> max_jitter_ms{0.0};
        std::atomic<double> max_process_ms{0.0};
    };

    [[nodiscard]] const StripTelemetry& strip_telemetry(size_t idx) const noexcept { return strip_telemetry_[idx]; }
    [[nodiscard]] const BusTelemetry& bus_telemetry(size_t idx) const noexcept { return bus_telemetry_[idx]; }
    [[nodiscard]] const SchedulerTelemetry& scheduler_telemetry() const noexcept { return scheduler_telemetry_; }
    void record_scheduler_tick(double jitter_ms, double process_ms, bool late, bool resync) noexcept;
    void reset_telemetry() noexcept;
    size_t drain_debug_events(core::AudioDebugEvent* dst, size_t max_count) noexcept {
        return engine_debug_events_.read(dst, max_count);
    }

private:
    void push_engine_event(core::GlitchStage stage, uint8_t index, core::GlitchKind kind, float value) noexcept {
        core::AudioDebugEvent ev{core::debug_now_ms(), stage, index, kind, value};
        engine_debug_events_.write(&ev, 1);
    }
    core::WavRecorder recorder_;
    std::unique_ptr<std::atomic<bool>[]> input_connected_, input_virtual_cable_, output_connected_;
    core::SpscRingBuffer<DspUpdate> dsp_updates_{256};
    size_t num_strips_;
    size_t num_buses_;
    size_t max_block_frames_;
    float sample_rate_;

    RoutingGrid routing_;
    std::vector<dsp::GainRamp> route_ramps_;
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
    std::vector<size_t> strip_starve_count_;
    std::vector<float> strip_last_l_, strip_last_r_;
    std::vector<bool> strip_fade_in_;
    std::vector<bool> strip_was_active_, bus_was_active_, bus_needed_, bus_receives_;

    // Closed-loop PI clock drift correctors per strip
    std::vector<std::unique_ptr<core::ClockDriftCorrector>> drift_correctors_;
    std::vector<float> drift_in_planar_l_;
    std::vector<float> drift_in_planar_r_;

    // Real-time crackle & glitch telemetry
    std::unique_ptr<StripTelemetry[]> strip_telemetry_;
    std::unique_ptr<BusTelemetry[]> bus_telemetry_;
    SchedulerTelemetry scheduler_telemetry_{};
    std::vector<core::SignalGlitchDetector> strip_glitch_detectors_;
    std::vector<core::SignalGlitchDetector> bus_glitch_detectors_;
    core::SpscRingBuffer<core::AudioDebugEvent> engine_debug_events_{256};
};

} // namespace metvoice::matrix
