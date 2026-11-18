# Clean-Room Engineering & Standards Compliance Statement

## 1. Clean-Room Policy
Muxella was conceived and developed strictly adhering to **clean-room software engineering principles**:
- **Zero Proprietary Decompilation:** Closed-source commercial binaries (including VB-Audio VoiceMeeter, Virtual Audio Cable, or similar commercial software) were neither decompiled, disassembled, nor reverse-engineered.
- **Independent Derivation:** All algorithms, mixing matrices, DSP biquad equations, lock-free ring buffer primitives, and synchronization routines were derived from first principles, published academic literature, and official Microsoft developer documentation.

## 2. Technical References & Open Standards
The design relies exclusively on documented public standards:
1. **Windows Core Audio APIs:**
   - Microsoft Learn documentation: *Core Audio APIs (WASAPI, MMDeviceAPI, DeviceTopology)*
   - Multimedia Class Scheduler Service (MMCSS) API (`AvSetMmThreadCharacteristics`)
2. **Audio Digital Signal Processing:**
   - Robert Bristow-Johnson: *Cookbook formulae for audio equalizer biquad filter coefficients* (Audio EQ Cookbook).
   - Zolzer, Udo: *DAFX - Digital Audio Effects*, John Wiley & Sons.
   - Catmull-Rom cubic Hermite spline interpolation for band-limited fractional sample delay.
3. **Hardware Audio Interface:**
   - Steinberg ASIO 2.0 Open Host Specification for multi-channel pro-audio interfaces.
4. **Lock-Free Concurrency:**
   - C++20 Standard Memory Model (`std::memory_order_acquire`, `std::memory_order_release`).
   - Single-Producer Single-Consumer (SPSC) lock-free bounded circular queue patterns.
