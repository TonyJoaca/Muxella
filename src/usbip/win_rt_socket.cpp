#include "muxella/usbip/win_rt_socket.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <mmsystem.h>
#include <timeapi.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace muxella::usbip {

namespace {
std::once_flag g_rt_once;
std::once_flag g_wsa_once;
} // namespace

void ensure_winsock_initialized() noexcept {
    std::call_once(g_wsa_once, []() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

void enable_process_realtime() noexcept {
    ensure_winsock_initialized();
    std::call_once(g_rt_once, []() {
        HANDLE proc = GetCurrentProcess();
        SetPriorityClass(proc, HIGH_PRIORITY_CLASS);

        using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
        if (auto set_proc_info = reinterpret_cast<SetProcessInformationFn>(reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")))) {
            struct {
                ULONG Version;
                ULONG ControlMask;
                ULONG StateMask;
            } throttling{1, 0x1u | 0x4u, 0};
            if (!set_proc_info(proc, 4 /* ProcessPowerThrottling */, &throttling, sizeof(throttling))) {
                throttling.ControlMask = 0x1u;
                set_proc_info(proc, 4, &throttling, sizeof(throttling));
            }
        }
        timeBeginPeriod(1);
    });
}

IsoWaiter::IsoWaiter() noexcept {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) {
        timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    abort_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

IsoWaiter::~IsoWaiter() {
    if (timer_) {
        CloseHandle(timer_);
        timer_ = nullptr;
    }
    if (abort_event_) {
        CloseHandle(abort_event_);
        abort_event_ = nullptr;
    }
}

void IsoWaiter::abort() noexcept {
    if (abort_event_) {
        SetEvent(abort_event_);
    }
}

void IsoWaiter::reset_abort() noexcept {
    if (abort_event_) {
        ResetEvent(abort_event_);
    }
}

bool IsoWaiter::wait_until(Clock::time_point complete_at, const std::atomic<bool>& conn_closed,
                           const std::atomic<bool>& job_cancelled) noexcept {
    if (conn_closed.load(std::memory_order_acquire) || job_cancelled.load(std::memory_order_acquire)) {
        return false;
    }
    if (complete_at == Clock::time_point{}) {
        return true;
    }
    const auto remaining = complete_at - Clock::now();
    if (remaining <= std::chrono::nanoseconds(0)) {
        return !conn_closed.load(std::memory_order_acquire) && !job_cancelled.load(std::memory_order_acquire);
    }

    if (!timer_ || !abort_event_) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        if (ms > 0) Sleep(static_cast<DWORD>(ms));
        return !conn_closed.load(std::memory_order_acquire) && !job_cancelled.load(std::memory_order_acquire);
    }

    int64_t units100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100;
    if (units100ns < 1) units100ns = 1;
    LARGE_INTEGER due_time{};
    due_time.QuadPart = -units100ns;
    if (!SetWaitableTimer(timer_, &due_time, 0, nullptr, nullptr, FALSE)) {
        return !conn_closed.load(std::memory_order_acquire) && !job_cancelled.load(std::memory_order_acquire);
    }

    HANDLE handles[2] = {timer_, abort_event_};
    const DWORD res = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (res == WAIT_OBJECT_0 + 1) {
        return false;
    }
    return !conn_closed.load(std::memory_order_acquire) && !job_cancelled.load(std::memory_order_acquire);
}

RtTcpSocket::RtTcpSocket(SOCKET sock) noexcept
    : sock_(sock), read_buf_(64 * 1024) {
    read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    abort_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

RtTcpSocket::~RtTcpSocket() {
    abort();
    close_socket();
    if (read_event_) CloseHandle(read_event_);
    if (write_event_) CloseHandle(write_event_);
    if (abort_event_) CloseHandle(abort_event_);
}

void RtTcpSocket::configure_realtime() noexcept {
    if (sock_ == INVALID_SOCKET) return;
    const BOOL nodelay = TRUE;
    setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    const BOOL keepalive = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepalive), sizeof(keepalive));
    const int buf_size = 256 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));
}

void RtTcpSocket::abort() noexcept {
    aborted_.store(true, std::memory_order_release);
    if (abort_event_) {
        SetEvent(abort_event_);
    }
}

