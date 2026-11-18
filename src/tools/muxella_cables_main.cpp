#include "muxella/usbip/server.hpp"
#include "muxella/usbip/uac1_device.hpp"
#include "muxella/usbip/win_rt_socket.hpp"

#include <nlohmann/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "muxella-pcm-1";

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string trim_ascii(std::string s) {
    auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_ws));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_ws).base(), s.end());
    return s;
}

std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string executable_path() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0) return "unknown";
    return wide_to_utf8(std::wstring(buf, len));
}

fs::path app_data_dir() {
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA")) {
        if (*local) return fs::path(local) / L"Muxella Audio";
    }
    return fs::temp_directory_path() / L"Muxella Audio";
}

class FileLogger {
public:
    FileLogger() {
        std::error_code ec;
        const fs::path dir = app_data_dir();
        fs::create_directories(dir, ec);
        path_ = dir / L"MuxellaCables.log";

        const bool truncate = fs::exists(path_, ec) && fs::file_size(path_, ec) > 2 * 1024 * 1024;
        out_.open(path_, std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
    }

    std::string path_utf8() const { return wide_to_utf8(path_.wstring()); }

    void log(const std::string& msg) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char ts[40]{};
        std::snprintf(ts, sizeof(ts), "%04u/%02u/%02u %02u:%02u:%02u.%03u000 ",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::lock_guard<std::mutex> lock(mu_);
        if (out_.is_open()) {
            out_ << ts << msg << "\n";
            out_.flush();
        }
    }

private:
    std::mutex mu_;
    fs::path path_;
    std::ofstream out_;
};

void show_fatal_dialog(const std::string& title, const std::string& msg) {
    const std::wstring wtitle = utf8_to_wide(title);
    const std::wstring wmsg = utf8_to_wide(msg);
    MessageBoxW(nullptr, wmsg.c_str(), wtitle.c_str(), MB_OK | MB_ICONERROR);
}

std::string find_usbip_executable() {
    auto exists_file = [](const fs::path& p) {
        std::error_code ec;
        return !p.empty() && fs::exists(p, ec) && !fs::is_directory(p, ec);
    };
    wchar_t found_buf[MAX_PATH * 2]{};
    if (SearchPathW(nullptr, L"usbip.exe", nullptr, static_cast<DWORD>(std::size(found_buf)), found_buf, nullptr) > 0) {
        return wide_to_utf8(found_buf);
    }
    const wchar_t* pf = _wgetenv(L"ProgramFiles");
    const wchar_t* pfx86 = _wgetenv(L"ProgramFiles(x86)");
    const wchar_t* folders[] = {L"USBip", L"usbip-win2", L"usbip"};
    for (const wchar_t* base : {pf, pfx86}) {
        if (!base || !*base) continue;
        for (const wchar_t* sub : folders) {
            const fs::path cand = fs::path(base) / sub / L"usbip.exe";
            if (exists_file(cand)) return wide_to_utf8(cand.wstring());
        }
    }
    return {};
}

bool run_hidden_process(const std::wstring& cmdline, uint32_t timeout_ms, std::string& output, DWORD& exit_code) {
    output.clear();
    exit_code = 1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rpipe = nullptr;
    HANDLE wpipe = nullptr;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return false;
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmdline;
    const BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wpipe);
    if (!ok) {
        CloseHandle(rpipe);
        return false;
    }

    std::string captured;
    std::thread reader([&]() {
        char buf[4096];
        DWORD read_n = 0;
        while (ReadFile(rpipe, buf, sizeof(buf), &read_n, nullptr) && read_n > 0) {
            captured.append(buf, read_n);
        }
    });

    const DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    reader.join();
    CloseHandle(rpipe);

    output = trim_ascii(std::move(captured));
    return wait == WAIT_OBJECT_0;
}

