#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace metvoice::core {

struct AudioProcessEntry {
    uint32_t pid{0};
    std::string name;
};

std::string app_sessions_json();
std::string set_app_session(const std::string& id, float volume, bool mute);

/// Returns deduplicated active audio playback processes for process-loopback routing.
std::vector<AudioProcessEntry> enumerate_audio_processes();

/// Resolves a target process executable name (e.g. "Discord.exe") to a running PID (0 if absent).
uint32_t find_process_pid_by_name(const std::string& exe_name);

} // namespace metvoice::core
