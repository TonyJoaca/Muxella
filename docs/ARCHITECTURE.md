# MetVoice Architecture Specification

MetVoice is a high-performance, open-source virtual audio matrix router and mixing workstation designed for low-latency Windows environments as a clean-room alternative to legacy tools.

---

## 1. High-Level System Architecture

```
  +-----------------------------------------------------------------------+
  |                             User Applications                          |
  |             (OBS Studio, Discord, DAW, Media Players, Games)          |
  +--------------------+--------------------------------+-----------------+
                       |                                |
  [Physical Inputs]    | [Virtual Inputs / Loopbacks]   | [Per-App Routing]
  - Microphones (USB)  | - Windows Default Sound        | - IAudioPolicyConfigFactory
  - Line-In Interfaces | - Virtual Cable Bridging       | - WASAPI Loopback
                       v                                v
  +-----------------------------------------------------------------------+
  |                    Hardware & API Ingestion Layer                     |
  |  - WASAPI Shared / Exclusive (Event-Driven, AUDCLNT_STREAMFLAGS_*)     |
  |  - ASIO 2.0 Host Interface (Ping-Pong Buffer Switch)                  |
  +-----------------------------------+-----------------------------------+
                                      |
                           Lock-Free SPSC Ring Buffers
                                      |
                                      v
  +-----------------------------------------------------------------------+
  |              MetVoice Real-Time Core (MMCSS "Pro Audio")              |
  |                                                                       |
  |  +-----------------------------------------------------------------+  |
  |  | Input Strips (1..N)                                             |  |
  |  | - Pre-Gain (-60 dB to +12 dB)                                   |  |
  |  | - Constant-Power Pan (-3 dB Law)                                |  |
  |  | - 3-Band Parametric Biquad EQ (Direct Form II Transposed)       |  |
  |  | - Feed-Forward Soft-Knee Dynamic Range Compressor               |  |
  |  | - Fast Peak & RMS Metering (Lock-Free Envelopes)                |  |
  |  +-------------------------------+---------------------------------+  |
  |                                  |                                    |
  |                                  v                                    |
  |  +-----------------------------------------------------------------+  |
  |  | N x M Crosspoint Routing Matrix                                 |  |
  |  | - Dynamic Send Gains & Phase Control                            |  |
  |  | - Solo & Mute Arbitration Engine                                |  |
  |  +-------------------------------+---------------------------------+  |
  |                                  |                                    |
  |                                  v                                    |
  |  +-----------------------------------------------------------------+  |
  |  | Master Output Buses (1..M)                                      |  |
  |  | - Summing Accumulators                                          |  |
  |  | - Master Gain Stages                                            |  |
  |  | - Zero-Latency Soft-Knee Saturation Limiter (0 dBFS Ceiling)    |  |
  |  | - Master Peak / True Peak & Clip Telemetry                      |  |
  |  +-------------------------------+---------------------------------+  |
  +-----------------------------------+-----------------------------------+
                                      |
                           Lock-Free SPSC Ring Buffers
                                      |
                                      v
  +-----------------------------------------------------------------------+
  |                     Output Dispatch & Render Layer                    |
  |  - WASAPI Render Endpoints (Physical Headphones / Monitors)           |
  |  - Virtual Output Endpoints (Broadcast Stream / Discord Feed)         |
  +-----------------------------------------------------------------------+
```

---

## 2. Real-Time Threading & Synchronization Model

### MMCSS (Multimedia Class Scheduler Service)
Audio processing threads register with the Windows Multimedia Class Scheduler Service:
```cpp
DWORD task_index = 0;
HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
AvSetMmThreadPriority(hAvrt, AVRT_PRIORITY_CRITICAL);
```
This guarantees that thread execution receives prioritized CPU quanta and avoids preemption by standard background tasks.

### Real-Time Audio Callback Constraints
Inside `MixerEngine::process_block()` and WASAPI audio pump routines:
1. **Zero dynamic memory allocation:** No calls to `malloc`, `free`, `new`, `delete`, or operations resizing STL containers.
2. **Lock-Free synchronization:** No standard OS mutexes (`std::mutex`, `CRITICAL_SECTION`, `SRWLOCK`) which cause priority inversion.
3. **Non-blocking I/O:** No disk logging, console output, or blocking network/pipe calls.
4. **Cache-line isolation:** Shared variables across producer/consumer threads are aligned to 64-byte boundaries (`alignas(64)`) to eliminate false sharing.

---

## 3. Clock Drift Correction

When bridging physical USB devices (e.g., a USB microphone running on crystal oscillator A) to output audio devices (e.g., a soundcard running on oscillator B), their effective sample rates differ slightly (e.g., 48,000.5 Hz vs 47,999.8 Hz).

MetVoice solves this cleanly via `ClockDriftCorrector`:
- A closed-loop Proportional-Integral (PI) feedback loop observes the fill level of the intermediate ring buffer against the nominal target (50% capacity).
- An adaptive resample ratio $r = 1.0 + K_p \cdot e + K_i \int e\,dt$ is dynamically maintained.
- Audio is resampled in real-time using a 4-point Catmull-Rom cubic Hermite spline interpolation:
  $$f(t) = a \cdot t^3 + b \cdot t^2 + c \cdot t + d$$
  providing distortion-free resampling with negligible CPU overhead.

---

## 4. Digital Signal Processing (DSP) Engine

### Biquad Filter (Robert Bristow-Johnson Audio EQ Cookbook)
Implemented using Direct Form II Transposed for numerical stability and minimal delay registers:
- Low-Pass (12 dB/oct)
- High-Pass (12 dB/oct)
- Band-Pass
- Notch
- Peaking / Parametric Bell (with adjustable center frequency, Q, and gain in dB)
- Low Shelf & High Shelf

### Dynamic Range Compressor
Feed-forward compressor with soft-knee smoothing:
- Threshold: $-60\,\text{dB}$ to $0\,\text{dB}$
- Ratio: $1:1$ to $20:1$
- Ballistics: Exponential smoothing for attack and release:
  $$\alpha = \exp\left(-\frac{1}{\tau \cdot f_s}\right)$$
- Soft knee parabolic interpolation across $[T - W/2, T + W/2]$ to avoid harsh slope transitions.

### Soft-Knee Saturation Limiter
Clamps audio below the ceiling ($-0.1\,\text{dBFS}$) using a hyperbolic tangent rational transfer curve:
$$f(x) = \text{sign}(x) \cdot \left(x_0 + (C - x_0) \tanh\left(\frac{|x| - x_0}{C - x_0}\right)\right) \quad \text{for } |x| > x_0$$
This guarantees that summed signals across multiple strips never clip into digital wrap distortion.

---

## 5. Control & Inter-Process Communication (IPC)

Control of MetVoice is completely decoupled from the audio engine thread. External graphical user interfaces (Qt, Slint, Dear ImGui, Electron) and command-line tools interface through a Windows Named Pipe (`\\.\pipe\MetVoice_Control`):

- **Status & Telemetry:**
  `meters` -> Returns JSON containing Peak L/R, RMS L/R, Gain Reduction, and Limiter clip counts.
- **Channel Adjustments:**
  `gain <strip> <db>` -> Updates atomic linear gain factor.
  `pan <strip> <val>` -> Updates constant-power pan coefficients.
  `mute <strip> <0|1>` -> Atomically silences channel strip.
  `solo <strip> <0|1>` -> Engages exclusive solo listening mode.
  `route <strip> <bus> <0|1> [gain]` -> Toggles matrix crosspoint routing.
