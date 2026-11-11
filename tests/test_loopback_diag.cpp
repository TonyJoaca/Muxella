#include "metvoice/core/wasapi_backend.hpp"
#include <iostream>
#include <vector>
#include <cmath>

int main() {
    std::cout << "=== Pin Loopback Diagnostic ===\n";
    auto renders = metvoice::core::WasapiEndpoint::enumerate_devices(metvoice::core::DeviceType::Render);
    auto captures = metvoice::core::WasapiEndpoint::enumerate_devices(metvoice::core::DeviceType::Capture);

    std::cout << "\n--- TEST LOOPBACK ON METVOICE MAIN VAIO ---\n";
    for (size_t i = 0; i < renders.size(); ++i) {
        if (renders[i].friendly_name.find(L"MetVoice Main VAIO") != std::wstring::npos) {
            std::wcout << L"Found: " << renders[i].friendly_name << L"\n";
            metvoice::core::DeviceInfo loop_info = renders[i];
            loop_info.type = metvoice::core::DeviceType::LoopbackRender;
            metvoice::core::AudioStreamConfig cfg{48000, 2, 480};
            auto ring = std::make_shared<metvoice::core::SpscRingBuffer<float>>(16384);
            metvoice::core::WasapiEndpoint ep(loop_info, cfg, ring);
            bool ok = ep.start();
            std::cout << "Loopback start result: " << (ok ? "SUCCESS" : "FAILED") << "\n";
            ep.stop();
        }
    }

    return 0;
}
