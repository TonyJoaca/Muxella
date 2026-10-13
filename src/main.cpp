#include "metvoice/core/wasapi_backend.hpp"
#include "metvoice/core/asio_backend.hpp"
#include "metvoice/core/mmcss.hpp"
#include "metvoice/matrix/engine.hpp"
#include "metvoice/ipc/named_pipe_server.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <csignal>

static std::atomic<bool> g_shutdown{false};

void signal_handler(int) {
    g_shutdown.store(true);
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "=================================================================\n";
    std::cout << "  MetVoice - Open-Source Low-Latency Windows Audio Matrix Engine \n";
    std::cout << "  Clean-Room High-Performance Audio Routing Workstation          \n";
    std::cout << "=================================================================\n\n";

    // 1. Enumerate WASAPI Audio Devices
    std::cout << "[Device Discovery] Enumerating WASAPI Audio Endpoints...\n";
    auto capture_devices = metvoice::core::WasapiEndpoint::enumerate_devices(metvoice::core::DeviceType::Capture);
    auto render_devices = metvoice::core::WasapiEndpoint::enumerate_devices(metvoice::core::DeviceType::Render);

    std::wcout << L"  Available Input / Capture Endpoints (" << capture_devices.size() << L"):\n";
    for (size_t i = 0; i < capture_devices.size(); ++i) {
        std::wcout << L"    [" << i << L"] " << capture_devices[i].friendly_name
                   << (capture_devices[i].is_default ? L" [DEFAULT]" : L"")
                   << L" (" << capture_devices[i].channels << L"ch, "
                   << capture_devices[i].default_sample_rate << L" Hz)\n";
    }

    std::wcout << L"  Available Output / Render Endpoints (" << render_devices.size() << L"):\n";
    for (size_t i = 0; i < render_devices.size(); ++i) {
        std::wcout << L"    [" << i << L"] " << render_devices[i].friendly_name
                   << (render_devices[i].is_default ? L" [DEFAULT]" : L"")
                   << L" (" << render_devices[i].channels << L"ch, "
                   << render_devices[i].default_sample_rate << L" Hz)\n";
    }

    // 2. Discover Registered ASIO 2.0 Drivers
    std::cout << "\n[Hardware Discovery] Scanning Registered ASIO Drivers...\n";
    auto asio_drivers = metvoice::core::AsioHostInterface::enumerate_drivers();
    if (asio_drivers.empty()) {
        std::cout << "  No hardware ASIO drivers registered in HKLM\\SOFTWARE\\ASIO.\n";
    } else {
        for (size_t i = 0; i < asio_drivers.size(); ++i) {
            std::cout << "  [" << i << "] ASIO Driver: " << asio_drivers[i].name
                      << " (" << asio_drivers[i].description << ")\n";
        }
    }

    // 3. Initialize Mixing Engine: 3 Input Strips, 2 Output Buses
    constexpr size_t NUM_STRIPS = 3;
    constexpr size_t NUM_BUSES = 2;
    constexpr size_t BLOCK_SIZE = 480; // 10ms at 48kHz
    constexpr float SAMPLE_RATE = 48000.0f;

    std::cout << "\n[Engine Setup] Initializing 3x2 Matrix Mixer at 48000 Hz, block size " << BLOCK_SIZE << " frames (~10 ms)...\n";
    metvoice::matrix::MixerEngine engine(NUM_STRIPS, NUM_BUSES, BLOCK_SIZE, SAMPLE_RATE);

    engine.set_strip_name(0, "Physical Mic (In 1)");
    engine.set_strip_name(1, "Virtual Aux (In 2)");
    engine.set_strip_name(2, "Virtual Apps (In 3)");

    engine.set_bus_name(0, "Physical Out A1 (Monitors)");
    engine.set_bus_name(1, "Virtual Out B1 (Broadcast/Stream)");

    // Configure Default Routing:
    // Strip 0 (Mic) -> Bus B1 (Broadcast)
    // Strip 1 (Aux) -> Bus A1 (Monitors) and Bus B1 (Broadcast)
    // Strip 2 (Apps) -> Bus A1 (Monitors) and Bus B1 (Broadcast)
    engine.routing().set_route(0, 1, true, 0.0f); // Mic -> Stream
    engine.routing().set_route(1, 0, true, 0.0f); // Aux -> Monitor
    engine.routing().set_route(1, 1, true, -3.0f); // Aux -> Stream (-3dB)
    engine.routing().set_route(2, 0, true, 0.0f); // Apps -> Monitor
    engine.routing().set_route(2, 1, true, -6.0f); // Apps -> Stream (-6dB)

    // Configure Channel Strip 0 EQ & Compressor (Vocal processing)
    engine.strip(0).set_eq_low(-3.0f, 80.0f);  // Low rumble cut
    engine.strip(0).set_eq_mid(+2.0f, 2500.0f, 1.2f); // Speech clarity presence boost
    engine.strip(0).set_eq_high(+1.5f, 10000.0f); // Air / sheen
    engine.strip(0).enable_eq(true);
    engine.strip(0).configure_compressor(-18.0f, 3.5f, 12.0f, 80.0f, 4.0f, 3.0f);
    engine.strip(0).enable_compressor(true);

    // 4. Start Named Pipe IPC Server
    std::cout << "[IPC Server] Starting control interface on \\\\.\\pipe\\MetVoice_Control...\n";
    metvoice::ipc::NamedPipeServer pipe_server(engine);
    pipe_server.start();

    // 5. Connect Real Audio Endpoints if available
    std::unique_ptr<metvoice::core::WasapiEndpoint> capture_ep;
    std::unique_ptr<metvoice::core::WasapiEndpoint> render_ep;

    metvoice::core::AudioStreamConfig stream_cfg;
    stream_cfg.sample_rate = 48000;
    stream_cfg.channels = 2;
    stream_cfg.buffer_frames = BLOCK_SIZE;

    if (!capture_devices.empty()) {
        std::wcout << L"[Endpoint Attach] Attaching Strip 0 to input device: " << capture_devices[0].friendly_name << L"\n";
        capture_ep = std::make_unique<metvoice::core::WasapiEndpoint>(
            capture_devices[0], stream_cfg, engine.strip_input_ring_buffer(0));
        capture_ep->start();
    }

    if (!render_devices.empty()) {
        std::wcout << L"[Endpoint Attach] Attaching Bus 0 to output device: " << render_devices[0].friendly_name << L"\n";
        render_ep = std::make_unique<metvoice::core::WasapiEndpoint>(
            render_devices[0], stream_cfg, engine.bus_output_ring_buffer(0));
        render_ep->start();
    }

    // 6. Real-Time Audio Engine Processing Loop
    std::cout << "\n[Engine Active] Starting Real-Time Mixing Thread with MMCSS Pro Audio scheduling...\n";
    std::thread audio_engine_thread([&]() {
        // Elevate priority via MMCSS Pro Audio
        metvoice::core::ScopedMmcss mmcss(L"Pro Audio");

        const auto frame_duration = std::chrono::microseconds(static_cast<int64_t>(BLOCK_SIZE * 1000000.0f / SAMPLE_RATE));
        auto next_tick = std::chrono::steady_clock::now();

        while (!g_shutdown.load(std::memory_order_relaxed)) {
            engine.process_block(BLOCK_SIZE);

            next_tick += frame_duration;
            auto now = std::chrono::steady_clock::now();
            if (next_tick > now) {
                std::this_thread::sleep_until(next_tick);
            } else {
                // Audio callback overrun / deadline miss detected
                next_tick = now;
            }
        }
    });

    std::cout << "Engine successfully running. Press Enter to shutdown or use metvoice_ctl...\n";
    
    // Non-blocking wait for shutdown signal or Enter key
    while (!g_shutdown.load()) {
        if (WaitForSingleObject(GetStdHandle(STD_INPUT_HANDLE), 200) == WAIT_OBJECT_0) {
            INPUT_RECORD ir[128];
            DWORD read_count = 0;
            if (ReadConsoleInput(GetStdHandle(STD_INPUT_HANDLE), ir, 128, &read_count)) {
                for (DWORD i = 0; i < read_count; ++i) {
                    if (ir[i].EventType == KEY_EVENT && ir[i].Event.KeyEvent.bKeyDown &&
                        ir[i].Event.KeyEvent.wVirtualKeyCode == VK_RETURN) {
                        g_shutdown.store(true);
                        break;
                    }
                }
            }
        }
    }

    std::cout << "\n[Shutdown] Stopping MetVoice engine cleanly...\n";
    g_shutdown.store(true);

    if (capture_ep) capture_ep->stop();
    if (render_ep) render_ep->stop();
    pipe_server.stop();

    if (audio_engine_thread.joinable()) {
        audio_engine_thread.join();
    }

    std::cout << "[Shutdown] Teardown complete.\n";
    return 0;
}
