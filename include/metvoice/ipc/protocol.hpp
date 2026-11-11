#pragma once

#include "metvoice/matrix/engine.hpp"
#include <string>
#include <sstream>
#include <iomanip>

#include <functional>

namespace metvoice::core {
    class EndpointManager;
}

namespace metvoice::ipc {

/**
 * @brief Dispatcher for incoming JSON / Text commands to control the MixerEngine.
 */
class CommandDispatcher {
public:
    static std::string execute_command(matrix::MixerEngine& engine,
                                       core::EndpointManager* ep_mgr,
                                       const std::string& request,
                                       std::function<void()>* on_shutdown = nullptr);

    static std::string serialize_meters_json(const matrix::MixerEngine& engine);
    static std::string serialize_status_json(const matrix::MixerEngine& engine);
    static std::string serialize_devices_json(core::EndpointManager* ep_mgr);
};

} // namespace metvoice::ipc
