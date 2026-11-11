#pragma once

#include "metvoice/matrix/engine.hpp"
#include "metvoice/core/endpoint_manager.hpp"
#include <windows.h>
#include <thread>
#include <atomic>
#include <string>

namespace metvoice::ipc {

class NamedPipeServer {
public:
    NamedPipeServer(matrix::MixerEngine& engine,
                    core::EndpointManager* ep_mgr = nullptr,
                    const std::wstring& pipe_name = L"\\\\.\\pipe\\MetVoice_Control");
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

} // namespace metvoice::ipc
