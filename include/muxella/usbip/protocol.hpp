#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace muxella::usbip {

inline constexpr uint16_t kProtocolVersion = 0x0111;

inline constexpr uint16_t kOpReqImport = 0x8003;
inline constexpr uint16_t kOpRepImport = 0x0003;
inline constexpr uint16_t kOpReqDevlist = 0x8005;
inline constexpr uint16_t kOpRepDevlist = 0x0005;

inline constexpr uint32_t kCmdSubmit = 0x00000001;
inline constexpr uint32_t kCmdUnlink = 0x00000002;
inline constexpr uint32_t kRetSubmit = 0x00000003;
inline constexpr uint32_t kRetUnlink = 0x00000004;

inline constexpr uint32_t kDirectionOut = 0;
inline constexpr uint32_t kDirectionIn = 1;
inline constexpr uint32_t kSpeedFull = 2;

inline constexpr uint32_t kNoIsoPackets = 0xFFFFFFFFu;
inline constexpr uint32_t kMaxTransferLength = 16u * 1024u * 1024u;

inline constexpr int32_t kStatusOK = 0;
inline constexpr int32_t kStatusNoEntry = -2;
inline constexpr int32_t kStatusInvalid = -22;
inline constexpr int32_t kStatusPipe = -32;
inline constexpr int32_t kStatusConnReset = -104;

struct OpHeader {
    uint16_t version{kProtocolVersion};
    uint16_t code{0};
    uint32_t status{0};
};

struct BasicHeader {
    uint32_t command{0};
    uint32_t sequence{0};
    uint32_t device_id{0};
    uint32_t direction{0};
    uint32_t endpoint{0};
};

struct SubmitRequest {
    BasicHeader basic;
    uint32_t transfer_flags{0};
    uint32_t transfer_buffer_length{0};
    uint32_t start_frame{0};
    uint32_t number_of_packets{0};
    uint32_t interval{0};
    std::array<uint8_t, 8> setup{};

    [[nodiscard]] bool is_isochronous() const noexcept {
        return number_of_packets != kNoIsoPackets && number_of_packets != 0;
    }
};

struct IsoPacket {
    uint32_t offset{0};
    uint32_t length{0};
    uint32_t actual_length{0};
    int32_t status{0};
};

inline uint16_t read_be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t read_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint16_t read_le16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline void write_be16(uint8_t* dst, uint16_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v >> 8);
    dst[1] = static_cast<uint8_t>(v & 0xFF);
}

inline void write_be32(uint8_t* dst, uint32_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v >> 24);
    dst[1] = static_cast<uint8_t>(v >> 16);
    dst[2] = static_cast<uint8_t>(v >> 8);
    dst[3] = static_cast<uint8_t>(v & 0xFF);
}

OpHeader decode_op_header(const uint8_t* wire8) noexcept;
void encode_op_header(std::vector<uint8_t>& dst, uint16_t code, uint32_t status);

BasicHeader decode_basic_header(const uint8_t* wire20) noexcept;
SubmitRequest decode_submit_body(const BasicHeader& basic, const uint8_t* wire28) noexcept;
void decode_iso_packets(const uint8_t* raw, uint32_t count, std::vector<IsoPacket>& out);

void encode_ret_submit(std::vector<uint8_t>& frame, const SubmitRequest& req, int32_t status,
                       uint32_t actual_length, const uint8_t* data, size_t data_len,
                       const IsoPacket* packets, size_t packet_count, uint32_t error_count);

void encode_ret_unlink(std::vector<uint8_t>& frame, const BasicHeader& req, int32_t status);

void mark_iso_packets(std::vector<IsoPacket>& packets, int actual_total) noexcept;
void mark_iso_error(std::vector<IsoPacket>& packets, int32_t status) noexcept;

} // namespace muxella::usbip
