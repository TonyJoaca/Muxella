#include "muxella/core/wasapi_backend.hpp"
#include "muxella/core/mmcss.hpp"
#include <functiondiscoverykeys_devpkey.h>
#include <initguid.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace muxella::core {

namespace {
#ifndef AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
    constexpr wchar_t kVirtualProcessLoopbackDevice[] = L"VAD\\Process_Loopback";
    enum LocalAudioClientActivationType {
        kActivationTypeDefault = 0,
        kActivationTypeProcessLoopback = 1
    };
    enum LocalProcessLoopbackMode {
        kLoopbackIncludeTree = 0,
        kLoopbackExcludeTree = 1
    };
    struct LocalProcessLoopbackParams {
        DWORD TargetProcessId;
        LocalProcessLoopbackMode ProcessLoopbackMode;
    };
    struct LocalAudioClientActivationParams {
        LocalAudioClientActivationType ActivationType;
        union {
            LocalProcessLoopbackParams ProcessLoopbackParams;
        };
    };
#else
    constexpr wchar_t kVirtualProcessLoopbackDevice[] = VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK;
    using LocalProcessLoopbackParams = AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
    using LocalAudioClientActivationParams = AUDIOCLIENT_ACTIVATION_PARAMS;
    constexpr auto kActivationTypeProcessLoopback = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    constexpr auto kLoopbackIncludeTree = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    constexpr auto kLoopbackExcludeTree = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
#endif

    // {94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90}
    constexpr GUID kIidIAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

    class ProcessLoopbackCompletionHandler final : public IActivateAudioInterfaceCompletionHandler {
    public:
        ProcessLoopbackCompletionHandler() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
        ~ProcessLoopbackCompletionHandler() {
            if (op_) op_->Release();
            if (event_) CloseHandle(event_);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
            if (!ppv) return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
                riid == kIidIAgileObject) {
                *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1; }
        ULONG STDMETHODCALLTYPE Release() override {
            const ULONG next = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (next == 0) delete this;
            return next;
        }
        HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
            if (operation) {
                op_ = operation;
                op_->AddRef();
            }
            if (event_) SetEvent(event_);
            return S_OK;
        }

