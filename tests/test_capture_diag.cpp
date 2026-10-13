#include "metvoice/core/wasapi_backend.hpp"
#include <iostream>
#include <iomanip>

int main() {
    std::cout << "=== WASAPI Capture Diagnostic Test ===\n";
    auto captures = metvoice::core::WasapiEndpoint::enumerate_devices(metvoice::core::DeviceType::Capture);
    std::cout << "Found " << captures.size() << " capture devices.\n";
    for (size_t i = 0; i < captures.size(); ++i) {
        std::wcout << L"[" << i << L"] " << captures[i].friendly_name
                   << L" (Rate: " << captures[i].default_sample_rate
                   << L", Ch: " << captures[i].channels << L")\n";
    }

    if (captures.empty()) {
        std::cout << "No capture devices found.\n";
        return 1;
    }

    auto ring = std::make_shared<metvoice::core::SpscRingBuffer<float>>(8192);
    metvoice::core::AudioStreamConfig cfg;
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.buffer_frames = 480;

    std::cout << "Testing start() on device [0]...\n";
    metvoice::core::WasapiEndpoint ep(captures[0], cfg, ring);
    bool started = ep.start();
    std::cout << "Device [0] start result: " << (started ? "SUCCESS" : "FAILED") << "\n";
    if (started) {
        Sleep(500);
        std::cout << "Ring buffer available read frames: " << (ring->available_read() / 2) << "\n";
        ep.stop();
        std::cout << "Stopped cleanly.\n";
    }

    return 0;
}
