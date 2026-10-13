#include "metvoice/core/wasapi_backend.hpp"
#include "metvoice/core/mmcss.hpp"
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <vector>

namespace metvoice::core {

namespace {
    // Helper to extract device name from property store
    std::wstring get_device_property(IPropertyStore* props, const PROPERTYKEY& key) {
        if (!props) return L"Unknown";
        PROPVARIANT var;
        PropVariantInit(&var);
        std::wstring result = L"Unknown";
        if (SUCCEEDED(props->GetValue(key, &var))) {
            if (var.vt == VT_LPWSTR && var.pwszVal) {
                result = var.pwszVal;
            }
        }
        PropVariantClear(&var);
        return result;
    }
}

std::vector<DeviceInfo> WasapiEndpoint::enumerate_devices(DeviceType type) {
    std::vector<DeviceInfo> results;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_uninit = SUCCEEDED(hr) || hr == S_FALSE;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        if (co_uninit) CoUninitialize();
        return results;
    }

    EDataFlow flow = (type == DeviceType::Capture) ? eCapture : eRender;
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);

    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);

        IMMDevice* default_dev = nullptr;
        std::wstring default_id;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &default_dev)) && default_dev) {
            LPWSTR def_str = nullptr;
            if (SUCCEEDED(default_dev->GetId(&def_str)) && def_str) {
                default_id = def_str;
                CoTaskMemFree(def_str);
            }
            default_dev->Release();
        }

        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(collection->Item(i, &device)) && device) {
                LPWSTR id_str = nullptr;
                device->GetId(&id_str);
                std::wstring id = id_str ? id_str : L"";
                if (id_str) CoTaskMemFree(id_str);

                IPropertyStore* props = nullptr;
                device->OpenPropertyStore(STGM_READ, &props);
                std::wstring name = get_device_property(props, PKEY_Device_FriendlyName);
                if (props) props->Release();

                DeviceInfo info;
                info.id = id;
                info.friendly_name = name;
                info.type = type;
                info.is_default = (id == default_id);

                // Check default mix format
                IAudioClient* client = nullptr;
                if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void**>(&client))) && client) {
                    WAVEFORMATEX* wfx = nullptr;
                    if (SUCCEEDED(client->GetMixFormat(&wfx)) && wfx) {
                        info.default_sample_rate = wfx->nSamplesPerSec;
                        info.channels = wfx->nChannels;
                        CoTaskMemFree(wfx);
                    }
                    client->Release();
                }

                results.push_back(info);
                device->Release();
            }
        }
        collection->Release();
    }

    enumerator->Release();
    if (co_uninit) CoUninitialize();
    return results;
}

WasapiEndpoint::WasapiEndpoint(const DeviceInfo& info, const AudioStreamConfig& config,
                               std::shared_ptr<SpscRingBuffer<float>> ring_buffer)
    : info_(info), config_(config), ring_buffer_(std::move(ring_buffer)) {
}

WasapiEndpoint::~WasapiEndpoint() {
    stop();
}

bool WasapiEndpoint::start() {
    if (running_.load()) return true;

    stop_signal_.store(false);
    if (info_.type == DeviceType::Capture || info_.type == DeviceType::LoopbackRender) {
        worker_thread_ = std::thread(&WasapiEndpoint::capture_thread_proc, this);
    } else {
        worker_thread_ = std::thread(&WasapiEndpoint::render_thread_proc, this);
    }

    // Wait until started or failed
    for (int i = 0; i < 50; ++i) {
        if (running_.load()) return true;
        Sleep(10);
    }
    return running_.load();
}

