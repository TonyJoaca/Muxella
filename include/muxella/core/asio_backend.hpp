#pragma once

#include "muxella/core/audio_device.hpp"
#include "muxella/core/ring_buffer.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace muxella::core {

struct AsioDriverInfo {
    std::string name;
    std::string clsid_str;
    std::string description;
};

/**
 * @brief Clean-room ASIO 2.0 Host Enumeration & Streaming Endpoint.
 *
 * Enumerates registered ASIO drivers via Windows Registry
 * HKEY_LOCAL_MACHINE\SOFTWARE\ASIO and hosts COM x64 ASIO driver callbacks
 * onto lock-free stereo SPSC ring buffers.
 */
class AsioHostInterface {
public:
    static std::vector<AsioDriverInfo> enumerate_drivers();
};

class AsioEndpoint {
public:
    AsioEndpoint(const AsioDriverInfo& driver, DeviceType direction,
                 const AudioStreamConfig& config,
                 std::shared_ptr<SpscRingBuffer<float>> ring_buffer,
                 std::atomic<bool>* connection_state = nullptr);
    ~AsioEndpoint();

    AsioEndpoint(const AsioEndpoint&) = delete;
    AsioEndpoint& operator=(const AsioEndpoint&) = delete;

    bool start();
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t device_sample_rate() const noexcept { return device_sample_rate_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t device_channels() const noexcept { return device_channels_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t device_bits() const noexcept { return device_bits_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t buffer_frames() const noexcept { return buffer_frames_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t underrun_events() const noexcept { return underrun_events_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t discard_events() const noexcept { return discard_events_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t discard_samples() const noexcept { return discard_samples_.load(std::memory_order_relaxed); }
    void reset_telemetry() noexcept {
        underrun_events_.store(0, std::memory_order_relaxed);
        discard_events_.store(0, std::memory_order_relaxed);
        discard_samples_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] const AsioDriverInfo& driver() const noexcept { return driver_; }
    [[nodiscard]] DeviceType direction() const noexcept { return direction_; }

private:
    void worker_proc();
    void set_running(bool value) noexcept {
        running_.store(value, std::memory_order_relaxed);
        if (connection_state_) connection_state_->store(value, std::memory_order_release);
    }

    AsioDriverInfo driver_;
    DeviceType direction_;
    AudioStreamConfig config_;
    std::shared_ptr<SpscRingBuffer<float>> ring_buffer_;
    std::atomic<bool>* connection_state_{nullptr};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_signal_{false};
    std::atomic<uint32_t> device_sample_rate_{0};
    std::atomic<uint16_t> device_channels_{0};
    std::atomic<uint16_t> device_bits_{0};
    std::atomic<uint32_t> buffer_frames_{0};
    std::atomic<uint64_t> underrun_events_{0};
    std::atomic<uint64_t> discard_events_{0};
    std::atomic<uint64_t> discard_samples_{0};

    std::thread worker_thread_;
    HANDLE stop_event_{nullptr};

public:
    struct Impl;
};

} // namespace muxella::core
