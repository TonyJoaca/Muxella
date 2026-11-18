#include "muxella/core/asio_backend.hpp"
#include "muxella/core/mmcss.hpp"
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace muxella::core {

std::vector<AsioDriverInfo> AsioHostInterface::enumerate_drivers() {
    std::vector<AsioDriverInfo> drivers;
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char subKeyName[256];
        DWORD index = 0;
        DWORD nameLen = sizeof(subKeyName);

        while (RegEnumKeyExA(hKey, index, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            HKEY hSubKey = nullptr;
            if (RegOpenKeyExA(hKey, subKeyName, 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
                char clsid[128] = {0};
                char desc[256] = {0};
                DWORD clsidLen = sizeof(clsid);
                DWORD descLen = sizeof(desc);

                RegQueryValueExA(hSubKey, "CLSID", nullptr, nullptr, reinterpret_cast<LPBYTE>(clsid), &clsidLen);
                RegQueryValueExA(hSubKey, "Description", nullptr, nullptr, reinterpret_cast<LPBYTE>(desc), &descLen);

                AsioDriverInfo info;
                info.name = subKeyName;
                info.clsid_str = clsid;
                info.description = (descLen > 0 && desc[0] != '\0') ? desc : subKeyName;
                drivers.push_back(info);

                RegCloseKey(hSubKey);
            }
            index++;
            nameLen = sizeof(subKeyName);
        }
        RegCloseKey(hKey);
    }
    return drivers;
}

namespace {
#pragma pack(push, 4)
    using AsioBool = long;
    using AsioError = long;
    using AsioSampleType = long;
    using AsioSampleRate = double;
    struct AsioSamples { unsigned long hi; unsigned long lo; };
    struct AsioTimeStamp { unsigned long hi; unsigned long lo; };

    struct AsioClockSource {
        long index;
        long associatedChannel;
        long associatedGroup;
        AsioBool isCurrentSource;
        char name[32];
    };

    struct AsioChannelInfo {
        long channel;
        AsioBool isInput;
        AsioBool isActive;
        long channelGroup;
        AsioSampleType type;
        char name[32];
    };

    struct AsioBufferInfo {
        AsioBool isInput;
        long channelNum;
        void* buffers[2];
    };

    struct AsioTimeInfo {
        double speed;
        AsioTimeStamp systemTime;
        AsioSamples samplePosition;
        AsioSampleRate sampleRate;
        unsigned long flags;
        char reserved[12];
    };

    struct AsioTime {
        long reserved[4];
        AsioTimeInfo timeInfo;
        char timeCode[44];
    };

    struct AsioCallbacks {
        void (*bufferSwitch)(long doubleBufferIndex, AsioBool directProcess);
        void (*sampleRateDidChange)(AsioSampleRate sRate);
        long (*asioMessage)(long selector, long value, void* message, double* opt);
        AsioTime* (*bufferSwitchTimeInfo)(AsioTime* params, long doubleBufferIndex, AsioBool directProcess);
    };
#pragma pack(pop)

    enum : long {
        kAsioOk = 0,
        kAsioSuccess = 0x3f4847a0,
        kAsioSelectorSupported = 1,
        kAsioEngineVersion = 2,
        kAsioSupportsTimeInfo = 7,
        kAsioSampleInt16LSB = 16,
        kAsioSampleInt24LSB = 17,
        kAsioSampleInt32LSB = 18,
        kAsioSampleFloat32LSB = 19,
        kAsioSampleFloat64LSB = 20,
    };

    class IAsioRaw : public IUnknown {
    public:
        virtual AsioBool init(void* sysHandle) = 0;
        virtual void getDriverName(char* name) = 0;
        virtual long getDriverVersion() = 0;
        virtual void getErrorMessage(char* string) = 0;
        virtual AsioError start() = 0;
        virtual AsioError stop() = 0;
        virtual AsioError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
        virtual AsioError getLatencies(long* inputLatency, long* outputLatency) = 0;
        virtual AsioError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
        virtual AsioError canSampleRate(AsioSampleRate sampleRate) = 0;
        virtual AsioError getSampleRate(AsioSampleRate* sampleRate) = 0;
        virtual AsioError setSampleRate(AsioSampleRate sampleRate) = 0;
        virtual AsioError getClockSources(AsioClockSource* clocks, long* numSources) = 0;
        virtual AsioError setClockSource(long reference) = 0;
        virtual AsioError getSamplePosition(AsioSamples* sPos, AsioTimeStamp* tStamp) = 0;
        virtual AsioError getChannelInfo(AsioChannelInfo* info) = 0;
        virtual AsioError createBuffers(AsioBufferInfo* bufferInfos, long numChannels, long bufferSize, AsioCallbacks* callbacks) = 0;
        virtual AsioError disposeBuffers() = 0;
        virtual AsioError controlPanel() = 0;
        virtual AsioError future(long selector, void* opt) = 0;
        virtual AsioError outputReady() = 0;
    };

    float read_asio_sample(const void* buf, size_t idx, AsioSampleType type) noexcept {
        switch (type) {
            case kAsioSampleFloat32LSB:
                return static_cast<const float*>(buf)[idx];
            case kAsioSampleFloat64LSB:
                return static_cast<float>(static_cast<const double*>(buf)[idx]);
            case kAsioSampleInt32LSB:
                return static_cast<float>(static_cast<const int32_t*>(buf)[idx] / 2147483648.0);
            case kAsioSampleInt24LSB: {
                const auto* p = static_cast<const uint8_t*>(buf) + idx * 3;
                int32_t v = (static_cast<int32_t>(p[0])) | (static_cast<int32_t>(p[1]) << 8) |
                            (static_cast<int32_t>(static_cast<int8_t>(p[2])) << 16);
                return static_cast<float>(v / 8388608.0);
            }
            case kAsioSampleInt16LSB:
                return static_cast<float>(static_cast<const int16_t*>(buf)[idx] / 32768.0);
            default:
                return 0.0f;
        }
    }

    void write_asio_sample(void* buf, size_t idx, AsioSampleType type, float val) noexcept {
        val = std::clamp(val, -1.0f, 1.0f);
        switch (type) {
            case kAsioSampleFloat32LSB:
                static_cast<float*>(buf)[idx] = val;
                break;
            case kAsioSampleFloat64LSB:
                static_cast<double*>(buf)[idx] = static_cast<double>(val);
                break;
            case kAsioSampleInt32LSB:
                static_cast<int32_t*>(buf)[idx] = static_cast<int32_t>(val * 2147483647.0);
                break;
            case kAsioSampleInt24LSB: {
                int32_t s24 = static_cast<int32_t>(val * 8388607.0f);
                auto* p = static_cast<uint8_t*>(buf) + idx * 3;
                p[0] = static_cast<uint8_t>(s24 & 0xFF);
                p[1] = static_cast<uint8_t>((s24 >> 8) & 0xFF);
                p[2] = static_cast<uint8_t>((s24 >> 16) & 0xFF);
                break;
            }
            case kAsioSampleInt16LSB:
                static_cast<int16_t*>(buf)[idx] = static_cast<int16_t>(val * 32767.0f);
                break;
            default:
                break;
        }
    }

    uint16_t sample_type_bits(AsioSampleType type) noexcept {
        switch (type) {
            case kAsioSampleInt16LSB: return 16;
            case kAsioSampleInt24LSB: return 24;
            case kAsioSampleInt32LSB:
            case kAsioSampleFloat32LSB: return 32;
            case kAsioSampleFloat64LSB: return 64;
            default: return 32;
        }
    }

    std::wstring utf8_to_wide(const std::string& str) {
        if (str.empty()) return {};
        const int sz = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), out.data(), sz);
        return out;
    }

    // ASIO 2.0 C callbacks carry no user-data pointer. Support one active instance cleanly per callback slot.
    std::atomic<AsioEndpoint::Impl*> g_active_impl{nullptr};
} // namespace

