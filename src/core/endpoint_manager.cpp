#include "metvoice/core/endpoint_manager.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>

namespace metvoice::core {

namespace {
    std::string wstring_to_utf8(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        std::string str(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &str[0], size_needed, nullptr, nullptr);
        return str;
    }
}

EndpointManager::EndpointManager(matrix::MixerEngine& engine)
    : engine_(engine) {
    capture_endpoints_.resize(engine_.num_strips());
    render_endpoints_.resize(engine_.num_buses());
    capture_names_.resize(engine_.num_strips(), "None");
    render_names_.resize(engine_.num_buses(), "None");
}

EndpointManager::~EndpointManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& ep : capture_endpoints_) {
        if (ep) ep->stop();
    }
    for (auto& ep : render_endpoints_) {
        if (ep) ep->stop();
    }
}

bool EndpointManager::attach_input(size_t strip_idx, const std::string& query) {
    if (strip_idx >= engine_.num_strips()) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // Stop existing endpoint on this strip
    if (capture_endpoints_[strip_idx]) {
        capture_endpoints_[strip_idx]->stop();
        capture_endpoints_[strip_idx].reset();
        capture_names_[strip_idx] = "None";
    }

    std::string clean_q = query;
    while (!clean_q.empty() && (clean_q.front() == ' ' || clean_q.front() == '\t')) clean_q.erase(0, 1);
    while (!clean_q.empty() && (clean_q.back() == ' ' || clean_q.back() == '\t' || clean_q.back() == '\r' || clean_q.back() == '\n')) clean_q.pop_back();
    if (clean_q.rfind("[WDM] ", 0) == 0) {
        clean_q = clean_q.substr(6);
        while (!clean_q.empty() && clean_q.front() == ' ') clean_q.erase(0, 1);
    }

    if (clean_q == "disconnect" || clean_q == "Select Device" || clean_q == "none" || clean_q.empty()) {
        return true;
    }

    auto devices = WasapiEndpoint::enumerate_devices(DeviceType::Capture);
    if (devices.empty()) return false;

    // Check if clean_q is a pure number index
    int found_idx = -1;
    bool is_pure_number = !clean_q.empty() && std::all_of(clean_q.begin(), clean_q.end(), ::isdigit);
    if (is_pure_number) {
        try {
            size_t parsed_idx = std::stoul(clean_q);
            if (parsed_idx < devices.size()) {
                found_idx = static_cast<int>(parsed_idx);
            }
        } catch (...) {}
    }

    // Otherwise search by friendly name (case-insensitive)
    if (found_idx == -1) {
        std::string clean_lower = clean_q;
        std::transform(clean_lower.begin(), clean_lower.end(), clean_lower.begin(), ::tolower);

        // 1. Exact match
        for (size_t i = 0; i < devices.size(); ++i) {
            std::string d_name = wstring_to_utf8(devices[i].friendly_name);
            std::string d_lower = d_name;
            std::transform(d_lower.begin(), d_lower.end(), d_lower.begin(), ::tolower);
            if (d_lower == clean_lower) {
                found_idx = static_cast<int>(i);
                break;
            }
        }

        // 2. Substring match
        if (found_idx == -1) {
            for (size_t i = 0; i < devices.size(); ++i) {
                std::string d_name = wstring_to_utf8(devices[i].friendly_name);
                std::string d_lower = d_name;
                std::transform(d_lower.begin(), d_lower.end(), d_lower.begin(), ::tolower);
                if (d_lower.find(clean_lower) != std::string::npos || clean_lower.find(d_lower) != std::string::npos) {
                    found_idx = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    if (found_idx < 0 || found_idx >= static_cast<int>(devices.size())) {
        std::cerr << "[EndpointManager] Could not find capture device matching: " << query << "\n";
        return false;
    }

    const auto& dev_info = devices[found_idx];
    AudioStreamConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.buffer_frames = 480;

    auto ep = std::make_unique<WasapiEndpoint>(dev_info, config, engine_.strip_input_ring_buffer(strip_idx));
    if (ep->start()) {
        capture_endpoints_[strip_idx] = std::move(ep);
        capture_names_[strip_idx] = wstring_to_utf8(dev_info.friendly_name);
        std::cout << "[EndpointManager] Strip " << strip_idx << " attached to capture device: "
                  << capture_names_[strip_idx] << "\n";
        return true;
    }
    return false;
}

bool EndpointManager::attach_output(size_t bus_idx, const std::string& query) {
    if (bus_idx >= engine_.num_buses()) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    if (render_endpoints_[bus_idx]) {
        render_endpoints_[bus_idx]->stop();
        render_endpoints_[bus_idx].reset();
        render_names_[bus_idx] = "None";
    }

    std::string clean_q = query;
    while (!clean_q.empty() && (clean_q.front() == ' ' || clean_q.front() == '\t')) clean_q.erase(0, 1);
    while (!clean_q.empty() && (clean_q.back() == ' ' || clean_q.back() == '\t' || clean_q.back() == '\r' || clean_q.back() == '\n')) clean_q.pop_back();
    if (clean_q.rfind("[WDM] ", 0) == 0) {
        clean_q = clean_q.substr(6);
        while (!clean_q.empty() && clean_q.front() == ' ') clean_q.erase(0, 1);
    }

    if (clean_q == "disconnect" || clean_q == "Select Device" || clean_q == "none" || clean_q.empty()) {
        return true;
    }

    auto devices = WasapiEndpoint::enumerate_devices(DeviceType::Render);
    if (devices.empty()) return false;

    int found_idx = -1;
    bool is_pure_number = !clean_q.empty() && std::all_of(clean_q.begin(), clean_q.end(), ::isdigit);
    if (is_pure_number) {
        try {
            size_t parsed_idx = std::stoul(clean_q);
            if (parsed_idx < devices.size()) {
                found_idx = static_cast<int>(parsed_idx);
            }
        } catch (...) {}
    }

    if (found_idx == -1) {
        std::string clean_lower = clean_q;
        std::transform(clean_lower.begin(), clean_lower.end(), clean_lower.begin(), ::tolower);

        // 1. Exact match
        for (size_t i = 0; i < devices.size(); ++i) {
            std::string d_name = wstring_to_utf8(devices[i].friendly_name);
            std::string d_lower = d_name;
            std::transform(d_lower.begin(), d_lower.end(), d_lower.begin(), ::tolower);
            if (d_lower == clean_lower) {
                found_idx = static_cast<int>(i);
                break;
            }
        }

        // 2. Substring match
        if (found_idx == -1) {
            for (size_t i = 0; i < devices.size(); ++i) {
                std::string d_name = wstring_to_utf8(devices[i].friendly_name);
                std::string d_lower = d_name;
                std::transform(d_lower.begin(), d_lower.end(), d_lower.begin(), ::tolower);
                if (d_lower.find(clean_lower) != std::string::npos || clean_lower.find(d_lower) != std::string::npos) {
                    found_idx = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    if (found_idx < 0 || found_idx >= static_cast<int>(devices.size())) {
        std::cerr << "[EndpointManager] Could not find render device matching: " << query << "\n";
        return false;
    }

    const auto& dev_info = devices[found_idx];
    AudioStreamConfig config;
    config.sample_rate = 48000;
    config.channels = 2;
    config.buffer_frames = 480;

    auto ep = std::make_unique<WasapiEndpoint>(dev_info, config, engine_.bus_output_ring_buffer(bus_idx));
    if (ep->start()) {
        render_endpoints_[bus_idx] = std::move(ep);
        render_names_[bus_idx] = wstring_to_utf8(dev_info.friendly_name);
        std::cout << "[EndpointManager] Bus " << bus_idx << " attached to render device: "
                  << render_names_[bus_idx] << "\n";
        return true;
    }
    return false;
}

void EndpointManager::detach_input(size_t strip_idx) {
    attach_input(strip_idx, "disconnect");
}

void EndpointManager::detach_output(size_t bus_idx) {
    attach_output(bus_idx, "disconnect");
}

std::string EndpointManager::get_input_name(size_t strip_idx) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (strip_idx < capture_names_.size()) return capture_names_[strip_idx];
    return "None";
}

std::string EndpointManager::get_output_name(size_t bus_idx) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bus_idx < render_names_.size()) return render_names_[bus_idx];
    return "None";
}

std::string EndpointManager::serialize_devices_json() const {
    auto captures = WasapiEndpoint::enumerate_devices(DeviceType::Capture);
    auto renders = WasapiEndpoint::enumerate_devices(DeviceType::Render);

    std::ostringstream ss;
    ss << "{\"type\":\"devices\"";

    ss << ",\"attached_inputs\":[";
    for (size_t i = 0; i < capture_names_.size(); ++i) {
        if (i > 0) ss << ",";
        std::string clean;
        for (char c : capture_names_[i]) { if (c == '\"' || c == '\\') clean += '\\'; clean += c; }
        ss << "\"" << clean << "\"";
    }
    ss << "]";

    ss << ",\"attached_outputs\":[";
    for (size_t j = 0; j < render_names_.size(); ++j) {
        if (j > 0) ss << ",";
        std::string clean;
        for (char c : render_names_[j]) { if (c == '\"' || c == '\\') clean += '\\'; clean += c; }
        ss << "\"" << clean << "\"";
    }
    ss << "]";

    ss << ",\"captures\":[";
    for (size_t i = 0; i < captures.size(); ++i) {
        if (i > 0) ss << ",";
        std::string name = wstring_to_utf8(captures[i].friendly_name);
        std::string clean;
        for (char c : name) { if (c == '\"' || c == '\\') clean += '\\'; clean += c; }
        ss << "{\"index\":" << i
           << ",\"name\":\"" << clean << "\""
           << ",\"default\":" << (captures[i].is_default ? "true" : "false")
           << ",\"rate\":" << captures[i].default_sample_rate
           << ",\"channels\":" << captures[i].channels
           << "}";
    }
    ss << "],\"renders\":[";
    for (size_t j = 0; j < renders.size(); ++j) {
        if (j > 0) ss << ",";
        std::string name = wstring_to_utf8(renders[j].friendly_name);
        std::string clean;
        for (char c : name) { if (c == '\"' || c == '\\') clean += '\\'; clean += c; }
        ss << "{\"index\":" << j
           << ",\"name\":\"" << clean << "\""
           << ",\"default\":" << (renders[j].is_default ? "true" : "false")
           << ",\"rate\":" << renders[j].default_sample_rate
           << ",\"channels\":" << renders[j].channels
           << "}";
    }
    ss << "]}";
    return ss.str();
}

} // namespace metvoice::core
