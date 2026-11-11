#pragma once
#include "metvoice/core/audio_buffer.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace metvoice::core {
struct RecordingSource { bool bus; size_t index; };
class WavRecorder {
public:
    WavRecorder();
    ~WavRecorder();
    WavRecorder(const WavRecorder&) = delete;
    WavRecorder& operator=(const WavRecorder&) = delete;
    bool start(const std::string& folder, const std::vector<RecordingSource>& sources,
               uint32_t rate, size_t block_frames);
    void stop();
    std::string status_json() const;
    bool records_bus(size_t index) const noexcept { return index < 64 && (bus_sources_.load(std::memory_order_relaxed) & (uint64_t{1} << index)); }
    void capture(const std::vector<AudioBuffer>& strips, const std::vector<AudioBuffer>& buses, size_t frames) noexcept;
private:
    struct Session;
    std::unique_ptr<Session> session_; // Only the serialized IPC thread manages lifetime.
    std::atomic<Session*> active_{nullptr};
    std::atomic<unsigned> readers_{0};
    std::string error_;
    std::string last_status_;
    std::atomic<uint64_t> bus_sources_{0};
};
}
