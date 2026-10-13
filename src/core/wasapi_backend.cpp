#include "metvoice/core/wasapi_backend.hpp"
#include "metvoice/core/mmcss.hpp"
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>

namespace metvoice::core {

namespace {
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
        std::cerr << "[Capture Error] CoCreateInstance failed: hr=0x" << std::hex << hr << std::dec << "\n";
        if (co_init) CoUninitialize();
        return;
    }

    if (info_.id.empty()) {
        EDataFlow flow = (info_.type == DeviceType::LoopbackRender) ? eRender : eCapture;
        hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device_);
        if (FAILED(hr)) {
            std::cerr << "[Capture Error] GetDefaultAudioEndpoint failed: hr=0x" << std::hex << hr << std::dec << "\n";
        }
    } else {
        hr = enumerator->GetDevice(info_.id.c_str(), &device_);
        if (FAILED(hr)) {
            std::cerr << "[Capture Error] GetDevice failed for ID: hr=0x" << std::hex << hr << std::dec << "\n";
        }
    }
    enumerator->Release();

    if (!device_) {
        std::cerr << "[Capture Error] device_ is null\n";
        if (co_init) CoUninitialize();
        return;
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr) || !audio_client_) {
        std::cerr << "[Capture Error] Activate IAudioClient failed: hr=0x" << std::hex << hr << std::dec << "\n";
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    hr = audio_client_->GetMixFormat(&wave_format_);
    if (FAILED(hr) || !wave_format_) {
        std::cerr << "[Capture Error] GetMixFormat failed: hr=0x" << std::hex << hr << std::dec << "\n";
        audio_client_->Release();
        audio_client_ = nullptr;
        device_->Release();
        device_ = nullptr;
        if (co_init) CoUninitialize();
        return;
    }

    REFERENCE_TIME req_duration = static_cast<REFERENCE_TIME>(config_.buffer_frames) * 10000000 / wave_format_->nSamplesPerSec;
    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (info_.type == DeviceType::LoopbackRender) {
        stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, req_duration,
                                   0, wave_format_, nullptr);
    if (FAILED(hr)) {
        // Fallback to 0 (default device buffer duration) for virtual/third-party drivers
        hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0,
                                       0, wave_format_, nullptr);
    }

    if (FAILED(hr)) {
        std::cerr << "[WasapiEndpoint Capture] Initialize failed: hr=0x" << std::hex << hr << std::dec << "\n";
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
    hr = audio_client_->SetEventHandle(audio_event_);
    if (FAILED(hr)) {
        std::cerr << "[Capture Error] SetEventHandle failed: hr=0x" << std::hex << hr << std::dec << "\n";
    }
    audio_client_->GetBufferSize(&buffer_frame_count_);
    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_client_));
    if (FAILED(hr)) {
        std::cerr << "[Capture Error] GetService IAudioCaptureClient failed: hr=0x" << std::hex << hr << std::dec << "\n";
    }

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

    const size_t in_channels = wave_format_->nChannels;
    const uint32_t in_rate = wave_format_->nSamplesPerSec;
    const bool is_float = (wave_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (wave_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wave_format_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));

    std::vector<float> stereo_conv_buf;
    stereo_conv_buf.reserve(buffer_frame_count_ * 4);

    audio_client_->Start();
    running_.store(true);

    double resample_phase = 0.0;
    const double resample_ratio = static_cast<double>(in_rate) / 48000.0;

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
            if (SUCCEEDED(hr) && data && frames_available > 0) {
                stereo_conv_buf.clear();

                auto extract_sample = [&](size_t offset) -> float {
                    if (is_float) {
                        return reinterpret_cast<const float*>(data)[offset];
                    } else if (wave_format_->wBitsPerSample == 16) {
                        return static_cast<float>(reinterpret_cast<const int16_t*>(data)[offset]) / 32768.0f;
                    } else if (wave_format_->wBitsPerSample == 24) {
                        if (wave_format_->nBlockAlign == wave_format_->nChannels * 3) {
                            const uint8_t* p = data + (offset * 3);
                            int32_t s24 = static_cast<int32_t>(p[0]) |
                                          (static_cast<int32_t>(p[1]) << 8) |
                                          (static_cast<int32_t>(static_cast<int8_t>(p[2])) << 16);
                            return static_cast<float>(s24) / 8388608.0f;
                        } else {
                            return static_cast<float>(reinterpret_cast<const int32_t*>(data)[offset] >> 8) / 8388608.0f;
                        }
                    } else if (wave_format_->wBitsPerSample == 32) {
                        return static_cast<float>(reinterpret_cast<const int32_t*>(data)[offset]) / 2147483648.0f;
                    }
                    return 0.0f;
                };

                // Convert whatever input format to clean stereo
                std::vector<float> input_stereo(frames_available * 2);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(input_stereo.begin(), input_stereo.end(), 0.0f);
                } else {
                    for (size_t f = 0; f < frames_available; ++f) {
                        float left = 0.0f;
                        float right = 0.0f;

                        if (in_channels == 1) {
                            float s = extract_sample(f);
                            left = s;
                            right = s; // Duplicate mono microphone to both Left and Right!
                        } else {
                            left = extract_sample(f * in_channels + 0);
                            right = extract_sample(f * in_channels + 1);
                        }

                        input_stereo[f * 2 + 0] = left;
                        input_stereo[f * 2 + 1] = right;
                    }
                }

                // If sample rate is already 48000 Hz:
                if (in_rate == 48000) {
                    if (ring_buffer_) {
                        ring_buffer_->write(input_stereo.data(), input_stereo.size());
                    }
                } else {
                    // Resample to 48000 Hz using linear interpolation
                    size_t in_f = 0;
                    while (in_f < frames_available) {
                        size_t idx0 = in_f;
                        size_t idx1 = std::min(in_f + 1, static_cast<size_t>(frames_available - 1));
                        float frac = static_cast<float>(resample_phase);

                        float l = input_stereo[idx0 * 2 + 0] * (1.0f - frac) + input_stereo[idx1 * 2 + 0] * frac;
                        float r = input_stereo[idx0 * 2 + 1] * (1.0f - frac) + input_stereo[idx1 * 2 + 1] * frac;

                        stereo_conv_buf.push_back(l);
                        stereo_conv_buf.push_back(r);

                        resample_phase += resample_ratio;
                        while (resample_phase >= 1.0) {
                            resample_phase -= 1.0;
                            in_f++;
                        }
                    }

                    if (ring_buffer_ && !stereo_conv_buf.empty()) {
                        ring_buffer_->write(stereo_conv_buf.data(), stereo_conv_buf.size());
                    }
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
        // Fallback to 0 (default device buffer duration) for virtual/third-party drivers
        hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                       0, 0, wave_format_, nullptr);
    }

    if (FAILED(hr)) {
        std::cerr << "[WasapiEndpoint Render] Initialize failed: hr=0x" << std::hex << hr << std::dec << "\n";
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

    const size_t out_channels = wave_format_->nChannels;
    const bool is_float = (wave_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (wave_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wave_format_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));

    std::vector<float> stereo_read_buf;
    stereo_read_buf.resize(buffer_frame_count_ * 2, 0.0f);

    // Pre-buffer / prime the ring buffer with 20ms of silence so render never starves on cold start
    if (ring_buffer_) {
        std::vector<float> prime_silence(buffer_frame_count_ * 4, 0.0f);
        ring_buffer_->write(prime_silence.data(), prime_silence.size());
    }

    // Prime hardware render buffer
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
            const size_t stereo_samples_needed = frames_needed * 2;

            size_t samples_read = 0;
            if (ring_buffer_) {
                samples_read = ring_buffer_->read(stereo_read_buf.data(), stereo_samples_needed);
            }

            if (samples_read < stereo_samples_needed) {
                float last_l = (samples_read >= 2) ? stereo_read_buf[samples_read - 2] : 0.0f;
                float last_r = (samples_read >= 2) ? stereo_read_buf[samples_read - 1] : 0.0f;
                for (size_t s = samples_read; s < stereo_samples_needed; s += 2) {
                    last_l *= 0.95f;
                    last_r *= 0.95f;
                    stereo_read_buf[s] = last_l;
                    stereo_read_buf[s + 1] = last_r;
                }
            }

            auto write_sample = [&](size_t offset, float val) {
                val = std::clamp(val, -1.0f, 1.0f);
                if (is_float) {
                    reinterpret_cast<float*>(render_data)[offset] = val;
                } else if (wave_format_->wBitsPerSample == 16) {
                    reinterpret_cast<int16_t*>(render_data)[offset] = static_cast<int16_t>(val * 32767.0f);
                } else if (wave_format_->wBitsPerSample == 24) {
                    if (wave_format_->nBlockAlign == wave_format_->nChannels * 3) {
                        int32_t s24 = static_cast<int32_t>(val * 8388607.0f);
                        uint8_t* p = render_data + (offset * 3);
                        p[0] = static_cast<uint8_t>(s24 & 0xFF);
                        p[1] = static_cast<uint8_t>((s24 >> 8) & 0xFF);
                        p[2] = static_cast<uint8_t>((s24 >> 16) & 0xFF);
                    } else {
                        int32_t s24 = static_cast<int32_t>(val * 8388607.0f);
                        reinterpret_cast<int32_t*>(render_data)[offset] = (s24 << 8);
                    }
                } else if (wave_format_->wBitsPerSample == 32) {
                    reinterpret_cast<int32_t*>(render_data)[offset] = static_cast<int32_t>(val * 2147483647.0f);
                }
            };

            for (size_t f = 0; f < frames_needed; ++f) {
                float left = stereo_read_buf[f * 2 + 0];
                float right = stereo_read_buf[f * 2 + 1];

                if (out_channels == 1) {
                    write_sample(f, 0.5f * (left + right));
                } else if (out_channels == 2) {
                    write_sample(f * 2 + 0, left);
                    write_sample(f * 2 + 1, right);
                } else {
                    write_sample(f * out_channels + 0, left);
                    write_sample(f * out_channels + 1, right);
                    for (size_t c = 2; c < out_channels; ++c) {
                        write_sample(f * out_channels + c, 0.0f);
                    }
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