bool run_usbip(const std::string& exe, FileLogger& logger, const std::vector<std::string>& args,
               std::string& output, std::string* err = nullptr) {
    std::ostringstream cmd;
    cmd << "\"" << exe << "\" --tcp-port 3241";
    std::string joined;
    for (const auto& a : args) {
        cmd << " \"" << a << "\"";
        if (!joined.empty()) joined += ' ';
        joined += a;
    }
    logger.log("usbip.exe " + joined);

    DWORD code = 1;
    const bool finished = run_hidden_process(utf8_to_wide(cmd.str()), 15000, output, code);
    if (!output.empty()) {
        std::string flat = output;
        std::replace(flat.begin(), flat.end(), '\r', ' ');
        std::replace(flat.begin(), flat.end(), '\n', '|');
        logger.log("usbip: " + flat);
    }
    if (!finished || code != 0) {
        if (err) *err = "usbip " + joined + " failed (exit " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

void detach_own_usbip_ports(const std::string& exe, FileLogger& logger) {
    std::string out;
    std::string err;
    if (!run_usbip(exe, logger, {"port"}, out, &err)) {
        logger.log("Could not inspect existing USB/IP ports: " + err);
        return;
    }
    std::regex port_re(R"(^Port\s+(\d+):)", std::regex_constants::icase);
    std::istringstream iss(out);
    std::string line;
    std::string current_port;
    std::vector<std::string> own_ports;
    while (std::getline(iss, line)) {
        const std::string t = trim_ascii(line);
        std::smatch m;
        if (std::regex_search(t, m, port_re)) {
            current_port = m[1].str();
            continue;
        }
        if (!current_port.empty() && t.find("usbip://127.0.0.1:3241/") != std::string::npos) {
            own_ports.push_back(current_port);
            current_port.clear();
        }
    }
    for (const auto& port : own_ports) {
        std::string ignored;
        if (!run_usbip(exe, logger, {"detach", "-p", port}, ignored, &err)) {
            logger.log("Could not detach Virtual Cables port " + port + ": " + err);
        }
    }
}

bool run_attach_helper(int count, FileLogger& logger, std::string* err = nullptr) {
    if (count < 1 || count > 32) {
        if (err) *err = "invalid cable count";
        return false;
    }
    const std::string exe = find_usbip_executable();
    if (exe.empty()) {
        if (err) *err = "usbip.exe was not found; install the driver from Muxella's menu";
        return false;
    }
    logger.log("Elevated Windows device synchronization started for " + std::to_string(count) + " cable(s)");
    logger.log("usbip.exe: " + exe);

    std::string ver_out;
    if (!run_usbip(exe, logger, {"--version"}, ver_out, err)) {
        return false;
    }
    std::regex ver_re(R"((\d+)\.(\d+)\.(\d+))");
    std::smatch vm;
    if (!std::regex_search(ver_out, vm, ver_re)) {
        if (err) *err = "could not identify USB/IP transport version";
        return false;
    }
    const int major = std::stoi(vm[1].str());
    const int minor = std::stoi(vm[2].str());
    const int patch = std::stoi(vm[3].str());
    if (major == 0 && (minor < 9 || (minor == 9 && patch < 8))) {
        if (err) *err = "USB/IP 0.9.8.0 or newer is required";
        return false;
    }

    std::string list_out;
    bool list_ok = false;
    for (int attempt = 1; attempt <= 20; ++attempt) {
        if (run_usbip(exe, logger, {"list", "-r", "127.0.0.1"}, list_out, err)) {
            list_ok = true;
            break;
        }
        Sleep(150);
    }
    if (!list_ok) {
        if (err) *err = "local USB/IP server is not ready";
        return false;
    }

    detach_own_usbip_ports(exe, logger);
    for (int n = 1; n <= count; ++n) {
        const std::string bus_id = "1-" + std::to_string(n);
        std::string attach_out;
        if (!run_usbip(exe, logger, {"attach", "-r", "127.0.0.1", "-b", bus_id}, attach_out, err)) {
            return false;
        }
    }
    logger.log("Windows device synchronization completed for " + std::to_string(count) + " cable(s)");
    return true;
}

bool send_tcp_line(SOCKET sock, const std::string& line) {
    const std::string full = line + "\n";
    size_t sent = 0;
    while (sent < full.size()) {
        const int n = send(sock, full.data() + sent, static_cast<int>(full.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_tcp_line(SOCKET sock, std::string& line, std::string& carry, DWORD timeout_ms) {
    if (timeout_ms > 0) {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    } else {
        DWORD zero = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&zero), sizeof(zero));
    }
    while (true) {
        const size_t pos = carry.find('\n');
        if (pos != std::string::npos) {
            line = trim_ascii(carry.substr(0, pos));
            carry.erase(0, pos + 1);
            return true;
        }
        char buf[1024];
        const int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        carry.append(buf, static_cast<size_t>(n));
    }
}

int run_attach_broker(const std::string& address, const std::string& token, FileLogger& logger) {
    muxella::usbip::ensure_winsock_initialized();
    const size_t colon = address.rfind(':');
    if (address.rfind("127.0.0.1:", 0) != 0 || colon == std::string::npos || token.size() != 64) {
        logger.log("Administrator broker: invalid parameters");
        return 1;
    }
    const int port = std::atoi(address.substr(colon + 1).c_str());
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return 1;

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
        closesocket(sock);
        logger.log("Administrator broker: connect failed");
        return 1;
    }

    if (!send_tcp_line(sock, token)) {
        closesocket(sock);
        return 1;
    }

    std::string carry;
    std::string line;
    while (recv_tcp_line(sock, line, carry, 0)) {
        if (line == "QUIT") {
            break;
        }
        if (line.rfind("SYNC ", 0) == 0) {
            const int count = std::atoi(line.substr(5).c_str());
            std::string err;
            if (!run_attach_helper(count, logger, &err)) {
                logger.log("Broker synchronization failed: " + err);
                send_tcp_line(sock, "ERR synchronization failed");
            } else {
                send_tcp_line(sock, "OK");
            }
        } else {
            send_tcp_line(sock, "ERR invalid request");
        }
    }

    if (const std::string exe = find_usbip_executable(); !exe.empty()) {
        detach_own_usbip_ports(exe, logger);
    }
    send_tcp_line(sock, "STOPPED");
    closesocket(sock);
    return 0;
}

class HostBrokerClient {
public:
    explicit HostBrokerClient(FileLogger& logger) : logger_(logger) {}
    ~HostBrokerClient() { stop(); }

    void start(int cables) {
        std::lock_guard<std::mutex> lock(mu_);
        desired_ = cables;
        if (starting_ || client_sock_ != INVALID_SOCKET) return;
        if (find_usbip_executable().empty()) {
            logger_.log("usbip.exe not found; waiting for driver installation");
            return;
        }

        muxella::usbip::ensure_winsock_initialized();
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) return;

        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        sa.sin_port = 0;
        if (bind(ls, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR ||
            listen(ls, 1) == SOCKET_ERROR) {
            closesocket(ls);
            return;
        }
        int slen = sizeof(sa);
        getsockname(ls, reinterpret_cast<sockaddr*>(&sa), &slen);
        const uint16_t port = ntohs(sa.sin_port);

        uint8_t raw_tok[32]{};
        if (BCryptGenRandom(nullptr, raw_tok, sizeof(raw_tok), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            closesocket(ls);
            return;
        }
        std::ostringstream hex;
        for (uint8_t b : raw_tok) {
            hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        }
        const std::string token = hex.str();

        const fs::path exe_p = utf8_to_wide(executable_path());
        const std::string params = "--attach-broker --broker-address 127.0.0.1:" +
                                   std::to_string(port) + " --broker-token " + token;
        const std::wstring wexe = exe_p.wstring();
        const std::wstring wparams = utf8_to_wide(params);
        const std::wstring wdir = exe_p.parent_path().wstring();

        HINSTANCE res = ShellExecuteW(nullptr, L"runas", wexe.c_str(), wparams.c_str(), wdir.c_str(), SW_HIDE);
        if (reinterpret_cast<intptr_t>(res) <= 32) {
            closesocket(ls);
            logger_.log("Administrator broker was not started (UAC cancelled or ShellExecute error)");
            return;
        }

        starting_ = true;
        accept_thread_ = std::thread([this, ls, token]() {
            accept_broker(ls, token);
        });
    }

    void request_sync(int cables) {
        bool connected = false;
        bool need_start = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            desired_ = cables;
            connected = (client_sock_ != INVALID_SOCKET);
            need_start = (!connected && !starting_);
        }
        if (connected) {
            std::lock_guard<std::mutex> cmd_lock(cmd_mu_);
            send_sync_locked(cables);
        } else if (need_start) {
            start(cables);
        }
    }

    void stop() {
        stopping_.store(true, std::memory_order_release);
        std::thread to_join;
        SOCKET cs = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cs = client_sock_;
            client_sock_ = INVALID_SOCKET;
            to_join.swap(accept_thread_);
        }
        if (cs != INVALID_SOCKET) {
            std::lock_guard<std::mutex> cmd_lock(cmd_mu_);
            send_tcp_line(cs, "QUIT");
            std::string dummy, carry;
            recv_tcp_line(cs, dummy, carry, 8000);
            closesocket(cs);
        }
        if (to_join.joinable()) {
            to_join.join();
        }
    }

private:
    void accept_broker(SOCKET ls, const std::string& expected_token) {
        SOCKET accepted = INVALID_SOCKET;
        for (int i = 0; i < 300 && !stopping_.load(std::memory_order_acquire); ++i) {
            fd_set fds{};
            FD_ZERO(&fds);
            FD_SET(ls, &fds);
            timeval tv{0, 200000};
            if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
                accepted = accept(ls, nullptr, nullptr);
                break;
            }
        }
        closesocket(ls);

        if (accepted == INVALID_SOCKET) {
            std::lock_guard<std::mutex> lock(mu_);
            starting_ = false;
            logger_.log("Administrator broker did not connect within timeout");
            return;
        }

        std::string recv_tok, carry;
        if (!recv_tcp_line(accepted, recv_tok, carry, 5000) || recv_tok != expected_token) {
            closesocket(accepted);
            std::lock_guard<std::mutex> lock(mu_);
            starting_ = false;
            logger_.log("Administrator broker authentication failed");
            return;
        }

        int count = 1;
        {
            std::lock_guard<std::mutex> lock(mu_);
            client_sock_ = accepted;
            starting_ = false;
            count = desired_;
        }
        logger_.log("One-time administrator broker connected");
        std::lock_guard<std::mutex> cmd_lock(cmd_mu_);
        send_sync_locked(count);
    }

    void send_sync_locked(int count) {
        SOCKET cs = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cs = client_sock_;
        }
        if (cs == INVALID_SOCKET) return;
        std::string reply, carry;
        if (!send_tcp_line(cs, "SYNC " + std::to_string(count)) ||
            !recv_tcp_line(cs, reply, carry, 45000)) {
            logger_.log("Broker SYNC communication failed");
            std::lock_guard<std::mutex> lock(mu_);
            if (client_sock_ == cs) {
                closesocket(cs);
                client_sock_ = INVALID_SOCKET;
            }
            return;
        }
        if (reply == "OK") {
            logger_.log("Windows Sound synchronized with " + std::to_string(count) + " cable(s)");
        } else {
            logger_.log("Windows device synchronization failed: " + reply);
        }
    }

    FileLogger& logger_;
    std::mutex mu_;
    std::mutex cmd_mu_;
    std::atomic<bool> stopping_{false};
    SOCKET client_sock_{INVALID_SOCKET};
    int desired_{6};
    bool starting_{false};
    std::thread accept_thread_;
};

