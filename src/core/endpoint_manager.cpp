#include "muxella/core/endpoint_manager.hpp"
#include "muxella/core/app_sessions.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>

namespace muxella::core {
namespace {

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide_from_utf8(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

struct ParsedQuery {
    std::string target;
    bool exclusive{false};
    bool asio{false};
    bool app_name{false};
    bool app_pid{false};
};

ParsedQuery parse_query(std::string raw) {
    ParsedQuery out;
    std::string s = trim(std::move(raw));
    for (const char* prefix : {"[WDM] ", "[VAIO] "}) {
        if (s.rfind(prefix, 0) == 0) s = trim(s.substr(std::char_traits<char>::length(prefix)));
    }
    if (s.rfind("[EX] ", 0) == 0) {
        out.exclusive = true;
        s = trim(s.substr(5));
    } else if (lower(s).rfind("exclusive:", 0) == 0) {
        out.exclusive = true;
        s = trim(s.substr(10));
    }
    if (s.rfind("[ASIO] ", 0) == 0) {
        out.asio = true;
        s = trim(s.substr(7));
    } else if (lower(s).rfind("asio:", 0) == 0) {
        out.asio = true;
        s = trim(s.substr(5));
    } else if (s.rfind("[APP] ", 0) == 0) {
        out.app_name = true;
        s = trim(s.substr(6));
    } else if (lower(s).rfind("app:", 0) == 0) {
        out.app_name = true;
        s = trim(s.substr(4));
    } else if (lower(s).rfind("pid:", 0) == 0) {
        out.app_pid = true;
        s = trim(s.substr(4));
    }
    out.target = std::move(s);
    return out;
}

bool named_match(const std::string& query, const std::string& name) {
    if (query == name) return true;
    for (const char* alias : {"muxella main", "muxella spotify", "muxella discord", "muxella out b1", "muxella out b2", "muxella out b3"}) {
        if (query == alias) return name.rfind(query + " (", 0) == 0 || name.find("(" + query + ")") != std::string::npos;
    }
    return false;
}

// -1 absent, -2 ambiguous. An ID request never falls back to a name/index.
int resolve(const std::vector<DeviceInfo>& devices, const std::string& query) {
    if (query.rfind("id:", 0) == 0) {
        for (size_t i = 0; i < devices.size(); ++i) if (utf8(devices[i].id) == query.substr(3)) return static_cast<int>(i);
        return -1;
    }
    if (!query.empty() && std::all_of(query.begin(), query.end(), [](unsigned char c) { return std::isdigit(c); })) {
        try { const auto i = std::stoul(query); return i < devices.size() ? static_cast<int>(i) : -1; } catch (...) { return -1; }
    }
    int result = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (named_match(lower(query), lower(utf8(devices[i].friendly_name)))) {
            if (result != -1) return -2;
            result = static_cast<int>(i);
        }
    }
    return result;
}

int resolve_asio(const std::vector<AsioDriverInfo>& drivers, const std::string& query) {
    const std::string q = lower(query);
    for (size_t i = 0; i < drivers.size(); ++i) {
        if (lower(drivers[i].name) == q || lower(drivers[i].clsid_str) == q) return static_cast<int>(i);
    }
    return -1;
}

bool disconnected(const std::string& query) {
    const auto value = lower(query);
    return value.empty() || value == "disconnect" || value == "select device" || value == "none";
}

} // namespace

class EndpointManager::NotificationClient final : public IMMNotificationClient {
public:
    explicit NotificationClient(EndpointManager* owner) : owner_(owner) {}