struct AsioEndpoint::Impl {
    AsioEndpoint* owner{nullptr};
    IAsioRaw* driver{nullptr};
    AsioBufferInfo buffers[2]{};
    AsioSampleType sample_type{kAsioSampleInt32LSB};
    long channel_count{0};
    long buf_size{0};
    double asio_rate{48000.0};
    std::vector<float> scratch;
    bool supports_output_ready{false};

    void handle_buffer_switch(long double_buf_idx) noexcept {
        if (!owner || !owner->ring_buffer_ || channel_count == 0 || buf_size <= 0) return;
        const size_t frames = static_cast<size_t>(buf_size);
        const size_t stereo = frames * 2;
        if (scratch.size() < stereo) return;

        void* ch0 = buffers[0].buffers[double_buf_idx];
        void* ch1 = (channel_count > 1) ? buffers[1].buffers[double_buf_idx] : ch0;
        if (!ch0 || !ch1) return;

        if (owner->direction_ == DeviceType::Capture) {
            for (size_t f = 0; f < frames; ++f) {
                scratch[f * 2 + 0] = read_asio_sample(ch0, f, sample_type);
                scratch[f * 2 + 1] = (channel_count > 1) ? read_asio_sample(ch1, f, sample_type) : scratch[f * 2 + 0];
            }
            owner->ring_buffer_->write(scratch.data(), stereo);
        } else {
            const size_t avail = owner->ring_buffer_->available_read();
            const size_t healthy = std::max<size_t>(stereo * 2, owner->config_.buffer_frames * 4);
            const size_t limit = std::max<size_t>(healthy * 3, static_cast<size_t>(owner->config_.sample_rate * 2 * 0.150));
            if (avail > limit) {
                const size_t drop = (avail - healthy) & ~size_t(1);
                owner->ring_buffer_->discard(drop);
                owner->discard_events_.fetch_add(1, std::memory_order_relaxed);
                owner->discard_samples_.fetch_add(drop, std::memory_order_relaxed);
            }
            const size_t got = owner->ring_buffer_->read(scratch.data(), stereo);
            if (got < stereo) {
                owner->underrun_events_.fetch_add(1, std::memory_order_relaxed);
                std::fill(scratch.begin() + got, scratch.begin() + stereo, 0.0f);
            }
            for (size_t f = 0; f < frames; ++f) {
                write_asio_sample(ch0, f, sample_type, scratch[f * 2 + 0]);
                if (channel_count > 1) {
                    write_asio_sample(ch1, f, sample_type, scratch[f * 2 + 1]);
                }
            }
            if (supports_output_ready && driver) driver->outputReady();
        }
    }