void WasapiEndpoint::stop() {
    stop_signal_.store(true);
    if (audio_event_) {
        SetEvent(audio_event_);
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    running_.store(false);
}

void WasapiEndpoint::capture_thread_proc() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_init = SUCCEEDED(hr) || hr == S_FALSE;

    ScopedMmcss mmcss(L"Pro Audio");

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        if (co_init) CoUninitialize();
        return;
    }

    if (info_.id.empty()) {
        EDataFlow flow = (info_.type == DeviceType::LoopbackRender) ? eRender : eCapture;
        enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device_);
    } else {
        enumerator->GetDevice(info_.id.c_str(), &device_);
    }
    enumerator->Release();

    if (!device_) {
        if (co_init) CoUninitialize();
        return;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr) || !audio_client_) {
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    hr = audio_client_->GetMixFormat(&wave_format_);
    if (FAILED(hr) || !wave_format_) {
        audio_client_->Release();
        audio_client_ = nullptr;
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    // 10ms default requested duration (100,000 in 100ns units)
    REFERENCE_TIME req_duration = static_cast<REFERENCE_TIME>(config_.buffer_frames) * 10000000 / wave_format_->nSamplesPerSec;
    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (info_.type == DeviceType::LoopbackRender) {
        stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, req_duration,
                                   0, wave_format_, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(wave_format_);
        wave_format_ = nullptr;
        audio_client_->Release();
        audio_client_ = nullptr;
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    audio_client_->SetEventHandle(audio_event_);
    audio_client_->GetBufferSize(&buffer_frame_count_);
    audio_client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_client_));

    if (!capture_client_) {
        CloseHandle(audio_event_);
        audio_event_ = nullptr;
        CoTaskMemFree(wave_format_);
        wave_format_ = nullptr;
        audio_client_->Release();
        audio_client_ = nullptr;
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    std::vector<float> temp_conv_buf;
    temp_conv_buf.resize(buffer_frame_count_ * wave_format_->nChannels);

    audio_client_->Start();
    running_.store(true);

    while (!stop_signal_.load(std::memory_order_relaxed)) {
        DWORD wait_res = WaitForSingleObject(audio_event_, 100);
        if (stop_signal_.load(std::memory_order_relaxed)) break;
        if (wait_res != WAIT_OBJECT_0) continue;

        UINT32 packet_length = 0;
        hr = capture_client_->GetNextPacketSize(&packet_length);
        while (SUCCEEDED(hr) && packet_length > 0) {
            BYTE* data = nullptr;
            UINT32 frames_available = 0;
            DWORD flags = 0;

            hr = capture_client_->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
            if (SUCCEEDED(hr) && data) {
                const size_t total_samples = frames_available * wave_format_->nChannels;

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill_n(temp_conv_buf.data(), total_samples, 0.0f);
                } else {
                    // Check if standard 32-bit float or 16-bit integer PCM
                    if (wave_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                        (wave_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wave_format_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                        const float* float_src = reinterpret_cast<const float*>(data);
                        std::memcpy(temp_conv_buf.data(), float_src, total_samples * sizeof(float));
                    } else if (wave_format_->wBitsPerSample == 16) {
                        const int16_t* s16 = reinterpret_cast<const int16_t*>(data);
                        constexpr float SCALE = 1.0f / 32768.0f;
                        for (size_t s = 0; s < total_samples; ++s) {
                            temp_conv_buf[s] = static_cast<float>(s16[s]) * SCALE;
                        }
                    } else if (wave_format_->wBitsPerSample == 24 || wave_format_->wBitsPerSample == 32) {
                        const int32_t* s32 = reinterpret_cast<const int32_t*>(data);
                        constexpr float SCALE = 1.0f / 2147483648.0f;
                        for (size_t s = 0; s < total_samples; ++s) {
                            temp_conv_buf[s] = static_cast<float>(s32[s]) * SCALE;
                        }
                    }
                }

                // Write to lock-free ring buffer
                if (ring_buffer_) {
                    ring_buffer_->write(temp_conv_buf.data(), total_samples);
                }

                capture_client_->ReleaseBuffer(frames_available);
            }
            hr = capture_client_->GetNextPacketSize(&packet_length);
        }
    }

    audio_client_->Stop();
    running_.store(false);

    if (capture_client_) { capture_client_->Release(); capture_client_ = nullptr; }
    if (audio_event_) { CloseHandle(audio_event_); audio_event_ = nullptr; }
    if (wave_format_) { CoTaskMemFree(wave_format_); wave_format_ = nullptr; }
    if (audio_client_) { audio_client_->Release(); audio_client_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }

    if (co_init) CoUninitialize();
}

void WasapiEndpoint::render_thread_proc() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_init = SUCCEEDED(hr) || hr == S_FALSE;

    ScopedMmcss mmcss(L"Pro Audio");

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        if (co_init) CoUninitialize();
        return;
    }

    if (info_.id.empty()) {
        enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    } else {
        enumerator->GetDevice(info_.id.c_str(), &device_);
    }
    enumerator->Release();

    if (!device_) {
        if (co_init) CoUninitialize();
        return;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr) || !audio_client_) {
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    hr = audio_client_->GetMixFormat(&wave_format_);
    if (FAILED(hr) || !wave_format_) {
        audio_client_->Release();
        audio_client_ = nullptr;
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    REFERENCE_TIME req_duration = static_cast<REFERENCE_TIME>(config_.buffer_frames) * 10000000 / wave_format_->nSamplesPerSec;
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                   req_duration, 0, wave_format_, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(wave_format_);
        wave_format_ = nullptr;
        audio_client_->Release();
        audio_client_ = nullptr;
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    audio_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    audio_client_->SetEventHandle(audio_event_);
    audio_client_->GetBufferSize(&buffer_frame_count_);
    audio_client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client_));

    if (!render_client_) {
        CloseHandle(audio_event_);
        audio_event_ = nullptr;
        CoTaskMemFree(wave_format_);
        wave_format_ = nullptr;
        audio_client_->Release();
        audio_client_ = nullptr;
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    std::vector<float> temp_conv_buf;
    temp_conv_buf.resize(buffer_frame_count_ * wave_format_->nChannels, 0.0f);

    // Prime the render buffer with silence before starting
    BYTE* initial_buffer = nullptr;
    if (SUCCEEDED(render_client_->GetBuffer(buffer_frame_count_, &initial_buffer)) && initial_buffer) {
        render_client_->ReleaseBuffer(buffer_frame_count_, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    audio_client_->Start();
    running_.store(true);

    while (!stop_signal_.load(std::memory_order_relaxed)) {
        DWORD wait_res = WaitForSingleObject(audio_event_, 100);
        if (stop_signal_.load(std::memory_order_relaxed)) break;
        if (wait_res != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        hr = audio_client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) continue;

        UINT32 frames_needed = buffer_frame_count_ - padding;
        if (frames_needed == 0) continue;

        BYTE* render_data = nullptr;
        hr = render_client_->GetBuffer(frames_needed, &render_data);
        if (SUCCEEDED(hr) && render_data) {
            const size_t samples_needed = frames_needed * wave_format_->nChannels;

            size_t samples_read = 0;
            if (ring_buffer_) {
                samples_read = ring_buffer_->read(temp_conv_buf.data(), samples_needed);
            }

            // Zero out any remaining samples in case of underrun
            if (samples_read < samples_needed) {
                std::fill(temp_conv_buf.data() + samples_read, temp_conv_buf.data() + samples_needed, 0.0f);
            }

            // Output format conversion
            if (wave_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (wave_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wave_format_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                std::memcpy(render_data, temp_conv_buf.data(), samples_needed * sizeof(float));
            } else if (wave_format_->wBitsPerSample == 16) {
                int16_t* dst16 = reinterpret_cast<int16_t*>(render_data);
                for (size_t s = 0; s < samples_needed; ++s) {
                    float val = std::clamp(temp_conv_buf[s], -1.0f, 1.0f);
                    dst16[s] = static_cast<int16_t>(val * 32767.0f);
                }
            } else if (wave_format_->wBitsPerSample == 24 || wave_format_->wBitsPerSample == 32) {
                int32_t* dst32 = reinterpret_cast<int32_t*>(render_data);
                for (size_t s = 0; s < samples_needed; ++s) {
                    float val = std::clamp(temp_conv_buf[s], -1.0f, 1.0f);
                    dst32[s] = static_cast<int32_t>(val * 2147483647.0f);
                }
            }

            render_client_->ReleaseBuffer(frames_needed, 0);
        }
    }

    audio_client_->Stop();
    running_.store(false);

    if (render_client_) { render_client_->Release(); render_client_ = nullptr; }
    if (audio_event_) { CloseHandle(audio_event_); audio_event_ = nullptr; }
    if (wave_format_) { CoTaskMemFree(wave_format_); wave_format_ = nullptr; }
    if (audio_client_) { audio_client_->Release(); audio_client_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }

    if (co_init) CoUninitialize();
}

} // namespace metvoice::core
