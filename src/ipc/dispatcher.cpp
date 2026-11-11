#include "metvoice/ipc/protocol.hpp"
#include "metvoice/core/endpoint_manager.hpp"
#include "metvoice/core/app_sessions.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <unordered_map>

namespace metvoice::ipc {
using Json = nlohmann::json;
namespace {
std::string reply(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
}
std::string error(const std::string& message) { return reply({{"status", "error"}, {"message", message}}); }
}

std::string CommandDispatcher::serialize_devices_json(core::EndpointManager* manager) {
    return manager ? manager->serialize_devices_json() : Json{{"type", "devices"}, {"captures", Json::array()}, {"renders", Json::array()}}.dump();
}
std::string CommandDispatcher::serialize_meters_json(const matrix::MixerEngine& engine) {
    Json result = {{"type", "meters"}, {"strips", Json::array()}, {"buses", Json::array()}};
    for (size_t i = 0; i < engine.num_strips(); ++i) {
        const auto& s = engine.strip(i);
        result["strips"].push_back({{"id", i}, {"name", s.name()}, {"peak_l", s.peak_left_db()},
            {"enabled", s.is_enabled()},
            {"peak_r", s.peak_right_db()}, {"gr", s.gain_reduction_db()}, {"mute", s.is_muted()}, {"solo", s.is_solo()}});
    }
    for (size_t i = 0; i < engine.num_buses(); ++i) {
        const auto& b = engine.bus(i);
        result["buses"].push_back({{"id", i}, {"name", b.name()}, {"peak_l", b.peak_left_db()},
            {"enabled", b.is_enabled()},
            {"peak_r", b.peak_right_db()}, {"clips", b.clip_events()}, {"mute", b.is_muted()}});
    }
    return result.dump(-1, ' ', false, Json::error_handler_t::replace);
}
std::string CommandDispatcher::serialize_status_json(const matrix::MixerEngine& engine) {
    return Json{{"type", "status"}, {"engine_version", "cables-v9"}, {"sample_rate", engine.sample_rate()},
                {"buffer_frames", engine.block_frames()}, {"strips", engine.num_strips()}, {"buses", engine.num_buses()}}.dump();
}

std::string CommandDispatcher::execute_command(matrix::MixerEngine& engine, core::EndpointManager* manager,
                                               const std::string& request, std::function<void()>* shutdown) {
    std::istringstream input(request);
    std::string command;
    input >> command;
    static const std::unordered_map<std::string, std::string> aliases = {
        {"GET_METERS", "meters"}, {"GET_STATUS", "status"}, {"GET_DEVICES", "devices"},
        {"SET_INPUT", "attach_input"}, {"SET_OUTPUT", "attach_output"},
        {"SET_STRIP_NAME", "rename_strip"}, {"SET_BUS_NAME", "rename_bus"},
        {"SET_GAIN", "gain"}, {"SET_PAN", "pan"}, {"SET_MUTE", "mute"}, {"SET_SOLO", "solo"},
        {"SET_ROUTE", "route"}, {"SET_BUS_GAIN", "bus_gain"}, {"SET_BUS_MUTE", "bus_mute"},
        {"SET_EQ", "eq"}, {"SET_COMP", "comp"}, {"SET_GATE", "gate"}, {"SET_DENOISE", "denoise"},
        {"SET_REVERB", "reverb"}, {"SET_STRIP_MONO", "strip_mono"}, {"SET_BUS_MONO", "bus_mono"},
        {"SET_BUS_EQ", "bus_eq"}, {"SET_BUS_EQ_PARAMS", "bus_set_eq"}, {"GET_BUS_EQ", "get_bus_eq"}};
    if (auto it = aliases.find(command); it != aliases.end()) command = it->second;
    if (command == "record_status") return engine.recorder().status_json() + "\n";
    if (command == "record_stop") {
        engine.recorder().stop();
        return engine.recorder().status_json() + "\n";
    }
    if (command == "record_start") {
        std::string payload; std::getline(input >> std::ws, payload);
        const auto spec = Json::parse(payload, nullptr, false);
        if (!spec.is_object() || !spec.contains("folder") || !spec["folder"].is_string() ||
            !spec.contains("sources") || !spec["sources"].is_array() || spec["sources"].empty() || spec["sources"].size() > 16)
            return error("invalid recording selection");
        std::vector<core::RecordingSource> sources;
        for (const auto& item : spec["sources"]) {
            if (!item.is_object() || !item.contains("kind") || !item["kind"].is_string() ||
                !item.contains("index") || !item["index"].is_number_unsigned()) return error("invalid recording source");
            const std::string kind = item["kind"];
            if (kind != "input" && kind != "bus") return error("invalid recording source kind");
            const size_t index = item["index"];
            const bool bus = kind == "bus";
            if (index >= (bus ? engine.num_buses() : engine.num_strips())) return error("invalid recording source index");
            for (const auto& previous : sources) if (previous.bus == bus && previous.index == index) return error("duplicate recording source");
            sources.push_back({bus, index});
        }
        if (!engine.recorder().start(spec["folder"], sources, static_cast<uint32_t>(engine.sample_rate()), engine.block_frames()))
            return reply({{"status", "error"}, {"message", Json::parse(engine.recorder().status_json())["error"]}});
        return engine.recorder().status_json() + "\n";
    }
    if (command == "apps") return core::app_sessions_json() + "\n";
    if (command == "app_set") {
        std::string id; float level; int muted;
        if (!(input >> id >> level >> muted) || (muted != 0 && muted != 1)) return error("invalid app volume command");
        return core::set_app_session(id, level, muted != 0) + "\n";
    }
    if (command == "health") return manager ? manager->serialize_health_json() + "\n" : error("endpoint manager unavailable");
    if (command == "reset_health") {
        if (!manager) return error("endpoint manager unavailable");
        manager->reset_health();
        return reply({{"status", "ok"}, {"action", "reset_health"}});
    }
    if (command == "meters") return serialize_meters_json(engine) + "\n";
    if (command == "status") return serialize_status_json(engine) + "\n";
    if (command == "devices") return serialize_devices_json(manager) + "\n";
    if (command == "shutdown") {
        if (shutdown && *shutdown) (*shutdown)();
        return reply({{"status", "ok"}, {"action", "shutdown"}});
    }
    size_t index = 0;
    if (!(input >> index)) return error("missing index or unknown command: " + command);
    const bool bus_command = command.rfind("bus_", 0) == 0 || command == "rename_bus" ||
                             command == "attach_output" || command == "detach_output" || command == "get_bus_eq";
    if (index >= (bus_command ? engine.num_buses() : engine.num_strips())) return error("invalid index");
    Json result = {{"status", "ok"}, {bus_command ? "bus" : "strip", index}};
    if (command == "strip_enable" || command == "bus_enable") {
        int enabled;
        if (!(input >> enabled) || (enabled != 0 && enabled != 1) || !manager) return error("invalid channel enable command");
        const bool ok = command == "strip_enable" ? manager->set_input_enabled(index, enabled != 0) :
                                                    manager->set_output_enabled(index, enabled != 0);
        if (!ok) return error("failed to change channel state");
        result["enabled"] = enabled != 0;
        return reply(result);
    }
    if (command == "attach_input" || command == "attach_output" || command == "rename_strip" || command == "rename_bus") {
        std::string value;
        std::getline(input >> std::ws, value);
        if (value.empty()) return error("missing device or name");
        if (command == "rename_strip" || command == "rename_bus") {
            if (command == "rename_strip") engine.set_strip_name(index, value); else engine.set_bus_name(index, value);
            result["name"] = value;
        } else {
            if (!manager || !(command == "attach_input" ? manager->attach_input(index, value) : manager->attach_output(index, value)))
                return error("failed to attach selected endpoint");
            result["device"] = value;
        }
        return reply(result);
    }
    if (command == "detach_input" || command == "detach_output") {
        if (!manager) return error("endpoint manager unavailable");
        if (command == "detach_input") manager->detach_input(index); else manager->detach_output(index);
        result["device"] = "detached";
        return reply(result);
    }
    if (command == "get_bus_eq") {
        const auto& b = engine.bus(index);
        return reply({{"status", "ok"}, {"bus", index}, {"enabled", b.is_eq_enabled()},
            {"low_gain", b.eq_low_gain()}, {"mid_gain", b.eq_mid_gain()}, {"high_gain", b.eq_high_gain()},
            {"low_freq", b.eq_low_freq()}, {"mid_freq", b.eq_mid_freq()}, {"high_freq", b.eq_high_freq()}, {"mid_q", b.eq_mid_q()}});
    }
    if (command == "route") {
        size_t bus = 0; int enabled = 0; float gain = 0;
        if (!(input >> bus >> enabled) || bus >= engine.num_buses()) return error("invalid route");
        if (!(input >> gain)) gain = 0;
        if (!std::isfinite(gain)) return error("invalid route gain");
        engine.routing().set_route(index, bus, enabled != 0, gain);
        return reply({{"status", "ok"}, {"route", {{"strip", index}, {"bus", bus}, {"enabled", enabled != 0}}}});
    }
    using Kind = matrix::MixerEngine::DspKind;
    matrix::MixerEngine::DspUpdate update{};
    update.index = index;
    if (command == "eq" || command == "bus_set_eq") {
        auto& v = update.values;
        if (!(input >> v[0] >> v[1] >> v[2])) return error("invalid EQ gains");
        if (command == "bus_set_eq") {
            v[3] = 100; v[4] = 1000; v[5] = 8000; v[6] = 1;
            if (input >> std::ws && input.peek() != EOF && !(input >> v[3] >> v[4] >> v[5] >> v[6])) return error("invalid EQ parameters");
            update.kind = Kind::BusEq;
        } else update.kind = Kind::StripEq;
        for (float value : v) if (!std::isfinite(value)) return error("invalid EQ value");
        if (!engine.enqueue_dsp(update)) return error("DSP update queue full");
        result["eq"] = "updated";
        return reply(result);
    }
    float value = 0;
    if (!(input >> value) || !std::isfinite(value)) return error("invalid parameter");
    const bool enabled = value != 0;
    if (command == "gain") engine.strip(index).set_gain_db(std::clamp(value, -120.0f, 24.0f));
    else if (command == "pan") engine.strip(index).set_pan(value);
    else if (command == "mute") engine.strip(index).set_mute(enabled);
    else if (command == "solo") engine.strip(index).set_solo(enabled);
    else if (command == "strip_mono") engine.strip(index).set_mono(enabled);
    else if (command == "bus_gain") engine.bus(index).set_gain_db(std::clamp(value, -120.0f, 24.0f));
    else if (command == "bus_mute") engine.bus(index).set_mute(enabled);
    else if (command == "bus_mono") engine.bus(index).set_mono(enabled);
    else if (command == "bus_eq") engine.bus(index).enable_eq(enabled);
    else if (command == "bus_limiter") engine.bus(index).enable_limiter(enabled);
    else {
        if (command == "comp") update.kind = Kind::Compressor;
        else if (command == "gate") update.kind = Kind::Gate;
        else if (command == "denoise") update.kind = Kind::Denoise;
        else if (command == "reverb") update.kind = Kind::Reverb;
        else return error("unknown command: " + command);
        update.values[0] = value;
        if (!engine.enqueue_dsp(update)) return error("DSP update queue full");
    }
    if (command == "mute" || command == "bus_mute") result["mute"] = enabled;
    else if (command == "solo") result["solo"] = enabled;
    else if (command == "strip_mono" || command == "bus_mono") result["mono"] = enabled;
    else if (command == "bus_eq") result["eq"] = enabled;
    else result[command == "bus_gain" ? "gain" : command] = value;
    return reply(result);
}
} // namespace metvoice::ipc
