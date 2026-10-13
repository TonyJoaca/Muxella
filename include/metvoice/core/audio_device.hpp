#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace metvoice::core {

enum class DeviceType {
    Capture,
    Render,
    LoopbackRender // Captures output of a render device via WASAPI Loopback
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
};

struct AudioStreamConfig {
    uint32_t sample_rate{48000};
    uint32_t channels{2};
    uint32_t buffer_frames{480}; // ~10 ms default at 48kHz
    ShareMode share_mode{ShareMode::Shared};
};

} // namespace metvoice::core
