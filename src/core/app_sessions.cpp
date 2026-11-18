#include "metvoice/core/app_sessions.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <cmath>

namespace metvoice::core {
namespace {
using Json = nlohmann::json;
template<class T> struct Com {
    T* ptr{};
    ~Com() { if (ptr) ptr->Release(); }
    T** out() { return &ptr; }
    T* operator->() const { return ptr; }
};
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::string hex(const std::string& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char c : value) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
std::string process_name(DWORD pid) {
    if (!pid) return "System sounds";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return "Process " + std::to_string(pid);
    wchar_t path[32768]; DWORD size = 32768;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    return ok ? utf8(std::filesystem::path(std::wstring(path, size)).filename().wstring()) : "Process " + std::to_string(pid);
}
bool visit(const std::function<void(const Json&, ISimpleAudioVolume*)>& callback) {
    Apartment apartment;
    if (FAILED(apartment.result)) return false;
    Com<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.out())))) return false;
    Com<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.out()))) return false;
    UINT device_count = 0; devices->GetCount(&device_count);
    for (UINT d = 0; d < device_count; ++d) {
        Com<IMMDevice> device;
        if (FAILED(devices->Item(d, device.out()))) continue;
        LPWSTR endpoint_id = nullptr;
        if (FAILED(device->GetId(&endpoint_id))) continue;
        const std::string endpoint = utf8(endpoint_id); CoTaskMemFree(endpoint_id);
        std::string device_name = endpoint;
        Com<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.out()))) {
            PROPVARIANT name; PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
                device_name = utf8(name.pwszVal);
            PropVariantClear(&name);
        }
        Com<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(manager.out())))) continue;
        Com<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(sessions.out()))) continue;
        int count = 0; sessions->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            Com<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(i, control.out()))) continue;
            AudioSessionState state;
            if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired) continue;
            Com<IAudioSessionControl2> control2;
            Com<ISimpleAudioVolume> volume;
            if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(control2.out()))) ||
                FAILED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(volume.out())))) continue;
            LPWSTR instance = nullptr;
            if (FAILED(control2->GetSessionInstanceIdentifier(&instance))) continue;
            const std::string id = hex(endpoint + "|" + utf8(instance)); CoTaskMemFree(instance);
            DWORD pid = 0; control2->GetProcessId(&pid);
            float level = 1.0f; BOOL muted = FALSE;
            if (FAILED(volume->GetMasterVolume(&level)) || FAILED(volume->GetMute(&muted))) continue;
            callback(Json{{"id", id}, {"name", process_name(pid)}, {"pid", pid}, {"device", device_name},
                {"volume", level}, {"mute", muted != FALSE}, {"active", state == AudioSessionStateActive}}, volume.ptr);
        }
    }
    return true;
}
}
std::string app_sessions_json() {
    Json data = {{"type", "apps"}, {"sessions", Json::array()}};
    if (!visit([&](const Json& session, ISimpleAudioVolume*) { data["sessions"].push_back(session); }))
        return Json{{"status", "error"}, {"message", "Windows audio sessions unavailable"}}.dump();
    return data.dump(-1, ' ', false, Json::error_handler_t::replace);
}
std::string set_app_session(const std::string& id, float level, bool muted) {
    if (!std::isfinite(level) || level < 0 || level > 1)
        return Json{{"status", "error"}, {"message", "Volume must be between 0 and 100%"}}.dump();
    bool found = false, ok = false;
    visit([&](const Json& session, ISimpleAudioVolume* volume) {
        if (session["id"] != id || found) return;
        found = true;
        ok = SUCCEEDED(volume->SetMasterVolume(level, nullptr)) && SUCCEEDED(volume->SetMute(muted, nullptr));
    });
    return Json{{"status", ok ? "ok" : "error"},
        {"message", ok ? "Updated" : found ? "Windows rejected the change" : "App session ended; refresh the list"}}.dump();
}

std::vector<AudioProcessEntry> enumerate_audio_processes() {
    std::vector<AudioProcessEntry> entries;
    std::vector<std::string> seen;
    auto lowercase = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    visit([&](const Json& session, ISimpleAudioVolume*) {
        const uint32_t pid = session.value("pid", 0u);
        const std::string name = session.value("name", std::string{});
        if (pid == 0 || name.empty() || pid == GetCurrentProcessId()) return;
        const std::string key = lowercase(name);
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            entries.push_back({pid, name});
        }
    });
    std::sort(entries.begin(), entries.end(), [&](const AudioProcessEntry& a, const AudioProcessEntry& b) {
        return lowercase(a.name) < lowercase(b.name);
    });
    return entries;
}

uint32_t find_process_pid_by_name(const std::string& exe_name) {
    if (exe_name.empty()) return 0;
    auto lowercase = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    const std::string target = lowercase(exe_name);
    for (const auto& entry : enumerate_audio_processes()) {
        if (lowercase(entry.name) == target) {
            return entry.pid;
        }
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    uint32_t found = 0;
    const DWORD self_pid = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == self_pid) continue;
            if (lowercase(utf8(pe.szExeFile)) == target) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}
}
