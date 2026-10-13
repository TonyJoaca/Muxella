#include "metvoice/ipc/named_pipe_server.hpp"
#include "metvoice/ipc/protocol.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>

namespace metvoice::ipc {

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

std::string CommandDispatcher::execute_command(matrix::MixerEngine& engine, const std::string& request) {
    // Simple command line / JSON parser
    std::istringstream iss(request);
    std::string cmd;
    iss >> cmd;

    if (cmd == "meters" || cmd == "GET_METERS") {
        return serialize_meters_json(engine) + "\n";
    }
    if (cmd == "status" || cmd == "GET_STATUS") {
        return serialize_status_json(engine) + "\n";
    }

    if (cmd == "gain" || cmd == "SET_GAIN") {
        // Syntax: gain <strip_idx> <gain_db>
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
        // Syntax: pan <strip_idx> <pan_val> (-1.0 to 1.0)
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
        // Syntax: mute <strip_idx> <0|1>
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
        // Syntax: solo <strip_idx> <0|1>
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
        // Syntax: route <strip_idx> <bus_idx> <0|1> [gain_db]
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
        // Syntax: bus_gain <bus_idx> <gain_db>
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
        // Syntax: bus_mute <bus_idx> <0|1>
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
        // Syntax: eq <strip_idx> <low_db> <mid_db> <high_db>
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
        // Syntax: comp <strip_idx> <amount 0..10>
        size_t idx = 0;
        float amount = 0.0f;
        if (iss >> idx >> amount) {
            if (idx < engine.num_strips()) {
                if (amount <= 0.01f) {
                    engine.strip(idx).enable_compressor(false);
                } else {
                    float thresh = -10.0f - (amount * 3.0f); // -13dB to -40dB
                    float ratio = 1.5f + (amount * 0.8f);   // 2.3:1 to 9.5:1
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

NamedPipeServer::NamedPipeServer(matrix::MixerEngine& engine, const std::wstring& pipe_name)
    : engine_(engine), pipe_name_(pipe_name) {
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

    // Create a dummy client connection to unblock any ConnectNamedPipe
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

                // Trim trailing carriage return/newlines
                while (!req.empty() && (req.back() == '\r' || req.back() == '\n' || req.back() == ' ')) {
                    req.pop_back();
                }

                std::string resp = CommandDispatcher::execute_command(engine_, req);
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
