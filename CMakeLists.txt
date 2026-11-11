cmake_minimum_required(VERSION 3.20)
project(MetVoice VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable aggressive optimization for real-time DSP
if (MSVC)
    add_compile_options(/O2 /fp:fast /W4)
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
add_library(metvoice_core STATIC
    src/core/drift_corrector.cpp
    src/core/wasapi_backend.cpp
    src/core/endpoint_manager.cpp
    src/core/app_sessions.cpp
    src/core/wav_recorder.cpp
)
target_link_libraries(metvoice_core PUBLIC ole32 avrt ksuser uuid winmm)


# 2. DSP Library
add_library(metvoice_dsp STATIC
    src/dsp/biquad.cpp
    src/dsp/compressor.cpp
    src/dsp/limiter.cpp
)
target_link_libraries(metvoice_dsp PUBLIC RnNoise)

# 3. Matrix Routing Engine
add_library(metvoice_matrix STATIC
    src/matrix/engine.cpp
)
target_link_libraries(metvoice_matrix PUBLIC metvoice_core metvoice_dsp)

# 4. IPC Library
add_library(metvoice_ipc STATIC
    src/ipc/named_pipe_server.cpp
    src/ipc/dispatcher.cpp
)
target_link_libraries(metvoice_ipc PUBLIC metvoice_matrix)

# 5. Main Audio Engine Executable
add_executable(metvoice_server src/main.cpp)
target_link_libraries(metvoice_server PRIVATE metvoice_ipc metvoice_matrix metvoice_core metvoice_dsp)

# 5b. Native C++ USB/IP Virtual Cable Library & Host
add_library(metvoice_usbip STATIC
    src/usbip/uac1_device.cpp
    src/usbip/protocol.cpp
    src/usbip/win_rt_socket.cpp
    src/usbip/server.cpp
)
target_link_libraries(metvoice_usbip PUBLIC ws2_32 avrt winmm)

add_executable(muxella_cables WIN32 src/tools/muxella_cables_main.cpp)
target_link_libraries(muxella_cables PRIVATE metvoice_usbip ole32 shell32 winhttp bcrypt)
if (MSVC)
    set_target_properties(muxella_cables PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
endif()

# 6. Control CLI Tool
add_executable(metvoice_ctl cli/metvoice_ctl.cpp)

# 7. Unit Tests & Benchmarks
enable_testing()

add_executable(test_ring_buffer tests/test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer PRIVATE metvoice_core)
add_test(NAME TestRingBuffer COMMAND test_ring_buffer)

add_executable(test_dsp tests/test_dsp.cpp)
target_link_libraries(test_dsp PRIVATE metvoice_dsp)
add_test(NAME TestDsp COMMAND test_dsp)

add_executable(test_matrix tests/test_matrix.cpp)
target_link_libraries(test_matrix PRIVATE metvoice_matrix metvoice_dsp metvoice_core)
add_test(NAME TestMatrix COMMAND test_matrix)

add_executable(test_drift tests/test_drift.cpp)
target_link_libraries(test_drift PRIVATE metvoice_core)
add_test(NAME TestDrift COMMAND test_drift)

add_executable(benchmark_callback tests/benchmark_callback.cpp)
target_link_libraries(benchmark_callback PRIVATE metvoice_matrix metvoice_dsp metvoice_core)

add_executable(test_capture_diag tests/test_capture_diag.cpp)
target_link_libraries(test_capture_diag PRIVATE metvoice_core)

add_executable(test_loopback_diag tests/test_loopback_diag.cpp)
target_link_libraries(test_loopback_diag PRIVATE metvoice_core)

add_executable(test_audio_flow tests/test_audio_flow.cpp)
target_link_libraries(test_audio_flow PRIVATE metvoice_core)

add_executable(test_virtual_input_engine tests/test_virtual_input_engine.cpp)
target_link_libraries(test_virtual_input_engine PRIVATE metvoice_matrix metvoice_core metvoice_dsp)

add_executable(test_link_cable tests/test_link_cable.cpp)
target_link_libraries(test_link_cable PRIVATE metvoice_core)

add_executable(scan_loop tests/scan_loop.cpp)
target_link_libraries(scan_loop PRIVATE metvoice_core)

add_executable(test_usbip tests/test_usbip.cpp)
target_link_libraries(test_usbip PRIVATE metvoice_usbip)
add_test(NAME TestUsbip COMMAND test_usbip)

# Virtual endpoints use muxella_cables.exe and the signed USB/IP transport.
# The old Voicemeeter diagnostics and renaming installer are retired.
