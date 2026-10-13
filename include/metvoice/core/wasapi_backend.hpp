#pragma once

#include "metvoice/core/audio_device.hpp"
#include "metvoice/core/ring_buffer.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <thread>
#include <memory>
#include <functional>

namespace metvoice::core {

class WasapiEndpoint {
public:
    WasapiEndpoint(const DeviceInfo& info, const AudioStreamConfig& config,
                   std::shared_ptr<SpscRingBuffer<float>> ring_buffer);
    ~WasapiEndpoint();

    WasapiEndpoint(const WasapiEndpoint&) = delete;
    WasapiEndpoint& operator=(const WasapiEndpoint&) = delete;

    bool start();
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] const DeviceInfo& info() const noexcept { return info_; }
    [[nodiscard]] const AudioStreamConfig& config() const noexcept { return config_; }

    static std::vector<DeviceInfo> enumerate_devices(DeviceType type);

private:
    void capture_thread_proc();
    void render_thread_proc();

    DeviceInfo info_;
    AudioStreamConfig config_;
    std::shared_ptr<SpscRingBuffer<float>> ring_buffer_;

    std::atomic<bool> running_{false};
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

} // namespace metvoice::core
