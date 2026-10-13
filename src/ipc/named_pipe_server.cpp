#include "metvoice/ipc/named_pipe_server.hpp"
#include "metvoice/ipc/protocol.hpp"
#include "metvoice/core/wasapi_backend.hpp"
#include "metvoice/core/endpoint_manager.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <windows.h>

namespace metvoice::ipc {

std::string CommandDispatcher::serialize_devices_json(core::EndpointManager* ep_mgr) {
    if (ep_mgr) {
        return ep_mgr->serialize_devices_json();
    }
    return "{\"type\":\"devices\",\"captures\":[],\"renders\":[]}";
}

std::string CommandDispatcher::serialize_meters_json(const matrix::MixerEngine& engine) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    ss << "{\"type\":\"meters\",\"strips\":[";
    for (size_t i = 0; i < engine.num_strips(); ++i) {
        if (i > 0) ss << ",";
        const auto& s = engine.strip(i);
        ss << "{\"id\":" << i
           << ",\"name\":\"" << s.name() << "\""
           << ",\"peak_l\":" << s.peak_left_db()
           << ",\"peak_r\":" << s.peak_right_db()
           << ",\"gr\":" << s.gain_reduction_db()
           << ",\"mute\":" << (s.is_muted() ? "true" : "false")
           << ",\"solo\":" << (s.is_solo() ? "true" : "false")
           << "}";
    }
    ss << "],\"buses\":[";
    for (size_t j = 0; j < engine.num_buses(); ++j) {
        if (j > 0) ss << ",";
        const auto& b = engine.bus(j);
        ss << "{\"id\":" << j
           << ",\"name\":\"" << b.name() << "\""
           << ",\"peak_l\":" << b.peak_left_db()
           << ",\"peak_r\":" << b.peak_right_db()
           << ",\"clips\":" << b.clip_events()
           << ",\"mute\":" << (b.is_muted() ? "true" : "false")
           << "}";
    }
    ss << "]}";
    return ss.str();
}

std::string CommandDispatcher::serialize_status_json(const matrix::MixerEngine& engine) {
    std::ostringstream ss;
    ss << "{\"type\":\"status\""
       << ",\"sample_rate\":" << engine.sample_rate()
       << ",\"strips\":" << engine.num_strips()
       << ",\"buses\":" << engine.num_buses()
       << "}";
    return ss.str();
}

