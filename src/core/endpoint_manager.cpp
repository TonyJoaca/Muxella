#include "metvoice/core/endpoint_manager.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace metvoice::core {
namespace {
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::string clean(std::string query) {
    const auto first = query.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    query = query.substr(first, query.find_last_not_of(" \t\r\n") - first + 1);
    for (const char* prefix : {"[WDM] ", "[VAIO] "})
        if (query.rfind(prefix, 0) == 0) query = query.substr(std::char_traits<char>::length(prefix));
    return query;
}
bool named_match(const std::string& query, const std::string& name) {
    if (query == name) return true;
    for (const char* alias : {"muxella main", "muxella spotify", "muxella discord", "muxella out b1", "muxella out b2", "muxella out b3"})
        if (query == alias) return name.rfind(query + " (", 0) == 0 || name.find("(" + query + ")") != std::string::npos;
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
bool disconnected(const std::string& query) {
    const auto value = lower(query);
    return value.empty() || value == "disconnect" || value == "select device" || value == "none";
}
}

EndpointManager::EndpointManager(matrix::MixerEngine& engine) : engine_(engine) {
    capture_endpoints_.resize(engine.num_strips()); render_endpoints_.resize(engine.num_buses());
    capture_names_.resize(engine.num_strips(), "None"); render_names_.resize(engine.num_buses(), "None");
}
EndpointManager::~EndpointManager() {
    for (auto& endpoint : capture_endpoints_) if (endpoint) endpoint->stop();
    for (auto& endpoint : render_endpoints_) if (endpoint) endpoint->stop();
}
bool EndpointManager::attach_input(size_t index, const std::string& selection) {
    if (index >= capture_endpoints_.size()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    engine_.input_connection_state(index)->store(false);
    engine_.input_virtual_state(index)->store(false);
    if (capture_endpoints_[index]) capture_endpoints_[index]->stop();
    capture_endpoints_[index].reset(); capture_names_[index] = "None";
    const auto query = clean(selection);
    if (disconnected(query)) return true;
    if (!engine_.strip(index).is_enabled()) return false;
    auto devices = WasapiEndpoint::enumerate_devices(DeviceType::Capture);
    int found = resolve(devices, query);
    if (found == -2) return false;
    if (found < 0) {
        if (query.rfind("id:", 0) != 0 &&
            (lower(query).rfind("muxella ", 0) == 0 ||
             std::all_of(query.begin(), query.end(), [](unsigned char c) { return std::isdigit(c); }))) return false;
        devices = WasapiEndpoint::enumerate_devices(DeviceType::Render);
        found = resolve(devices, query);
        if (found < 0) return false;
        devices[found].type = DeviceType::LoopbackRender;
    }
    AudioStreamConfig config;
    config.sample_rate = static_cast<uint32_t>(engine_.sample_rate()); config.channels = 2;
    config.buffer_frames = static_cast<uint32_t>(engine_.block_frames());
    engine_.strip_input_ring_buffer(index)->reset_metrics();
    engine_.input_virtual_state(index)->store(devices[found].friendly_name.find(L"Muxella") != std::wstring::npos);
    auto endpoint = std::make_unique<WasapiEndpoint>(devices[found], config, engine_.strip_input_ring_buffer(index), engine_.input_connection_state(index), static_cast<uint8_t>(index));
    if (!endpoint->start()) return false;
    capture_names_[index] = utf8(devices[found].friendly_name);
    capture_endpoints_[index] = std::move(endpoint);
    return true;
}
bool EndpointManager::attach_output(size_t index, const std::string& selection) {
    if (index >= render_endpoints_.size()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    engine_.output_connection_state(index)->store(false);
    if (render_endpoints_[index]) render_endpoints_[index]->stop();
    render_endpoints_[index].reset(); render_names_[index] = "None";
    const auto query = clean(selection);
    if (disconnected(query)) return true;
    if (!engine_.bus(index).is_enabled()) return false;
    auto devices = WasapiEndpoint::enumerate_devices(DeviceType::Render);
    const int found = resolve(devices, query);
    if (found < 0) return false;
    AudioStreamConfig config;
    config.sample_rate = static_cast<uint32_t>(engine_.sample_rate()); config.channels = 2;
    config.buffer_frames = static_cast<uint32_t>(engine_.block_frames());
    engine_.bus_output_ring_buffer(index)->reset_metrics();
    auto endpoint = std::make_unique<WasapiEndpoint>(devices[found], config, engine_.bus_output_ring_buffer(index), engine_.output_connection_state(index), static_cast<uint8_t>(index));
    if (!endpoint->start()) return false;
    render_names_[index] = utf8(devices[found].friendly_name);
    render_endpoints_[index] = std::move(endpoint);
    return true;
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
        // The render consumer has stopped; discard its old output before reuse.
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
    std::lock_guard<std::mutex> lock(mutex_);
    Json data = {{"type", "devices"}, {"captures", Json::array()}, {"renders", Json::array()},
        {"attached_inputs", Json::array()}, {"attached_outputs", Json::array()},
        {"attached_input_ids", Json::array()}, {"attached_output_ids", Json::array()}};
    auto add_devices = [&](const char* key, const auto& devices) {
        for (size_t i = 0; i < devices.size(); ++i) {
            const auto& d = devices[i];
            data[key].push_back({{"index", i}, {"id", utf8(d.id)}, {"name", utf8(d.friendly_name)},
                {"default", d.is_default}, {"rate", d.default_sample_rate}, {"channels", d.channels}});
        }
    };
    add_devices("captures", captures); add_devices("renders", renders);
    auto add_attached = [&](const char* key, const char* ids, const auto& endpoints, const auto& names) {
        for (size_t i = 0; i < endpoints.size(); ++i) {
            const bool running = endpoints[i] && endpoints[i]->is_running();
            data[key].push_back(running ? names[i] : "None");
            data[ids].push_back(running ? utf8(endpoints[i]->info().id) : "");
        }
    };
    add_attached("attached_inputs", "attached_input_ids", capture_endpoints_, capture_names_);
    add_attached("attached_outputs", "attached_output_ids", render_endpoints_, render_names_);
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

    auto add = [&](bool output, const auto& endpoints, const auto& names) {
        for (size_t i = 0; i < endpoints.size(); ++i) {
            auto ring = output ? engine_.bus_output_ring_buffer(i) : engine_.strip_input_ring_buffer(i);
            const auto queued = std::min(ring->available_read(), ring->capacity());
            const bool active = endpoints[i] && endpoints[i]->is_running();

            uint64_t starves = 0;
            uint64_t discards = 0;
            uint64_t discard_samples = 0;
            uint64_t discont = 0;
            uint64_t glitches = active ? endpoints[i]->waveform_glitches() : 0;
            double drift_ppm = 0.0;

            if (!output) {
                const auto& st = engine_.strip_telemetry(i);
                starves = st.starve_events.load(std::memory_order_relaxed) +
                          st.unprime_events.load(std::memory_order_relaxed) +
                          st.truncate_events.load(std::memory_order_relaxed);
                discont = active ? endpoints[i]->discontinuity_events() : 0;
                glitches += st.dsp_glitches.load(std::memory_order_relaxed);
                drift_ppm = st.drift_ppm.load(std::memory_order_relaxed);
            } else {
                const auto& bt = engine_.bus_telemetry(i);
                starves = active ? endpoints[i]->render_underrun_events() : 0;
                discards = active ? endpoints[i]->render_discard_events() : 0;
                discard_samples = active ? endpoints[i]->render_discard_samples() : 0;
                glitches += bt.bus_glitches.load(std::memory_order_relaxed);
            }

            data["streams"].push_back({
                {"kind", output ? "output" : "input"},
                {"index", i},
                {"enabled", output ? engine_.bus(i).is_enabled() : engine_.strip(i).is_enabled()},
                {"device", names[i]},
                {"active", active},
                {"queue_ms", active ? queued * 500.0 / engine_.sample_rate() : 0.0},
                {"shortfall_samples", ring->underrun_count()},
                {"dropped_samples", ring->overrun_count()},
                {"clips", output ? engine_.bus(i).clip_events() : 0},
                {"raw", active ? endpoints[i]->raw_status() : 0},
                {"device_rate", active ? endpoints[i]->device_sample_rate() : 0},
                {"device_channels", active ? endpoints[i]->device_channels() : 0},
                {"device_bits", active ? endpoints[i]->device_bits() : 0},
                {"wasapi_buffer_frames", active ? endpoints[i]->wasapi_buffer_frames() : 0},
                {"drift_ppm", drift_ppm},
                {"starves", starves},
                {"discards", discards},
                {"discard_samples", discard_samples},
                {"discontinuities", discont},
                {"glitches", glitches}
            });
        }
    };
    add(false, capture_endpoints_, capture_names_);
    add(true, render_endpoints_, render_names_);

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
}
} // namespace metvoice::core
