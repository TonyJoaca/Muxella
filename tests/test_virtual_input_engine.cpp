#include "muxella/core/wasapi_backend.hpp"
#include "muxella/core/endpoint_manager.hpp"
#include "muxella/matrix/engine.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>

int main() {
    std::cout << "=== End-to-End Virtual Input Engine Test ===\n";
    constexpr size_t NUM_STRIPS = 8;
    constexpr size_t NUM_BUSES = 8;
    constexpr size_t BLOCK_SIZE = 480;
    constexpr float SAMPLE_RATE = 48000.0f;

    muxella::matrix::MixerEngine engine(NUM_STRIPS, NUM_BUSES, BLOCK_SIZE, SAMPLE_RATE);
    muxella::core::EndpointManager ep_mgr(engine);

    // Route Strip 5 -> Bus 0
    engine.routing().set_route(5, 0, true, 0.0f);
    engine.bus(0).set_mute(false);

    // Attach Strip 5 to Muxella Main VAIO
    bool attached = ep_mgr.attach_input(5, "Muxella Main VAIO");
    std::cout << "Attach Strip 5 result: " << (attached ? "SUCCESS" : "FAILED") << "\n";
    if (!attached) return 1;

    // Start a test playback into Muxella Main VAIO to simulate an app playing sound
    auto renders = muxella::core::WasapiEndpoint::enumerate_devices(muxella::core::DeviceType::Render);
    muxella::core::DeviceInfo main_vaio_dev;
    bool found_render = false;
    for (const auto& dev : renders) {
        if (dev.friendly_name.find(L"Muxella Main VAIO") != std::wstring::npos) {
            main_vaio_dev = dev;
            found_render = true;
            break;
        }
    }
    if (!found_render) {
        std::cerr << "Render device not found!\n";
        return 1;
    }

    muxella::core::AudioStreamConfig cfg{48000, 2, 480};
    auto test_play_ring = std::make_shared<muxella::core::SpscRingBuffer<float>>(32768);
    muxella::core::WasapiEndpoint test_player(main_vaio_dev, cfg, test_play_ring);
    if (!test_player.start()) {
        std::cerr << "Failed to start test player on Muxella Main VAIO\n";
        return 1;
    }

    // Run engine processing loop for 1.5 seconds while feeding 440Hz sine wave
    std::vector<float> sine_chunk(BLOCK_SIZE * 2);
    double phase = 0.0;
    double phase_inc = 2.0 * 3.141592653589793 * 440.0 / 48000.0;
    float max_strip5_peak = -120.0f;
    float max_bus0_peak = -120.0f;

    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() < 1500) {
        // Feed player
        if (test_play_ring->available_write() >= sine_chunk.size()) {
            for (size_t f = 0; f < BLOCK_SIZE; ++f) {
                float sample = static_cast<float>(std::sin(phase) * 0.5); // -6 dB
                phase += phase_inc;
                if (phase >= 2.0 * 3.141592653589793) phase -= 2.0 * 3.141592653589793;
                sine_chunk[f * 2 + 0] = sample;
                sine_chunk[f * 2 + 1] = sample;
            }
            test_play_ring->write(sine_chunk.data(), sine_chunk.size());
        }

        // Process audio engine block
        engine.process_block(BLOCK_SIZE);

        float s5 = std::max(engine.strip(5).peak_left_db(), engine.strip(5).peak_right_db());
        float b0 = std::max(engine.bus(0).peak_left_db(), engine.bus(0).peak_right_db());
        if (s5 > max_strip5_peak) max_strip5_peak = s5;
        if (b0 > max_bus0_peak) max_bus0_peak = b0;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    test_player.stop();
    ep_mgr.detach_input(5);

    std::cout << "Max Strip 5 Peak dB: " << max_strip5_peak << " dB\n";
    std::cout << "Max Bus 0 Peak dB: " << max_bus0_peak << " dB\n";

    if (max_strip5_peak > -15.0f && max_bus0_peak > -15.0f) {
        std::cout << ">>> TEST PASSED: Virtual input audio ingested into engine and routed to Bus 0! <<<\n";
        return 0;
    } else {
        std::cerr << ">>> TEST FAILED: Audio did not reach expected peak levels in engine! <<<\n";
        return 1;
    }
}
