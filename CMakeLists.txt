cmake_minimum_required(VERSION 3.20)
project(MetVoice VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable aggressive optimization for real-time DSP
if (MSVC)
    add_compile_options(/O2 /fp:fast /W4)
else()
    add_compile_options(-O3 -ffast-math -Wall -Wextra -Wpedantic)
endif()

include_directories(include)

# 1. Core Library
add_library(metvoice_core STATIC
    src/core/drift_corrector.cpp
    src/core/wasapi_backend.cpp
)
target_link_libraries(metvoice_core PUBLIC ole32 avrt ksuser uuid)

# 2. DSP Library
add_library(metvoice_dsp STATIC
    src/dsp/biquad.cpp
    src/dsp/compressor.cpp
    src/dsp/limiter.cpp
)

# 3. Matrix Routing Engine
add_library(metvoice_matrix STATIC
    src/matrix/engine.cpp
)
target_link_libraries(metvoice_matrix PUBLIC metvoice_core metvoice_dsp)

# 4. IPC Library
add_library(metvoice_ipc STATIC
    src/ipc/named_pipe_server.cpp
)
target_link_libraries(metvoice_ipc PUBLIC metvoice_matrix)

# 5. Main Audio Engine Executable
add_executable(metvoice_server src/main.cpp)
target_link_libraries(metvoice_server PRIVATE metvoice_ipc metvoice_matrix metvoice_core metvoice_dsp)

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
