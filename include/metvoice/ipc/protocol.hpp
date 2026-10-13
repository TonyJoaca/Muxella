#pragma once

#include "metvoice/matrix/engine.hpp"
#include <string>
#include <sstream>
#include <iomanip>

namespace metvoice::ipc {

/**
 * @brief Dispatcher for incoming JSON / Text commands to control the MixerEngine.
 */
class CommandDispatcher {
public:
    static std::string execute_command(matrix::MixerEngine& engine, const std::string& request);

    static std::string serialize_meters_json(const matrix::MixerEngine& engine);
    static std::string serialize_status_json(const matrix::MixerEngine& engine);
    static std::string serialize_devices_json();
};

} // namespace metvoice::ipc