    void detach_owner() noexcept {
        std::lock_guard<std::mutex> lock(owner_mu_);
        owner_ = nullptr;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
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

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        notify();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        notify();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        notify();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override {
        notify();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    void notify() {
        std::lock_guard<std::mutex> lock(owner_mu_);
        if (owner_) owner_->notify_hotplug(true);
    }

    std::atomic<ULONG> ref_count_{1};
    std::mutex owner_mu_;
    EndpointManager* owner_{nullptr};
};

EndpointManager::EndpointManager(matrix::MixerEngine& engine) : engine_(engine) {
    capture_endpoints_.resize(engine.num_strips());
    render_endpoints_.resize(engine.num_buses());
    capture_asio_endpoints_.resize(engine.num_strips());
    render_asio_endpoints_.resize(engine.num_buses());
    capture_names_.resize(engine.num_strips(), "None");
    render_names_.resize(engine.num_buses(), "None");
    capture_ids_.resize(engine.num_strips());
    render_ids_.resize(engine.num_buses());
    desired_input_queries_.resize(engine.num_strips());
    desired_output_queries_.resize(engine.num_buses());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_init = SUCCEEDED(hr) || hr == S_FALSE;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&hotplug_enumerator_))) &&
        hotplug_enumerator_) {
        hotplug_client_ = new NotificationClient(this);
        if (FAILED(hotplug_enumerator_->RegisterEndpointNotificationCallback(hotplug_client_))) {
            hotplug_client_->detach_owner();
            hotplug_client_->Release();
            hotplug_client_ = nullptr;
            hotplug_enumerator_->Release();
            hotplug_enumerator_ = nullptr;
        }
    }
    if (com_init) CoUninitialize();
    hotplug_thread_ = std::thread(&EndpointManager::hotplug_worker_proc, this);
}

EndpointManager::~EndpointManager() {
    stop_hotplug_.store(true);
    hotplug_cv_.notify_all();
    if (hotplug_thread_.joinable()) {
        hotplug_thread_.join();
    }

    if (hotplug_client_) {
        hotplug_client_->detach_owner();
        if (hotplug_enumerator_) {
            hotplug_enumerator_->UnregisterEndpointNotificationCallback(hotplug_client_);
        }
        hotplug_client_->Release();
        hotplug_client_ = nullptr;
    }
    if (hotplug_enumerator_) {
        hotplug_enumerator_->Release();
        hotplug_enumerator_ = nullptr;
    }

    for (auto& endpoint : capture_endpoints_) if (endpoint) endpoint->stop();
    for (auto& endpoint : capture_asio_endpoints_) if (endpoint) endpoint->stop();
    for (auto& endpoint : render_endpoints_) if (endpoint) endpoint->stop();
    for (auto& endpoint : render_asio_endpoints_) if (endpoint) endpoint->stop();
}

void EndpointManager::notify_hotplug(bool device_topology_changed) {
    if (device_topology_changed) {
        hotplug_generation_.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(hotplug_mutex_);
        hotplug_dirty_ = true;
    }
    hotplug_cv_.notify_one();
}

void EndpointManager::hotplug_worker_proc() {
    while (!stop_hotplug_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lock(hotplug_mutex_);
            hotplug_cv_.wait_for(lock, std::chrono::milliseconds(1500), [&] {
                return stop_hotplug_.load(std::memory_order_relaxed) || hotplug_dirty_;
            });
            hotplug_dirty_ = false;
        }
        if (stop_hotplug_.load(std::memory_order_relaxed)) break;
        reconcile_endpoints();
    }
}

void EndpointManager::reconcile_endpoints() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < capture_endpoints_.size(); ++i) {
        if (!engine_.strip(i).is_enabled() || desired_input_queries_[i].empty()) continue;
        const bool wasapi_alive = capture_endpoints_[i] && capture_endpoints_[i]->is_running();
        const bool asio_alive = capture_asio_endpoints_[i] && capture_asio_endpoints_[i]->is_running();
        if (!wasapi_alive && !asio_alive) {
            if (capture_endpoints_[i] || capture_asio_endpoints_[i]) {
                detach_input_locked(i, false);
            }
            attach_input_locked(i, desired_input_queries_[i], false);
        }
    }
    for (size_t j = 0; j < render_endpoints_.size(); ++j) {
        if (!engine_.bus(j).is_enabled() || desired_output_queries_[j].empty()) continue;
        const bool wasapi_alive = render_endpoints_[j] && render_endpoints_[j]->is_running();
        const bool asio_alive = render_asio_endpoints_[j] && render_asio_endpoints_[j]->is_running();
        if (!wasapi_alive && !asio_alive) {
            if (render_endpoints_[j] || render_asio_endpoints_[j]) {
                detach_output_locked(j, false);
            }
            attach_output_locked(j, desired_output_queries_[j], false);
        }
    }
}

