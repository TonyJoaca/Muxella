# Contributing to Muxella & Muxella

Thank you for your interest in contributing to Muxella! We welcome issues, documentation improvements, and pull requests.

---

## 1. Clean-Room Policy (Strict Requirement)

Muxella is engineered strictly following **clean-room software development practices**. To protect the project, its maintainers, and its contributors:
- **Never submit proprietary code:** Code extracted, decompiled, or disassembled from commercial closed-source software (such as VB-AUDIO VoiceMeeter, Virtual Audio Cable, or similar software) is strictly prohibited.
- **Reference only documented standards:** Rely only on public specifications, standard Windows SDK APIs (WASAPI, MMCSS, COM), and published academic literature for DSP algorithms.
- **Attribution:** If integrating or adapting open-source algorithms, verify license compatibility and attribute the original authors in `THIRD_PARTY_NOTICES.md`.

---

## 2. Development Setup

### Prerequisites
- Windows 10 or Windows 11 (x64)
- C++20 compliant compiler (GCC 12+, Clang 15+, or MSVC 2022+)
- CMake 3.20+
- Python 3.10+ (with `PyQt6`, `pystray`, `psutil`)

### Building the C++ Engine
```powershell
# Configure CMake
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# Build all libraries, daemon, and tests
cmake --build build -j4
```

### Running the Test Suite
```powershell
# Run C++ Unit Tests
ctest --test-dir build --output-on-failure

# Run GUI Unit Tests
python tests/test_b_bus_gui.py
```

### Running the GUI in Development
```powershell
# Run directly with Python
python muxella_gui.py
```

---

## 3. Real-Time Audio Coding Standards

When contributing to `muxella_core`, `muxella_dsp`, or `muxella_matrix`:
1. **Zero Dynamic Allocation:** Never call `malloc`, `new`, `std::vector::push_back`, or allocate heap memory inside active audio callbacks.
2. **Lock-Free Concurrency:** Use Single-Producer Single-Consumer (SPSC) ring buffers with acquire-release memory order. Never acquire mutexes or perform blocking I/O on the real-time audio thread.
3. **Floating-Point Audio:** Internal signal processing operates strictly on 32-bit floating point (`float`, normalized `[-1.0, 1.0]`).