        bool wait_for_client(DWORD timeout_ms, IAudioClient*& out_client) {
            out_client = nullptr;
            if (!event_ || WaitForSingleObject(event_, timeout_ms) != WAIT_OBJECT_0 || !op_) return false;
            HRESULT activate_hr = E_FAIL;
            IUnknown* unk = nullptr;
            if (FAILED(op_->GetActivateResult(&activate_hr, &unk)) || FAILED(activate_hr) || !unk) {
                if (unk) unk->Release();
                return false;
            }
            const HRESULT qi = unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&out_client));
            unk->Release();
            return SUCCEEDED(qi) && out_client != nullptr;
        }

    private:
        std::atomic<ULONG> ref_count_{1};
        HANDLE event_{nullptr};
        IActivateAudioInterfaceAsyncOperation* op_{nullptr};
    };

    WAVEFORMATEX* allocate_stereo_float_format(uint32_t sample_rate) {
        auto* wfex = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
        if (!wfex) return nullptr;
        std::memset(wfex, 0, sizeof(WAVEFORMATEXTENSIBLE));
        wfex->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfex->Format.nChannels = 2;
        wfex->Format.nSamplesPerSec = sample_rate;
        wfex->Format.wBitsPerSample = 32;
        wfex->Format.nBlockAlign = 8;
        wfex->Format.nAvgBytesPerSec = sample_rate * 8;
        wfex->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfex->Samples.wValidBitsPerSample = 32;
        wfex->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wfex->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        return reinterpret_cast<WAVEFORMATEX*>(wfex);
    }

    WAVEFORMATEX* allocate_pcm_format(uint32_t sample_rate, uint16_t bits_per_sample, uint16_t container_bits) {
        auto* wfex = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
        if (!wfex) return nullptr;
        std::memset(wfex, 0, sizeof(WAVEFORMATEXTENSIBLE));
        wfex->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfex->Format.nChannels = 2;
        wfex->Format.nSamplesPerSec = sample_rate;
        wfex->Format.wBitsPerSample = container_bits;
        wfex->Format.nBlockAlign = static_cast<WORD>(2 * (container_bits / 8));
        wfex->Format.nAvgBytesPerSec = sample_rate * wfex->Format.nBlockAlign;
        wfex->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfex->Samples.wValidBitsPerSample = bits_per_sample;
        wfex->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wfex->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        return reinterpret_cast<WAVEFORMATEX*>(wfex);
    }

    bool activate_process_loopback_client(uint32_t pid, bool include_tree, uint32_t sample_rate,
                                          IAudioClient*& out_client, WAVEFORMATEX*& out_wfx) {
        out_client = nullptr;
        out_wfx = nullptr;
        if (pid == 0) return false;

        LocalAudioClientActivationParams params{};
        params.ActivationType = kActivationTypeProcessLoopback;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode =
            include_tree ? kLoopbackIncludeTree : kLoopbackExcludeTree;

        PROPVARIANT activate_params{};
        activate_params.vt = VT_BLOB;
        activate_params.blob.cbSize = sizeof(params);
        activate_params.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        auto* handler = new ProcessLoopbackCompletionHandler();
        IActivateAudioInterfaceAsyncOperation* async_op = nullptr;
        HRESULT hr = ActivateAudioInterfaceAsync(
            kVirtualProcessLoopbackDevice,
            __uuidof(IAudioClient),
            &activate_params,
            handler,
            &async_op);
        bool ok = false;
        if (SUCCEEDED(hr)) {
            ok = handler->wait_for_client(3000, out_client);
        }
        if (async_op) async_op->Release();
        handler->Release();

        if (!ok || !out_client) {
            std::cerr << "[ProcessLoopback] ActivateAudioInterfaceAsync failed for PID " << pid
                      << " (hr=0x" << std::hex << hr << std::dec << ")\n";
            return false;
        }
        out_wfx = allocate_stereo_float_format(sample_rate);
        if (!out_wfx) {
            out_client->Release();
            out_client = nullptr;
            return false;
        }
        return true;
    }

    bool initialize_exclusive_render(IMMDevice* device, uint32_t req_rate, uint32_t req_frames,
                                     IAudioClient*& client, WAVEFORMATEX*& out_wfx) {
        if (!device || !client) return false;
        std::vector<WAVEFORMATEX*> candidates;
        candidates.push_back(allocate_stereo_float_format(req_rate));
        candidates.push_back(allocate_pcm_format(req_rate, 24, 32));
        candidates.push_back(allocate_pcm_format(req_rate, 24, 24));
        candidates.push_back(allocate_pcm_format(req_rate, 16, 16));
        if (req_rate != 48000) {
            candidates.push_back(allocate_pcm_format(48000, 24, 32));
            candidates.push_back(allocate_pcm_format(48000, 16, 16));
        }
        auto free_candidates = [&](WAVEFORMATEX* keep) {
            for (WAVEFORMATEX* c : candidates) {
                if (c && c != keep) CoTaskMemFree(c);
            }
        };

        WAVEFORMATEX* chosen = nullptr;
        for (WAVEFORMATEX* cand : candidates) {
            if (!cand) continue;
            if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, cand, nullptr) == S_OK) {
                chosen = cand;
                break;
            }
        }
        if (!chosen) {
            free_candidates(nullptr);
            return false;
        }

        REFERENCE_TIME hns_period = static_cast<REFERENCE_TIME>(req_frames) * 10000000LL / chosen->nSamplesPerSec;
        if (hns_period < 10000) hns_period = 10000;
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        hns_period, hns_period, chosen, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 aligned_frames = 0;
            if (SUCCEEDED(client->GetBufferSize(&aligned_frames)) && aligned_frames > 0) {
                hns_period = static_cast<REFERENCE_TIME>(10000000.0 * aligned_frames / chosen->nSamplesPerSec + 0.5);
                client->Release();
                client = nullptr;
                if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void**>(&client))) && client) {
                    hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                            hns_period, hns_period, chosen, nullptr);
                } else {
                    hr = E_FAIL;
                }
            }
        }
        if (FAILED(hr)) {
            free_candidates(nullptr);
            return false;
        }
        free_candidates(chosen);
        out_wfx = chosen;
        return true;
    }

    bool is_virtual_cable(const DeviceInfo& info) {
        return info.friendly_name.find(L"Muxella") != std::wstring::npos;
    }

    int configure_cable_raw_stream(IAudioClient* client, const DeviceInfo& info) {
        if (!client || !is_virtual_cable(info)) return 0;
        IAudioClient2* client2 = nullptr;
        if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(&client2)))) {
            AudioClientProperties properties{};
            properties.cbSize = sizeof(properties);
            properties.eCategory = AudioCategory_Other;
            properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
            const HRESULT hr = client2->SetClientProperties(&properties);
            if (FAILED(hr)) std::cerr << "[Cable] RAW stream unavailable: 0x" << std::hex << hr << std::dec << "\n";
            client2->Release();
            return SUCCEEDED(hr) ? 1 : -1;
        }
        return -1;
    }
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
                               std::shared_ptr<SpscRingBuffer<float>> ring_buffer, std::atomic<bool>* connection_state,
                               uint8_t stream_index)
    : connection_state_(connection_state), stream_index_(stream_index), info_(info), config_(config), ring_buffer_(std::move(ring_buffer)) {
}

