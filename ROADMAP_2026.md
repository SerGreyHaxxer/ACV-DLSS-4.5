# Tensor-Curie 2026: Modernization Roadmap (Active)

**Current Status:** Phase 1 & 2 COMPLETE. Phase 3 & 8 are CRITICAL PRIORITY.

---

## ✅ COMPLETED PHASES

### Phase 1: Tech Debt Elimination (Done)
- [x] `ModConfig` reference members removed (Trivially Copyable)
- [x] `d3d12_wrappers.cpp` split into modular components
- [x] Dead code/artifacts purged
- [x] `LogStartup` made thread-safe and lazy

### Phase 2: C++23 Modernization (Done)
- [x] `std::expected` error handling implemented
- [x] C-style casts and CRT functions replaced (`std::format`, `std::ofstream`)
- [x] `constexpr` / strong typing for VTables and configs
- [x] `std::string_view` adoption

---

## 🚨 IMMEDIATE PRIORITY (Do This Next)

### Phase 8: Safety & Crash Handling (URGENT)
*Why: Your current crash handler calls `fopen` and `fprintf` inside a VEH. If the crash happens inside `malloc` or a CRT lock, the crash handler itself will deadlock, freezing the game forever instead of logging.*

- [ ] **Rewrite `src/crash_handler.cpp`**:
  - Remove all CRT calls (`fopen`, `fprintf`, `time`).
  - Use `CreateFileW`, `WriteFile`, `GetSystemTimeAsFileTime` (Win32 API is async-signal-safe).
  - Pre-allocate a 4KB static buffer for the log message (no `std::string` or allocations during crash).
- [ ] **Isolate SEH in `hooks.cpp`**:
  - Move `__try`/`__except` blocks to purely C-style helper functions to avoid C++ object unwinding issues.

### Phase 3: Architectural Decoupling
*Why: `StreamlineIntegration` calls `ImGuiOverlay`, which calls `ConfigManager`, which calls `StreamlineIntegration`. You cannot test these in isolation.*

- [ ] **Kill the Singletons**:
  - Create a `ProxyContext` struct in `main.cpp`/`proxy.cpp` that holds unique pointers to all systems.
  - Pass `ProxyContext&` to `Initialize()` methods.
  - Remove `Get()` methods from `StreamlineIntegration`, `ImGuiOverlay`, `InputHandler`.
- [ ] **Event Bus Implementation**:
  - Replace direct calls (e.g., `UpdateControls`) with a simple listener pattern.
  - `ConfigManager` fires `OnConfigChanged`.
  - `StreamlineIntegration` and `ImGuiOverlay` subscribe to it.

---

## 🚧 HIGH PRIORITY (Performance & Stability)

### Phase 4: Thread Safety & Concurrency
*Current State: `g_swapChainMutex`, `ResourceDetector::m_mutex`, and `s_startupTraceMutex` are used without a defined order.*

- [ ] **Document Lock Hierarchy**:
  - Define strict order: `SwapChain` > `Hooks` > `Resources` > `Logging`.
- [ ] **Optimize `ResourceDetector`**:
  - Replace `std::mutex` with `std::shared_mutex` (99% of access is reading resource states).
  - Use `std::atomic` for `m_bestColorResource` and score tracking to avoid locking on every frame.

### Phase 5: Build System Hardening
- [ ] **Dependency Pinning**: `vcpkg.json` currently uses "latest". Pin versions to ensure reproducible builds.
- [ ] **CMake Presets**: Create `CMakePresets.json` for standardized "Dev", "CI", and "Release" builds.

---

## 🔮 MEDIUM/LONG TERM

### Phase 6: Testing Strategy
- [ ] **Unit Tests**: Add `Catch2`. Test `ConfigManager` IO and `PatternScanner` logic (no GPU needed).
- [ ] **Integration Tests**: Create a "Null Device" test that initializes the proxy without a real game.

### Phase 7: Performance Optimization
- [ ] **SIMD Scanning**: `PatternScanner` is byte-by-byte. Implement AVX2 scanning.
- [ ] **Precompiled Shaders**: Embed `resource_analysis.hlsl` bytecode as a C++ array header to remove runtime compilation and `d3dcompiler.dll` dependency.

### Phase 9: AI Features
- [ ] **ML Resource Detection**: Replace heuristic scoring with a tiny Decision Tree trained on texture properties.
- [ ] **Smart Frame Gen**: Use rolling average FPS to dynamically toggle FG multipliers.

### Phase 10: Tooling
- [ ] **Visual Debugger**: Add an "Internals" tab to the ImGui overlay showing hook status and resource confidence scores.