// Simple WinHTTP GET supporting GitHub HTTPS redirects (to objects.githubusercontent.com).
bool validate_github_url(const std::wstring& url_str) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    if (!WinHttpCrackUrl(url_str.c_str(), 0, 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    const std::string h = to_lower_ascii(wide_to_utf8(host));
    return h == "github.com" || h == "api.github.com" ||
           h == "objects.githubusercontent.com" ||
           (h.size() > 19 && h.compare(h.size() - 19, 19, ".githubusercontent.com") == 0);
}

bool winhttp_download_bytes(const std::wstring& url_str, const wchar_t* accept_hdr,
                           std::string* out_bytes, const fs::path* out_file,
                           int redirect_depth = 0, std::string* err = nullptr) {
    if (redirect_depth > 10) {
        if (err) *err = "too many HTTP redirects";
        return false;
    }
    if (!validate_github_url(url_str)) {
        if (err) *err = "refused non-GitHub HTTPS URL";
        return false;
    }
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(url_str.c_str(), 0, 0, &uc)) {
        if (err) *err = "invalid URL";
        return false;
    }

    HINTERNET hSess = WinHttpOpen(L"Virtual-Cables/0.5.13", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return false;
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(hSess, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));

    HINTERNET hConn = WinHttpConnect(hSess, host, uc.nPort, 0);
    HINTERNET hReq = hConn ? WinHttpOpenRequest(hConn, L"GET", path[0] ? path : L"/",
                                                nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                           : nullptr;
    bool ok = false;
    if (hReq) {
        if (accept_hdr && *accept_hdr) {
            WinHttpAddRequestHeaders(hReq, accept_hdr, static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
        }
        if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(hReq, nullptr)) {
            DWORD status = 0;
            DWORD s_len = sizeof(status);
            WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                nullptr, &status, &s_len, nullptr);
            if (status >= 300 && status < 400) {
                wchar_t loc[2048]{};
                DWORD loc_len = sizeof(loc);
                if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION, nullptr, loc, &loc_len, nullptr)) {
                    WinHttpCloseHandle(hReq);
                    WinHttpCloseHandle(hConn);
                    WinHttpCloseHandle(hSess);
                    return winhttp_download_bytes(loc, accept_hdr, out_bytes, out_file, redirect_depth + 1, err);
                }
            }
            if (status == 200) {
                std::ofstream file_stream;
                if (out_file) {
                    file_stream.open(*out_file, std::ios::binary | std::ios::trunc);
                }
                std::string buf_str;
                char chunk[16384];
                DWORD avail = 0;
                ok = true;
                while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                    DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(chunk)));
                    DWORD read_n = 0;
                    if (!WinHttpReadData(hReq, chunk, to_read, &read_n) || read_n == 0) break;
                    if (out_file) file_stream.write(chunk, read_n);
                    else buf_str.append(chunk, read_n);
                }
                if (out_bytes) *out_bytes = std::move(buf_str);
            } else if (err) {
                *err = "HTTP status " + std::to_string(status);
            }
        } else if (err) {
            *err = "WinHTTP request failed (" + std::to_string(GetLastError()) + ")";
        }
    }
    if (hReq) WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return ok;
}