    static void cb_buffer_switch(long idx, AsioBool) {
        if (auto* self = g_active_impl.load(std::memory_order_acquire)) {
            self->handle_buffer_switch(idx);
        }
    }
    static void cb_sample_rate_changed(AsioSampleRate) {}
    static long cb_asio_message(long selector, long value, void*, double*) {
        if (selector == kAsioSelectorSupported) {
            return (value == kAsioEngineVersion || value == kAsioSupportsTimeInfo) ? 1 : 0;
        }
        if (selector == kAsioEngineVersion) return 2;
        if (selector == kAsioSupportsTimeInfo) return 0;
        return 0;
    }
    static AsioTime* cb_buffer_switch_time(AsioTime* params, long idx, AsioBool dp) {
        cb_buffer_switch(idx, dp);
        return params;
    }
};

AsioEndpoint::AsioEndpoint(const AsioDriverInfo& driver, DeviceType direction,
                           const AudioStreamConfig& config,
                           std::shared_ptr<SpscRingBuffer<float>> ring_buffer,
                           std::atomic<bool>* connection_state)
    : driver_(driver), direction_(direction), config_(config),
      ring_buffer_(std::move(ring_buffer)), connection_state_(connection_state) {}

AsioEndpoint::~AsioEndpoint() {
    stop();
}

bool AsioEndpoint::start() {
    if (running_.load()) return true;
    if (driver_.clsid_str.empty()) return false;
    stop_signal_.store(false);
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) return false;
    worker_thread_ = std::thread(&AsioEndpoint::worker_proc, this);
    for (int i = 0; i < 100; ++i) {
        if (running_.load() || stop_signal_.load()) break;
        Sleep(15);
    }
    return running_.load();
}

void AsioEndpoint::stop() {
    stop_signal_.store(true);
    if (stop_event_) SetEvent(stop_event_);
    if (worker_thread_.joinable()) worker_thread_.join();
    if (stop_event_) { CloseHandle(stop_event_); stop_event_ = nullptr; }
    set_running(false);
}

