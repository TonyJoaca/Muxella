#include "metvoice/core/wasapi_backend.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>

int main() {
    std::cout << "=== Loopback Audio Flow Test ===\n";
    auto renders = metvoice::core::WasapiEndpoint::enumerate_devices(metvoice::core::DeviceType::Render);

    metvoice::core::DeviceInfo main_vaio_render;
    bool found = false;
    for (const auto& dev : renders) {
        if (dev.friendly_name.find(L"MetVoice Main VAIO") != std::wstring::npos) {
            main_vaio_render = dev;
            found = true;
            std::wcout << L"Found render device: " << dev.friendly_name << L"\n";
            break;
        }
    }

    if (!found) {
        std::cerr << "MetVoice Main VAIO render device not found!\n";
        return 1;
    }

    // 1. Setup Loopback Capture on MetVoice Main VAIO
    metvoice::core::DeviceInfo loop_info = main_vaio_render;
    loop_info.type = metvoice::core::DeviceType::LoopbackRender;
    metvoice::core::AudioStreamConfig cfg{48000, 2, 480};
    auto capture_ring = std::make_shared<metvoice::core::SpscRingBuffer<float>>(32768);
    metvoice::core::WasapiEndpoint loop_capture(loop_info, cfg, capture_ring);

    if (!loop_capture.start()) {
        std::cerr << "Failed to start loopback capture!\n";
        return 1;
    }
    std::cout << "Loopback capture started successfully.\n";

    // 2. Setup Render on MetVoice Main VAIO and feed a 440Hz sine wave
    auto render_ring = std::make_shared<metvoice::core::SpscRingBuffer<float>>(32768);
    metvoice::core::WasapiEndpoint test_render(main_vaio_render, cfg, render_ring);
    if (!test_render.start()) {
        std::cerr << "Failed to start test render playback!\n";
        loop_capture.stop();
        return 1;
    }
    std::cout << "Test render playback started successfully.\n";

    // Feed 1 second of 440Hz sine wave at -6 dBFS
    std::vector<float> sine_chunk(960 * 2);
    double phase = 0.0;
    double phase_inc = 2.0 * 3.141592653589793 * 440.0 / 48000.0;
    float peak_captured = 0.0f;
    size_t total_captured_samples = 0;

    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() < 1500) {
        // Feed render buffer
        if (render_ring->available_write() >= sine_chunk.size()) {
            for (size_t f = 0; f < 960; ++f) {
                float sample = static_cast<float>(std::sin(phase) * 0.5); // -6 dB
                phase += phase_inc;
                if (phase >= 2.0 * 3.141592653589793) phase -= 2.0 * 3.141592653589793;
                sine_chunk[f * 2 + 0] = sample;
                sine_chunk[f * 2 + 1] = sample;
            }
            render_ring->write(sine_chunk.data(), sine_chunk.size());
        }

        // Read capture buffer
        size_t avail = capture_ring->available_read();
        if (avail > 0) {
            std::vector<float> read_buf(avail);
            capture_ring->read(read_buf.data(), avail);
            total_captured_samples += avail;
            for (float s : read_buf) {
                float a = std::abs(s);
                if (a > peak_captured) peak_captured = a;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    test_render.stop();
    loop_capture.stop();

    std::cout << "Total captured samples: " << total_captured_samples << "\n";
    std::cout << "Peak captured amplitude: " << peak_captured << "\n";
    float peak_db = (peak_captured > 1e-6f) ? 20.0f * std::log10(peak_captured) : -120.0f;
    std::cout << "Peak captured dBFS: " << peak_db << " dB\n";

    if (total_captured_samples > 0 && peak_captured > 0.05f) {
        std::cout << ">>> TEST PASSED: Loopback audio flowed from Render to Capture with signal intact! <<<\n";
        return 0;
    } else {
        std::cerr << ">>> TEST FAILED: No audio or silence captured! <<<\n";
        return 1;
    }
}
