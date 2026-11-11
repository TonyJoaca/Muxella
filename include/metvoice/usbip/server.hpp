#pragma once

#include "metvoice/usbip/protocol.hpp"
#include "metvoice/usbip/uac1_device.hpp"
#include "metvoice/usbip/win_rt_socket.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace metvoice::usbip {

class IsoScheduler {
public:
    Clock::time_point reserve(uint32_t endpoint, uint32_t packets, Clock::time_point now) noexcept;
    void reset(uint32_t endpoint) noexcept;

private:
    std::mutex mu_;
    std::unordered_map<uint32_t, Clock::time_point> iso_next_;
};

class UsbipServer {
public:
    using LogFn = std::function<void(const std::string&)>;

    UsbipServer(std::string address, std::vector<std::unique_ptr<Uac1Device>> devices, LogFn logger = nullptr);
    ~UsbipServer();

    UsbipServer(const UsbipServer&) = delete;
    UsbipServer& operator=(const UsbipServer&) = delete;

    bool start(std::string* err = nullptr);
    void stop();

    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] uint16_t bound_port() const noexcept { return bound_port_.load(std::memory_order_acquire); }
    [[nodiscard]] const std::vector<std::unique_ptr<Uac1Device>>& devices() const noexcept { return devices_; }

private:
    void accept_loop();
    void handle_connection(SOCKET client_sock, std::string remote_addr);
    bool write_device_list(RtTcpSocket& sock);
    Uac1Device* find_device(const std::string& bus_id) const noexcept;
    void log(const std::string& msg) const;

    std::string address_;
    std::vector<std::unique_ptr<Uac1Device>> devices_;
    LogFn logger_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint16_t> bound_port_{0};
    std::atomic<uint64_t> urb_count_{0};

    std::mutex listen_mu_;
    SOCKET listen_sock_{INVALID_SOCKET};
    std::thread accept_thread_;

    std::mutex conn_mu_;
    std::unordered_map<std::string, bool> imported_;
    std::unordered_set<RtTcpSocket*> active_sockets_;
    std::vector<std::thread> conn_threads_;
};

} // namespace metvoice::usbip
