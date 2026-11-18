cmake_minimum_required(VERSION 3.20)
project(Muxella VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Global compile definitions for Windows API cleanliness
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

# Enable aggressive optimization for real-time DSP
if (MSVC)
    add_compile_options(/O2 /fp:fast /W4 /wd4324 /wd4244 /wd4189)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-O3 -ffast-math -Wall -Wextra -Wpedantic)
    add_link_options(-static -static-libgcc -static-libstdc++)
    add_compile_definitions(FLOAT_APPROX)
endif()

include_directories(include)
include_directories(third_party)
include_directories(noise-suppression-for-voice/external/rnnoise/include)

# Neural Noise Suppression (werman / rnnoise)
add_subdirectory(noise-suppression-for-voice/external/rnnoise)

# 1. Core Library
add_library(muxella_core STATIC
    src/core/drift_corrector.cpp
    src/core/wasapi_backend.cpp
    src/core/asio_backend.cpp
    src/core/endpoint_manager.cpp
    src/core/app_sessions.cpp
    src/core/wav_recorder.cpp
)
target_link_libraries(muxella_core PUBLIC ole32 avrt ksuser uuid winmm mmdevapi)


# 2. DSP Library
add_library(muxella_dsp STATIC
    src/dsp/biquad.cpp
    src/dsp/compressor.cpp
    src/dsp/limiter.cpp
)
target_link_libraries(muxella_dsp PUBLIC RnNoise)

# 3. Matrix Routing Engine
add_library(muxella_matrix STATIC
    src/matrix/engine.cpp
)
target_link_libraries(muxella_matrix PUBLIC muxella_core muxella_dsp)

# 4. IPC Library
add_library(muxella_ipc STATIC
    src/ipc/named_pipe_server.cpp
    src/ipc/dispatcher.cpp
)
target_link_libraries(muxella_ipc PUBLIC muxella_matrix)

# 5. Main Audio Engine Executable
add_executable(muxella_server src/main.cpp)
target_link_libraries(muxella_server PRIVATE muxella_ipc muxella_matrix muxella_core muxella_dsp winmm)

# 5b. Native C++ USB/IP Virtual Cable Library & Host
add_library(muxella_usbip STATIC
    src/usbip/uac1_device.cpp
    src/usbip/protocol.cpp
    src/usbip/win_rt_socket.cpp
    src/usbip/server.cpp
)
target_link_libraries(muxella_usbip PUBLIC ws2_32 avrt winmm)

add_executable(muxella_cables WIN32 src/tools/muxella_cables_main.cpp)
target_link_libraries(muxella_cables PRIVATE muxella_usbip ole32 shell32 winhttp bcrypt)
if (MSVC)
    set_target_properties(muxella_cables PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

# 6. Control CLI Tool
add_executable(muxella_ctl cli/muxella_ctl.cpp)

# 7. Unit Tests & Benchmarks
enable_testing()

add_executable(test_ring_buffer tests/test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer PRIVATE muxella_core)
add_test(NAME TestRingBuffer COMMAND test_ring_buffer)

add_executable(test_dsp tests/test_dsp.cpp)
target_link_libraries(test_dsp PRIVATE muxella_dsp)
add_test(NAME TestDsp COMMAND test_dsp)

add_executable(test_matrix tests/test_matrix.cpp)
target_link_libraries(test_matrix PRIVATE muxella_matrix muxella_dsp muxella_core)
add_test(NAME TestMatrix COMMAND test_matrix)

add_executable(test_drift tests/test_drift.cpp)
target_link_libraries(test_drift PRIVATE muxella_core)
add_test(NAME TestDrift COMMAND test_drift)

add_executable(benchmark_callback tests/benchmark_callback.cpp)
target_link_libraries(benchmark_callback PRIVATE muxella_matrix muxella_dsp muxella_core)

add_executable(test_capture_diag tests/test_capture_diag.cpp)
target_link_libraries(test_capture_diag PRIVATE muxella_core)

add_executable(test_loopback_diag tests/test_loopback_diag.cpp)
target_link_libraries(test_loopback_diag PRIVATE muxella_core)

add_executable(test_audio_flow tests/test_audio_flow.cpp)
target_link_libraries(test_audio_flow PRIVATE muxella_core)

add_executable(test_virtual_input_engine tests/test_virtual_input_engine.cpp)
target_link_libraries(test_virtual_input_engine PRIVATE muxella_matrix muxella_core muxella_dsp)

add_executable(test_link_cable tests/test_link_cable.cpp)
target_link_libraries(test_link_cable PRIVATE muxella_core)

add_executable(scan_loop tests/scan_loop.cpp)
target_link_libraries(scan_loop PRIVATE muxella_core)

add_executable(test_usbip tests/test_usbip.cpp)
target_link_libraries(test_usbip PRIVATE muxella_usbip)
add_test(NAME TestUsbip COMMAND test_usbip)

# Virtual endpoints use muxella_cables.exe and the signed USB/IP transport.
# The old Voicemeeter diagnostics and renaming installer are retired.