void EndpointManager::detach_input_locked(size_t index, bool clear_desired) {
    engine_.input_connection_state(index)->store(false);
    engine_.input_virtual_state(index)->store(false);
    if (capture_endpoints_[index]) capture_endpoints_[index]->stop();
    if (capture_asio_endpoints_[index]) capture_asio_endpoints_[index]->stop();
    capture_endpoints_[index].reset();
    capture_asio_endpoints_[index].reset();
    capture_names_[index] = "None";
    capture_ids_[index].clear();
    if (clear_desired) desired_input_queries_[index].clear();
}

void EndpointManager::detach_output_locked(size_t index, bool clear_desired) {
    engine_.output_connection_state(index)->store(false);
    if (render_endpoints_[index]) render_endpoints_[index]->stop();
    if (render_asio_endpoints_[index]) render_asio_endpoints_[index]->stop();
    render_endpoints_[index].reset();
    render_asio_endpoints_[index].reset();
    render_names_[index] = "None";
    render_ids_[index].clear();
    if (clear_desired) desired_output_queries_[index].clear();
}

bool EndpointManager::attach_input_locked(size_t index, const std::string& selection, bool remember_query) {
    detach_input_locked(index, false);
    const ParsedQuery pq = parse_query(selection);
    if (disconnected(pq.target)) {
        if (remember_query) desired_input_queries_[index].clear();
        return true;
    }
    if (remember_query) desired_input_queries_[index] = selection;
    if (!engine_.strip(index).is_enabled()) return false;

    AudioStreamConfig config;
    config.sample_rate = static_cast<uint32_t>(engine_.sample_rate());
    config.channels = 2;
    config.buffer_frames = static_cast<uint32_t>(engine_.block_frames());
    config.share_mode = pq.exclusive ? ShareMode::Exclusive : ShareMode::Shared;

    if (pq.asio) {
        auto drivers = AsioHostInterface::enumerate_drivers();
        const int found = resolve_asio(drivers, pq.target);
        if (found < 0) return false;
        engine_.strip_input_ring_buffer(index)->reset_metrics();
        auto endpoint = std::make_unique<AsioEndpoint>(
            drivers[found], DeviceType::Capture, config,
            engine_.strip_input_ring_buffer(index), engine_.input_connection_state(index));
        if (!endpoint->start()) return false;
        capture_names_[index] = "[ASIO] " + drivers[found].name;
        capture_ids_[index] = "asio:" + drivers[found].name;
        capture_asio_endpoints_[index] = std::move(endpoint);
        return true;
    }

    if (pq.app_name || pq.app_pid) {
        uint32_t pid = 0;
        std::string label = pq.target;
        std::string id_tag;
        if (pq.app_pid) {
            try { pid = static_cast<uint32_t>(std::stoul(pq.target)); } catch (...) { pid = 0; }
            label = "PID " + std::to_string(pid);
            id_tag = "pid:" + std::to_string(pid);
        } else {
            pid = find_process_pid_by_name(pq.target);
            id_tag = "app:" + pq.target;
        }
        if (pid == 0) return false;

        DeviceInfo info{};
        info.id = wide_from_utf8(id_tag);
        info.friendly_name = wide_from_utf8("[APP] " + label);
        info.type = DeviceType::ProcessLoopback;
        info.default_sample_rate = config.sample_rate;
        info.channels = 2;
        info.target_pid = pid;
        info.include_process_tree = true;

        engine_.strip_input_ring_buffer(index)->reset_metrics();
        auto endpoint = std::make_unique<WasapiEndpoint>(
            info, config, engine_.strip_input_ring_buffer(index),
            engine_.input_connection_state(index), static_cast<uint8_t>(index));
        if (!endpoint->start()) return false;
        capture_names_[index] = "[APP] " + label;
        capture_ids_[index] = id_tag;
        capture_endpoints_[index] = std::move(endpoint);
        return true;
    }

    auto devices = WasapiEndpoint::enumerate_devices(DeviceType::Capture);
    int found = resolve(devices, pq.target);
    if (found == -2) return false;
    if (found < 0) {
        if (pq.target.rfind("id:", 0) != 0 &&
            (lower(pq.target).rfind("muxella ", 0) == 0 ||
             std::all_of(pq.target.begin(), pq.target.end(), [](unsigned char c) { return std::isdigit(c); }))) {
            return false;
        }
        devices = WasapiEndpoint::enumerate_devices(DeviceType::Render);
        found = resolve(devices, pq.target);
        if (found < 0) return false;
        devices[found].type = DeviceType::LoopbackRender;
    }

    engine_.strip_input_ring_buffer(index)->reset_metrics();
    engine_.input_virtual_state(index)->store(devices[found].friendly_name.find(L"Muxella") != std::wstring::npos);
    auto endpoint = std::make_unique<WasapiEndpoint>(
        devices[found], config, engine_.strip_input_ring_buffer(index),
        engine_.input_connection_state(index), static_cast<uint8_t>(index));
    if (!endpoint->start()) return false;
    capture_names_[index] = utf8(devices[found].friendly_name);
    capture_ids_[index] = utf8(devices[found].id);
    capture_endpoints_[index] = std::move(endpoint);
    return true;
}

