#include "muxella/ipc/named_pipe_server.hpp"
#include "muxella/ipc/protocol.hpp"
#include <array>
#include <iostream>

namespace muxella::ipc {
namespace {
// Keep OVERLAPPED alive until cancellation completes; every wait observes stop.
bool complete_io(HANDLE pipe, HANDLE stop, OVERLAPPED& operation, BOOL immediate,
                 DWORD initial_error, DWORD timeout, DWORD& transferred) {
    if (!immediate && initial_error != ERROR_IO_PENDING) return false;
    if (immediate) return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    HANDLE events[] = {stop, operation.hEvent};
    const DWORD wait = WaitForMultipleObjects(2, events, FALSE, timeout);
    if (wait != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
}
bool transfer(HANDLE pipe, HANDLE stop, void* bytes, DWORD size, bool write, DWORD& transferred) {
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    BOOL ok = write ? WriteFile(pipe, bytes, size, nullptr, &operation) : ReadFile(pipe, bytes, size, nullptr, &operation);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    const bool complete = complete_io(pipe, stop, operation, ok, error, 2000, transferred);
    CloseHandle(operation.hEvent);
    return complete;
}
}

NamedPipeServer::NamedPipeServer(matrix::MixerEngine& engine, core::EndpointManager* manager,
                               const std::wstring& pipe_name)
    : engine_(engine), ep_mgr_(manager), pipe_name_(pipe_name) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
NamedPipeServer::~NamedPipeServer() {
    stop();
    if (stop_event_) CloseHandle(stop_event_);
}
bool NamedPipeServer::start() {
    if (running_.load()) return true;
    if (!stop_event_) return false;
    stop_signal_.store(false);
    ResetEvent(stop_event_);
    running_.store(true);
    worker_thread_ = std::thread(&NamedPipeServer::server_worker_thread, this);
    return true;
}
void NamedPipeServer::stop() {
    stop_signal_.store(true);
    if (stop_event_) SetEvent(stop_event_);
    if (worker_thread_.joinable()) worker_thread_.join();
    running_.store(false);
}
void NamedPipeServer::server_worker_thread() {
    while (!stop_signal_.load()) {
        HANDLE pipe = CreateNamedPipeW(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 65536, 32768, 50, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(stop_event_, 100);
            continue;
        }
        OVERLAPPED connect{};
        connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!connect.hEvent) { CloseHandle(pipe); WaitForSingleObject(stop_event_, 100); continue; }
        BOOL connected = ConnectNamedPipe(pipe, &connect);
        DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        DWORD transferred = 0;
        bool ready = error == ERROR_PIPE_CONNECTED ||
            complete_io(pipe, stop_event_, connect, connected, error, INFINITE, transferred);
        CloseHandle(connect.hEvent);
        if (ready && !stop_signal_.load()) {
            std::array<char, 32768> buffer{};
            if (transfer(pipe, stop_event_, buffer.data(), static_cast<DWORD>(buffer.size()-1), false, transferred) && transferred) {
                std::string request(buffer.data(), transferred);
                std::string response;
                try {
                    response = CommandDispatcher::execute_command(engine_, ep_mgr_, request, on_shutdown_ ? &on_shutdown_ : nullptr);
                } catch (const std::exception& exception) {
                    std::cerr << "[IPC] " << exception.what() << "\n";
                    response = "{\"status\":\"error\",\"message\":\"command failed\"}\n";
                }
                DWORD written = 0;
                if (transfer(pipe, stop_event_, response.data(), static_cast<DWORD>(response.size()), true, written)) {
                    // An ACK confirms that the reply was consumed. Older clients
                    // close instead, which also ends this bounded read immediately.
                    char ack = 0;
                    DWORD ignored = 0;
                    transfer(pipe, stop_event_, &ack, 1, false, ignored);
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    running_.store(false);
}
} // namespace muxella::ipc
