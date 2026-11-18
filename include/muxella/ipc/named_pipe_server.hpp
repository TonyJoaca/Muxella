#pragma once

#include "muxella/matrix/engine.hpp"
#include "muxella/core/endpoint_manager.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <thread>
#include <atomic>
#include <string>

namespace muxella::ipc {

class NamedPipeServer {
public:
    NamedPipeServer(matrix::MixerEngine& engine,
                    core::EndpointManager* ep_mgr = nullptr,
                    const std::wstring& pipe_name = L"\\\\.\\pipe\\Muxella_Control");
    ~NamedPipeServer();

    bool start();
    void stop();

    void set_shutdown_handler(std::function<void()> handler) { on_shutdown_ = std::move(handler); }

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

private:
    void server_worker_thread();

    matrix::MixerEngine& engine_;
    core::EndpointManager* ep_mgr_{nullptr};
    std::wstring pipe_name_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_signal_{false};
    std::function<void()> on_shutdown_{nullptr};
    std::thread worker_thread_;
    HANDLE stop_event_{nullptr};
};

} // namespace muxella::ipc
