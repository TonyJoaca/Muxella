#pragma once

#include "metvoice/usbip/byte_ring.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace metvoice::usbip {

inline constexpr uint32_t kUacSampleRate = 48000;
inline constexpr uint32_t kUacChannels = 2;

inline constexpr uint8_t kDescDevice = 0x01;
inline constexpr uint8_t kDescConfiguration = 0x02;
inline constexpr uint8_t kDescString = 0x03;
inline constexpr uint8_t kDescInterface = 0x04;
inline constexpr uint8_t kDescEndpoint = 0x05;
inline constexpr uint8_t kDescCsInterface = 0x24;
inline constexpr uint8_t kDescCsEndpoint = 0x25;

inline constexpr uint8_t kReqGetStatus = 0x00;
inline constexpr uint8_t kReqClearFeature = 0x01;
inline constexpr uint8_t kReqSetFeature = 0x03;
inline constexpr uint8_t kReqSetAddress = 0x05;
inline constexpr uint8_t kReqGetDescriptor = 0x06;
inline constexpr uint8_t kReqSetDescriptor = 0x07;
inline constexpr uint8_t kReqGetConfiguration = 0x08;
inline constexpr uint8_t kReqSetConfiguration = 0x09;
inline constexpr uint8_t kReqGetInterface = 0x0A;
inline constexpr uint8_t kReqSetInterface = 0x0B;
inline constexpr uint8_t kReqSynchFrame = 0x0C;

inline constexpr uint8_t kAudioSetCur = 0x01;
inline constexpr uint8_t kAudioGetCur = 0x81;
inline constexpr uint8_t kAudioGetMin = 0x82;
inline constexpr uint8_t kAudioGetMax = 0x83;
inline constexpr uint8_t kAudioGetRes = 0x84;

struct SetupPacket {
    uint8_t request_type{0};
    uint8_t request{0};
    uint16_t value{0};
    uint16_t index{0};
    uint16_t length{0};
};

bool parse_setup(const uint8_t* data, size_t len, SetupPacket& out) noexcept;

struct Uac1Descriptors {
    int cable_number{1};
    int pcm_bits{24};
    int pcm_bytes{3};
    std::string product;
    std::string serial;
    std::vector<uint8_t> device;
    std::vector<uint8_t> config;
    std::array<std::vector<uint8_t>, 4> strings;

    static bool create(int cable_number, int pcm_bits, Uac1Descriptors& out, std::string* err = nullptr);
    bool get_descriptor(uint8_t type, uint8_t index, std::vector<uint8_t>& out) const;
    bool validate(std::string* err = nullptr) const;
};

class Uac1Device {
public:
    Uac1Device(int number, int latency_ms, int pcm_bits);
    static std::unique_ptr<Uac1Device> create(int number, int latency_ms, int pcm_bits, std::string* err = nullptr);

    [[nodiscard]] int number() const noexcept { return number_; }
    [[nodiscard]] const std::string& bus_id() const noexcept { return bus_id_; }
    [[nodiscard]] const std::string& product() const noexcept { return descriptors_.product; }
    [[nodiscard]] int pcm_bits() const noexcept { return descriptors_.pcm_bits; }
    [[nodiscard]] int pcm_bytes() const noexcept { return descriptors_.pcm_bytes; }
    [[nodiscard]] const Uac1Descriptors& descriptors() const noexcept { return descriptors_; }

    int32_t handle_control(const SetupPacket& setup, const uint8_t* out_data, size_t out_len,
                           std::vector<uint8_t>& response_data);

    [[nodiscard]] bool playback_active() const noexcept;
    [[nodiscard]] bool capture_active() const noexcept;

    size_t write_playback(uint8_t* data, size_t len);
    size_t read_capture(uint8_t* dst, size_t len);

private:
    int number_{1};
    std::string bus_id_;
    Uac1Descriptors descriptors_;
    ByteRing buffer_;

    mutable std::mutex mu_;
    uint8_t configuration_{0};
    std::array<uint8_t, 3> alt_setting_{0, 0, 0};
    uint32_t sample_rate_{kUacSampleRate};
    std::array<bool, 2> mute_{false, false};
    std::array<int16_t, 2> volume_{0, 0};
};

} // namespace metvoice::usbip