bool EndpointManager::attach_output_locked(size_t index, const std::string& selection, bool remember_query) {
    detach_output_locked(index, false);
    const ParsedQuery pq = parse_query(selection);
    if (disconnected(pq.target)) {
        if (remember_query) desired_output_queries_[index].clear();
        return true;
    }
    if (remember_query) desired_output_queries_[index] = selection;
    if (!engine_.bus(index).is_enabled()) return false;

    AudioStreamConfig config;
    config.sample_rate = static_cast<uint32_t>(engine_.sample_rate());
    config.channels = 2;
    config.buffer_frames = static_cast<uint32_t>(engine_.block_frames());
    config.share_mode = pq.exclusive ? ShareMode::Exclusive : ShareMode::Shared;

    if (pq.asio) {
        auto drivers = AsioHostInterface::enumerate_drivers();
        const int found = resolve_asio(drivers, pq.target);
        if (found < 0) return false;
        engine_.bus_output_ring_buffer(index)->reset_metrics();
        auto endpoint = std::make_unique<AsioEndpoint>(
            drivers[found], DeviceType::Render, config,
            engine_.bus_output_ring_buffer(index), engine_.output_connection_state(index));
        if (!endpoint->start()) return false;
        render_names_[index] = "[ASIO] " + drivers[found].name;
        render_ids_[index] = "asio:" + drivers[found].name;
        render_asio_endpoints_[index] = std::move(endpoint);
        return true;
    }

    auto devices = WasapiEndpoint::enumerate_devices(DeviceType::Render);
    const int found = resolve(devices, pq.target);
    if (found < 0) return false;

    engine_.bus_output_ring_buffer(index)->reset_metrics();
    auto endpoint = std::make_unique<WasapiEndpoint>(
        devices[found], config, engine_.bus_output_ring_buffer(index),
        engine_.output_connection_state(index), static_cast<uint8_t>(index));
    if (!endpoint->start()) return false;
    render_names_[index] = (endpoint->is_exclusive() ? "[EX] " : "") + utf8(devices[found].friendly_name);
    render_ids_[index] = (pq.exclusive ? "exclusive:" : "") + utf8(devices[found].id);
    if (!pq.exclusive) render_ids_[index] = utf8(devices[found].id);
    render_endpoints_[index] = std::move(endpoint);
    return true;
}