void AsioEndpoint::worker_proc() {
    // ASIO COM in-proc servers typically require an STA apartment.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool co_init = SUCCEEDED(hr) || hr == S_FALSE;
    ScopedMmcss mmcss(L"Pro Audio");

    CLSID clsid{};
    const std::wstring wclsid = utf8_to_wide(driver_.clsid_str);
    if (FAILED(CLSIDFromString(wclsid.c_str(), &clsid))) {
        std::cerr << "[ASIO] Invalid CLSID for driver " << driver_.name << "\n";
        stop_signal_.store(true);
        if (co_init) CoUninitialize();
        return;
    }

    IAsioRaw* asio = nullptr;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&asio));
    if (FAILED(hr) || !asio) {
        std::cerr << "[ASIO] CoCreateInstance failed for " << driver_.name
                  << " hr=0x" << std::hex << hr << std::dec << "\n";
        stop_signal_.store(true);
        if (co_init) CoUninitialize();
        return;
    }

    if (!asio->init(GetDesktopWindow())) {
        char err_buf[256] = {};
        asio->getErrorMessage(err_buf);
        std::cerr << "[ASIO] init() failed for " << driver_.name << ": " << err_buf << "\n";
        asio->Release();
        stop_signal_.store(true);
        if (co_init) CoUninitialize();
        return;
    }

    long in_ch = 0, out_ch = 0;
    asio->getChannels(&in_ch, &out_ch);
    const bool is_capture = (direction_ == DeviceType::Capture);
    const long available_ch = is_capture ? in_ch : out_ch;
    const long want_ch = std::min<long>(available_ch, 2);
    if (want_ch <= 0) {
        std::cerr << "[ASIO] Driver " << driver_.name << " has no channels in requested direction.\n";
        asio->Release();
        stop_signal_.store(true);
        if (co_init) CoUninitialize();
        return;
    }

    if (asio->canSampleRate(static_cast<double>(config_.sample_rate)) == kAsioOk) {
        asio->setSampleRate(static_cast<double>(config_.sample_rate));
    }
    double actual_rate = static_cast<double>(config_.sample_rate);
    asio->getSampleRate(&actual_rate);

    long min_sz = 64, max_sz = 2048, pref_sz = static_cast<long>(config_.buffer_frames), gran = 0;
    asio->getBufferSize(&min_sz, &max_sz, &pref_sz, &gran);
    long buf_sz = std::clamp(static_cast<long>(config_.buffer_frames), min_sz, max_sz);
    if (pref_sz > 0 && gran == 0) buf_sz = pref_sz;

    AsioChannelInfo ch_info{};
    ch_info.channel = 0;
    ch_info.isInput = is_capture ? 1 : 0;
    asio->getChannelInfo(&ch_info);

    Impl local{};
    local.owner = this;
    local.driver = asio;
    local.sample_type = ch_info.type;
    local.channel_count = want_ch;
    local.buf_size = buf_sz;
    local.asio_rate = actual_rate;
    local.scratch.assign(static_cast<size_t>(buf_sz) * 2, 0.0f);
    local.supports_output_ready = (asio->outputReady() == kAsioOk);
    for (long c = 0; c < want_ch; ++c) {
        local.buffers[c].isInput = is_capture ? 1 : 0;
        local.buffers[c].channelNum = c;
    }

    AsioCallbacks callbacks{
        &Impl::cb_buffer_switch,
        &Impl::cb_sample_rate_changed,
        &Impl::cb_asio_message,
        &Impl::cb_buffer_switch_time
    };

    AsioEndpoint::Impl* expected = nullptr;
    if (!g_active_impl.compare_exchange_strong(expected, &local, std::memory_order_acq_rel)) {
        std::cerr << "[ASIO] Another ASIO endpoint is already active; only one hardware ASIO stream supported at once.\n";
        asio->Release();
        stop_signal_.store(true);
        if (co_init) CoUninitialize();
        return;
    }

    if (asio->createBuffers(local.buffers, want_ch, buf_sz, &callbacks) != kAsioOk) {
        std::cerr << "[ASIO] createBuffers failed for " << driver_.name << "\n";
        g_active_impl.store(nullptr, std::memory_order_release);
        asio->Release();
        stop_signal_.store(true);
        if (co_init) CoUninitialize();
        return;
    }

    device_sample_rate_.store(static_cast<uint32_t>(actual_rate), std::memory_order_relaxed);
    device_channels_.store(static_cast<uint16_t>(want_ch), std::memory_order_relaxed);
    device_bits_.store(sample_type_bits(ch_info.type), std::memory_order_relaxed);
    buffer_frames_.store(static_cast<uint32_t>(buf_sz), std::memory_order_relaxed);

    if (asio->start() != kAsioOk) {
        std::cerr << "[ASIO] start() failed for " << driver_.name << "\n";
        asio->disposeBuffers();
        g_active_impl.store(nullptr, std::memory_order_release);
        asio->Release();
        stop_signal_.store(true);
        if (co_init) CoUninitialize();
        return;
    }

    set_running(true);
    while (!stop_signal_.load(std::memory_order_relaxed)) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(1, &stop_event_, FALSE, 50, QS_ALLINPUT);
    }

    asio->stop();
    set_running(false);
    asio->disposeBuffers();
    g_active_impl.store(nullptr, std::memory_order_release);
    asio->Release();
    if (co_init) CoUninitialize();
}

} // namespace muxella::core
