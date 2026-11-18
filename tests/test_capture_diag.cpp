#include "muxella/core/wasapi_backend.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>

int main() {
    std::cout << "=== Pin Pair Loopback Matrix Test ===\n";
    auto renders = muxella::core::WasapiEndpoint::enumerate_devices(muxella::core::DeviceType::Render);
    auto captures = muxella::core::WasapiEndpoint::enumerate_devices(muxella::core::DeviceType::Capture);

    int b1_cap_idx = -1;
    for (size_t i = 0; i < captures.size(); ++i) {
        if (captures[i].friendly_name.find(L"Muxella Out B1") != std::wstring::npos) {
            b1_cap_idx = static_cast<int>(i);
            break;
        }
    }

    if (b1_cap_idx == -1) {
        std::cout << "Could not find Muxella Out B1 capture!\n";
        return 1;
    }

    std::wcout << L"Testing target capture: " << captures[b1_cap_idx].friendly_name << L"\n\n";

    muxella::core::AudioStreamConfig cfg;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.buffer_frames = 480;

    for (size_t r = 0; r < renders.size(); ++r) {
        if (renders[r].friendly_name.find(L"Speakers") != std::wstring::npos ||
            renders[r].friendly_name.find(L"LG") != std::wstring::npos ||
            renders[r].friendly_name.find(L"Digital") != std::wstring::npos ||
            renders[r].friendly_name.find(L"NVIDIA") != std::wstring::npos) {
            continue; // Skip physical speakers/screens
        }

        std::wcout << L"--> Testing Render [" << r << L"]: " << renders[r].friendly_name << L"... ";

        auto cap_ring = std::make_shared<muxella::core::SpscRingBuffer<float>>(16384);
        auto ren_ring = std::make_shared<muxella::core::SpscRingBuffer<float>>(16384);

        muxella::core::WasapiEndpoint cap_ep(captures[b1_cap_idx], cfg, cap_ring);
        muxella::core::WasapiEndpoint ren_ep(renders[r], cfg, ren_ring);

        if (!cap_ep.start() || !ren_ep.start()) {
            std::cout << "Failed to start!\n";
            continue;
        }

        // Send 500ms of 1 kHz tone
        const float freq = 1000.0f;
        std::vector<float> sine_buf(960);
        uint64_t frame_count = 0;
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 400) {
            for (size_t f = 0; f < 480; ++f) {
                float val = 0.5f * std::sin(2.0f * 3.14159265f * freq * (frame_count++) / 48000.0f);
                sine_buf[f * 2] = val;
                sine_buf[f * 2 + 1] = val;
            }
            ren_ring->write(sine_buf.data(), 960);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::vector<float> read_buf(16384);
        size_t available = cap_ring->available_read();
        size_t n = cap_ring->read(read_buf.data(), std::min(available, read_buf.size()));
        float max_amp = 0.0f;
        for (size_t j = 0; j < n; ++j) {
            max_amp = std::max(max_amp, std::abs(read_buf[j]));
        }

        std::cout << "Peak Amplitude received: " << max_amp << "\n";

        ren_ep.stop();
        cap_ep.stop();
    }

    return 0;
}
