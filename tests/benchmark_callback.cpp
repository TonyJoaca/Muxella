#include "muxella/matrix/engine.hpp"
#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <iomanip>

int main() {
    std::cout << "===============================================================\n";
    std::cout << "  Muxella Audio Engine Benchmark: Real-Time Callback Latency  \n";
    std::cout << "===============================================================\n\n";

    constexpr size_t STRIPS = 8;
    constexpr size_t BUSES = 8;
    constexpr size_t BLOCK_SIZE = 480; // 10 ms at 48kHz
    constexpr size_t ITERATIONS = 50000; // 50,000 blocks = 500 seconds of audio

    std::cout << "Configuring 8x8 Full Matrix Mixer (8 Strips x 8 Buses)...\n";
    muxella::matrix::MixerEngine engine(STRIPS, BUSES, BLOCK_SIZE, 48000.0f);

    // Enable full processing load: EQ + Compressor on all 8 strips, limiter on all 8 buses
    for (size_t i = 0; i < STRIPS; ++i) {
        engine.strip(i).enable_eq(true);
        engine.strip(i).set_eq_low(2.0f, 120.0f);
        engine.strip(i).set_eq_mid(-3.0f, 1500.0f, 1.5f);
        engine.strip(i).set_eq_high(1.0f, 8000.0f);
        engine.strip(i).enable_compressor(true);
        engine.strip(i).configure_compressor(-15.0f, 4.0f, 10.0f, 80.0f, 4.0f, 2.0f);

        // Fully connected matrix: route strip i to all 8 buses
        for (size_t j = 0; j < BUSES; ++j) {
            engine.routing().set_route(i, j, true, -6.0f);
        }
    }

    // Pre-fill input ring buffers with test tone
    std::vector<float> input_block(BLOCK_SIZE * 2, 0.3f);
    for (size_t i = 0; i < STRIPS; ++i) {
        engine.strip_input_ring_buffer(i)->write(input_block.data(), input_block.size());
    }

    std::vector<double> latencies_us;
    latencies_us.reserve(ITERATIONS);

    // Warm-up run
    for (int w = 0; w < 1000; ++w) {
        engine.process_block(BLOCK_SIZE);
        for (size_t i = 0; i < STRIPS; ++i) {
            engine.strip_input_ring_buffer(i)->write(input_block.data(), input_block.size());
            engine.bus_output_ring_buffer(i)->discard(BLOCK_SIZE * 2);
        }
    }

    std::cout << "Executing " << ITERATIONS << " real-time audio callback blocks...\n";

    auto start_total = std::chrono::high_resolution_clock::now();

    for (size_t iter = 0; iter < ITERATIONS; ++iter) {
        // Replenish inputs
        for (size_t i = 0; i < STRIPS; ++i) {
            engine.strip_input_ring_buffer(i)->write(input_block.data(), input_block.size());
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        engine.process_block(BLOCK_SIZE);
        auto t2 = std::chrono::high_resolution_clock::now();

        // Discard bus outputs
        for (size_t j = 0; j < BUSES; ++j) {
            engine.bus_output_ring_buffer(j)->discard(BLOCK_SIZE * 2);
        }

        double us = std::chrono::duration<double, std::micro>(t2 - t1).count();
        latencies_us.push_back(us);
    }

    auto end_total = std::chrono::high_resolution_clock::now();
    double total_time_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();

    std::sort(latencies_us.begin(), latencies_us.end());

    double min_us = latencies_us.front();
    double max_us = latencies_us.back();
    double median_us = latencies_us[ITERATIONS / 2];
    double p95_us = latencies_us[static_cast<size_t>(ITERATIONS * 0.95)];
    double p99_us = latencies_us[static_cast<size_t>(ITERATIONS * 0.99)];
    double p999_us = latencies_us[static_cast<size_t>(ITERATIONS * 0.999)];
    double sum_us = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
    double avg_us = sum_us / ITERATIONS;

    constexpr double DEADLINE_US = 10000.0; // 10ms deadline at 48kHz / 480 frames
    double cpu_load_percent = (avg_us / DEADLINE_US) * 100.0;

    std::cout << "\nBenchmark Results:\n";
    std::cout << "  Simulated Audio Time : " << (ITERATIONS * 10.0 / 1000.0) << " seconds (500.0 s)\n";
    std::cout << "  Actual Benchmark Wall: " << (total_time_ms / 1000.0) << " seconds\n";
    std::cout << "  Speedup Factor       : " << std::fixed << std::setprecision(1)
              << ((ITERATIONS * 10.0) / total_time_ms) << "x Real-Time\n\n";

    std::cout << "Per-Callback Execution Latency (10ms / 480-frame budget = 10,000 us):\n";
    std::cout << "  Min Latency     : " << std::setprecision(2) << min_us << " us\n";
    std::cout << "  Median Latency  : " << median_us << " us\n";
    std::cout << "  Average Latency : " << avg_us << " us\n";
    std::cout << "  95th Percentile : " << p95_us << " us\n";
    std::cout << "  99th Percentile : " << p99_us << " us\n";
    std::cout << "  99.9th %ile     : " << p999_us << " us\n";
    std::cout << "  Max Latency     : " << max_us << " us\n\n";

    std::cout << "Real-Time Health & Budget Analysis:\n";
    std::cout << "  Average Audio Thread CPU Load : " << cpu_load_percent << " %\n";
    std::cout << "  Real-Time Safety Headroom    : " << (100.0 - cpu_load_percent) << " %\n";

    uint64_t total_underruns = 0;
    uint64_t total_overruns = 0;
    for (size_t i = 0; i < STRIPS; ++i) {
        total_underruns += engine.strip_input_ring_buffer(i)->underrun_count();
        total_overruns += engine.strip_input_ring_buffer(i)->overrun_count();
    }
    std::cout << "  Ring Buffer Underruns : " << total_underruns << "\n";
    std::cout << "  Ring Buffer Overruns  : " << total_overruns << "\n";

    if (max_us < DEADLINE_US) {
        std::cout << "\n[PASS] All callbacks executed well within the 10ms real-time audio deadline!\n";
    } else {
        std::cout << "\n[WARN] Some callbacks exceeded deadline.\n";
    }

    return 0;
}