std::string CommandDispatcher::execute_command(matrix::MixerEngine& engine,
                                               core::EndpointManager* ep_mgr,
                                               const std::string& request) {
    std::istringstream iss(request);
    std::string cmd;
    iss >> cmd;

    if (cmd == "meters" || cmd == "GET_METERS") {
        return serialize_meters_json(engine) + "\n";
    }
    if (cmd == "status" || cmd == "GET_STATUS") {
        return serialize_status_json(engine) + "\n";
    }
    if (cmd == "devices" || cmd == "GET_DEVICES") {
        return serialize_devices_json(ep_mgr) + "\n";
    }

    if (cmd == "attach_input" || cmd == "SET_INPUT") {
        size_t idx = 0;
        if (iss >> idx) {
            std::string query;
            std::getline(iss, query);
            while (!query.empty() && (query.front() == ' ' || query.front() == '\t')) query.erase(0, 1);
            while (!query.empty() && (query.back() == ' ' || query.back() == '\t' || query.back() == '\r' || query.back() == '\n')) query.pop_back();
            if (ep_mgr && ep_mgr->attach_input(idx, query)) {
                return "{\"status\":\"ok\",\"strip\":" + std::to_string(idx) + ",\"device\":\"" + query + "\"}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"failed to attach input device\"}\n";
    }

    if (cmd == "attach_output" || cmd == "SET_OUTPUT") {
        size_t idx = 0;
        if (iss >> idx) {
            std::string query;
            std::getline(iss, query);
            while (!query.empty() && (query.front() == ' ' || query.front() == '\t')) query.erase(0, 1);
            while (!query.empty() && (query.back() == ' ' || query.back() == '\t' || query.back() == '\r' || query.back() == '\n')) query.pop_back();
            if (ep_mgr && ep_mgr->attach_output(idx, query)) {
                return "{\"status\":\"ok\",\"bus\":" + std::to_string(idx) + ",\"device\":\"" + query + "\"}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"failed to attach output device\"}\n";
    }

    if (cmd == "detach_input") {
        size_t idx = 0;
        if (iss >> idx && ep_mgr) {
            ep_mgr->detach_input(idx);
            return "{\"status\":\"ok\",\"strip\":" + std::to_string(idx) + ",\"device\":\"detached\"}\n";
        }
    }

    if (cmd == "detach_output") {
        size_t idx = 0;
        if (iss >> idx && ep_mgr) {
            ep_mgr->detach_output(idx);
            return "{\"status\":\"ok\",\"bus\":" + std::to_string(idx) + ",\"device\":\"detached\"}\n";
        }
    }

    if (cmd == "rename_strip" || cmd == "SET_STRIP_NAME") {
        size_t idx = 0;
        if (iss >> idx) {
            std::string new_name;
            std::getline(iss, new_name);
            while (!new_name.empty() && new_name.front() == ' ') new_name.erase(0, 1);
            if (idx < engine.num_strips() && !new_name.empty()) {
                engine.set_strip_name(idx, new_name);
                return "{\"status\":\"ok\",\"strip\":" + std::to_string(idx) + ",\"name\":\"" + new_name + "\"}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid rename parameters\"}\n";
    }

    if (cmd == "rename_bus" || cmd == "SET_BUS_NAME") {
        size_t idx = 0;
        if (iss >> idx) {
            std::string new_name;
            std::getline(iss, new_name);
            while (!new_name.empty() && new_name.front() == ' ') new_name.erase(0, 1);
            if (idx < engine.num_buses() && !new_name.empty()) {
                engine.set_bus_name(idx, new_name);
                return "{\"status\":\"ok\",\"bus\":" + std::to_string(idx) + ",\"name\":\"" + new_name + "\"}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid rename parameters\"}\n";
    }

    if (cmd == "gain" || cmd == "SET_GAIN") {
        size_t idx = 0;
        float gain_db = 0.0f;
        if (iss >> idx >> gain_db) {
            if (idx < engine.num_strips()) {
                engine.strip(idx).set_gain_db(gain_db);
                return "{\"status\":\"ok\",\"strip\":" + std::to_string(idx) + ",\"gain\":" + std::to_string(gain_db) + "}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid index or parameters\"}\n";
    }

    if (cmd == "pan" || cmd == "SET_PAN") {
        size_t idx = 0;
        float pan = 0.0f;
        if (iss >> idx >> pan) {
            if (idx < engine.num_strips()) {
                engine.strip(idx).set_pan(pan);
                return "{\"status\":\"ok\",\"strip\":" + std::to_string(idx) + ",\"pan\":" + std::to_string(pan) + "}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid index or parameters\"}\n";
    }

    if (cmd == "mute" || cmd == "SET_MUTE") {
        size_t idx = 0;
        int m = 0;
        if (iss >> idx >> m) {
            if (idx < engine.num_strips()) {
                engine.strip(idx).set_mute(m != 0);
                return "{\"status\":\"ok\",\"strip\":" + std::to_string(idx) + ",\"mute\":" + (m ? "true" : "false") + "}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid parameters\"}\n";
    }

    if (cmd == "solo" || cmd == "SET_SOLO") {
        size_t idx = 0;
        int s = 0;
        if (iss >> idx >> s) {
            if (idx < engine.num_strips()) {
                engine.strip(idx).set_solo(s != 0);
                return "{\"status\":\"ok\",\"strip\":" + std::to_string(idx) + ",\"solo\":" + (s ? "true" : "false") + "}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid parameters\"}\n";
    }

    if (cmd == "route" || cmd == "SET_ROUTE") {
        size_t s_idx = 0, b_idx = 0;
        int enable = 0;
        float gain = 0.0f;
        if (iss >> s_idx >> b_idx >> enable) {
            if (iss >> gain) {}
            if (s_idx < engine.num_strips() && b_idx < engine.num_buses()) {
                engine.routing().set_route(s_idx, b_idx, enable != 0, gain);
                return "{\"status\":\"ok\",\"route\":{\"strip\":" + std::to_string(s_idx) + ",\"bus\":" + std::to_string(b_idx) + ",\"enabled\":" + (enable ? "true" : "false") + "}}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid route parameters\"}\n";
    }

    if (cmd == "bus_gain" || cmd == "SET_BUS_GAIN") {
        size_t idx = 0;
        float gain_db = 0.0f;
        if (iss >> idx >> gain_db) {
            if (idx < engine.num_buses()) {
                engine.bus(idx).set_gain_db(gain_db);
                return "{\"status\":\"ok\",\"bus\":" + std::to_string(idx) + ",\"gain\":" + std::to_string(gain_db) + "}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid bus index\"}\n";
    }

    if (cmd == "bus_mute" || cmd == "SET_BUS_MUTE") {
        size_t idx = 0;
        int m = 0;
        if (iss >> idx >> m) {
            if (idx < engine.num_buses()) {
                engine.bus(idx).set_mute(m != 0);
                return "{\"status\":\"ok\",\"bus\":" + std::to_string(idx) + ",\"mute\":" + (m ? "true" : "false") + "}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid bus mute parameters\"}\n";
    }

    if (cmd == "eq" || cmd == "SET_EQ") {
        size_t idx = 0;
        float low = 0.0f, mid = 0.0f, high = 0.0f;
        if (iss >> idx >> low >> mid >> high) {
            if (idx < engine.num_strips()) {
                engine.strip(idx).set_eq_low(low);
                engine.strip(idx).set_eq_mid(mid);
                engine.strip(idx).set_eq_high(high);
                engine.strip(idx).enable_eq(true);
                return "{\"status\":\"ok\",\"strip\":" + std::to_string(idx) + ",\"eq\":\"updated\"}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid eq parameters\"}\n";
    }

    if (cmd == "comp" || cmd == "SET_COMP") {
        size_t idx = 0;
        float amount = 0.0f;
        if (iss >> idx >> amount) {
            if (idx < engine.num_strips()) {
                if (amount <= 0.01f) {
                    engine.strip(idx).enable_compressor(false);
                } else {
                    float thresh = -10.0f - (amount * 3.0f);
                    float ratio = 1.5f + (amount * 0.8f);
                    engine.strip(idx).configure_compressor(thresh, ratio, 10.0f, 80.0f, 4.0f, amount * 0.8f);
                    engine.strip(idx).enable_compressor(true);
                }
                return "{\"status\":\"ok\",\"strip\":" + std::to_string(idx) + ",\"comp\":" + std::to_string(amount) + "}\n";
            }
        }
        return "{\"status\":\"error\",\"message\":\"invalid comp parameters\"}\n";
    }

    return "{\"status\":\"error\",\"message\":\"unknown command: " + cmd + "\"}\n";
}

NamedPipeServer::NamedPipeServer(matrix::MixerEngine& engine,
                                 core::EndpointManager* ep_mgr,
                                 const std::wstring& pipe_name)
    : engine_(engine), ep_mgr_(ep_mgr), pipe_name_(pipe_name) {
}

NamedPipeServer::~NamedPipeServer() {
    stop();
}

bool NamedPipeServer::start() {
    if (running_.load()) return true;

    stop_signal_.store(false);
    worker_thread_ = std::thread(&NamedPipeServer::server_worker_thread, this);
    running_.store(true);
    return true;
}

void NamedPipeServer::stop() {
    if (!running_.load()) return;

    stop_signal_.store(true);

    HANDLE dummy = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dummy != INVALID_HANDLE_VALUE) {
        CloseHandle(dummy);
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    running_.store(false);
}

void NamedPipeServer::server_worker_thread() {
    while (!stop_signal_.load()) {
        HANDLE hPipe = CreateNamedPipeW(
            pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, 50, nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (connected && !stop_signal_.load()) {
            char buffer[1024];
            DWORD bytesRead = 0;

            if (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                std::string req(buffer);

                while (!req.empty() && (req.back() == '\r' || req.back() == '\n' || req.back() == ' ')) {
                    req.pop_back();
                }

                std::string resp = CommandDispatcher::execute_command(engine_, ep_mgr_, req);
                DWORD bytesWritten = 0;
                WriteFile(hPipe, resp.c_str(), static_cast<DWORD>(resp.size()), &bytesWritten, nullptr);
                FlushFileBuffers(hPipe);
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

} // namespace metvoice::ipc