bool install_usbip_driver(FileLogger& logger, std::string* err) {
    std::string release_json;
    if (!winhttp_download_bytes(L"https://api.github.com/repos/vadimgrn/usbip-win2/releases/latest",
                                L"Accept: application/vnd.github+json\r\n", &release_json, nullptr, 0, err)) {
        return false;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(release_json);
    } catch (const std::exception& e) {
        if (err) *err = std::string("invalid GitHub release JSON: ") + e.what();
        return false;
    }

    int best_score = -1;
    std::string best_name;
    std::string best_url;
    uint64_t expected_size = 0;
    if (parsed.contains("assets") && parsed["assets"].is_array()) {
        for (const auto& asset : parsed["assets"]) {
            const std::string name = asset.value("name", "");
            const std::string dl = asset.value("browser_download_url", "");
            const std::string lower = to_lower_ascii(name);
            if (dl.empty()) continue;
            if (lower.find("arm64") != std::string::npos || lower.find("aarch64") != std::string::npos ||
                lower.find("symbols") != std::string::npos || lower.find("pdb") != std::string::npos ||
                lower.find("source") != std::string::npos || lower.find("report") != std::string::npos) {
                continue;
            }
            int score = 0;
            if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0) score += 100;
            else if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".msi") == 0) score += 95;
            else continue;
            if (lower.find("x64") != std::string::npos || lower.find("amd64") != std::string::npos ||
                lower.find("win64") != std::string::npos) score += 60;
            if (lower.find("installer") != std::string::npos || lower.find("setup") != std::string::npos) score += 30;
            if (lower.find("usbip") != std::string::npos) score += 10;
            if (score > best_score) {
                best_score = score;
                best_name = fs::path(name).filename().string();
                best_url = dl;
                expected_size = asset.value("size", uint64_t{0});
            }
        }
    }
    if (best_score < 0 || best_name.empty()) {
        if (err) *err = "release does not contain a Windows x64 EXE/MSI installer";
        return false;
    }

    PWSTR dl_ptr = nullptr;
    fs::path dl_dir = fs::temp_directory_path() / L"Virtual Cables";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dl_ptr)) && dl_ptr) {
        dl_dir = fs::path(dl_ptr) / L"Virtual Cables";
        CoTaskMemFree(dl_ptr);
    }
    std::error_code ec;
    fs::create_directories(dl_dir, ec);
    const fs::path final_path = dl_dir / utf8_to_wide(best_name);
    const fs::path part_path = dl_dir / utf8_to_wide(best_name + ".part");
    fs::remove(part_path, ec);

    logger.log("Downloading official GitHub release asset: " + best_url);
    if (!winhttp_download_bytes(utf8_to_wide(best_url), nullptr, nullptr, &part_path, 0, err)) {
        fs::remove(part_path, ec);
        return false;
    }
    const uint64_t got_size = fs::file_size(part_path, ec);
    if (ec || got_size < 64 * 1024 || (expected_size > 0 && got_size != expected_size)) {
        fs::remove(part_path, ec);
        if (err) *err = "downloaded installer size mismatch";
        return false;
    }
    fs::remove(final_path, ec);
    fs::rename(part_path, final_path, ec);
    if (ec) {
        fs::remove(part_path, ec);
        if (err) *err = "could not finalize downloaded installer";
        return false;
    }

    // Verify Authenticode signature before launching installer elevated.
    std::string escaped = wide_to_utf8(final_path.wstring());
    std::string escaped_sq;
    for (char c : escaped) {
        if (c == '\'') escaped_sq += "''";
        else escaped_sq += c;
    }
    const std::string ps_cmd = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
                               "$s=Get-AuthenticodeSignature -LiteralPath '" + escaped_sq + "'; "
                               "Write-Output ('Status=' + $s.Status); "
                               "if ($s.Status -ne 'Valid') { exit 23 }\"";
    std::string ps_out;
    DWORD ps_code = 1;
    if (!run_hidden_process(utf8_to_wide(ps_cmd), 30000, ps_out, ps_code) || ps_code != 0) {
        if (err) *err = "Authenticode status is not Valid: " + ps_out;
        return false;
    }
    logger.log("Installer Authenticode check passed: " + ps_out);

    const std::string lower_ext = to_lower_ascii(final_path.extension().string());
    const std::wstring wfile = (lower_ext == ".msi") ? L"msiexec.exe" : final_path.wstring();
    const std::wstring wparams = (lower_ext == ".msi") ? (L"/i \"" + final_path.wstring() + L"\"") : L"";
    const std::wstring wdir = final_path.parent_path().wstring();
    HINSTANCE sh = ShellExecuteW(nullptr, L"runas", wfile.c_str(), wparams.c_str(), wdir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(sh) <= 32) {
        if (err) *err = "failed to launch installer elevated";
        return false;
    }
    return true;
}

