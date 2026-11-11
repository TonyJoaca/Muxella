#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace metvoice::usbip {

using Clock = std::chrono::steady_clock;

void enable_process_realtime() noexcept;
void ensure_winsock_initialized() noexcept;

class IsoWaiter {
public:
    IsoWaiter() noexcept;
    ~IsoWaiter();

    IsoWaiter(const IsoWaiter&) = delete;
    IsoWaiter& operator=(const IsoWaiter&) = delete;

    void abort() noexcept;
    void reset_abort() noexcept;
    // Returns false if aborted/cancelled before deadline.
    bool wait_until(Clock::time_point complete_at, const std::atomic<bool>& conn_closed,
                    const std::atomic<bool>& job_cancelled) noexcept;

private:
    HANDLE timer_{nullptr};
    HANDLE abort_event_{nullptr};
};

class RtTcpSocket {
public:
    explicit RtTcpSocket(SOCKET sock) noexcept;
    ~RtTcpSocket();

    RtTcpSocket(const RtTcpSocket&) = delete;
    RtTcpSocket& operator=(const RtTcpSocket&) = delete;

    void configure_realtime() noexcept;
    void set_io_timeout_ms(uint32_t ms) noexcept { timeout_ms_ = ms; }
    void abort() noexcept;
    void close_socket() noexcept;

    bool read_exact(uint8_t* dst, size_t len) noexcept;
    bool write_all(const uint8_t* data, size_t len) noexcept;

    [[nodiscard]] bool last_read_eof() const noexcept { return read_eof_; }

private:
    int recv_chunk(uint8_t* dst, size_t max_len) noexcept;

    SOCKET sock_{INVALID_SOCKET};
    HANDLE read_event_{nullptr};
    HANDLE write_event_{nullptr};
    HANDLE abort_event_{nullptr};
    std::atomic<bool> aborted_{false};
    uint32_t timeout_ms_{INFINITE};
    bool read_eof_{false};

    std::vector<uint8_t> read_buf_;
    size_t read_pos_{0};
    size_t read_end_{0};

    std::mutex write_mu_;
};

} // namespace metvoice::usbip
