# Windows Virtual Audio Endpoints & Driver Layer Guide

This document analyzes the strategies for exposing virtual input and output endpoints in Windows, comparing driver-based and driverless approaches in a clean-room, standards-compliant architecture.

---

## 1. Architectural Approaches Overview

| Approach | Latency | Privileges Needed | Signing Requirements | Stability / Risk |
| :--- | :--- | :--- | :--- | :--- |
| **Microsoft Sysvad (KMDF)** | 2–5 ms | Kernel-mode driver | WHQL / EV Code Signing | High isolation; requires kernel driver build |
| **Audio Processing Objects (APO)** | 5–10 ms | User-mode (`audiodg.exe`)| Standard DLL signing | Runs in Windows audio engine service |
| **Driverless WASAPI Loopback** | 5–10 ms | Standard User-mode | None (Standard user app) | Extremely safe; no driver installation needed |
| **Bridged Virtual Audio Cable** | 3–8 ms | User-mode client | Leverages installed virtual endpoints | High flexibility; instant plug-and-play |

---

## 2. Approach A: Microsoft Sysvad Virtual Audio Driver (KMDF)

The official Microsoft reference sample for creating virtual audio endpoints is **Sysvad** (available in the Windows Driver Samples repository):

### How Sysvad Works
1. **Virtual WaveRT Miniport:**
   - Sysvad exposes a virtual `IMiniportWaveRT` and `IMiniportWaveRTStream` interface to Windows PortCls.
   - It registers two pins:
     - **Render Pin:** Appears to Windows applications as a Playback Device (e.g. *"MetVoice Virtual Input 1"*).
     - **Capture Pin:** Appears to Windows applications as a Recording Device (e.g. *"MetVoice Virtual Output 1"*).
2. **Buffer Sharing:**
   - When an application renders audio to the Virtual Render Pin, the driver places the PCM packets into a circular buffer allocated in Kernel Non-Paged Pool.
   - An IOCTL or shared memory section (`MmMapLockedPagesSpecifyCache`) allows the MetVoice user-mode audio engine to ingest the data without kernel-user copying overhead.
3. **Deployment Considerations:**
   - On 64-bit Windows 10/11, kernel drivers require Microsoft WHQL attestation signing or Secure Boot test signing (`bcdedit /set testsigning on`).

---

## 3. Approach B: Driverless Architecture via WASAPI Loopback

For users and environments where installing third-party kernel drivers is restricted (e.g. corporate laptops, gaming anti-cheat environments that flag unsigned drivers), MetVoice provides a 100% driverless mode:

### Native WASAPI Loopback Capture
Windows Vista and later natively support capturing any render endpoint's output stream:
```cpp
// Opening a Loopback Capture stream on an existing endpoint
IAudioClient* client = nullptr;
device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);

DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK;
client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, buffer_duration, 0, mix_format, nullptr);
```

By dedicating a secondary output device (or an existing unused HDMI/DisplayPort audio out channel) to an application, MetVoice captures the loopback stream into an Input Strip in real-time.

---

## 4. Application-Specific Audio Routing (Windows 10 / 11)

Windows 10 (Build 1803+) and Windows 11 introduce per-application endpoint assignment in `Settings -> System -> Sound -> Volume mixer`.

### Clean-Room Automation via Windows COM
Applications can be assigned programmatically using the undocumented COM interface `IAudioPolicyConfigFactory`:

```cpp
// CLSID: {2a59116d-6c4f-45e0-a74f-749e460e3d42}
// IID:   {ab21ac81-28f1-4d33-ac58-03164b5570e7}

MIDL_INTERFACE("ab21ac81-28f1-4d33-ac58-03164b5570e7")
IAudioPolicyConfigFactory : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
        DWORD processId,
        EDataFlow flow,
        ERole role,
        LPCWSTR deviceId) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(
        DWORD processId,
        EDataFlow flow,
        ERole role,
        LPWSTR* deviceId) = 0;

    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};
```

This allows MetVoice to direct targeted processes (e.g. `discord.exe`, `spotify.exe`, `game.exe`) into specific MetVoice virtual strips without user intervention.

---

## 5. Summary Recommendation

1. **Phase 1 (Production Ready):** Combine WASAPI Loopback and per-app routing with established virtual endpoints.
2. **Phase 2 (Hardware Emulation):** Provide optional signed KMDF Sysvad companion package for zero-latency virtual cables.
