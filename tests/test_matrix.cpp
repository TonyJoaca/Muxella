#include "metvoice/matrix/engine.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>

void test_matrix_summing_and_routing() {
    std::cout << "[Test] Matrix: Summing math and routing crosspoints... ";
    constexpr size_t STRIPS = 4;
    constexpr size_t BUSES = 2;
    constexpr size_t FRAMES = 256;

    metvoice::matrix::MixerEngine engine(STRIPS, BUSES, FRAMES);

    // Route Strip 0 -> Bus 0 (gain 0dB = 1.0)
    // Route Strip 1 -> Bus 0 (gain -6dB ~ 0.501)
    // Strip 2 and Strip 3 unrouted to Bus 0
    engine.routing().set_route(0, 0, true, 0.0f);
    engine.routing().set_route(1, 0, true, -6.0206f);

    // Feed known signals into Strip 0 and Strip 1
    std::vector<float> strip0_in(FRAMES * 2, 0.4f); // stereo 0.4
    std::vector<float> strip1_in(FRAMES * 2, 0.6f); // stereo 0.6
    engine.strip_input_ring_buffer(0)->write(strip0_in.data(), strip0_in.size());
    engine.strip_input_ring_buffer(1)->write(strip1_in.data(), strip1_in.size());

    engine.process_block(FRAMES);

    std::vector<float> bus0_out(FRAMES * 2, 0.0f);
    size_t read_cnt = engine.bus_output_ring_buffer(0)->read(bus0_out.data(), bus0_out.size());
    (void)read_cnt;
    assert(read_cnt == FRAMES * 2);

    // Expected sum: Strip 0 (0.4) + Strip 1 (0.6 * 0.5 = 0.3) = ~0.7
    // With constant-power center pan (0.7071 * gain):
    // 0.4 * 0.7071 + 0.6 * 0.5 * 0.7071 = 0.28284 + 0.21213 = 0.495
    for (size_t i = 0; i < FRAMES * 2; ++i) {
        assert(std::abs(bus0_out[i] - 0.495f) < 0.02f);
    }
    std::cout << "PASSED\n";
}

void test_solo_and_mute_arbitration() {
    std::cout << "[Test] Matrix: Solo and Mute arbitration logic... ";
    constexpr size_t STRIPS = 3;
    constexpr size_t BUSES = 1;
    constexpr size_t FRAMES = 128;

    metvoice::matrix::MixerEngine engine(STRIPS, BUSES, FRAMES);
    engine.routing().set_route(0, 0, true, 0.0f);
    engine.routing().set_route(1, 0, true, 0.0f);
    engine.routing().set_route(2, 0, true, 0.0f);

    // Set Solo on Strip 1 only!
    engine.strip(1).set_solo(true);

    std::vector<float> audio_in(FRAMES * 2, 1.0f);
    engine.strip_input_ring_buffer(0)->write(audio_in.data(), audio_in.size());
    engine.strip_input_ring_buffer(1)->write(audio_in.data(), audio_in.size());
    engine.strip_input_ring_buffer(2)->write(audio_in.data(), audio_in.size());

    engine.process_block(FRAMES);

    std::vector<float> bus_out(FRAMES * 2, 0.0f);
    engine.bus_output_ring_buffer(0)->read(bus_out.data(), bus_out.size());

    // Only Strip 1 should be heard; Strips 0 and 2 must be muted!
    // Strip 1 output = 1.0 * 0.7071 = ~0.7071
    for (size_t i = 0; i < FRAMES * 2; ++i) {
        assert(std::abs(bus_out[i] - 0.7071f) < 0.02f);
    }

    // Now test Mute on soloed strip
    engine.strip(1).set_mute(true);
    engine.strip_input_ring_buffer(1)->write(audio_in.data(), audio_in.size());
    engine.process_block(FRAMES);
    engine.bus_output_ring_buffer(0)->read(bus_out.data(), bus_out.size());

    // Should be complete silence now
    for (size_t i = 0; i < FRAMES * 2; ++i) {
        assert(std::abs(bus_out[i]) < 1e-5f);
    }

    std::cout << "PASSED\n";
}

void test_starve_fade_continuity() {
    std::cout << "[Test] Matrix: Smooth fade-out and fade-in across strip starve... ";
    constexpr size_t FRAMES = 256;
    metvoice::matrix::MixerEngine engine(1, 1, FRAMES);
    engine.routing().set_route(0, 0, true, 0.0f);
    engine.input_connection_state(0)->store(true);

    // Prime strip with > 2048 floats (1024+ frames) of steady 0.8f audio
    std::vector<float> audio_in(2048, 0.8f);
    engine.strip_input_ring_buffer(0)->write(audio_in.data(), audio_in.size());
    engine.process_block(FRAMES);

    std::vector<float> bus_out(FRAMES * 2, 0.0f);
    size_t got = engine.bus_output_ring_buffer(0)->read(bus_out.data(), bus_out.size());
    (void)got;
    assert(got == FRAMES * 2);
    // First block fades in smoothly from 0 instead of stepping straight to full level
    assert(std::abs(bus_out[0]) < 0.05f);
    assert(bus_out[FRAMES * 2 - 1] > 0.4f);

    // Drain remaining samples so next block triggers StripStarve
    std::vector<float> drain(engine.strip_input_ring_buffer(0)->available_read());
    engine.strip_input_ring_buffer(0)->read(drain.data(), drain.size());
    engine.process_block(FRAMES);
    got = engine.bus_output_ring_buffer(0)->read(bus_out.data(), bus_out.size());
    (void)got;
    assert(got == FRAMES * 2);
    assert(engine.strip_telemetry(0).starve_events.load() == 1);
    // Starved block ramps smoothly downward from previous level rather than instant 0 step
    assert(bus_out[0] > 0.3f);
    assert(std::abs(bus_out[FRAMES * 2 - 1]) < 1e-5f);

    std::cout << "PASSED\n";
}

int main() {
    test_matrix_summing_and_routing();
    test_solo_and_mute_arbitration();
    test_starve_fade_continuity();
    std::cout << "All Matrix Routing tests passed successfully!\n";
    return 0;
}
