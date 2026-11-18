#pragma once

#include "muxella/core/audio_device.hpp"
#include "muxella/core/ring_buffer.hpp"
#include "muxella/core/debug_telemetry.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <thread>
#include <memory>
#include <functional>

namespace muxella::core {

class WasapiEndpoint {
public:
    WasapiEndpoint(const DeviceInfo& info, const AudioStreamConfig& config,
                   std::shared_ptr<SpscRingBuffer<float>> ring_buffer,
                   std::atomic<bool>* connection_state = nullptr,
                   uint8_t stream_index = 0);
    ~WasapiEndpoint();

    WasapiEndpoint(const WasapiEndpoint&) = delete;
    WasapiEndpoint& operator=(const WasapiEndpoint&) = delete;

    bool start();
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool is_exclusive() const noexcept { return exclusive_active_.load(std::memory_order_relaxed); }
    [[nodiscard]] int raw_status() const noexcept { return raw_status_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t device_sample_rate() const noexcept { return device_sample_rate_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t device_channels() const noexcept { return device_channels_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t device_bits() const noexcept { return device_bits_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t wasapi_buffer_frames() const noexcept { return wasapi_buffer_frames_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t discontinuity_events() const noexcept { return discontinuity_events_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t render_discard_events() const noexcept { return render_discard_events_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t render_discard_samples() const noexcept { return render_discard_samples_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t render_underrun_events() const noexcept { return render_underrun_events_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t waveform_glitches() const noexcept { return waveform_glitches_.load(std::memory_order_relaxed); }
    void reset_telemetry() noexcept {
        discontinuity_events_.store(0, std::memory_order_relaxed);
        render_discard_events_.store(0, std::memory_order_relaxed);
        render_discard_samples_.store(0, std::memory_order_relaxed);
        render_underrun_events_.store(0, std::memory_order_relaxed);
        waveform_glitches_.store(0, std::memory_order_relaxed);
        debug_events_.reset();
    }
    size_t drain_debug_events(AudioDebugEvent* dst, size_t max_count) noexcept {
        return debug_events_.read(dst, max_count);
    }
    [[nodiscard]] const DeviceInfo& info() const noexcept { return info_; }
    [[nodiscard]] const AudioStreamConfig& config() const noexcept { return config_; }

    static std::vector<DeviceInfo> enumerate_devices(DeviceType type);

private:
    void push_event(GlitchStage stage, GlitchKind kind, float value) noexcept {
        AudioDebugEvent ev{debug_now_ms(), stage, stream_index_, kind, value};
        debug_events_.write(&ev, 1);
    }
    void set_running(bool value) noexcept {
        running_.store(value, std::memory_order_relaxed);
        if (connection_state_) connection_state_->store(value, std::memory_order_release);
    }
    std::atomic<bool>* connection_state_{nullptr};
    uint8_t stream_index_{0};
    void capture_thread_proc();
    void render_thread_proc();

    DeviceInfo info_;
    AudioStreamConfig config_;
    std::shared_ptr<SpscRingBuffer<float>> ring_buffer_;

    std::atomic<bool> running_{false};
    std::atomic<bool> exclusive_active_{false};
    std::atomic<int> raw_status_{0}; // 0: not requested, 1: accepted, -1: unavailable
    std::atomic<uint32_t> device_sample_rate_{0};
    std::atomic<uint16_t> device_channels_{0};
    std::atomic<uint16_t> device_bits_{0};
    std::atomic<uint32_t> wasapi_buffer_frames_{0};
    std::atomic<uint64_t> discontinuity_events_{0};
    std::atomic<uint64_t> render_discard_events_{0};
    std::atomic<uint64_t> render_discard_samples_{0};
    std::atomic<uint64_t> render_underrun_events_{0};
    std::atomic<uint64_t> waveform_glitches_{0};
    SpscRingBuffer<AudioDebugEvent> debug_events_{128};
    std::atomic<bool> stop_signal_{false};
    std::thread worker_thread_;

    IMMDevice* device_{nullptr};
    IAudioClient* audio_client_{nullptr};
    IAudioCaptureClient* capture_client_{nullptr};
    IAudioRenderClient* render_client_{nullptr};
    HANDLE audio_event_{nullptr};
    WAVEFORMATEX* wave_format_{nullptr};
    UINT32 buffer_frame_count_{0};
};

} // namespace muxella::core
