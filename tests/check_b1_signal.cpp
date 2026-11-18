#include "muxella/core/wasapi_backend.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>

int main() {
    auto captures = muxella::core::WasapiEndpoint::enumerate_devices(muxella::core::DeviceType::Capture);
    int b1_idx = -1;
    for (size_t i = 0; i < captures.size(); ++i) {
        if (captures[i].friendly_name.find(L"Muxella Out B1") != std::wstring::npos) {
            b1_idx = static_cast<int>(i);
            break;
        }
    }
    if (b1_idx == -1) {
        std::cout << "Muxella Out B1 not found\n";
        return 1;
    }
    std::wcout << L"Reading from Windows Recording Device: " << captures[b1_idx].friendly_name << L"\n";
    auto ring = std::make_shared<muxella::core::SpscRingBuffer<float>>(16384);
    muxella::core::AudioStreamConfig cfg{48000, 2, 480};
    muxella::core::WasapiEndpoint cap_ep(captures[b1_idx], cfg, ring);
    if (!cap_ep.start()) {
        std::cout << "Could not start capture endpoint\n";
        return 1;
    }
    std::cout << "Monitoring for 1.5 seconds...\n";
    float max_peak = 0.0f;
    for (int t = 0; t < 15; ++t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::vector<float> buf(4096);
        size_t n = ring->read(buf.data(), ring->available_read());
        for (size_t j = 0; j < n; ++j) {
            max_peak = std::max(max_peak, std::abs(buf[j]));
        }
    }
    std::cout << "Peak captured signal: " << max_peak << "\n";
    cap_ep.stop();
    return 0;
}
