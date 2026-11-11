#include "metvoice/usbip/byte_ring.hpp"
#include "metvoice/usbip/protocol.hpp"
#include "metvoice/usbip/server.hpp"
#include "metvoice/usbip/uac1_device.hpp"
#include "metvoice/usbip/win_rt_socket.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#define CHECK(expr)                                                                                 \
    do {                                                                                            \
        if (!(expr)) {                                                                              \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
            std::abort();                                                                           \
        }                                                                                           \
    } while (0)

using namespace metvoice::usbip;

namespace {

bool tcp_send_all(SOCKET s, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = send(s, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool tcp_recv_all(SOCKET s, uint8_t* dst, size_t len) {
    size_t got = 0;
    while (got < len) {
        const int n = recv(s, reinterpret_cast<char*>(dst + got), static_cast<int>(len - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

std::vector<uint8_t> build_submit(uint32_t seq, uint32_t dir, uint32_t ep, uint32_t len,
                                  uint32_t num_packets, const uint8_t setup[8],
                                  const uint8_t* payload, const std::vector<IsoPacket>& pkts) {
    std::vector<uint8_t> buf(48, 0);
    write_be32(buf.data(), kCmdSubmit);
    write_be32(buf.data() + 4, seq);
    write_be32(buf.data() + 12, dir);
    write_be32(buf.data() + 16, ep);
    write_be32(buf.data() + 24, len);
    write_be32(buf.data() + 32, num_packets);
    if (setup) std::memcpy(buf.data() + 40, setup, 8);
    if (dir == kDirectionOut && payload && len > 0) {
        buf.insert(buf.end(), payload, payload + len);
    }
    for (const auto& p : pkts) {
        const size_t base = buf.size();
        buf.resize(base + 16);
        write_be32(buf.data() + base, p.offset);
        write_be32(buf.data() + base + 4, p.length);
        write_be32(buf.data() + base + 8, 0);
        write_be32(buf.data() + base + 12, 0);
    }
    return buf;
}

void test_byte_ring() {
    std::cout << "[Test] USB/IP ByteRing overflow & silence padding... ";
    ByteRing ring(8);
    const uint8_t d1[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t d2[4] = {7, 8, 9, 10};
    CHECK(ring.write(d1, 6) == 6);
    CHECK(ring.write(d2, 4) == 4); // overflows oldest 2 bytes (1, 2)
    CHECK(ring.available() == 8);

    uint8_t out[10]{};
    const size_t n = ring.read_silence(out, 10);
    CHECK(n == 8);
    CHECK(out[0] == 3 && out[7] == 10 && out[8] == 0 && out[9] == 0);
    std::cout << "PASSED\n";
}

void test_uac1_descriptors_and_loopback() {
    std::cout << "[Test] USB/IP UAC1 descriptors & PCM loopback (16/24/32-bit)... ";
    for (int bits : {16, 24, 32}) {
        auto dev = Uac1Device::create(2, 100, bits);
        CHECK(dev != nullptr);
        CHECK(dev->product() == "Muxella Spotify");
        CHECK(dev->descriptors().validate());

        std::vector<uint8_t> resp;
        CHECK(dev->handle_control({0x00, kReqSetConfiguration, 1, 0, 0}, nullptr, 0, resp) == 0);
        CHECK(dev->handle_control({0x01, kReqSetInterface, 1, 1, 0}, nullptr, 0, resp) == 0);
        CHECK(dev->handle_control({0x01, kReqSetInterface, 1, 2, 0}, nullptr, 0, resp) == 0);
        CHECK(dev->playback_active() && dev->capture_active());

        const size_t frame_bytes = static_cast<size_t>(kUacChannels) * static_cast<size_t>(bits / 8);
        std::vector<uint8_t> pcm(frame_bytes * 4);
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>(0x31 + i);
        std::vector<uint8_t> copy_in = pcm;
        CHECK(dev->write_playback(copy_in.data(), copy_in.size()) == pcm.size());

        std::vector<uint8_t> cap(pcm.size(), 0);
        CHECK(dev->read_capture(cap.data(), cap.size()) == pcm.size());
        CHECK(cap == pcm);
    }
    std::cout << "PASSED\n";
}

void test_iso_scheduler_cadence() {
    std::cout << "[Test] USB/IP IsoScheduler jitter absorption & reset... ";
    IsoScheduler sched;
    const auto t0 = Clock::now();
    const auto d1 = sched.reserve(2, 10, t0);
    CHECK(d1 == t0 + std::chrono::milliseconds(10));

    // 2ms and 15ms jitter must preserve exact 1ms/packet deadline grid
    const auto d2 = sched.reserve(2, 10, t0 + std::chrono::milliseconds(12));
    CHECK(d2 == t0 + std::chrono::milliseconds(20));

    const auto d2b = sched.reserve(2, 10, t0 + std::chrono::milliseconds(35));
    CHECK(d2b == t0 + std::chrono::milliseconds(30));

    // Genuine >25ms stall re-anchors timeline
    const auto resume = t0 + std::chrono::milliseconds(65);
    const auto d3 = sched.reserve(2, 10, resume);
    CHECK(d3 == resume + std::chrono::milliseconds(10));

    sched.reset(2);
    const auto after_reset = resume + std::chrono::milliseconds(2);
    const auto d4 = sched.reserve(2, 10, after_reset);
    CHECK(d4 == after_reset + std::chrono::milliseconds(10));
    std::cout << "PASSED\n";
}

void test_iso_waiter_cancellation() {
    std::cout << "[Test] USB/IP IsoWaiter cancellation... ";
    enable_process_realtime();
    IsoWaiter waiter;
    std::atomic<bool> closed{false};
    std::atomic<bool> cancelled{false};

    std::thread canceller([&]() {
        Sleep(10);
        cancelled.store(true, std::memory_order_release);
        waiter.abort();
    });

    const auto start = Clock::now();
    const bool ok = waiter.wait_until(start + std::chrono::milliseconds(500), closed, cancelled);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    canceller.join();

    CHECK(!ok);
    CHECK(elapsed < 150);
    std::cout << "PASSED\n";
}

void test_server_end_to_end_tcp_and_unlink() {
    std::cout << "[Test] USB/IP Server loopback Devlist/Import/Audio/Unlink... ";
    enable_process_realtime();
    std::vector<std::unique_ptr<Uac1Device>> devs;
    devs.push_back(Uac1Device::create(1, 100, 16));

    UsbipServer srv("127.0.0.1:0", std::move(devs));
    std::string err;
    CHECK(srv.start(&err));
    const uint16_t port = srv.bound_port();
    CHECK(port != 0);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(s != INVALID_SOCKET);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    CHECK(connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);

    // 1. OpReqImport "1-1"
    std::vector<uint8_t> imp;
    encode_op_header(imp, kOpReqImport, 0);
    imp.resize(8 + 32, 0);
    std::memcpy(imp.data() + 8, "1-1", 3);
    CHECK(tcp_send_all(s, imp.data(), imp.size()));

    uint8_t rep_imp[8 + 312]{};
    CHECK(tcp_recv_all(s, rep_imp, sizeof(rep_imp)));
    const OpHeader rep_h = decode_op_header(rep_imp);
    CHECK(rep_h.code == kOpRepImport && rep_h.status == 0);

    // 2. SET_CONFIGURATION(1), SET_INTERFACE(1,1), SET_INTERFACE(2,1)
    auto send_ctrl = [&](uint32_t seq, uint8_t req_type, uint8_t req, uint16_t val, uint16_t idx) {
        uint8_t setup[8]{req_type, req, static_cast<uint8_t>(val), static_cast<uint8_t>(val >> 8),
                         static_cast<uint8_t>(idx), static_cast<uint8_t>(idx >> 8), 0, 0};
        auto pkt = build_submit(seq, kDirectionOut, 0, 0, kNoIsoPackets, setup, nullptr, {});
        CHECK(tcp_send_all(s, pkt.data(), pkt.size()));
        uint8_t rbuf[48]{};
        CHECK(tcp_recv_all(s, rbuf, sizeof(rbuf)));
        CHECK(read_be32(rbuf) == kRetSubmit && read_be32(rbuf + 4) == seq);
        CHECK(static_cast<int32_t>(read_be32(rbuf + 20)) == kStatusOK);
    };
    send_ctrl(10, 0x00, kReqSetConfiguration, 1, 0);
    send_ctrl(11, 0x01, kReqSetInterface, 1, 1);
    send_ctrl(12, 0x01, kReqSetInterface, 1, 2);

    // 3. Send EP1 OUT non-iso PCM packet & read back via EP2 IN non-iso packet
    // Stereo 16-bit = 4 bytes/frame -> 192 bytes (48 frames = 1ms)
    std::vector<uint8_t> pcm(192);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>(0x40 + (i % 73));
    auto ep1 = build_submit(20, kDirectionOut, 1, static_cast<uint32_t>(pcm.size()), 0, nullptr, pcm.data(), {});
    CHECK(tcp_send_all(s, ep1.data(), ep1.size()));
    uint8_t r20[48]{};
    CHECK(tcp_recv_all(s, r20, sizeof(r20)));
    CHECK(read_be32(r20 + 24) == pcm.size());

    auto ep2 = build_submit(21, kDirectionIn, 2, static_cast<uint32_t>(pcm.size()), 0, nullptr, nullptr, {});
    CHECK(tcp_send_all(s, ep2.data(), ep2.size()));
    std::vector<uint8_t> r21(48 + pcm.size());
    CHECK(tcp_recv_all(s, r21.data(), r21.size()));
    CHECK(read_be32(r21.data() + 24) == pcm.size());
    CHECK(std::memcmp(r21.data() + 48, pcm.data(), pcm.size()) == 0);

    // 4. Submit long 200ms EP2 IN iso URB then UNLINK it immediately
    std::vector<IsoPacket> iso_pkts(200);
    for (uint32_t i = 0; i < 200; ++i) {
        iso_pkts[i] = {i * 192, 192, 0, 0};
    }
    auto iso_req = build_submit(30, kDirectionIn, 2, 200 * 192, 200, nullptr, nullptr, iso_pkts);
    CHECK(tcp_send_all(s, iso_req.data(), iso_req.size()));
    Sleep(5);

    std::vector<uint8_t> unlink_cmd(48, 0);
    write_be32(unlink_cmd.data(), kCmdUnlink);
    write_be32(unlink_cmd.data() + 4, 31);
    write_be32(unlink_cmd.data() + 20, 30); // target seq = 30
    CHECK(tcp_send_all(s, unlink_cmd.data(), unlink_cmd.size()));

    bool saw_ret_submit = false;
    bool saw_ret_unlink = false;
    while (!saw_ret_submit || !saw_ret_unlink) {
        uint8_t hdr[48]{};
        CHECK(tcp_recv_all(s, hdr, sizeof(hdr)));
        const uint32_t cmd = read_be32(hdr);
        if (cmd == kRetUnlink) {
            CHECK(static_cast<int32_t>(read_be32(hdr + 20)) == kStatusConnReset);
            saw_ret_unlink = true;
        } else if (cmd == kRetSubmit) {
            CHECK(read_be32(hdr + 4) == 30);
            CHECK(static_cast<int32_t>(read_be32(hdr + 20)) == kStatusConnReset);
            std::vector<uint8_t> desc_trash(200 * 16);
            CHECK(tcp_recv_all(s, desc_trash.data(), desc_trash.size()));
            saw_ret_submit = true;
        } else {
            CHECK(false && "unexpected reply opcode");
        }
    }

    closesocket(s);
    srv.stop();
    std::cout << "PASSED\n";
}

} // namespace

int main() {
    test_byte_ring();
    test_uac1_descriptors_and_loopback();
    test_iso_scheduler_cadence();
    test_iso_waiter_cancellation();
    test_server_end_to_end_tcp_and_unlink();
    std::cout << "All C++ USB/IP virtual cable tests passed successfully!\n";
    return 0;
}
