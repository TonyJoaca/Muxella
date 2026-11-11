#include "metvoice/core/wasapi_backend.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <endpointvolume.h>

static void print_endpoint_volume(const metvoice::core::DeviceInfo& info) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioEndpointVolume* volume = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) &&
        SUCCEEDED(enumerator->GetDevice(info.id.c_str(), &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&volume)))) {
        BOOL muted = FALSE;
        float level = 0.0f;
        volume->GetMute(&muted);
        volume->GetMasterVolumeLevelScalar(&level);
        std::cout << "Endpoint muted=" << muted << ", volume=" << level << "\n";
    }
    if (volume) volume->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (SUCCEEDED(init)) CoUninitialize();
}

int main(int argc, char* argv[]) {
    int bus = 1;
    if (argc > 1) {
        try { bus = std::stoi(argv[1]); } catch (...) { return 2; }
    }
    if (bus < 1 || bus > 3) return 2;
    const auto capture_name = L"MetVoice Out B" + std::to_wstring(bus);
    const auto original_capture_name = L"Voicemeeter Out B" + std::to_wstring(bus);
    const auto link_name = capture_name + L" Link";
    const auto original_link_name = L"Voicemeeter In " + std::to_wstring(bus);
    std::wstring render_override;
    if (argc > 2) {
        for (const char* ch = argv[2]; *ch; ++ch) render_override.push_back(static_cast<unsigned char>(*ch));
    }
    std::cout << "=== Testing playback link to B" << bus << " recording endpoint ===\n";
    auto renders = metvoice::core::WasapiEndpoint::enumerate_devices(metvoice::core::DeviceType::Render);
    auto captures = metvoice::core::WasapiEndpoint::enumerate_devices(metvoice::core::DeviceType::Capture);

    int r_idx = -1;
    for (size_t i = 0; i < renders.size(); ++i) {
        if ((!render_override.empty() && renders[i].friendly_name.find(render_override) != std::wstring::npos) ||
            (render_override.empty() && (renders[i].friendly_name.find(link_name) != std::wstring::npos ||
            renders[i].friendly_name.find(original_link_name) != std::wstring::npos))) {
            r_idx = static_cast<int>(i);
            break;
        }
    }

    int c_idx = -1;
    for (size_t i = 0; i < captures.size(); ++i) {
        if (captures[i].friendly_name.find(capture_name) != std::wstring::npos ||
            captures[i].friendly_name.find(original_capture_name) != std::wstring::npos) {
            c_idx = static_cast<int>(i);
            break;
        }
    }

    if (r_idx == -1 || c_idx == -1) {
        std::cout << "Devices not found: r_idx=" << r_idx << " c_idx=" << c_idx << "\n";
        return 1;
    }

    std::wcout << L"Render (Link): " << renders[r_idx].friendly_name << L"\n";
    print_endpoint_volume(renders[r_idx]);
    std::wcout << L"Capture (Mic): " << captures[c_idx].friendly_name << L"\n";
    print_endpoint_volume(captures[c_idx]);

    metvoice::core::AudioStreamConfig cfg{48000, 2, 480};
    auto play_ring = std::make_shared<metvoice::core::SpscRingBuffer<float>>(32768);
    auto cap_ring = std::make_shared<metvoice::core::SpscRingBuffer<float>>(32768);

    metvoice::core::WasapiEndpoint player(renders[r_idx], cfg, play_ring);
    metvoice::core::WasapiEndpoint recorder(captures[c_idx], cfg, cap_ring);

    if (!recorder.start()) {
        std::cout << "Failed to start recorder on MetVoice Out B1!\n";
        return 1;
    }
    if (!player.start()) {
        std::cout << "Failed to start player on MetVoice Out B1 Link!\n";
        recorder.stop();
        return 1;
    }

    std::cout << "Playing 440Hz sine wave to B" << bus << " playback link for 1.5 seconds...\n";
    std::vector<float> sine(480 * 2);
    double phase = 0.0;
    float max_captured = 0.0f;
    size_t captured_samples = 0;
    size_t captured_frames = 0;
    double tone_sine = 0.0, tone_cosine = 0.0, capture_power = 0.0;
    double window_sine = 0.0, window_cosine = 0.0, window_power = 0.0;
    size_t tone_frames = 0;

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 1500) {
        // Keep 80ms queued; feeding one block per Sleep(10) slowly starves
        // WASAPI and inserts gaps, invalidating a continuous-tone check.
        while (play_ring->available_read() < sine.size() * 8 && play_ring->available_write() >= sine.size()) {
            for (size_t f = 0; f < 480; ++f) {
                float s = 0.5f * std::sin(phase);
                phase += 2.0 * 3.141592653589793 * 440.0 / 48000.0;
                sine[f * 2 + 0] = s;
                sine[f * 2 + 1] = s;
            }
            play_ring->write(sine.data(), sine.size());
        }

        size_t n = cap_ring->available_read();
        if (n > 0) {
            std::vector<float> buf(n);
            cap_ring->read(buf.data(), n);
            captured_samples += n;
            for (float s : buf) max_captured = std::max(max_captured, std::abs(s));
            for (size_t s = 0; s + 1 < n; s += 2) {
                const double sample = (buf[s] + buf[s + 1]) * 0.5;
                const double angle = 2.0 * 3.141592653589793 * 440.0 * captured_frames++ / 48000.0;
                tone_sine += sample * std::sin(angle);
                tone_cosine += sample * std::cos(angle);
                capture_power += sample * sample;
                window_sine += sample * std::sin(angle);
                window_cosine += sample * std::cos(angle);
                window_power += sample * sample;
                if (captured_frames % 2400 == 0) {
                    const double amplitude = 2.0 * std::hypot(window_sine, window_cosine) / 2400.0;
                    const double fraction = window_power > 0.0 ? amplitude * amplitude * 2400.0 / (2.0 * window_power) : 0.0;
                    if (amplitude > 0.001 && fraction > 0.5) tone_frames += 2400;
                    window_sine = window_cosine = window_power = 0.0;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    player.stop();
    recorder.stop();

    std::cout << "Captured samples on B" << bus << ": " << captured_samples << "\n";
    std::cout << "Peak captured signal on B" << bus << ": " << max_captured << "\n";
    const double tone_amplitude = captured_frames ? 2.0 * std::hypot(tone_sine, tone_cosine) / captured_frames : 0.0;
    const double tone_fraction = capture_power > 0.0 ? tone_amplitude * tone_amplitude * captured_frames / (2.0 * capture_power) : 0.0;
    std::cout << "440Hz amplitude: " << tone_amplitude << ", fraction of captured power: " << tone_fraction << "\n";
    std::cout << "Frames with confirmed 440Hz tone: " << tone_frames << "\n";
    if (tone_frames >= 4800) {
        std::cout << ">>> PASS: Playback link delivers a recording signal. <<<\n";
    } else {
        std::cout << ">>> FAIL: No recording signal received in the current driver state. <<<\n";
        return 1;
    }
    return 0;
}
