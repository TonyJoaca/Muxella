#include "muxella/core/wav_recorder.hpp"
#include "muxella/core/ring_buffer.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <thread>
#include <mutex>
#include <array>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <limits>

namespace muxella::core {
namespace {
using Json = nlohmann::json;
std::wstring widen(const std::string& value) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid recording folder");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::string utf8(const std::wstring& value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
void write_all(HANDLE file, const void* source, size_t size) {
    auto data = static_cast<const char*>(source);
    while (size) {
        DWORD written = 0;
        if (!WriteFile(file, data, static_cast<DWORD>(std::min<size_t>(size, 1 << 20)), &written, nullptr) || !written)
            throw std::runtime_error("Recording write failed (Windows error " + std::to_string(GetLastError()) + ")");
        data += written; size -= written;
    }
}
constexpr size_t WAV_HEADER_SIZE = 94;
std::array<char, WAV_HEADER_SIZE> header(uint32_t rate, uint64_t frames) {
    std::array<char, WAV_HEADER_SIZE> h{};
    auto text = [&](size_t offset, const char* value) { std::memcpy(h.data() + offset, value, 4); };
    auto u16 = [&](size_t offset, uint16_t value) { std::memcpy(h.data() + offset, &value, 2); };
    auto u32 = [&](size_t offset, uint32_t value) { std::memcpy(h.data() + offset, &value, 4); };
    auto u64 = [&](size_t offset, uint64_t value) { std::memcpy(h.data() + offset, &value, 8); };
    const uint64_t bytes = frames * 8;
    const bool rf64 = bytes + WAV_HEADER_SIZE - 8 > std::numeric_limits<uint32_t>::max();
    text(0, rf64 ? "RF64" : "RIFF"); u32(4, rf64 ? 0xffffffff : static_cast<uint32_t>(bytes + WAV_HEADER_SIZE - 8)); text(8, "WAVE");
    text(12, rf64 ? "ds64" : "JUNK"); u32(16, 28);
    if (rf64) { u64(20, bytes + WAV_HEADER_SIZE - 8); u64(28, bytes); u64(36, frames); u32(44, 0); }
    text(48, "fmt "); u32(52, 18); u16(56, 3); u16(58, 2); // IEEE float stereo, WAVEFORMATEX
    u32(60, rate); u32(64, rate * 8); u16(68, 8); u16(70, 32);
    u16(72, 0); // cbSize: no format extension
    text(74, "fact"); u32(78, 4); u32(82, static_cast<uint32_t>(std::min<uint64_t>(frames, 0xffffffff)));
    text(86, "data"); u32(90, rf64 ? 0xffffffff : static_cast<uint32_t>(bytes));
    return h;
}
struct File {
    HANDLE handle{INVALID_HANDLE_VALUE};
    std::string path;
    uint64_t frames{0};
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
struct Block { uint64_t offset{}; uint32_t frames{}; };
}
struct WavRecorder::Session {
    std::atomic<uint64_t>* bus_mask;
    uint32_t rate;
    size_t block_frames, channels;
    std::vector<RecordingSource> sources;
    std::vector<std::unique_ptr<File>> files;
    SpscRingBuffer<float> samples;
    SpscRingBuffer<Block> blocks{256};
    std::vector<float> producer, consumer, stereo, silence;
    std::atomic<bool> stopping{false}, failed{false};
    std::atomic<uint64_t> total_frames{0}, dropped_frames{0}, written_frames{0};
    uint64_t producer_frames{0};
    std::thread worker;
    mutable std::mutex error_mutex;
    std::string error;

