#include "muxella/core/wasapi_backend.hpp"
#include <iostream>
#include <vector>
#include <cmath>

int main() {
    std::cout << "=== Pin Loopback Diagnostic ===\n";
    auto renders = muxella::core::WasapiEndpoint::enumerate_devices(muxella::core::DeviceType::Render);
    auto captures = muxella::core::WasapiEndpoint::enumerate_devices(muxella::core::DeviceType::Capture);

    std::cout << "\n--- TEST LOOPBACK ON Muxella MAIN VAIO ---\n";
    for (size_t i = 0; i < renders.size(); ++i) {
        if (renders[i].friendly_name.find(L"Muxella Main VAIO") != std::wstring::npos) {
            std::wcout << L"Found: " << renders[i].friendly_name << L"\n";
            muxella::core::DeviceInfo loop_info = renders[i];
            loop_info.type = muxella::core::DeviceType::LoopbackRender;
            muxella::core::AudioStreamConfig cfg{48000, 2, 480};
            auto ring = std::make_shared<muxella::core::SpscRingBuffer<float>>(16384);
            muxella::core::WasapiEndpoint ep(loop_info, cfg, ring);
            bool ok = ep.start();
            std::cout << "Loopback start result: " << (ok ? "SUCCESS" : "FAILED") << "\n";
            ep.stop();
        }
    }

    return 0;
}