bool run_self_test(const std::vector<std::unique_ptr<muxella::usbip::Uac1Device>>& devices, std::string& err) {
    for (const auto& dev : devices) {
        if (!dev->descriptors().validate(&err)) return false;
        std::vector<uint8_t> resp;
        if (dev->handle_control({0x00, muxella::usbip::kReqSetConfiguration, 1, 0, 0}, nullptr, 0, resp) != 0) {
            err = "set configuration failed";
            return false;
        }
        for (uint16_t iface : {uint16_t{1}, uint16_t{2}}) {
            if (dev->handle_control({0x01, muxella::usbip::kReqSetInterface, 1, iface, 0}, nullptr, 0, resp) != 0) {
                err = "activate interface failed";
                return false;
            }
        }
        const size_t frame_bytes = static_cast<size_t>(muxella::usbip::kUacChannels) * static_cast<size_t>(dev->pcm_bytes());
        std::vector<uint8_t> pcm(frame_bytes * 2);
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>(0x11 + i);
        std::vector<uint8_t> pcm_copy = pcm;
        if (dev->write_playback(pcm_copy.data(), pcm_copy.size()) != pcm.size()) {
            err = "PCM write failed";
            return false;
        }
        std::vector<uint8_t> out(pcm.size(), 0);
        dev->read_capture(out.data(), out.size());
        if (out != pcm) {
            err = "PCM loopback mismatch";
            return false;
        }
    }
    return true;
}