WasapiEndpoint::~WasapiEndpoint() {
    stop();
}

bool WasapiEndpoint::start() {
    if (running_.load()) return true;
    // An explicitly selected endpoint must never become the system default.
    if (info_.type == DeviceType::ProcessLoopback) {
        if (info_.target_pid == 0) return false;
    } else if (info_.id.empty()) {
        return false;
    }

    stop_signal_.store(false);
    if (info_.type == DeviceType::Capture || info_.type == DeviceType::LoopbackRender ||
        info_.type == DeviceType::ProcessLoopback) {
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
    set_running(false);
}

void WasapiEndpoint::capture_thread_proc() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_init = SUCCEEDED(hr) || hr == S_FALSE;

    ScopedMmcss mmcss(L"Pro Audio");

    if (info_.type == DeviceType::ProcessLoopback) {
        if (!activate_process_loopback_client(info_.target_pid, info_.include_process_tree,
                                              config_.sample_rate, audio_client_, wave_format_)) {
            if (co_init) CoUninitialize();
            return;
        }
        raw_status_.store(0);
    } else {
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

        raw_status_.store(configure_cable_raw_stream(audio_client_, info_));
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
    }

    REFERENCE_TIME req_duration = static_cast<REFERENCE_TIME>(config_.buffer_frames) * 10000000 / wave_format_->nSamplesPerSec;
    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (info_.type == DeviceType::LoopbackRender || info_.type == DeviceType::ProcessLoopback) {
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
    device_sample_rate_.store(in_rate, std::memory_order_relaxed);
    device_channels_.store(wave_format_->nChannels, std::memory_order_relaxed);
    device_bits_.store(wave_format_->wBitsPerSample, std::memory_order_relaxed);
    wasapi_buffer_frames_.store(buffer_frame_count_, std::memory_order_relaxed);
    const bool is_float = (wave_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (wave_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wave_format_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));

    std::vector<float> stereo_conv_buf;
    stereo_conv_buf.reserve(static_cast<size_t>(std::ceil(buffer_frame_count_ *
        static_cast<double>(config_.sample_rate) / in_rate) + 4) * 2);
    // WASAPI packets cannot exceed this client's negotiated buffer size.
    std::vector<float> input_stereo(buffer_frame_count_ * 2);

    audio_client_->Start();
    set_running(true);

    double resample_phase = 0.0;
    const double resample_ratio = static_cast<double>(in_rate) / config_.sample_rate;
    SignalGlitchDetector glitch_detector;

    while (!stop_signal_.load(std::memory_order_relaxed)) {
        DWORD wait_res = WaitForSingleObject(audio_event_, 100);
        if (stop_signal_.load(std::memory_order_relaxed)) break;
        if (wait_res != WAIT_OBJECT_0) continue;

        UINT32 packet_length = 0;
        hr = capture_client_->GetNextPacketSize(&packet_length);
        if (FAILED(hr)) break;
        while (SUCCEEDED(hr) && packet_length > 0) {
            BYTE* data = nullptr;
            UINT32 frames_available = 0;
            DWORD flags = 0;

            hr = capture_client_->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
            if (SUCCEEDED(hr) && frames_available > 0) {
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                    discontinuity_events_.fetch_add(1, std::memory_order_relaxed);
                    push_event(GlitchStage::Capture, GlitchKind::WasapiDiscontinuity, static_cast<float>(frames_available));
                    glitch_detector.reset();
                }
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
                input_stereo.resize(frames_available * 2);
                if (!data || (flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    std::fill(input_stereo.begin(), input_stereo.end(), 0.0f);
                    glitch_detector.reset();
                } else {
                    bool packet_glitch_logged = false;
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

                        GlitchKind gk{};
                        float gv = 0.0f;
                        if (glitch_detector.check_frame(left, right, gk, gv)) {
                            waveform_glitches_.fetch_add(1, std::memory_order_relaxed);
                            if (!packet_glitch_logged) {
                                push_event(GlitchStage::Capture, gk, gv);
                                packet_glitch_logged = true;
                            }
                        }
                    }
                }

                // If sample rate is already 48000 Hz:
                if (in_rate == config_.sample_rate) {
                    if (ring_buffer_) {
                        ring_buffer_->write(input_stereo.data(), input_stereo.size());
                    }
                } else {
                    // Resample to target sample rate using linear interpolation
                    stereo_conv_buf.clear();
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
    set_running(false);

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

    bool exclusive_mode = false;
    if (config_.share_mode == ShareMode::Exclusive) {
        exclusive_mode = initialize_exclusive_render(device_, config_.sample_rate, config_.buffer_frames,
                                                     audio_client_, wave_format_);
        if (!exclusive_mode && !audio_client_) {
            device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&audio_client_));
        }
    }
    exclusive_active_.store(exclusive_mode, std::memory_order_relaxed);

    if (!exclusive_mode) {
        if (!audio_client_) {
            device_->Release();
            device_ = nullptr;
            if (co_init) CoUninitialize();
            return;
        }
        raw_status_.store(configure_cable_raw_stream(audio_client_, info_));
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
    } else {
        raw_status_.store(0);
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
    device_sample_rate_.store(wave_format_->nSamplesPerSec, std::memory_order_relaxed);
    device_channels_.store(wave_format_->nChannels, std::memory_order_relaxed);
    device_bits_.store(wave_format_->wBitsPerSample, std::memory_order_relaxed);
    wasapi_buffer_frames_.store(buffer_frame_count_, std::memory_order_relaxed);
    const bool is_float = (wave_format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (wave_format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wave_format_)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));

    std::vector<float> stereo_read_buf;
    stereo_read_buf.resize(buffer_frame_count_ * 2, 0.0f);
    const double render_ratio = static_cast<double>(config_.sample_rate) / wave_format_->nSamplesPerSec;
    double render_phase = 0.0;
    float previous_frame[2] = {}, next_frame[2] = {};
    bool resampler_ready = false;
    auto read_frame = [&](float* frame) -> bool {
        if (ring_buffer_ && ring_buffer_->read(frame, 2) == 2) {
            return true;
        }
        frame[0] *= 0.95f;
        frame[1] *= 0.95f;
        return false;
    };

    // Keep a healthy 2-block cushion on attach so the device doesn't start starved.
    // Safe before set_running(true) because the mixer engine is not yet producing to this ring.
    if (ring_buffer_) {
        const size_t available = ring_buffer_->available_read();
        const size_t target_cushion = std::max<size_t>(buffer_frame_count_ * 4, config_.buffer_frames * 4);
        if (available > target_cushion) {
            ring_buffer_->discard((available - target_cushion) & ~size_t(1));
        } else if (available < target_cushion) {
            const size_t fill_count = (target_cushion - available) & ~size_t(1);
            std::vector<float> silence(fill_count, 0.0f);
            ring_buffer_->write(silence.data(), fill_count);
        }
    }

    // Prime hardware render buffer
    BYTE* initial_buffer = nullptr;
    if (SUCCEEDED(render_client_->GetBuffer(buffer_frame_count_, &initial_buffer)) && initial_buffer) {
        render_client_->ReleaseBuffer(buffer_frame_count_, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    audio_client_->Start();
    set_running(true);

    bool rebuffering = false;
    bool pending_crossfade = false;
    float hold_l = 0.0f;
    float hold_r = 0.0f;
    float xfade_from_l = 0.0f;
    float xfade_from_r = 0.0f;
    SignalGlitchDetector glitch_detector;

    while (!stop_signal_.load(std::memory_order_relaxed)) {
        DWORD wait_res = WaitForSingleObject(audio_event_, 100);
        if (stop_signal_.load(std::memory_order_relaxed)) break;
        if (wait_res != WAIT_OBJECT_0) continue;

        UINT32 frames_needed = buffer_frame_count_;
        if (!exclusive_mode) {
            UINT32 padding = 0;
            hr = audio_client_->GetCurrentPadding(&padding);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) break;
            if (FAILED(hr)) continue;

            frames_needed = buffer_frame_count_ - padding;
            if (frames_needed == 0) continue;
        }

        const size_t live_target = std::max<size_t>(frames_needed * 2, config_.buffer_frames * 2);
        const size_t healthy_cushion = live_target * 2;

        if (ring_buffer_) {
            const size_t available = ring_buffer_->available_read();
            const double max_backlog_sec = is_virtual_cable(info_) ? 0.320 : 0.150;
            const size_t backlog_limit = std::max<size_t>(healthy_cushion * 3, static_cast<size_t>(config_.sample_rate * 2 * max_backlog_sec));
            if (available > backlog_limit) {
                const size_t drop_samples = (available - healthy_cushion) & ~size_t(1);
                ring_buffer_->discard(drop_samples);
                render_discard_events_.fetch_add(1, std::memory_order_relaxed);
                render_discard_samples_.fetch_add(drop_samples, std::memory_order_relaxed);
                push_event(GlitchStage::Render, GlitchKind::RenderDiscard, static_cast<float>(drop_samples / 2));
                xfade_from_l = hold_l;
                xfade_from_r = hold_r;
                pending_crossfade = true;
                resampler_ready = false;
                render_phase = 0.0;
                glitch_detector.reset();
            }
        }

        BYTE* render_data = nullptr;
        hr = render_client_->GetBuffer(frames_needed, &render_data);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) break;
        if (SUCCEEDED(hr) && render_data) {
            const size_t stereo_samples_needed = frames_needed * 2;

            if (rebuffering && ring_buffer_) {
                const size_t resume_threshold = std::max<size_t>(healthy_cushion / 2, stereo_samples_needed);
                if (ring_buffer_->available_read() >= resume_threshold) {
                    rebuffering = false;
                    xfade_from_l = hold_l;
                    xfade_from_r = hold_r;
                    pending_crossfade = true;
                    resampler_ready = false;
                    render_phase = 0.0;
                    glitch_detector.reset();
                }
            }

            if (rebuffering) {
                for (size_t s = 0; s < stereo_samples_needed; s += 2) {
                    hold_l *= 0.95f;
                    hold_r *= 0.95f;
                    stereo_read_buf[s] = hold_l;
                    stereo_read_buf[s + 1] = hold_r;
                }
            } else if (render_ratio == 1.0) {
                size_t samples_read = 0;
                if (ring_buffer_) {
                    samples_read = ring_buffer_->read(stereo_read_buf.data(), stereo_samples_needed);
                }

                if (samples_read < stereo_samples_needed) {
                    if (samples_read & 1) {
                        samples_read &= ~size_t(1);
                    }
                    const size_t missing_frames = (stereo_samples_needed - samples_read) / 2;
                    render_underrun_events_.fetch_add(1, std::memory_order_relaxed);
                    push_event(GlitchStage::Render, GlitchKind::RenderUnderrun, static_cast<float>(missing_frames));

                    hold_l = (samples_read >= 2) ? stereo_read_buf[samples_read - 2] : hold_l;
                    hold_r = (samples_read >= 2) ? stereo_read_buf[samples_read - 1] : hold_r;
                    for (size_t s = samples_read; s < stereo_samples_needed; s += 2) {
                        hold_l *= 0.95f;
                        hold_r *= 0.95f;
                        stereo_read_buf[s] = hold_l;
                        stereo_read_buf[s + 1] = hold_r;
                    }
                    rebuffering = true;
                    push_event(GlitchStage::Render, GlitchKind::RenderRebuffer, static_cast<float>(healthy_cushion / 4));
                } else if (stereo_samples_needed >= 2) {
                    hold_l = stereo_read_buf[stereo_samples_needed - 2];
                    hold_r = stereo_read_buf[stereo_samples_needed - 1];
                }
            } else {
                bool underran = false;
                if (!resampler_ready) {
                    if (!read_frame(previous_frame) || !read_frame(next_frame)) {
                        underran = true;
                    }
                    resampler_ready = true;
                }
                for (size_t frame = 0; frame < frames_needed; ++frame) {
                    for (size_t ch = 0; ch < 2; ++ch)
                        stereo_read_buf[frame * 2 + ch] = previous_frame[ch] +
                            (next_frame[ch] - previous_frame[ch]) * static_cast<float>(render_phase);
                    render_phase += render_ratio;
                    while (render_phase >= 1.0) {
                        previous_frame[0] = next_frame[0]; previous_frame[1] = next_frame[1];
                        if (!read_frame(next_frame)) {
                            underran = true;
                        }
                        render_phase -= 1.0;
                    }
                }
                if (underran) {
                    render_underrun_events_.fetch_add(1, std::memory_order_relaxed);
                    push_event(GlitchStage::Render, GlitchKind::RenderUnderrun, static_cast<float>(frames_needed));
                    rebuffering = true;
                    resampler_ready = false;
                    render_phase = 0.0;
                    push_event(GlitchStage::Render, GlitchKind::RenderRebuffer, static_cast<float>(healthy_cushion / 4));
                }
                if (stereo_samples_needed >= 2) {
                    hold_l = stereo_read_buf[stereo_samples_needed - 2];
                    hold_r = stereo_read_buf[stereo_samples_needed - 1];
                }
            }

            if (!rebuffering) {
                if (pending_crossfade) {
                    const size_t fade_frames = std::min<size_t>(frames_needed, 192);
                    for (size_t f = 0; f < fade_frames; ++f) {
                        const float t = static_cast<float>(f + 1) / static_cast<float>(fade_frames);
                        stereo_read_buf[f * 2 + 0] = xfade_from_l * (1.0f - t) + stereo_read_buf[f * 2 + 0] * t;
                        stereo_read_buf[f * 2 + 1] = xfade_from_r * (1.0f - t) + stereo_read_buf[f * 2 + 1] * t;
                    }
                    if (stereo_samples_needed >= 2) {
                        hold_l = stereo_read_buf[stereo_samples_needed - 2];
                        hold_r = stereo_read_buf[stereo_samples_needed - 1];
                    }
                    pending_crossfade = false;
                }
                bool packet_glitch_logged = false;
                for (size_t f = 0; f < frames_needed; ++f) {
                    GlitchKind gk{};
                    float gv = 0.0f;
                    if (glitch_detector.check_frame(stereo_read_buf[f * 2 + 0], stereo_read_buf[f * 2 + 1], gk, gv)) {
                        waveform_glitches_.fetch_add(1, std::memory_order_relaxed);
                        if (!packet_glitch_logged) {
                            push_event(GlitchStage::Render, gk, gv);
                            packet_glitch_logged = true;
                        }
                    }
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
                    const double scaled = static_cast<double>(val) * 2147483647.0;
                    reinterpret_cast<int32_t*>(render_data)[offset] = static_cast<int32_t>(scaled);
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
    set_running(false);

    if (render_client_) { render_client_->Release(); render_client_ = nullptr; }
    if (audio_event_) { CloseHandle(audio_event_); audio_event_ = nullptr; }
    if (wave_format_) { CoTaskMemFree(wave_format_); wave_format_ = nullptr; }
    if (audio_client_) { audio_client_->Release(); audio_client_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }

    if (co_init) CoUninitialize();
}

} // namespace muxella::core
