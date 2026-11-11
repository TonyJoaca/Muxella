#include "metvoice/core/wasapi_backend.hpp"
#include <iostream>
#include <windows.h>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>

typedef struct tagVBVMR_AUDIOBUFFER {
    long audiobuffer_sr;
    long audiobuffer_nbs;
    long audiobuffer_nbi;
    long audiobuffer_nbo;
    float* audiobuffer_r[128];
    float* audiobuffer_w[128];
} VBVMR_T_AUDIOBUFFER;

typedef long (__stdcall *T_VBVMR_VBAUDIOCALLBACK)(void* lpUser, long nCommand, void* lpData, long nnn);
typedef long (__stdcall *T_VBVMR_Login)(void);
typedef long (__stdcall *T_VBVMR_Logout)(void);
typedef long (__stdcall *T_VBVMR_AudioCallbackRegister)(long mode, T_VBVMR_VBAUDIOCALLBACK pCallback, void* lpUser, char szClientName[64]);
typedef long (__stdcall *T_VBVMR_AudioCallbackStart)(void);
typedef long (__stdcall *T_VBVMR_AudioCallbackStop)(void);
typedef long (__stdcall *T_VBVMR_AudioCallbackUnregister)(void);

static uint64_t g_frames = 0;
static long __stdcall MyAudioCallback(void* lpUser, long nCommand, void* lpData, long nnn) {
    if (nCommand == 11) { // VBVMR_CBCOMMAND_BUFFER_OUT
        VBVMR_T_AUDIOBUFFER* buf = (VBVMR_T_AUDIOBUFFER*)lpData;
        if (buf) {
            // Write to B1 (40, 41)
            if (buf->audiobuffer_nbo > 41 && buf->audiobuffer_w[40] && buf->audiobuffer_w[41]) {
                for (long i = 0; i < buf->audiobuffer_nbs; ++i) {
                    float s = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * (g_frames++) / (float)buf->audiobuffer_sr);
                    buf->audiobuffer_w[40][i] = s;
                    buf->audiobuffer_w[41][i] = s;
                }
            }
            // Write to B2 (48, 49)
            if (buf->audiobuffer_nbo > 49 && buf->audiobuffer_w[48] && buf->audiobuffer_w[49]) {
                for (long i = 0; i < buf->audiobuffer_nbs; ++i) {
                    float s = 0.5f * std::sin(2.0f * 3.14159265f * 500.0f * (g_frames++) / (float)buf->audiobuffer_sr);
                    buf->audiobuffer_w[48][i] = s;
                    buf->audiobuffer_w[49][i] = s;
                }
            }
        }
    }
    return 0;
}

int main() {
    std::cout << "=== Testing Voicemeeter Remote B1 and B2 Injection ===\n";
    HMODULE hDll = LoadLibraryW(L"C:\\Program Files (x86)\\VB\\Voicemeeter\\VoicemeeterRemote64.dll");
    if (!hDll) return 1;

    auto fnLogin = (T_VBVMR_Login)GetProcAddress(hDll, "VBVMR_Login");
    auto fnLogout = (T_VBVMR_Logout)GetProcAddress(hDll, "VBVMR_Logout");
    auto fnCbReg = (T_VBVMR_AudioCallbackRegister)GetProcAddress(hDll, "VBVMR_AudioCallbackRegister");
    auto fnCbStart = (T_VBVMR_AudioCallbackStart)GetProcAddress(hDll, "VBVMR_AudioCallbackStart");
    auto fnCbStop = (T_VBVMR_AudioCallbackStop)GetProcAddress(hDll, "VBVMR_AudioCallbackStop");
    auto fnCbUnreg = (T_VBVMR_AudioCallbackUnregister)GetProcAddress(hDll, "VBVMR_AudioCallbackUnregister");

    typedef long (__stdcall *T_VBVMR_RunVoicemeeter)(long vType);
    auto fnRun = (T_VBVMR_RunVoicemeeter)GetProcAddress(hDll, "VBVMR_RunVoicemeeter");

    long login = fnLogin();
    std::cout << "VBVMR_Login returned: " << login << "\n";
    if (login != 0 && fnRun) {
        std::cout << "Starting Voicemeeter Potato via VBVMR_RunVoicemeeter(6)...\n";
        fnRun(6);
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            fnLogout();
            login = fnLogin();
            if (login == 0) {
                std::cout << "Voicemeeter successfully connected!\n";
                break;
            }
        }
    }

    char clientName[64] = "MetVoiceBridge";
    long reg = fnCbReg(2, MyAudioCallback, nullptr, clientName);
    std::cout << "VBVMR_AudioCallbackRegister returned: " << reg << "\n";
    long start_res = fnCbStart();
    std::cout << "VBVMR_AudioCallbackStart returned: " << start_res << "\n";

    auto captures = metvoice::core::WasapiEndpoint::enumerate_devices(metvoice::core::DeviceType::Capture);
    metvoice::core::AudioStreamConfig cfg{48000, 2, 480};

    // Check B1 and B2
    for (size_t i = 0; i < captures.size(); ++i) {
        if (captures[i].friendly_name.find(L"MetVoice Out B1") != std::wstring::npos ||
            captures[i].friendly_name.find(L"MetVoice Out B2") != std::wstring::npos) {
            std::wcout << L"Testing capture: " << captures[i].friendly_name << L"... ";
            auto ring = std::make_shared<metvoice::core::SpscRingBuffer<float>>(16384);
            metvoice::core::WasapiEndpoint cap_ep(captures[i], cfg, ring);
            if (cap_ep.start()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                std::vector<float> read_buf(16384);
                size_t n = ring->read(read_buf.data(), ring->available_read());
                float max_amp = 0.0f;
                for (size_t j = 0; j < n; ++j) max_amp = std::max(max_amp, std::abs(read_buf[j]));
                std::cout << "Peak amp: " << max_amp << "\n";
                cap_ep.stop();
            }
        }
    }
    std::cout << "Total callback frames called: " << g_frames << "\n";

    fnCbStop();
    fnCbUnreg();
    fnLogout();
    FreeLibrary(hDll);
    return 0;
}