bool EndpointManager::attach_input(size_t index, const std::string& selection) {
    if (index >= capture_endpoints_.size()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return attach_input_locked(index, selection, true);
}

bool EndpointManager::attach_output(size_t index, const std::string& selection) {
    if (index >= render_endpoints_.size()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return attach_output_locked(index, selection, true);
}

void EndpointManager::detach_input(size_t index) { attach_input(index, "disconnect"); }
void EndpointManager::detach_output(size_t index) { attach_output(index, "disconnect"); }

bool EndpointManager::set_input_enabled(size_t index, bool enabled) {
    if (index >= capture_endpoints_.size()) return false;
    engine_.strip(index).set_enabled(enabled);
    if (!enabled) detach_input(index);
    return true;
}

bool EndpointManager::set_output_enabled(size_t index, bool enabled) {
    if (index >= render_endpoints_.size()) return false;
    engine_.bus(index).set_enabled(enabled);
    if (!enabled) {
        detach_output(index);
        engine_.bus_output_ring_buffer(index)->reset();
    }
    return true;
}

std::string EndpointManager::get_input_name(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index < capture_names_.size() ? capture_names_[index] : "None";
}

std::string EndpointManager::get_output_name(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index < render_names_.size() ? render_names_[index] : "None";
}

std::string EndpointManager::serialize_devices_json() const {
    using Json = nlohmann::json;
    const auto captures = WasapiEndpoint::enumerate_devices(DeviceType::Capture);
    const auto renders = WasapiEndpoint::enumerate_devices(DeviceType::Render);
    const auto asios = AsioHostInterface::enumerate_drivers();
    const auto apps = enumerate_audio_processes();
    std::lock_guard<std::mutex> lock(mutex_);
    Json data = {
        {"type", "devices"},
        {"hotplug_generation", hotplug_generation_.load(std::memory_order_relaxed)},
        {"captures", Json::array()},
        {"renders", Json::array()},
        {"asios", Json::array()},
        {"apps", Json::array()},
        {"attached_inputs", Json::array()},
        {"attached_outputs", Json::array()},
        {"attached_input_ids", Json::array()},
        {"attached_output_ids", Json::array()}
    };
    auto add_devices = [&](const char* key, const auto& devices) {
        for (size_t i = 0; i < devices.size(); ++i) {
            const auto& d = devices[i];
            data[key].push_back({{"index", i}, {"id", utf8(d.id)}, {"name", utf8(d.friendly_name)},
                {"default", d.is_default}, {"rate", d.default_sample_rate}, {"channels", d.channels}});
        }
    };
    add_devices("captures", captures);
    add_devices("renders", renders);
    for (size_t i = 0; i < asios.size(); ++i) {
        data["asios"].push_back({{"index", i}, {"id", "asio:" + asios[i].name}, {"name", asios[i].name},
            {"description", asios[i].description}, {"clsid", asios[i].clsid_str}});
    }
    for (const auto& app : apps) {
        data["apps"].push_back({{"pid", app.pid}, {"id", "app:" + app.name}, {"name", app.name}});
    }
    auto add_attached = [&](const char* key, const char* ids, const auto& endpoints, const auto& asio_endpoints,
                            const auto& names, const auto& cached_ids) {
        for (size_t i = 0; i < endpoints.size(); ++i) {
            const bool running = (endpoints[i] && endpoints[i]->is_running()) ||
                                 (asio_endpoints[i] && asio_endpoints[i]->is_running());
            data[key].push_back(running ? names[i] : "None");
            data[ids].push_back(running ? cached_ids[i] : "");
        }
    };
    add_attached("attached_inputs", "attached_input_ids", capture_endpoints_, capture_asio_endpoints_, capture_names_, capture_ids_);
    add_attached("attached_outputs", "attached_output_ids", render_endpoints_, render_asio_endpoints_, render_names_, render_ids_);
    return data.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string EndpointManager::serialize_health_json() const {
    using Json = nlohmann::json;
    std::lock_guard<std::mutex> lock(mutex_);

    auto stage_name = [](GlitchStage s) -> const char* {
        switch (s) {
            case GlitchStage::Capture: return "Capture";
            case GlitchStage::StripInput: return "StripIn";
            case GlitchStage::StripDsp: return "StripDSP";
            case GlitchStage::BusOut: return "BusOut";
            case GlitchStage::Render: return "Render";
            case GlitchStage::EngineTimer: return "EngineTimer";
            default: return "Unknown";
        }
    };
    auto kind_name = [](GlitchKind k) -> const char* {
        switch (k) {
            case GlitchKind::WasapiDiscontinuity: return "WASAPI_DISCONTINUITY";
            case GlitchKind::StripStarve: return "STRIP_STARVE";
            case GlitchKind::StripUnprime: return "STRIP_UNPRIME";
            case GlitchKind::StripTruncate: return "STRIP_TRUNCATE";
            case GlitchKind::RenderUnderrun: return "RENDER_UNDERRUN";
            case GlitchKind::RenderDiscard: return "RENDER_DISCARD";
            case GlitchKind::RenderRebuffer: return "RENDER_REBUFFER";
            case GlitchKind::StepJump: return "STEP_JUMP";
            case GlitchKind::ZeroDrop: return "ZERO_DROP";
            case GlitchKind::LateTick: return "LATE_TICK";
            case GlitchKind::TimerResync: return "TIMER_RESYNC";
            default: return "UNKNOWN";
        }
    };

    AudioDebugEvent tmp[256];
    auto collect_events = [&](size_t count) {
        for (size_t k = 0; k < count; ++k) {
            recent_events_.push_back(tmp[k]);
        }
    };
    for (const auto& ep : capture_endpoints_) {
        if (ep) collect_events(ep->drain_debug_events(tmp, 256));
    }
    for (const auto& ep : render_endpoints_) {
        if (ep) collect_events(ep->drain_debug_events(tmp, 256));
    }
    collect_events(engine_.drain_debug_events(tmp, 256));
    std::stable_sort(recent_events_.begin(), recent_events_.end(),
                     [](const AudioDebugEvent& a, const AudioDebugEvent& b) {
                         return a.timestamp_ms < b.timestamp_ms;
                     });
    if (recent_events_.size() > 200) {
        recent_events_.erase(recent_events_.begin(), recent_events_.begin() + (recent_events_.size() - 200));
    }

    const auto& sched = engine_.scheduler_telemetry();
    Json data = {
        {"type", "health"},
        {"sample_rate", engine_.sample_rate()},
        {"block_ms", 1000.0 * engine_.block_frames() / engine_.sample_rate()},
        {"hotplug_generation", hotplug_generation_.load(std::memory_order_relaxed)},
        {"engine", {
            {"total_blocks", sched.total_blocks.load(std::memory_order_relaxed)},
            {"late_ticks", sched.late_ticks.load(std::memory_order_relaxed)},
            {"timer_resyncs", sched.timer_resyncs.load(std::memory_order_relaxed)},
            {"max_jitter_ms", sched.max_jitter_ms.load(std::memory_order_relaxed)},
            {"max_process_ms", sched.max_process_ms.load(std::memory_order_relaxed)}
        }},
        {"streams", Json::array()},
        {"events", Json::array()}
    };

    auto add = [&](bool output, const auto& endpoints, const auto& asio_endpoints, const auto& names) {
        for (size_t i = 0; i < endpoints.size(); ++i) {
            auto ring = output ? engine_.bus_output_ring_buffer(i) : engine_.strip_input_ring_buffer(i);
            const auto queued = std::min(ring->available_read(), ring->capacity());
            const bool wasapi_active = endpoints[i] && endpoints[i]->is_running();
            const bool asio_active = asio_endpoints[i] && asio_endpoints[i]->is_running();
            const bool active = wasapi_active || asio_active;

            uint64_t starves = 0;
            uint64_t discards = 0;
            uint64_t discard_samples = 0;
            uint64_t discont = 0;
            uint64_t glitches = wasapi_active ? endpoints[i]->waveform_glitches() : 0;
            double drift_ppm = 0.0;

            if (!output) {
                const auto& st = engine_.strip_telemetry(i);
                starves = st.starve_events.load(std::memory_order_relaxed) +
                          st.unprime_events.load(std::memory_order_relaxed) +
                          st.truncate_events.load(std::memory_order_relaxed);
                discont = wasapi_active ? endpoints[i]->discontinuity_events() : 0;
                glitches += st.dsp_glitches.load(std::memory_order_relaxed);
                drift_ppm = st.drift_ppm.load(std::memory_order_relaxed);
            } else {
                const auto& bt = engine_.bus_telemetry(i);
                starves = wasapi_active ? endpoints[i]->render_underrun_events()
                        : (asio_active ? asio_endpoints[i]->underrun_events() : 0);
                discards = wasapi_active ? endpoints[i]->render_discard_events()
                         : (asio_active ? asio_endpoints[i]->discard_events() : 0);
                discard_samples = wasapi_active ? endpoints[i]->render_discard_samples()
                                : (asio_active ? asio_endpoints[i]->discard_samples() : 0);
                glitches += bt.bus_glitches.load(std::memory_order_relaxed);
            }

            data["streams"].push_back({
                {"kind", output ? "output" : "input"},
                {"index", i},
                {"enabled", output ? engine_.bus(i).is_enabled() : engine_.strip(i).is_enabled()},
                {"device", names[i]},
                {"active", active},
                {"exclusive", wasapi_active ? endpoints[i]->is_exclusive() : false},
                {"asio", asio_active},
                {"queue_ms", active ? queued * 500.0 / engine_.sample_rate() : 0.0},
                {"shortfall_samples", ring->underrun_count()},
                {"dropped_samples", ring->overrun_count()},
                {"clips", output ? engine_.bus(i).clip_events() : 0},
                {"raw", wasapi_active ? endpoints[i]->raw_status() : 0},
                {"device_rate", wasapi_active ? endpoints[i]->device_sample_rate()
                              : (asio_active ? asio_endpoints[i]->device_sample_rate() : 0)},
                {"device_channels", wasapi_active ? endpoints[i]->device_channels()
                                  : (asio_active ? asio_endpoints[i]->device_channels() : 0)},
                {"device_bits", wasapi_active ? endpoints[i]->device_bits()
                              : (asio_active ? asio_endpoints[i]->device_bits() : 0)},
                {"wasapi_buffer_frames", wasapi_active ? endpoints[i]->wasapi_buffer_frames()
                                       : (asio_active ? asio_endpoints[i]->buffer_frames() : 0)},
                {"drift_ppm", drift_ppm},
                {"starves", starves},
                {"discards", discards},
                {"discard_samples", discard_samples},
                {"discontinuities", discont},
                {"glitches", glitches}
            });
        }
    };
    add(false, capture_endpoints_, capture_asio_endpoints_, capture_names_);
    add(true, render_endpoints_, render_asio_endpoints_, render_names_);

    for (const auto& ev : recent_events_) {
        data["events"].push_back({
            {"ts_ms", ev.timestamp_ms},
            {"stage", stage_name(ev.stage)},
            {"index", ev.index},
            {"kind", kind_name(ev.kind)},
            {"value", ev.value}
        });
    }
    return data.dump(-1, ' ', false, Json::error_handler_t::replace);
}

void EndpointManager::reset_health() {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_events_.clear();
    engine_.reset_telemetry();
    for (const auto& ep : capture_endpoints_) {
        if (ep) ep->reset_telemetry();
    }
    for (const auto& ep : render_endpoints_) {
        if (ep) ep->reset_telemetry();
    }
    for (const auto& ep : capture_asio_endpoints_) {
        if (ep) ep->reset_telemetry();
    }
    for (const auto& ep : render_asio_endpoints_) {
        if (ep) ep->reset_telemetry();
    }
}

} // namespace muxella::core
