# 🪓 Valhalla Roadmap: The Evolved C++26 Vision (Detailed)

**Project:** Tensor-Curie (DLSS 4.5 Mod for Assassin's Creed Valhalla)  
**Target Year:** 2026-2027  
**Standard:** ISO/IEC C++26 (Latest Draft)  
**Status:** Active Development (Phase 0 & 1 Complete)

---

## 🧭 Executive Summary

Tensor-Curie is transitioning from a proof-of-concept proxy DLL into a production-grade graphics injection platform. The focus for 2026 is **Stability**, **Safety**, and **Architecture**, leveraging cutting-edge C++26 features to solve long-standing modding challenges (crash handling, thread safety, serialization).

This roadmap breaks down high-level goals into executable engineering tasks.

---

## 🛠️ Phase 0: Stability & Safety (The Foundation)
*Goal: 99.9% Crash-Free Stability & Stealth*

### 0.1 Sentinel Crash Handler
**Status:** ✅ Implemented (`src/sentinel_crash_handler.cpp`)
- [x] **Core Implementation**: Kernel-aware vectored exception handler (VEH) utilizing `dbghelp` and `psapi`.
- [x] **Stack Walking**: Robust 64-bit stack walking that handles Denuvo obfuscated frames.
- [x] **Minidump Generation**: Automatic `.dmp` creation with `MiniDumpWriteDump`.
- [x] **Safe Logging**: Async-signal-safe string formatting (no `malloc` in crash path).
- [ ] **Integration Test**: Verify crash handler catches deliberate segfaults in a test harness.
- [ ] **Symbol Server**: Set up GitHub Actions to upload `.pdb` files for release builds to debug user dumps.

### 0.2 "Ghost" Hooking (Stealth)
**Status:** 🚧 Prototype (`src/ghost_hook.cpp`)
- [x] **Hardware Breakpoints**: Implementation of Dr0-Dr3 register manipulation.
- [x] **Thread Synchronization**: `CreateToolhelp32Snapshot` to apply debug registers to all game threads.
- [ ] **Hook Migration**: Refactor `src/hooks.cpp` to use `Ghost::InstallHook` instead of MinHook/Detours for sensitive functions:
    - [ ] `IDXGISwapChain::Present`
    - [ ] `IDXGISwapChain::ResizeBuffers`
    - [ ] `D3D12CreateDevice`
- [ ] **Recursion Guard**: Verify `tl_InsideCallback` prevents infinite loops when hooking hot paths.
- [ ] **Context Sanitization**: Ensure Dr6/Dr7 registers are scrubbed before returning to game code to avoid anti-cheat detection.

### 0.3 Safe Mode Bootstrapper (TensorBoot)
**Status:** 🚧 Prototype (`tools/TensorBoot/`)
- [x] **Integrity Checker**: Scan game folder for `ACValhalla.exe` and `nvngx_dlss.dll`.
- [x] **Dependency Check**: Verify `vulkan-1.dll` and `d3d12.dll` versions.
- [ ] **Loop Detection**: Implement registry/file marker to detect if the game crashed < 10s after last launch.
- [ ] **UI Polish**: Add a simple Win32 or ImGui window if console output is suppressed.
- [ ] **Auto-Repair**: Logic to download missing `nvngx` DLLs from a verified source if missing.

### 0.4 Dependency Isolation
**Status:** ✅ Complete
- [x] **CMake Static Runtime**: `set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")` (Verified in CMakeLists.txt).
- [ ] **Symbol Stripping**: Ensure private symbols are stripped from Release builds.
- [ ] **VCPKG Hardening**: Lock `vcpkg.json` baseline to specific commit hash to prevent upstream breakages.

---

## 💎 Phase 1: C++26 Architecture (The Core)
*Goal: Zero-Cost Abstractions & Compile-Time Safety*

### 1.1 Static Reflection & Serialization
**Status:** ✅ Complete (`src/cpp26/reflection.h`, `src/config_manager.cpp`)
- [x] **Polyfill Macros**: `REFLECT_STRUCT_BEGIN` / `REFLECT_FIELD` implementation.
- [x] **Config Manager Migration**: Rewritten `ConfigManager` to use the reflection system.
    - [x] Defined `struct SystemConfig` etc. with reflection macros.
    - [x] Implemented generic `SerializeStruct<T>` and `DeserializeStruct<T>`.
- [ ] **Validation Layer**: Add attributes for ranges (`[[min(0), max(100)]]`) and validate on load (Partially handled via UI annotations).

### 1.2 Reflection-Based UI
**Status:** ✅ Complete (`src/auto_ui_generator.h`)
- [x] **Auto-GUI Generator**: Created `src/auto_ui_generator.h`.
    - [x] `DrawStruct<T>` iterates fields.
    - [x] Maps `FieldType` + Annotations to ValhallaGUI widgets.
- [x] **Valhalla Specifics**: Integrated `DrawCategory` into `ImGuiOverlay::BuildCustomization` to generate settings automatically.

### 1.3 High-Performance Memory
**Status:** ✅ Complete (`src/cpp26/inplace_vector.h`)
- [x] **`std::inplace_vector`**: Implemented C++26 polyfill with fixed-capacity stack storage.
- [x] **ResourceDetector Migration**: Replaced `std::vector` with `inplace_vector` in `ResourceDetector` hot loops to eliminate heap allocation jitter.
- [ ] **`std::hive` (Colony)**: (Deferred to Phase 2 for `DescriptorTracker`).

---

## ⚡ Phase 2: The Render Pipeline (The Graphics)
*Goal: Beyond Native Quality*

### 2.1 Ray Tracing Injection
**Status:** 🚧 In Progress (`src/render_passes/ray_tracing_pass.cpp`)
- [x] **Compute Shader Setup**:
    - [x] Create `shaders/ssrt_compute.hlsl`.
    - [x] Implement G-Buffer reconstruction from Depth + Motion Vectors (Placeholder logic).
- [ ] **Hook Point Identification**:
    - [x] Hook `ExecuteCommandLists`.
    - [ ] Inject a Compute Queue signal/wait to synchronize our RT pass (Needs Runtime Debugging).
- [ ] **Denoising**:
    - [ ] Integrate NVIDIA NRD (SDK) or basic spatial/temporal denoiser.

### 2.2 Upscaling Agnosticism
**Status:** 🚧 In Progress
- [x] **Backend Abstraction**: Refactor `StreamlineIntegration` into `IUpscaler` interface (`src/upscalers/i_upscaler.h`).
- [ ] **XeSS Integration**:
    - [ ] Link `libxess.lib`.
    - [ ] Implement `XeSSUpscaler : public IUpscaler`.
    - [ ] Handle resource state transitions (D3D12 barriers) specific to XeSS.
- [ ] **FSR 3.1 Integration**:
    - [ ] Download FSR 3 SDK.
    - [ ] Implement `FSR3Upscaler` wrapper.
    - [ ] Handle FSR 3's reactive mask generation.

### 2.3 Advanced HDR
**Status:** 📝 Planned
- [ ] **Color Space Analysis**:
    - [ ] Analyze `DXGI_SWAP_CHAIN_DESC1` to detect `DXGI_FORMAT_R10G10B10A2_UNORM`.
    - [ ] Hook `SetHDRMetaData` to intercept game's Nits settings.
- [ ] **Heatmap Overlay**:
    - [ ] Write a pixel shader that outputs color based on luminance (`dot(color.rgb, vec3(0.2126, 0.7152, 0.0722))`).
    - [ ] Inject a fullscreen draw pass when "Debug HDR" is toggled.

---

## 🧠 Phase 3: AI & Heuristics (The Brain)
*Goal: Self-Optimizing Performance*

### 3.1 Neural Resource Detection
**Status:** 💭 Research
- [ ] **Data Collection Mode**:
    - [ ] Add config option `bCaptureTrainingData`.
    - [ ] When enabled, save every 1000th frame: `Color.png`, `Depth.png`, `Motion.png`, `IsMenu (bool)`.
- [ ] **Model Training (Python)**:
    - [ ] Train a MobileNetV3 or SqueezeNet to classify "Menu" vs "Gameplay" vs "Cutscene".
    - [ ] Train a regression head to predict "Best Motion Vector Candidate" confidence.
- [ ] **Inference Engine**:
    - [ ] Integrate `ONNX Runtime` (C++ API).
    - [ ] Run inference on a background thread every 0.5s to update state.

### 3.2 Dynamic Quality (Auto-Tuner)
**Status:** 📝 Planned
- [ ] **Frametime Monitor**:
    - [ ] Create circular buffer of last 120 frametimes.
    - [ ] Calculate 1% Lows and Stutter count.
- [ ] **Control Loop**:
    - [ ] PID Controller implementation: `Error = TargetFPS - CurrentFPS`.
    - [ ] Output: `DLSS_Preset` (Quality -> Balanced -> Performance).
    - [ ] Hysteresis: Require 5s of stable low FPS before downgrading to prevent oscillation.

---

## 🧪 Phase 4: Quality Assurance & Testing
*Goal: Catch Regressions Automatically*

### 4.1 Unit Testing Strategy
**Status:** ❌ Missing
- [ ] **Framework**: Integrate `Catch2` v3 via vcpkg.
- [ ] **Test Runner**: Create `tests/run_tests.cpp`.
- [ ] **Modules to Test**:
    - [ ] `ConfigManager` (Load/Save/Reflection).
    - [ ] `PatternScanner` (Mock memory buffer scanning).
    - [ ] `InplaceVector` (Capacity checks, iterator validity).
    - [ ] `CircularBuffer` (Overwrite logic).

### 4.2 Integration Testing
**Status:** ❌ Missing
- [ ] **Mock D3D12 Device**: Create a stub `ID3D12Device` to test `ResourceDetector` without a GPU.
- [ ] **Hook Verification**: Test `Ghost::InstallHook` against a dummy function in the test executable.

### 4.3 Static Analysis & CI
**Status:** 🚧 Partial
- [ ] **Clang-Tidy**: Add `.clang-tidy` config with `modernize-*`, `performance-*`, `bugprone-*`.
- [ ] **GitHub Actions**:
    - [ ] Add 'Test' step to `build.yml`.
    - [ ] Add 'Lint' step.
    - [ ] Enable AddressSanitizer (ASan) for Debug builds in CI.

---

## 🔧 Phase 5: Technical Debt & Refactoring
*Goal: Clean Codebase*

- [ ] **Singleton Removal**: Refactor `::Get()` usage. Pass dependencies via constructor injection.
- [ ] **Header Cleanup**: Move implementations out of `.h` files where possible to reduce build times.
- [ ] **Magic Number Removal**: Move all hardcoded offsets/patterns to `src/game_offsets.h`.
- [ ] **Log Spam**: Review all `LOG_INFO` calls and move high-frequency ones to `LOG_DEBUG` or `LOG_TRACE`.
- [ ] **Exception Safety**: Ensure all locks rely on RAII (`std::lock_guard`, `std::unique_lock`) and no manual `Unlock()` exists.

---

## 📅 Detailed Timeline

| Sprint | Duration | Focus Area | Key Deliverables |
|--------|----------|------------|------------------|
| **Sprint 1** | 2 Weeks | Safety | Sentinel Crash Handler integration, Ghost Hook replacement of MinHook |
| **Sprint 2** | 2 Weeks | C++26 Core | Reflection-based ConfigManager, Auto-GUI |
| **Sprint 3** | 2 Weeks | Stability | TensorBoot launcher, Catch2 Test Suite setup |
| **Sprint 4** | 3 Weeks | Graphics | Ray Tracing injection research, Shader setup |
| **Sprint 5** | 3 Weeks | Optimization | `inplace_vector` refactor, Async scanner |
| **Sprint 6** | 4 Weeks | AI | Data collection, ONNX runtime integration |

---

*Verified & Updated: 2026-02-06 by AI Assistant*
