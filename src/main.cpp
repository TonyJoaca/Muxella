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
    // 3. Initialize Mixing Engine: 8 Input Strips, 8 Output Buses (VoiceMeeter Potato layout)
    constexpr size_t NUM_STRIPS = 8;
    constexpr size_t NUM_BUSES = 8;
    constexpr size_t BLOCK_SIZE = 480; // 10ms at 48kHz
    constexpr float SAMPLE_RATE = 48000.0f;

    std::cout << "\n[Engine Setup] Initializing 8x8 Matrix Mixer at 48000 Hz, block size " << BLOCK_SIZE << " frames (~10 ms)...\n";
    metvoice::matrix::MixerEngine engine(NUM_STRIPS, NUM_BUSES, BLOCK_SIZE, SAMPLE_RATE);

    // 5 Physical Inputs + 3 Virtual Inputs
    engine.set_strip_name(0, "Main Mic");
    engine.set_strip_name(1, "Laptop Mic");
    engine.set_strip_name(2, "Hardware In 3");
    engine.set_strip_name(3, "Hardware In 4");
    engine.set_strip_name(4, "Hardware In 5");
    engine.set_strip_name(5, "Main VAIO (Desktop)");
    engine.set_strip_name(6, "Spotify (AUX VAIO)");
    engine.set_strip_name(7, "Discord (VAIO 3)");

    // 5 Physical Buses + 3 Virtual Buses
    engine.set_bus_name(0, "A1 Speakers");
    engine.set_bus_name(1, "A2 Headphones");
    engine.set_bus_name(2, "A3 Monitors");
    engine.set_bus_name(3, "A4 Aux Out");
    engine.set_bus_name(4, "A5 Extra Out");
    engine.set_bus_name(5, "B1 Broadcast (OBS)");
    engine.set_bus_name(6, "B2 Comms (Discord)");
    engine.set_bus_name(7, "B3 Recording");

    // Configure Default Routing (Matching VoiceMeeter Potato defaults):
    // Mic (0) -> B1 (Broadcast) & B2 (Discord)
    engine.routing().set_route(0, 5, true, 0.0f); // Mic -> B1
    engine.routing().set_route(0, 6, true, 0.0f); // Mic -> B2

    // Main Desktop (5) -> A1 (Speakers)
    engine.routing().set_route(5, 0, true, 0.0f);

    // Spotify (6) -> A1 (Speakers) & B1 (Stream)
    engine.routing().set_route(6, 0, true, 0.0f);
    engine.routing().set_route(6, 5, true, -3.0f);

    // Discord (7) -> A1 (Speakers)
    engine.routing().set_route(7, 0, true, 0.0f);

    // Vocal processing preset on Main Mic (Strip 0)
    engine.strip(0).set_eq_low(-3.0f, 80.0f);
    engine.strip(0).set_eq_mid(+2.0f, 2500.0f, 1.2f);
    engine.strip(0).set_eq_high(+1.5f, 10000.0f);
    engine.strip(0).enable_eq(true);
    engine.strip(0).configure_compressor(-18.0f, 3.5f, 12.0f, 80.0f, 4.0f, 3.0f);
    engine.strip(0).enable_compressor(true);

    // Safety: Start all master buses muted so no unexpected audio plays
    for (size_t b = 0; b < NUM_BUSES; ++b) {
        engine.bus(b).set_mute(true);
    }


    // 4. Initialize Dynamic Endpoint Manager
    timeBeginPeriod(1);
    metvoice::core::EndpointManager ep_mgr(engine);

    // 5. Start Named Pipe IPC Server with EndpointManager
    std::cout << "[IPC Server] Starting control interface on \\\\.\\pipe\\MetVoice_Control...\n";
    metvoice::ipc::NamedPipeServer pipe_server(engine, &ep_mgr);
    pipe_server.start();

    // 6. Connect Default Hardware Endpoints
    if (!capture_devices.empty()) {
        std::wcout << L"[Endpoint Attach] Attaching Strip 0 to default input: " << capture_devices[0].friendly_name << L"\n";
        ep_mgr.attach_input(0, "0");
    }

    if (!render_devices.empty()) {
        std::wcout << L"[Endpoint Attach] Attaching Bus 0 to default output: " << render_devices[0].friendly_name << L"\n";
        ep_mgr.attach_output(0, "0");
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

    pipe_server.stop();

    if (audio_engine_thread.joinable()) {
        audio_engine_thread.join();
    }

    timeEndPeriod(1);


    std::cout << "[Shutdown] Teardown complete.\n";
    return 0;
}