    Session(uint32_t sample_rate, size_t max_frames, const std::vector<RecordingSource>& selected, std::atomic<uint64_t>* mask)
        : bus_mask(mask), rate(sample_rate), block_frames(max_frames), channels(selected.size() * 2), sources(selected),
          samples(static_cast<size_t>(sample_rate) * channels * 4),
          producer(max_frames * channels), consumer(max_frames * channels), stereo(max_frames * 2),
          silence(max_frames * 2, 0.0f) {}
    void fail(const std::string& message) {
        { std::lock_guard<std::mutex> lock(error_mutex); if (error.empty()) error = message; }
        failed.store(true);
        bus_mask->store(0);
    }
    void zeros(uint64_t frames) {
        while (frames) {
            const size_t count = std::min<uint64_t>(frames, block_frames);
            for (auto& file : files) {
                write_all(file->handle, silence.data(), count * 8);
                file->frames += count;
            }
            written_frames.fetch_add(count);
            frames -= count;
        }
    }
    void run() noexcept {
        try {
            for (;;) {
                Block block;
                if (!blocks.read(&block, 1)) {
                    if (stopping.load()) {
                        // Producer is detached before stopping is set. Recheck after acquire.
                        if (blocks.available_read()) continue;
                        const auto end = total_frames.load();
                        if (end > written_frames.load()) zeros(end - written_frames.load());
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                const uint64_t position = written_frames.load();
                if (block.offset > position) zeros(block.offset - position);
                const size_t count = static_cast<size_t>(block.frames) * channels;
                if (samples.read(consumer.data(), count) != count) throw std::runtime_error("Recording buffer lost alignment");
                for (size_t track = 0; track < files.size(); ++track) {
                    for (size_t f = 0; f < block.frames; ++f) {
                        stereo[f * 2] = consumer[f * channels + track * 2];
                        stereo[f * 2 + 1] = consumer[f * channels + track * 2 + 1];
                    }
                    write_all(files[track]->handle, stereo.data(), block.frames * 8);
                    files[track]->frames += block.frames;
                }
                written_frames.store(block.offset + block.frames);
            }
        } catch (const std::exception& e) { fail(e.what()); }
        // Finalize each file independently, including on disk errors; header describes
        // only complete frames actually written to that file.
        for (auto& file : files) {
            try {
                LARGE_INTEGER offset{};
                if (!SetFilePointerEx(file->handle, offset, nullptr, FILE_BEGIN)) throw std::runtime_error("Cannot finalize WAV header");
                const auto h = header(rate, file->frames);
                write_all(file->handle, h.data(), h.size());
                offset.QuadPart = WAV_HEADER_SIZE + static_cast<LONGLONG>(file->frames * 8);
                if (!SetFilePointerEx(file->handle, offset, nullptr, FILE_BEGIN) || !SetEndOfFile(file->handle))
                    throw std::runtime_error("Cannot finalize WAV length");
                if (!FlushFileBuffers(file->handle)) throw std::runtime_error("Cannot flush recorded WAV");
            } catch (const std::exception& e) { fail(e.what()); }
            CloseHandle(file->handle); file->handle = INVALID_HANDLE_VALUE;
        }
    }
};
WavRecorder::WavRecorder() = default;
WavRecorder::~WavRecorder() { stop(); }
bool WavRecorder::start(const std::string& folder, const std::vector<RecordingSource>& sources, uint32_t rate, size_t frames) {
    if (active_.load()) { error_ = "A recording is already active"; return false; }
    stop(); error_.clear();
    try {
        if (sources.empty() || sources.size() > 16 || !rate || !frames) throw std::runtime_error("Select at least one recording source");
        auto session = std::make_unique<Session>(rate, frames, sources, &bus_sources_);
        const auto directory = std::filesystem::path(widen(folder));
        std::filesystem::create_directories(directory);
        for (const auto& source : sources) {
            const std::string label = !source.bus ? "Input_" + std::to_string(source.index + 1) :
                source.index < 5 ? "A" + std::to_string(source.index + 1) : "B" + std::to_string(source.index - 4);
            auto file = std::make_unique<File>();
            const auto path = directory / (label + ".wav");
            file->path = utf8(path.wstring());
            file->handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file->handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create " + file->path + " (Windows error " + std::to_string(GetLastError()) + ")");
            const auto h = header(rate, 0); write_all(file->handle, h.data(), h.size());
            session->files.push_back(std::move(file));
        }
        session_ = std::move(session);
        session_->worker = std::thread([s = session_.get()] { s->run(); });
        uint64_t mask = 0;
        for (const auto& source : sources) if (source.bus && source.index < 64) mask |= uint64_t{1} << source.index;
        bus_sources_.store(mask);
        active_.store(session_.get());
        return true;
    } catch (const std::exception& e) { error_ = e.what(); session_.reset(); return false; }
}
void WavRecorder::stop() {
    active_.exchange(nullptr);
    bus_sources_.store(0);
    // Sequentially consistent reader guard keeps Session alive through any in-flight block.
    while (readers_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (session_) {
        session_->stopping.store(true);
        if (session_->worker.joinable()) session_->worker.join();
        last_status_ = status_json();
        session_.reset(); // Release potentially large recording buffers off the audio thread.
    }
}
void WavRecorder::capture(const std::vector<AudioBuffer>& strips, const std::vector<AudioBuffer>& buses, size_t frames) noexcept {
    readers_.fetch_add(1);
    auto s = active_.load();
    if (s && !s->failed.load() && frames <= s->block_frames) {
        const size_t count = frames * s->channels;
        if (s->samples.available_write() >= count && s->blocks.available_write()) {
            for (size_t track = 0; track < s->sources.size(); ++track) {
                const auto source = s->sources[track];
                const auto& buffer = source.bus ? buses[source.index] : strips[source.index];
                for (size_t f = 0; f < frames; ++f) {
                    s->producer[f * s->channels + track * 2] = buffer.channel_data(0)[f];
                    s->producer[f * s->channels + track * 2 + 1] = buffer.channel_data(1)[f];
                }
            }
            s->samples.write(s->producer.data(), count);
            const Block block{s->producer_frames, static_cast<uint32_t>(frames)};
            s->blocks.write(&block, 1);
        } else s->dropped_frames.fetch_add(frames);
        s->producer_frames += frames;
        s->total_frames.store(s->producer_frames);
    }
    readers_.fetch_sub(1);
}
std::string WavRecorder::status_json() const {
    Json data = {{"type", "recording"}, {"active", false}, {"seconds", 0}, {"dropped_frames", 0},
        {"files", Json::array()}, {"error", error_}};
    if (!session_ && !last_status_.empty()) {
        data = Json::parse(last_status_);
        if (!error_.empty()) data["error"] = error_;
    }
    if (session_) {
        data["active"] = active_.load() != nullptr && !session_->failed.load();
        data["seconds"] = static_cast<double>(session_->total_frames.load()) / session_->rate;
        data["dropped_frames"] = session_->dropped_frames.load();
        for (const auto& file : session_->files) data["files"].push_back(file->path);
        std::lock_guard<std::mutex> lock(session_->error_mutex);
        if (!session_->error.empty()) data["error"] = session_->error;
    }
    return data.dump(-1, ' ', false, Json::error_handler_t::replace);
}
}
