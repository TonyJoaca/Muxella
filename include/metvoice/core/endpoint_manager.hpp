#pragma once

#include "metvoice/core/wasapi_backend.hpp"
#include "metvoice/matrix/engine.hpp"
#include <vector>
#include <memory>
#include <string>
#include <mutex>

namespace metvoice::core {

class EndpointManager {
public:
    EndpointManager(matrix::MixerEngine& engine);
    ~EndpointManager();

    bool attach_input(size_t strip_idx, const std::string& device_query);
    bool attach_output(size_t bus_idx, const std::string& device_query);

    void detach_input(size_t strip_idx);
    void detach_output(size_t bus_idx);

    [[nodiscard]] std::string get_input_name(size_t strip_idx) const;
    [[nodiscard]] std::string get_output_name(size_t bus_idx) const;

    [[nodiscard]] std::string serialize_devices_json() const;

private:
    matrix::MixerEngine& engine_;
    mutable std::mutex mutex_;

    std::vector<std::unique_ptr<WasapiEndpoint>> capture_endpoints_;
    std::vector<std::unique_ptr<WasapiEndpoint>> render_endpoints_;
    std::vector<std::string> capture_names_;
    std::vector<std::string> render_names_;
};

} // namespace metvoice::core
