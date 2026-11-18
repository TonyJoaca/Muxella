#include "muxella/core/wasapi_backend.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>

int main() {
    std::cout << "=== Scanning which Render Device loops into Muxella Out B1 ===\n";
    auto renders = muxella::core::WasapiEndpoint::enumerate_devices(muxella::core::DeviceType::Render);
    auto captures = muxella::core::WasapiEndpoint::enumerate_devices(muxella::core::DeviceType::Capture);

    int c_idx = -1;
    for (size_t i = 0; i < captures.size(); ++i) {
        if (captures[i].friendly_name.find(L"Muxella Out B1") != std::wstring::npos) {
            c_idx = static_cast<int>(i);
            break;
        }
    }

    if (c_idx == -1) {
        std::cout << "Muxella Out B1 not found!\n";
        return 1;
    }

    muxella::core::AudioStreamConfig cfg{48000, 2, 480};
    auto cap_ring = std::make_shared<muxella::core::SpscRingBuffer<float>>(32768);
    muxella::core::WasapiEndpoint recorder(captures[c_idx], cfg, cap_ring);
    if (!recorder.start()) {
        std::cout << "Failed to start recorder on Muxella Out B1\n";
        return 1;
    }

    for (size_t r = 0; r < renders.size(); ++r) {
        std::wcout << L"Testing Render [" << r << L"] " << renders[r].friendly_name << L"... ";
        std::cout.flush();

        auto play_ring = std::make_shared<muxella::core::SpscRingBuffer<float>>(32768);
        muxella::core::WasapiEndpoint player(renders[r], cfg, play_ring);
        if (!player.start()) {
            std::cout << "Cannot start render\n";
            continue;
        }

        // Drain capture buffer first
        cap_ring->reset();

        // Feed 500ms of 1000Hz sine
        std::vector<float> sine(480 * 2);
        double phase = 0.0;
        float max_cap = 0.0f;
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 400) {
            if (play_ring->available_write() >= sine.size()) {
                for (size_t f = 0; f < 480; ++f) {
                    float s = 0.7f * std::sin(phase);
                    phase += 2.0 * 3.141592653589793 * 1000.0 / 48000.0;
                    sine[f * 2 + 0] = s;
                    sine[f * 2 + 1] = s;
                }
                play_ring->write(sine.data(), sine.size());
            }
            size_t n = cap_ring->available_read();
            if (n > 0) {
                std::vector<float> buf(n);
                cap_ring->read(buf.data(), n);
                for (float s : buf) max_cap = std::max(max_cap, std::abs(s));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        player.stop();
        std::cout << "Peak captured: " << max_cap << "\n";
        if (max_cap > 0.05f) {
            std::wcout << L"*** MATCH FOUND: " << renders[r].friendly_name << L" LOOPS DIRECTLY INTO Muxella Out B1! ***\n";
        }
    }

    recorder.stop();
    return 0;
}
