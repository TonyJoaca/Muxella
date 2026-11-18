# Muxella

[![CI](https://github.com/TonyJoaca/Muxella/actions/workflows/ci.yml/badge.svg)](https://github.com/TonyJoaca/Muxella/actions/workflows/ci.yml)
[![Language](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Python](https://img.shields.io/badge/Python-3.10%2B-blue.svg)](https://www.python.org/)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20x64-brightgreen.svg)](https://microsoft.com/windows)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3%20%2F%20MIT-blue.svg)](LICENSE)

**Muxella** is a modern, high-performance, open-source virtual audio matrix router and mixing workstation engineered for Windows 10/11. Built from the ground up in C++20 using documented Windows Core Audio APIs, it pairs an ultra-low latency audio core with an intuitive mixing frontend featuring real-time parametric EQ, dynamic range compression, neural noise suppression, cross-strip sidechain ducking, ITU-R BS.1770 LUFS metering, and integrated virtual audio cable routing.

---

## Key Features

### High-Performance Real-Time Audio Core
- **Ultra-Low Latency WASAPI Backend:** Event-driven WASAPI Shared and Exclusive mode streaming with Multimedia Class Scheduler Service (MMCSS `Pro Audio`) thread scheduling.
- **ASIO 2.0 Host Support:** Native enumeration and direct COM interface hosting for professional multi-channel ASIO sound cards.
- **Process Loopback (Per-App Audio Capture):** Capture individual application audio streams directly via Windows Process Loopback (`ActivateAudioInterfaceAsync`).
- **Dynamic Hotplug Recovery:** Windows `IMMNotificationClient` listener automatically detects device disconnects and reconnects active audio streams on device restoration.
- **Lock-Free Concurrency:** Single-Producer Single-Consumer (SPSC) ring buffers with cache-line alignment (64-byte padding) and strictly zero heap allocations on the audio path.
- **Clock Drift Compensation:** Closed-loop Proportional-Integral (PI) drift correction with 4-point Catmull-Rom cubic Hermite spline interpolation to smoothly bridge unsynchronized physical clocks without audio dropouts or clicks.

### Mixing Matrix & Studio DSP
- **N × M Software Routing Matrix:** Flexible routing from any physical, virtual, or process input strip to any physical (A1–A5) or virtual (B1–B3) master bus.
- **Full Studio Channel Strips:**
  - Pre-fader gain and constant-power pan ($-3\,\text{dB}$ law).
  - 3-band parametric EQ using Direct Form II Transposed biquad filters.
  - Feed-forward soft-knee dynamic range compressor with smooth envelope ballistics.
  - Neural speech noise suppression powered by RNNoise.
  - Cross-strip sidechain auto-ducking with configurable threshold, ratio, attack, hold, and release.
  - Per-bus lip-sync delay line (0–200 ms).
  - ITU-R BS.1770 K-weighted true LUFS loudness metering and peak/RMS ballistics.
  - Master bus summing accumulators with soft-knee saturation limiters ($-0.1\,\text{dBFS}$ ceiling) to prevent digital clipping.

### Native Virtual Audio Cables
- **Six Lossless USB Audio Pairs:** Muxella Main, Spotify, Discord, and Out B1/B2/B3 pairs.
- **Native C++ USB/IP Virtual Host:** Bundled `muxella_cables.exe` communicates over loopback (`127.0.0.1:3241`) with the Microsoft-signed `usbip-win2` transport.
- **No Test Mode Required:** Works directly with standard Windows `usbaudio.sys` without requiring Windows Test Signing mode or custom kernel drivers.
- **Selectable Bit Depths:** Supports lossless 16-bit, 24-bit, and 32-bit PCM at 48 kHz.

### Decoupled Control & Automation
- **Named Pipe IPC Interface:** Fast Windows Named Pipe (`\\.\pipe\Muxella_Control`) for external GUIs, automation scripts, and stream decks.
- **Interactive CLI Monitor:** Ships with `muxella_ctl.exe` for real-time ASCII VU-meter monitoring and terminal control.

---

## Architecture Overview

```
Physical Inputs (Mic / Line-In)       Virtual / Process Loopback (Discord / Spotify / Game)
               │                                                  │
               ▼                                                  ▼
     WASAPI / ASIO Capture                             WASAPI Process Loopback
               │                                                  │
               └───────────────────────┬──────────────────────────┘
                                       ▼
                           Lock-Free SPSC Ring Buffers
                                       │
                                       ▼
                 ┌───────────────────────────────────────────┐
                 │       Muxella Real-Time Audio Core       │
                 │   (MMCSS "Pro Audio", Zero Heap Allocs)   │
                 │                                           │
                 │ Input Strips: Gain -> Pan -> EQ -> Comp   │
                 │               -> RNNoise -> Ducker        │
                 │                         │                 │
                 │                         ▼                 │
                 │                N x M Routing Matrix       │
                 │                         │                 │
                 │                         ▼                 │
                 │ Master Buses: Summing -> Delay -> Limiter │
                 │               -> LUFS & Peak Meters       │
                 └───────────────────────────────────────────┘
                                       │
                           Lock-Free SPSC Ring Buffers
                                       │
                       ┌───────────────┴────────────────────────┐
                       ▼                                        ▼
             WASAPI / ASIO Playback                    Virtual Audio Cables
             (Headphones / Monitors)                    (OBS / Discord Stream)
```

---

## Getting Started

### Option 1: Standalone Release Binary
Download the pre-compiled `Muxella.exe` from the [GitHub Releases](https://github.com/TonyJoaca/Muxella/releases) tab. It bundles the audio engine, virtual cables, and cyber UI into a single self-contained executable.

### Option 2: Running from Source

#### Prerequisites
- Windows 10 or Windows 11 (x64)
- Modern C++20 compiler (MinGW GCC 12+, Clang 15+, or MSVC 2022+)
- CMake 3.20+
- Python 3.10+ with `pip install PyQt6 pystray psutil`

#### 1. Compile the C++ Audio Core
```powershell
# Clone the repository with submodules
git clone --recurse-submodules https://github.com/TonyJoaca/Muxella.git
cd Muxella

# Configure build with CMake
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# Compile all libraries, server daemon, CLI, and test suite
cmake --build build -j4
```

#### 2. Run the Test Suite
```powershell
# Execute C++ unit tests
ctest --test-dir build --output-on-failure

# Execute GUI unit tests
python tests/test_b_bus_gui.py
```

#### 3. Run the Mixing Workstation
```powershell
# Run the GUI directly with Python
python muxella_gui.py

# Or build the all-in-one standalone executable
.\build_app.bat
```

---

## Controlling the Engine via CLI

Muxella ships with `muxella_ctl.exe`, a command-line utility communicating directly with the Named Pipe IPC:

```powershell
# Launch the live ASCII VU-Meter monitor
./build/muxella_ctl.exe monitor

# Adjust channel strip gain (-6.0 dB)
./build/muxella_ctl.exe gain 0 -6.0

# Adjust panning (-1.0 Left to +1.0 Right)
./build/muxella_ctl.exe pan 0 0.5

# Toggle Mute or Solo
./build/muxella_ctl.exe mute 0 1
./build/muxella_ctl.exe solo 1 1

# Configure Matrix Routing (Route Strip 0 to Bus 1 with 0.0 dB gain)
./build/muxella_ctl.exe route 0 1 1 0.0

# Set Master Bus Output Gain
./build/muxella_ctl.exe bus_gain 0 -3.0

# Set Bus Output Delay (e.g. 45 ms lip-sync delay on Bus 0)
./build/muxella_ctl.exe bus_delay 0 45.0
```

---

## Documentation

- [Architecture Specification](docs/ARCHITECTURE.md)
- [Virtual Audio Endpoints & Driver Layer Guide](docs/VIRTUAL_ENDPOINTS_GUIDE.md)
- [Clean-Room Engineering Statement](docs/CLEAN_ROOM_STATEMENT.md)
- [Third-Party Notices & Attribution](THIRD_PARTY_NOTICES.md)
- [Legal & Trademark Disclaimers](DISCLAIMER.md)
- [Contributing Guidelines](CONTRIBUTING.md)
- [Security Policy](SECURITY.md)

---

## Legal Disclaimers & Licensing

- **Clean-Room Development:** Muxella is a clean-room software project developed strictly from public specifications and documented APIs. No closed-source commercial software was disassembled, decompiled, or reverse-engineered.
- **Trademarks:** VoiceMeeter is a registered trademark of VB-AUDIO Software. ASIO is a registered trademark of Steinberg Media Technologies GmbH. Virtual Audio Cable is a trademark of Eugene Muzychenko. Muxella is an independent project and is not affiliated with, sponsored by, or endorsed by any of these entities. See [DISCLAIMER.md](DISCLAIMER.md) for full trademark notices.
- **License:** The C++ Core Audio Engine and libraries are licensed under the [MIT License](LICENSE.MIT). The complete application bundle and Python graphical interface are licensed under the [GNU General Public License v3](LICENSE).
