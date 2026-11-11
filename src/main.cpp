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

int main(int argc, char* argv[]) {
    // Elevate process priority to HIGH so Windows scheduler prioritizes audio threads during CPU load,
    // and opt out of Windows 11 EcoQoS/timer-resolution throttling.
    HANDLE proc = GetCurrentProcess();
    SetPriorityClass(proc, HIGH_PRIORITY_CLASS);
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    if (auto set_proc_info = reinterpret_cast<SetProcessInformationFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")))) {
        struct {
            ULONG Version;
            ULONG ControlMask;
            ULONG StateMask;
        } throttling{1, 0x1u | 0x4u, 0};
        if (!set_proc_info(proc, 4 /* ProcessPowerThrottling */, &throttling, sizeof(throttling))) {
            throttling.ControlMask = 0x1u;
            set_proc_info(proc, 4, &throttling, sizeof(throttling));
        }
    }

    HANDLE owner = CreateMutexW(nullptr, TRUE, L"Local\\MuxellaAudioEngineV2");
    const DWORD mutex_error = GetLastError();
    if (!owner || mutex_error == ERROR_ALREADY_EXISTS) {
        if (owner) CloseHandle(owner);
        std::cerr << "An audio engine already owns the control pipe.\n";
        return 1;
    }
    std::unique_ptr<void, decltype(&CloseHandle)> engine_owner(owner, &CloseHandle);
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    size_t block_size = 480;
    float sample_rate = 48000.0f;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--sample-rate" || arg == "-r") && i + 1 < argc) {
            try {
                sample_rate = std::stof(argv[++i]);
            } catch (...) {}
        } else if ((arg == "--buffer" || arg == "-b") && i + 1 < argc) {
            try {
                block_size = std::stoul(argv[++i]);
            } catch (...) {}
        }
    }

    if (sample_rate <= 0.0f) sample_rate = 48000.0f;
    if (block_size == 0) block_size = 480;

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

    // 3. Initialize Mixing Engine: 8 Input Strips, 8 Output Buses
    constexpr size_t NUM_STRIPS = 8;
    constexpr size_t NUM_BUSES = 8;

    double latency_ms = (static_cast<double>(block_size) / static_cast<double>(sample_rate)) * 1000.0;
    std::cout << "\n[Engine Setup] Initializing 8x8 Matrix Mixer at " << static_cast<uint32_t>(sample_rate)
              << " Hz, block size " << block_size << " frames (~" << std::fixed << std::setprecision(1) << latency_ms << " ms)...\n";
    metvoice::matrix::MixerEngine engine(NUM_STRIPS, NUM_BUSES, block_size, sample_rate);

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
    engine.routing().set_route(6, 5, true, 0.0f);

    // Discord (7) -> A1 (Speakers)
    engine.routing().set_route(7, 0, true, 0.0f);

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
    pipe_server.set_shutdown_handler([&]() { g_shutdown.store(true); });
    pipe_server.start();

    // Hardware endpoints remain idle until the GUI restores an explicit choice.

    // Receive the cable recording endpoints for application playback.
    ep_mgr.attach_input(5, "Muxella Main");
    ep_mgr.attach_input(6, "Muxella Spotify");
    ep_mgr.attach_input(7, "Muxella Discord");

    // Send B mixes to the cable playback endpoints.
    ep_mgr.attach_output(5, "Muxella Out B1");
    ep_mgr.attach_output(6, "Muxella Out B2");
    ep_mgr.attach_output(7, "Muxella Out B3");


    // 6. Real-Time Audio Engine Processing Loop
    std::cout << "\n[Engine Active] Starting Real-Time Mixing Thread with High-Resolution MMCSS scheduling...\n";
    std::thread audio_engine_thread([&]() {
        // Elevate priority via MMCSS Pro Audio and TIME_CRITICAL thread priority
        metvoice::core::ScopedMmcss mmcss(L"Pro Audio");

        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) {
            timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }

        const auto frame_duration = std::chrono::microseconds(static_cast<int64_t>(block_size * 1000000.0f / sample_rate));

        // Pre-roll lead blocks so output ring buffers maintain a steady cushion against OS scheduling jitter
        constexpr size_t LEAD_BLOCKS = 3;
        for (size_t b = 0; b < LEAD_BLOCKS; ++b) {
            engine.process_block(block_size);
        }

        auto next_tick = std::chrono::steady_clock::now();

        while (!g_shutdown.load(std::memory_order_relaxed)) {
            const auto wake_time = std::chrono::steady_clock::now();
            const double wake_jitter_ms = std::max(0.0,
                std::chrono::duration<double, std::milli>(wake_time - next_tick).count());

            engine.process_block(block_size);

            const auto proc_end = std::chrono::steady_clock::now();
            const double process_ms = std::chrono::duration<double, std::milli>(proc_end - wake_time).count();
            const double block_ms = std::chrono::duration<double, std::milli>(frame_duration).count();
            const bool late = wake_jitter_ms > std::max(3.0, block_ms * 0.5);

            next_tick += frame_duration;
            auto now = std::chrono::steady_clock::now();
            bool resync = false;
            if (next_tick > now) {
                const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(next_tick - now).count();
                if (timer && wait_us > 0) {
                    LARGE_INTEGER due_time;
                    due_time.QuadPart = -(wait_us * 10LL); // 100ns units (negative = relative)
                    SetWaitableTimer(timer, &due_time, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, static_cast<DWORD>((wait_us / 1000) + 10));
                } else {
                    std::this_thread::sleep_until(next_tick);
                }
            } else if (now - next_tick > frame_duration * 20) {
                // Severe lag (system freeze / sleep > 200ms) - resync to prevent runaway processing
                next_tick = now;
                resync = true;
            }
            engine.record_scheduler_tick(wake_jitter_ms, process_ms, late, resync);
            // Minor jitter (< 200ms): do not reset next_tick! Process immediately to catch up and prevent DAC starvation!
        }

        if (timer) {
            CloseHandle(timer);
        }
    });

    std::cout << "Engine successfully running. Press Enter to shutdown or use metvoice_ctl...\n";
    
    // Non-blocking wait for shutdown signal or Enter key
    while (!g_shutdown.load()) {
        const DWORD input_wait = WaitForSingleObject(GetStdHandle(STD_INPUT_HANDLE), 200);
        if (input_wait == WAIT_FAILED) Sleep(200);
        if (input_wait == WAIT_OBJECT_0) {
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
