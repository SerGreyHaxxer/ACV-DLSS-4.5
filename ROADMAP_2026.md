# Tensor-Curie 2026 Roadmap

**Last Updated:** February 2026
**Project Version:** 4.5.0

---

## Current State Summary

The project is a functional DXGI proxy DLL that injects NVIDIA DLSS 4.5
(Super Resolution, Frame Generation, Ray Reconstruction, DeepDVC) into
Assassin's Creed Valhalla via Streamline SDK. The codebase has:

- **Build system**: CMake 3.25+ with vcpkg, CMakePresets (dev/release/ci/vs2022)
- **Language**: C++23 (MSVC)
- **CI/CD**: GitHub Actions (build + artifact upload)
- **Code quality**: .clang-format, .editorconfig, vcpkg baseline pinning
- **GUI**: Custom D2D overlay with modern dark theme and visual debugger
- **Hooking**: MinHook for VTable interception
- **Config**: TOML-based with thread-safe hot-reload (`m_configMutex`)
- **Concurrency**: Documented 5-level lock hierarchy, `shared_mutex` for hot-path reads
- **Logging**: spdlog (async, structured)
- **Crash handling**: Safe VEH with Win32 API (no CRT in crash path)
- **GPU analysis**: Precompiled compute shader (embedded bytecode, no d3dcompiler.dll)
- **Smart FG**: Rolling-average adaptive frame generation multiplier

**What's missing**: No tests, singleton-heavy architecture.

---

## Phase 1: Build Quality & Developer Experience _(Complete)_

_Goal: Make the project reproducible, formattable, and CI-ready._

- [x] **CMakePresets.json** — Standardized Dev / Release / CI configurations
- [x] **vcpkg-configuration.json** — Pin dependency registry for reproducible builds
- [x] **.clang-format** — Enforce consistent code style across all source files
- [x] **.editorconfig** — Cross-editor settings (encoding, indent, line endings)
- [x] **GitHub Actions CI** — Automated build verification on push/PR
- [x] **CMakeLists.txt hardening** — Version embedding, install target, preset support
- [x] **.gitignore cleanup** — Cover CMake user presets, IDE caches, generated files
- [x] **Precompile compute shader** — Embed bytecode as C++ header, remove `d3dcompiler.dll` runtime dependency
- [x] **Delete obsolete `ROADMAP.md`** — Single source of truth for roadmap

---

## Phase 2: Thread Safety & Concurrency _(Complete)_

_Goal: Eliminate data races and define a clear locking strategy._

- [x] **Upgrade `ResourceDetector` to `std::shared_mutex`** — 99% of access is
      reads; a shared lock eliminates contention on the hot path
- [x] **Document lock hierarchy** — Strict acquisition order defined:
      `SwapChain(1) > Hooks/Init(2) > Resources/Samplers/Descriptors/Camera(3) > Config/Input(4) > Logging(5)`
      with comments at every mutex declaration site
- [x] **Audit all `std::atomic` usage** — Fixed inconsistent orderings in
      `ResourceDetector`, `ImGuiOverlay`, and `dxgi_wrappers`; standardized
      `acquire`/`release` for thread-control flags, `relaxed` for metrics
- [x] **Thread-safe config hot-reload** — `Load()` now parses into a temporary
      `ModConfig` and swaps under `m_configMutex`; `Save()` snapshots under lock;
      added `DataSnapshot()` for safe cross-thread reads

---

## Phase 3: Testing Infrastructure _(Next)_

_Goal: Enable automated correctness verification without a GPU._

- [ ] **Integrate Catch2** via vcpkg — Add test executable target to CMake
- [ ] **ConfigManager tests** — Load / Save / Validation / Hot-reload round-trip
- [ ] **PatternScanner tests** — Known-good byte patterns against synthetic data
- [ ] **ResourceDetector scoring tests** — Verify heuristic scores on mock
      `D3D12_RESOURCE_DESC` inputs
- [ ] **Input handler tests** — Hotkey registration, callback dispatch, key name
      lookup
- [ ] **CI test step** — Run `ctest` in the GitHub Actions workflow

---

## Phase 4: Architectural Decoupling

_Goal: Break circular dependencies and enable isolated testing._

- [ ] **`ProxyContext` struct** — Single owner of all system instances, created
      in `main.cpp`, passed by reference to `Initialize()` methods
- [ ] **Remove `::Get()` singletons** — `StreamlineIntegration`, `ImGuiOverlay`,
      `InputHandler`, `ConfigManager`, `ResourceDetector`, `HeuristicScanner`,
      `HookManager` (7 singletons total)
- [ ] **Event bus** — `ConfigManager` fires `OnConfigChanged`;
      `StreamlineIntegration` and `ImGuiOverlay` subscribe. Replace direct
      cross-system calls
- [ ] **Interface abstractions** — Extract `IRenderer`, `IConfig`, `IHookManager`
      interfaces so test doubles can be injected

---

## Phase 5: Performance Optimization

_Goal: Reduce per-frame overhead and startup cost._

- [ ] **SIMD pattern scanner** — Replace byte-by-byte `PatternScanner` with
      AVX2 vectorized scanning (8-16x speedup on large memory regions)
- [ ] **Descriptor tracker optimization** — Current `std::unordered_map` lookup
      on every `CreateShaderResourceView` call; consider flat hash map or
      direct-mapped array for common descriptor sizes
- [ ] **Lazy NvAPI initialization** — Defer `NvAPI_Initialize()` to first
      metrics query instead of overlay startup
- [ ] **Profile hot paths** — Use PIX or Superluminal to identify actual
      bottlenecks in the per-frame hook chain

---

## Phase 6: Advanced Features _(In Progress)_

_Goal: Push capabilities beyond basic DLSS injection._

- [x] **Dynamic Smart Frame Gen** — Rolling-average FPS (10-sample window)
      automatically adjusts FG multiplier: ≤20 FPS → 4x, ≤40 → 3x, ≤70 → 2x,
      >70 → user setting. Includes auto-disable above configurable threshold.
      Logs transitions. Exposes rolling avg + computed mult to GUI.
- [x] **Visual debugger overlay** — "Internals" section (debug mode) showing
      hook status (Streamline, DLSS, FG, RR, DeepDVC, HDR, keyboard hook),
      resource detection state (color/depth/MV buffers with resolution and format),
      Smart FG telemetry, camera data, build version
- [ ] **Plugin / ASI loader** — Allow loading additional DLLs through the proxy
      with an exposed C API for overlay drawing
- [ ] **Auto-update checker** — Optional HTTP check against a GitHub releases
      endpoint to notify users of new versions

---

## Phase 7: Long-Term / Exploratory

- [ ] **ML resource detection** — Replace heuristic scoring with a tiny decision
      tree or random forest trained on texture properties
- [ ] **Multi-game support** — Abstract game-specific constants (exe name,
      resource patterns) into per-game profile files
- [ ] **Web control interface** — Embedded HTTP server for second-screen
      control from phone/tablet on the local network
- [ ] **Replay buffer** — Capture last N seconds of DLSS-enhanced frames for
      comparison screenshots
