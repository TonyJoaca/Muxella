#include "metvoice/usbip/protocol.hpp"
#include <algorithm>
#include <cstring>

namespace metvoice::usbip {

OpHeader decode_op_header(const uint8_t* wire8) noexcept {
    OpHeader h;
    h.version = read_be16(wire8);
    h.code = read_be16(wire8 + 2);
    h.status = read_be32(wire8 + 4);
    return h;
}

void encode_op_header(std::vector<uint8_t>& dst, uint16_t code, uint32_t status) {
    const size_t base = dst.size();
    dst.resize(base + 8);
    write_be16(dst.data() + base, kProtocolVersion);
    write_be16(dst.data() + base + 2, code);
    write_be32(dst.data() + base + 4, status);
}

BasicHeader decode_basic_header(const uint8_t* wire20) noexcept {
    BasicHeader h;
    h.command = read_be32(wire20);
    h.sequence = read_be32(wire20 + 4);
    h.device_id = read_be32(wire20 + 8);
    h.direction = read_be32(wire20 + 12);
    h.endpoint = read_be32(wire20 + 16);
    return h;
}

SubmitRequest decode_submit_body(const BasicHeader& basic, const uint8_t* wire28) noexcept {
    SubmitRequest req;
    req.basic = basic;
    req.transfer_flags = read_be32(wire28);
    req.transfer_buffer_length = read_be32(wire28 + 4);
    req.start_frame = read_be32(wire28 + 8);
    req.number_of_packets = read_be32(wire28 + 12);
    req.interval = read_be32(wire28 + 16);
    std::memcpy(req.setup.data(), wire28 + 20, 8);
    return req;
}

void decode_iso_packets(const uint8_t* raw, uint32_t count, std::vector<IsoPacket>& out) {
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = raw + static_cast<size_t>(i) * 16;
        out[i].offset = read_be32(p);
        out[i].length = read_be32(p + 4);
        out[i].actual_length = read_be32(p + 8);
        out[i].status = static_cast<int32_t>(read_be32(p + 12));
    }
}

void encode_ret_submit(std::vector<uint8_t>& frame, const SubmitRequest& req, int32_t status,
                       uint32_t actual_length, const uint8_t* data, size_t data_len,
                       const IsoPacket* packets, size_t packet_count, uint32_t error_count) {
    if (status != kStatusOK) {
        actual_length = 0;
        data = nullptr;
        data_len = 0;
    }

    uint32_t number_of_packets = kNoIsoPackets;
    uint32_t start_frame = 0;
    if (req.is_isochronous()) {
        number_of_packets = static_cast<uint32_t>(packet_count);
        start_frame = req.start_frame;
    }

    const size_t total_size = 48 + data_len + packet_count * 16;
    frame.resize(total_size);
    uint8_t* dst = frame.data();
    std::memset(dst, 0, 48);

    write_be32(dst, kRetSubmit);
    write_be32(dst + 4, req.basic.sequence);
    write_be32(dst + 20, static_cast<uint32_t>(status));
    write_be32(dst + 24, actual_length);
    write_be32(dst + 28, start_frame);
    write_be32(dst + 32, number_of_packets);
    write_be32(dst + 36, error_count);

    dst += 48;
    if (data_len > 0 && data) {
        std::memcpy(dst, data, data_len);
        dst += data_len;
    }
    for (size_t i = 0; i < packet_count; ++i) {
        write_be32(dst, packets[i].offset);
        write_be32(dst + 4, packets[i].length);
        write_be32(dst + 8, packets[i].actual_length);
        write_be32(dst + 12, static_cast<uint32_t>(packets[i].status));
        dst += 16;
    }
}

void encode_ret_unlink(std::vector<uint8_t>& frame, const BasicHeader& req, int32_t status) {
    frame.assign(48, 0);
    write_be32(frame.data(), kRetUnlink);
    write_be32(frame.data() + 4, req.sequence);
    write_be32(frame.data() + 20, static_cast<uint32_t>(status));
}

void mark_iso_packets(std::vector<IsoPacket>& packets, int actual_total) noexcept {
    int remaining = std::max(0, actual_total);
    for (auto& pkt : packets) {
        pkt.status = kStatusOK;
        const int wanted = std::min(static_cast<int>(pkt.length), remaining);
        pkt.actual_length = static_cast<uint32_t>(std::max(0, wanted));
        remaining -= wanted;
    }
}

void mark_iso_error(std::vector<IsoPacket>& packets, int32_t status) noexcept {
    for (auto& pkt : packets) {
        pkt.actual_length = 0;
        pkt.status = status;
    }
}

} // namespace metvoice::usbip
