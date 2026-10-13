# MetVoice

**MetVoice** is a modern, open-source virtual audio matrix router and mixing workstation designed for Windows 10/11. Built from the ground up in C++20 using documented Windows APIs, it serves as a clean-room, low-latency alternative to legacy closed-source tools like VoiceMeeter.

---

## Key Features

- **Low-Latency Audio Core:** Event-driven WASAPI (Shared & Exclusive mode) with MMCSS (`Pro Audio`) high-priority thread scheduling.
- **Lock-Free Concurrency:** Single-Producer Single-Consumer (SPSC) ring buffers with cache-line alignment (64-byte padding) and zero dynamic memory allocations on the audio path.
- **Clock Drift Compensation:** Closed-loop Proportional-Integral (PI) drift correction with 4-point Catmull-Rom cubic Hermite spline interpolation to smoothly bridge unsynchronized endpoints.
- **N × M Software Mixing Matrix:**
  - Per-strip pre-gain, constant-power pan ($-3\,\text{dB}$ law), mute, and solo arbitration.
  - Per-strip 3-band parametric EQ (Direct Form II Transposed biquad filters).
  - Per-strip feed-forward soft-knee dynamic range compressor.
  - Master bus summing accumulators with zero-latency soft-knee saturation limiters ($-0.1\,\text{dBFS}$ ceiling) to prevent digital wrap/clipping.
- **Hardware Integration:** ASIO 2.0 driver scanning and interface abstraction.
- **Telemetry & Metering:** Lock-free, real-time Peak, RMS, and Gain Reduction meters with smooth decay ballistics.
- **Decoupled Control & IPC:** Windows Named Pipe (`\\.\pipe\MetVoice_Control`) interface for external GUIs, automation scripts, and command-line monitoring.

---

## Architecture Overview

```
Physical Inputs (Mic / Line-In)     Virtual Inputs (Loopback / App Routing)
               │                                        │
               ▼                                        ▼
   WASAPI Capture Endpoints               WASAPI Loopback Capture
               │                                        │
               └───────────────┬────────────────────────┘
                               ▼
                   Lock-Free SPSC Ring Buffers
                               │
                               ▼
         ┌───────────────────────────────────────────┐
         │       MetVoice Real-Time Audio Core       │
         │  (MMCSS "Pro Audio", Zero Heap Allocs)   │
         │                                           │
         │  Input Strips: Gain -> Pan -> EQ -> Comp  │
         │                        │                  │
         │                        ▼                  │
         │             N x M Routing Matrix          │
         │                        │                  │
         │                        ▼                  │
         │  Master Buses: Summing -> Limiter -> Out  │
         └───────────────────────────────────────────┘
                               │
                   Lock-Free SPSC Ring Buffers
                               │
               ┌───────────────┴────────────────────────┐
               ▼                                        ▼
    WASAPI Render Endpoints                  Virtual Render Endpoints
    (Headphones / Monitors)                  (OBS / Discord Stream)
```

---

## Building from Source

### Prerequisites
- Modern C++20 compiler (GCC 12+, Clang 15+, or MSVC 2022+)
- CMake 3.20+
- Windows 10 or Windows 11

### Compilation

```powershell
# Configure build with CMake (Release mode recommended for audio DSP)
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# Compile all libraries, daemon, CLI tool, and tests
cmake --build build -j4
```

---

## Running Unit Tests & Benchmarks

MetVoice includes a comprehensive test suite validating all real-time audio constraints:

```powershell
# Run all unit tests via CTest
ctest --test-dir build --output-on-failure
```

### Running the Callback Latency Benchmark

```powershell
./build/benchmark_callback.exe
```

**Measured Performance on 8×8 Full Matrix Processing (10 ms / 480-frame budget):**
- **Average Callback Latency:** ~601 µs (0.60 ms)
- **Audio Thread CPU Load:** ~6.02%
- **Real-Time Safety Headroom:** ~93.98%
- **Speedup Factor:** 16.6× faster than real time
- **Ring Buffer Overruns / Underruns:** 0

---

## Controlling the Engine via CLI

MetVoice ships with `metvoice_ctl`, an interactive control utility that communicates over the Named Pipe IPC interface:

```powershell
# Launch the live ASCII VU-Meter monitor
./build/metvoice_ctl.exe monitor

# Adjust channel strip gain
./build/metvoice_ctl.exe gain 0 -6.0

# Adjust panning (-1.0 to +1.0)
./build/metvoice_ctl.exe pan 0 0.5

# Toggle Mute or Solo
./build/metvoice_ctl.exe mute 0 1
./build/metvoice_ctl.exe solo 1 1

# Configure Matrix Routing (Route Strip 0 to Bus 1 with 0 dB gain)
./build/metvoice_ctl.exe route 0 1 1 0.0

# Set Master Bus Output Gain
./build/metvoice_ctl.exe bus_gain 0 -3.0
```

---

## Documentation

- [Detailed Architecture Specification](docs/ARCHITECTURE.md)
- [Virtual Audio Endpoints & Driver Layer Guide](docs/VIRTUAL_ENDPOINTS_GUIDE.md)
- [Clean-Room Engineering Statement](docs/CLEAN_ROOM_STATEMENT.md)

---

## License

MetVoice is released under the MIT Open Source License.
