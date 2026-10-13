#include "metvoice/core/ring_buffer.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <cassert>

void test_single_thread_basic() {
    std::cout << "[Test] Ring Buffer: Single-thread basic read/write... ";
    metvoice::core::SpscRingBuffer<float> rb(128);
    assert(rb.capacity() == 128);
    assert(rb.available_read() == 0);
    assert(rb.available_write() == 128);

    std::vector<float> input = {1.0f, 2.0f, 3.0f, 4.0f};
    size_t written = rb.write(input.data(), input.size());
    assert(written == 4);
    assert(rb.available_read() == 4);

    std::vector<float> output(4, 0.0f);
    size_t read_count = rb.read(output.data(), 4);
    assert(read_count == 4);
    assert(output == input);
    assert(rb.available_read() == 0);
    std::cout << "PASSED\n";
}

void test_overrun_and_underrun() {
    std::cout << "[Test] Ring Buffer: Overrun and underrun tracking... ";
    metvoice::core::SpscRingBuffer<float> rb(64);

    // Try to read when empty -> underrun
    std::vector<float> out(10);
    size_t read_cnt = rb.read(out.data(), 10);
    assert(read_cnt == 0);
    assert(rb.underrun_count() == 10);

    // Fill buffer completely
    std::vector<float> in(64, 1.0f);
    size_t w = rb.write(in.data(), 64);
    assert(w == 64);

    // Write more -> overrun
    std::vector<float> extra(10, 2.0f);
    size_t w_extra = rb.write(extra.data(), 10);
    assert(w_extra == 0);
    assert(rb.overrun_count() == 10);
    std::cout << "PASSED\n";
}

void test_multithreaded_stress() {
    std::cout << "[Test] Ring Buffer: Multithreaded SPSC concurrency stress (1,000,000 items)... ";
    metvoice::core::SpscRingBuffer<uint32_t> rb(1024);
    constexpr uint32_t TOTAL_ITEMS = 1000000;

    std::thread producer([&]() {
        uint32_t current = 0;
        uint32_t chunk[64];
        while (current < TOTAL_ITEMS) {
            size_t batch = std::min<size_t>(64, TOTAL_ITEMS - current);
            for (size_t i = 0; i < batch; ++i) {
                chunk[i] = current + static_cast<uint32_t>(i);
            }
            size_t written = 0;
            while (written < batch) {
                written += rb.write(chunk + written, batch - written);
            }
            current += static_cast<uint32_t>(batch);
        }
    });

    std::thread consumer([&]() {
        uint32_t expected = 0;
        uint32_t chunk[64];
        while (expected < TOTAL_ITEMS) {
            size_t batch = std::min<size_t>(64, TOTAL_ITEMS - expected);
            size_t read_cnt = rb.read(chunk, batch);
            for (size_t i = 0; i < read_cnt; ++i) {
                assert(chunk[i] == expected);
                expected++;
            }
        }
    });

    producer.join();
    consumer.join();
    assert(rb.underrun_count() == 0 || rb.available_read() == 0);
    std::cout << "PASSED\n";
}

int main() {
    test_single_thread_basic();
    test_overrun_and_underrun();
    test_multithreaded_stress();
    std::cout << "All SPSC Ring Buffer tests passed successfully!\n";
    return 0;
}
