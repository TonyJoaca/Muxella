#pragma once
#include <string>
namespace metvoice::core {
std::string app_sessions_json();
std::string set_app_session(const std::string& id, float volume, bool mute);
}