std::vector<std::unique_ptr<muxella::usbip::Uac1Device>> make_devices(int count, int buffer_ms, int bits, std::string* err) {
    std::vector<std::unique_ptr<muxella::usbip::Uac1Device>> devs;
    devs.reserve(static_cast<size_t>(count));
    for (int i = 1; i <= count; ++i) {
        auto d = muxella::usbip::Uac1Device::create(i, buffer_ms, bits, err);
        if (!d) return {};
        devs.push_back(std::move(d));
    }
    return devs;
}

} // namespace

int main(int argc, char* argv[]) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    FileLogger logger;

    std::string address = "127.0.0.1:3241";
    int cables = 6;
    int buffer_ms = 250;
    int bits = 24;
    bool self_test = false;
    bool dump_descriptors = false;
    bool show_ver = false;
    bool attach_broker = false;
    std::string broker_address;
    std::string broker_token;
    bool headless = false;
    bool install_driver = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_val = [&](std::string& dst) -> bool {
            if (i + 1 < argc) {
                dst = argv[++i];
                return true;
            }
            return false;
        };
        if (arg == "--listen") next_val(address);
        else if (arg == "--cables" && i + 1 < argc) cables = std::atoi(argv[++i]);
        else if (arg == "--buffer-ms" && i + 1 < argc) buffer_ms = std::atoi(argv[++i]);
        else if (arg == "--bits" && i + 1 < argc) bits = std::atoi(argv[++i]);
        else if (arg == "--self-test") self_test = true;
        else if (arg == "--dump-descriptors") dump_descriptors = true;
        else if (arg == "--version") show_ver = true;
        else if (arg == "--attach-broker") attach_broker = true;
        else if (arg == "--broker-address") next_val(broker_address);
        else if (arg == "--broker-token") next_val(broker_token);
        else if (arg == "--headless") headless = true;
        else if (arg == "--install-driver") install_driver = true;
    }

    if (bits != 16 && bits != 24 && bits != 32) {
        logger.log("Invalid bit depth " + std::to_string(bits) + "; using 24");
        bits = 24;
    }
    if (cables < 1 || cables > 32) cables = 1;
    if (buffer_ms < 20 || buffer_ms > 5000) buffer_ms = 250;

    if (show_ver) {
        std::printf("Virtual Cables %s (windows/amd64 C++)\n", kVersion);
        return 0;
    }

    if (install_driver) {
        std::string err;
        if (!install_usbip_driver(logger, &err)) {
            logger.log("Driver installation failed: " + err);
            show_fatal_dialog("Virtual Cables Driver Setup", "Could not install USB/IP transport: " + err);
            return 1;
        }
        return 0;
    }

    if (attach_broker) {
        return run_attach_broker(broker_address, broker_token, logger);
    }

    logger.log("============================================================");
    logger.log(std::string("Virtual Cables ") + kVersion + " (windows/amd64 C++)");
    logger.log("Process started. Executable: " + executable_path());
    logger.log("Log file: " + logger.path_utf8());

    std::string err;
    auto devices = make_devices(cables, buffer_ms, bits, &err);
    if (devices.empty()) {
        logger.log("FATAL: " + err);
        return 1;
    }

    if (self_test) {
        if (!run_self_test(devices, err)) {
            logger.log("Self-test failed: " + err);
            std::fprintf(stderr, "Self-test failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("Virtual Cables %s self-test passed.\n", kVersion);
        logger.log("Self-test passed");
        return 0;
    }

    if (dump_descriptors) {
        for (const auto& d : devices) {
            std::printf("\n[%s | bus ID %s]\n", d->product().c_str(), d->bus_id().c_str());
            std::printf("Device descriptor: %zu bytes, Config descriptor: %zu bytes\n",
                        d->descriptors().device.size(), d->descriptors().config.size());
        }
        return 0;
    }

    muxella::usbip::UsbipServer server(address, std::move(devices), [&](const std::string& msg) {
        logger.log(msg);
    });
    if (!server.start(&err)) {
        logger.log("USB/IP server start failed: " + err);
        if (!headless) {
            show_fatal_dialog("Virtual Cables", "Could not start USB/IP server: " + err);
        }
        return 1;
    }

    HostBrokerClient broker(logger);
    broker.start(cables);

    if (headless) {
        // Parent GUI owns stdin; SYNC requests reattach, closing stdin shuts down cleanly.
        HANDLE h_stdin = GetStdHandle(STD_INPUT_HANDLE);
        std::string line_buf;
        char chunk[256];
        DWORD read_n = 0;
        while (h_stdin != INVALID_HANDLE_VALUE && ReadFile(h_stdin, chunk, sizeof(chunk), &read_n, nullptr) && read_n > 0) {
            line_buf.append(chunk, read_n);
            size_t nl = 0;
            while ((nl = line_buf.find('\n')) != std::string::npos) {
                const std::string cmd = trim_ascii(line_buf.substr(0, nl));
                line_buf.erase(0, nl + 1);
                if (cmd == "SYNC") {
                    broker.request_sync(cables);
                }
            }
        }
    } else {
        // Keep running if launched standalone until interrupted/signaled.
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    broker.stop();
    server.stop();
    logger.log("Virtual Cables stopped normally");
    CoUninitialize();
    return 0;
}
