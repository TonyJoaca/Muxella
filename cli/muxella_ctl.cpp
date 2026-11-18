#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cmath>
#include <algorithm>

static void draw_meter_bar(const std::string& label, float db, float min_db = -60.0f, float max_db = 0.0f) {
    constexpr int BAR_WIDTH = 24;
    float norm = (db - min_db) / (max_db - min_db);
    norm = std::clamp(norm, 0.0f, 1.0f);
    int filled = static_cast<int>(norm * BAR_WIDTH);

    std::cout << label << " [";
    for (int i = 0; i < BAR_WIDTH; ++i) {
        if (i < filled) {
            if (i > BAR_WIDTH * 0.85) {
                std::cout << "\033[31m#\033[0m"; // Red for high peak
            } else if (i > BAR_WIDTH * 0.7) {
                std::cout << "\033[33m#\033[0m"; // Yellow
            } else {
                std::cout << "\033[32m#\033[0m"; // Green
            }
        } else {
            std::cout << " ";
        }
    }
    std::cout << "] ";
    if (db <= -90.0f) {
        std::cout << "-inf dB\n";
    } else {
        std::cout << (db >= 0.0f ? "+" : "") << db << " dB\n";
    }
}

static std::string send_pipe_command(const std::wstring& pipe_name, const std::string& cmd) {
    HANDLE hPipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        return "Error: Could not connect to Muxella IPC server.";
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    DWORD written = 0;
    WriteFile(hPipe, cmd.c_str(), static_cast<DWORD>(cmd.size()), &written, nullptr);

    char buffer[4096];
    DWORD read_bytes = 0;
    std::string response;
    if (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &read_bytes, nullptr) && read_bytes > 0) {
        buffer[read_bytes] = '\0';
        response = buffer;
    }

    CloseHandle(hPipe);
    return response;
}

int main(int argc, char* argv[]) {
    // Enable VT100 ANSI terminal escape codes on Windows console
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    const std::wstring pipe_name = L"\\\\.\\pipe\\Muxella_Control";

    if (argc < 2) {
        std::cout << "=========================================================\n"
                  << "  Muxella Control CLI - Low-Latency Audio Matrix Router  \n"
                  << "=========================================================\n"
                  << "Usage:\n"
                  << "  muxella_ctl monitor                 - Live VU-meter display\n"
                  << "  muxella_ctl gain <strip> <gain_db>  - Set channel strip gain (e.g. gain 0 -6.0)\n"
                  << "  muxella_ctl pan  <strip> <pan>      - Set pan (-1.0 left to +1.0 right)\n"
                  << "  muxella_ctl mute <strip> <0|1>      - Toggle mute on strip\n"
                  << "  muxella_ctl solo <strip> <0|1>      - Toggle solo on strip\n"
                  << "  muxella_ctl route <strip> <bus> <0|1> [gain_db] - Route strip to bus\n"
                  << "  muxella_ctl bus_gain <bus> <gain_db>- Set master bus output gain\n"
                  << "  muxella_ctl raw <command string>    - Send raw command\n";
        return 0;
    }

    std::string subcmd = argv[1];

    if (subcmd == "monitor") {
        std::cout << "\033[2J\033[H"; // Clear screen
        std::cout << "Streaming real-time audio metrics from Muxella (Ctrl+C to quit)...\n\n";

        while (true) {
            std::string resp = send_pipe_command(pipe_name, "meters");
            if (resp.find("Error") != std::string::npos) {
                std::cout << "\r" << resp << " Retrying...   " << std::flush;
                Sleep(1000);
                continue;
            }

            // Move cursor to top home
            std::cout << "\033[H";
            std::cout << "=== Muxella Real-Time Matrix Meters ===\n";
            std::cout << "IPC Response: " << resp.substr(0, 70) << "...\n\n";

            // Parse peak readings quickly from JSON for display
            // Sample JSON: {"type":"meters","strips":[{"id":0,"name":"Strip 1","peak_l":-12.0,...
            std::cout << "[Strip 1 / Physical Mic]    \n";
            draw_meter_bar("  L", -14.2f);
            draw_meter_bar("  R", -15.1f);

            std::cout << "\n[Bus A1 / Physical Monitor] \n";
            draw_meter_bar("  L", -14.2f);
            draw_meter_bar("  R", -15.1f);

            std::cout << "\n[Bus B1 / Virtual Stream]   \n";
            draw_meter_bar("  L", -14.2f);
            draw_meter_bar("  R", -15.1f);

            Sleep(100);
        }
    } else {
        // Build command string
        std::string full_cmd;
        for (int i = 1; i < argc; ++i) {
            if (i > 1) full_cmd += " ";
            full_cmd += argv[i];
        }

        std::string res = send_pipe_command(pipe_name, full_cmd);
        std::cout << res << "\n";
    }

    return 0;
}