void RtTcpSocket::close_socket() noexcept {
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

int RtTcpSocket::recv_chunk(uint8_t* dst, size_t max_len) noexcept {
    if (aborted_.load(std::memory_order_acquire) || sock_ == INVALID_SOCKET) return -1;
    ResetEvent(read_event_);

    WSAOVERLAPPED ov{};
    ov.hEvent = read_event_;
    WSABUF wbuf{};
    wbuf.len = static_cast<ULONG>(max_len);
    wbuf.buf = reinterpret_cast<CHAR*>(dst);
    DWORD qty = 0;
    DWORD flags = 0;

    const int rc = WSARecv(sock_, &wbuf, 1, &qty, &flags, &ov, nullptr);
    if (rc == 0) {
        if (qty == 0) {
            read_eof_ = true;
            return 0;
        }
        return static_cast<int>(qty);
    }
    if (WSAGetLastError() != WSA_IO_PENDING) {
        return -1;
    }

    HANDLE handles[2] = {read_event_, abort_event_};
    const DWORD wait_res = WaitForMultipleObjects(2, handles, FALSE, timeout_ms_);
    if (wait_res != WAIT_OBJECT_0) {
        CancelIoEx(reinterpret_cast<HANDLE>(sock_), &ov);
        DWORD transferred = 0;
        DWORD out_flags = 0;
        WSAGetOverlappedResult(sock_, &ov, &transferred, TRUE, &out_flags);
        return -1;
    }

    DWORD transferred = 0;
    DWORD out_flags = 0;
    if (!WSAGetOverlappedResult(sock_, &ov, &transferred, TRUE, &out_flags)) {
        return -1;
    }
    if (transferred == 0) {
        read_eof_ = true;
        return 0;
    }
    return static_cast<int>(transferred);
}

bool RtTcpSocket::read_exact(uint8_t* dst, size_t len) noexcept {
    read_eof_ = false;
    while (len > 0) {
        const size_t cached = read_end_ - read_pos_;
        if (cached > 0) {
            const size_t take = std::min(cached, len);
            std::memcpy(dst, read_buf_.data() + read_pos_, take);
            read_pos_ += take;
            dst += take;
            len -= take;
            continue;
        }

        // Fill read-ahead buffer directly to coalesce adjacent USB/IP frames.
        const int got = recv_chunk(read_buf_.data(), read_buf_.size());
        if (got <= 0) {
            return false;
        }
        read_pos_ = 0;
        read_end_ = static_cast<size_t>(got);
    }
    return true;
}

bool RtTcpSocket::write_all(const uint8_t* data, size_t len) noexcept {
    if (!data || len == 0) return true;
    std::lock_guard<std::mutex> lock(write_mu_);
    size_t sent = 0;
    while (sent < len) {
        if (aborted_.load(std::memory_order_acquire) || sock_ == INVALID_SOCKET) return false;
        ResetEvent(write_event_);

        WSAOVERLAPPED ov{};
        ov.hEvent = write_event_;
        WSABUF wbuf{};
        wbuf.len = static_cast<ULONG>(std::min<size_t>(len - sent, 64 * 1024));
        wbuf.buf = const_cast<CHAR*>(reinterpret_cast<const CHAR*>(data + sent));
        DWORD qty = 0;

        const int rc = WSASend(sock_, &wbuf, 1, &qty, 0, &ov, nullptr);
        if (rc == 0) {
            if (qty == 0) return false;
            sent += qty;
            continue;
        }
        if (WSAGetLastError() != WSA_IO_PENDING) {
            return false;
        }

        HANDLE handles[2] = {write_event_, abort_event_};
        const DWORD wait_res = WaitForMultipleObjects(2, handles, FALSE, 4000);
        if (wait_res != WAIT_OBJECT_0) {
            CancelIoEx(reinterpret_cast<HANDLE>(sock_), &ov);
            DWORD transferred = 0;
            DWORD out_flags = 0;
            WSAGetOverlappedResult(sock_, &ov, &transferred, TRUE, &out_flags);
            return false;
        }

        DWORD transferred = 0;
        DWORD out_flags = 0;
        if (!WSAGetOverlappedResult(sock_, &ov, &transferred, TRUE, &out_flags) || transferred == 0) {
            return false;
        }
        sent += transferred;
    }
    return true;
}

} // namespace muxella::usbip
