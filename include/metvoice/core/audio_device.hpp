#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace metvoice::core {

enum class DeviceType {
    Capture,
    Render,
    LoopbackRender, // Captures output of a render device via WASAPI Loopback
    ProcessLoopback // Captures output of a target process tree via Windows 10/11 Process Loopback
};

enum class ShareMode {
    Shared,
    Exclusive
};

struct DeviceInfo {
    std::wstring id;
    std::wstring friendly_name;
    DeviceType type;
    uint32_t default_sample_rate{48000};
    uint32_t channels{2};
    bool is_default{false};
    uint32_t target_pid{0};
    bool include_process_tree{true};
};

struct AudioStreamConfig {
    uint32_t sample_rate{48000};
    uint32_t channels{2};
    uint32_t buffer_frames{480}; // ~10 ms default at 48kHz
    ShareMode share_mode{ShareMode::Shared};
};

} // namespace metvoice::core
