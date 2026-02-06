# Tensor-Curie 2026: The Mega Modernization Roadmap

**Vision:** Transform Tensor-Curie from a functional proof-of-concept into an engineering-grade, 2026-level C++ codebase — zero-compromise stability, blazing performance, and the kind of architecture that survives game patches without blinking.

**Privacy:** Strict "No-Home" Policy. Zero analytics, zero telemetry, zero data collection. Ever.

**Codebase Audit Summary:** Deep-scan of all 22 source files (~5,500+ lines of implementation code) revealed **87 actionable improvements** across 10 phases. This roadmap is ordered by impact and dependency — Phase 1 unblocks everything else.

---

## Phase 1: Scorched Earth — Kill the Tech Debt ✅ COMPLETE

*Goal: Rip out the anti-patterns that are actively making every other improvement harder.*

### 1.1 Nuke the `ModConfig` Reference Members
**Severity: CRITICAL** — `config_manager.h` lines 81-133

The `ModConfig` struct had **~50 reference member aliases** (`int& dlssMode = dlss.mode;` etc.) that existed only for "legacy compatibility." This was a ticking time bomb:
- Made the struct **non-copyable and non-movable** (references can't be rebound)
- `ResetToDefaults()` had to work around it by resetting sub-structs individually
- Prevented any future serialization/reflection system

- [x] **Deleted all `&` reference aliases** from `ModConfig`
- [x] **Search-and-replaced** all usages (e.g., `cfg.dlssMode` → `cfg.dlss.mode`, `cfg.deepDvcIntensity` → `cfg.dvc.intensity`) across the entire codebase (~225+ replacements)
- [x] Made `ModConfig` a **trivially copyable aggregate** — `ResetToDefaults()` is now just `m_config = ModConfig{}`
- [x] Added `static_assert(std::is_trivially_copyable_v<ModConfig>)` as a guard

### 1.2 Split the 64KB God-File: `d3d12_wrappers.cpp`
**Severity: HIGH** — Single file was ~1,180 lines, now ~490 lines

- [x] Extracted `camera_scanner.h/.cpp` — all `CameraCandidate`, `TryScan*`, `UpdateCameraCache`, CBV tracking (~600 lines)
- [x] Extracted `descriptor_tracker.h/.cpp` — `DescriptorRecord`, `TrackDescriptorHeap`, `TrackDescriptorResource` (~80 lines)
- [x] Extracted `sampler_interceptor.h/.cpp` — `SamplerRecord`, `ApplySamplerLodBias`, `RegisterSampler`, `ClearSamplers` (~60 lines)
- [x] `d3d12_wrappers.cpp` is now pure COM wrapper pass-throughs only

### 1.3 Purge Dead Code and Committed Build Artifacts
**Severity: MEDIUM**

- [x] Deleted `iat_utils.h` — every function was a `return false;` stub
- [x] Removed all `.obj` files from repo root (21 files)
- [x] `.gitignore` already had `*.obj` ✓
- [x] Removed the commented-out factory wrapping code in `proxy.cpp` (3 identical blocks of dead code)
- [x] Deleted `WrappedIDXGISwapChain` dead declaration (~70 lines) from `dxgi_wrappers.h`

### 1.4 Fix the `LogStartup` Performance Disaster
**Severity: MEDIUM** — `proxy.cpp`

- [x] Replaced with a **single static `FILE*`** opened once (lazy), flushed per-write
- [x] Added `std::mutex` protection
- [x] Added cleanup in `CleanupProxyGlobal()` to close the file handle

---

## Phase 2: Modern C++23/26 Language Upgrade ✅ COMPLETE

*Goal: Write code that a 2026 C++ developer would recognize as modern.*

### 2.1 `std::expected` Error Handling ✅ COMPLETE
Created `error_types.h` with `ProxyError`, `ScanError` enums + `to_string()`, `HookResult`, `ScanResult<T>`, `ProxyResult` type aliases. Refactored `HookManager::CreateHookInternal()` → `HookResult` (returns `std::expected<void, MH_STATUS>`). Refactored `PatternScanner::Scan()` → `ScanResult<uintptr_t>` with typed error codes.

- [x] Create typed error enums and `std::expected` aliases in `error_types.h`
- [x] Refactor `PatternScanner::Scan()` → `std::expected<uintptr_t, ScanError>`
- [x] Refactor `HookManager::CreateHook()` → `std::expected<void, MH_STATUS>`

### 2.2 Eliminate Every C-ism ✅ COMPLETE
Replaced all C-isms across 11 source files: `sprintf_s`/`fprintf`/`fopen_s` → `std::format`+`std::ofstream`, `(float)atof()` → `std::from_chars`, all C-style casts → `static_cast`/`reinterpret_cast`, `memcpy` → `std::copy_n`, `char buf[]`/`wchar_t[]` → `std::array`, removed `<stdio.h>`, replaced `NULL` → `nullptr`, `time.h` → `<ctime>`.

- [x] proxy.cpp: `FILE*`→`std::ofstream`, `sprintf_s`→`std::format`, `wchar_t[]`→`std::array`, C casts→`static_cast`
- [x] pattern_scanner.cpp: all C casts→proper casts, removed `<stdio.h>`
- [x] hooks.cpp: all `(void*)`→`reinterpret_cast`, magic numbers→`VTableIndex` enums
- [x] camera_scanner.cpp: `memcpy`→`std::copy_n`
- [x] streamline_integration.cpp/h: `memcpy`→`std::copy_n`, C casts→`static_cast`
- [x] input_handler.cpp: `char buf[]`→`std::array`, C cast→`reinterpret_cast`
- [x] config_manager.cpp: `wchar_t[]`→`std::array`, `atof`→`std::from_chars`, `char buf[]`→`std::array`
- [x] crash_handler.cpp: C headers→C++ headers, C casts→proper casts, `NULL`→`nullptr`
- [x] heuristic_scanner.cpp: C casts→`static_cast`
- [x] imgui_overlay.cpp: remaining C casts→`static_cast`

### 2.3 `constexpr` and `consteval` Everything ✅ COMPLETE
Complete rewrite of `dlss4_config.h` from 40+ `#define` macros to `inline constexpr` constants organized in 6 namespaces: `dlss4::`, `camera_config::`, `resource_config::`, `dvc_config::`, `streamline_config::`. All consumer files updated.

- [x] Convert all `#define` constants to `inline constexpr`
- [x] Group into namespaces: `dlss4`, `camera_config`, `resource_config`, `dvc_config`, `streamline_config`
- [x] Use `std::string_view` for string constants
- [x] Update all 6 consumer files

### 2.4 Concepts & Strong Types ✅ COMPLETE
Added `vtable::Device`, `vtable::CommandList`, `vtable::CommandQueue` scoped enum classes with named vtable indices. Added `D3D12Hookable` concept. Added type-safe `GetVTableEntry<R, E>()` helper. All magic numbers in hooks.cpp replaced.

- [x] Create VTableIndex enum classes in `vtable_utils.h`
- [x] Create `D3D12Hookable` concept
- [x] Replace all magic vtable numbers in `hooks.cpp`

### 2.5 Ranges and Views — Deferred
`std::span` for `float[16]` matrix params would require changing public API signatures in camera_scanner.h and streamline_integration.h. Deferred to Phase 5 (API redesign) to avoid breaking changes. Manual `for` loops over callbacks are already simple and clear. `ParsePattern()` is a performance-critical path where ranges overhead is not justified.

- [x] Evaluated feasibility — deferred to Phase 5 for API-level changes

---

## Phase 3: Architecture Renaissance 🏛️

*Goal: Decouple the monoliths. Make every component independently testable.*

### 3.1 Dependency Injection — Kill the Singletons
Every major class uses `static Foo& Get()`. This is untestable, creates hidden initialization order dependencies, and couples everything.

- [ ] Create a `ProxyContext` struct that owns all subsystems:
  ```cpp
  struct ProxyContext {
      std::unique_ptr<ConfigManager> config;
      std::unique_ptr<HookManager> hooks;
      std::unique_ptr<StreamlineIntegration> streamline;
      std::unique_ptr<ResourceDetector> detector;
      std::unique_ptr<ImGuiOverlay> overlay;
      std::unique_ptr<InputHandler> input;
      std::unique_ptr<PatternScanner> scanner;
      std::unique_ptr<HeuristicScanner> heuristics;
  };
  ```
- [ ] Pass `ProxyContext&` to systems that need cross-system access
- [ ] Keep a single `static ProxyContext*` only at the DllMain/proxy level
- [ ] Add a `TestProxyContext` factory for unit testing with mock implementations

### 3.2 State Machine for StreamlineIntegration
Currently 265 lines of header with ~60 member variables and no state transitions. If `slInit` fails, half the class is in an undefined state.

- [ ] Define explicit states:
  ```cpp
  enum class StreamlineState { Uninitialized, Initializing, Ready, Active, Error, ShuttingDown };
  ```
- [ ] Guard all method calls with state checks (not just `m_initialized`)
- [ ] Implement graceful degradation: DLSS-G fails → fall back to DLSS-only → fall back to passthrough
- [ ] Move the 30+ setters into a single `ApplyConfig(const ModConfig&)` method
- [ ] Make `m_optionsDirty` a proper change-tracking system (dirty flags per-feature, not global)

### 3.3 Event System
Replace the tight coupling between ImGuiOverlay ↔ StreamlineIntegration ↔ ConfigManager ↔ InputHandler.

- [ ] Implement a lightweight observer/signal pattern:
  ```cpp
  Signal<void(const ModConfig&)> onConfigChanged;
  Signal<void(float baseFps, float totalFps)> onFrameTiming;
  Signal<void(int vKey)> onHotkeyPressed;
  ```
- [ ] `ConfigManager` fires `onConfigChanged` → StreamlineIntegration and ImGuiOverlay both subscribe
- [ ] Eliminates the circular `StreamlineIntegration::UpdateControls()` → `ImGuiOverlay::UpdateControls()` call chain

### 3.4 Proper COM Wrapper Architecture
The current system has TWO parallel interception mechanisms: COM wrappers (`WrappedID3D12Device` etc.) AND VTable hooks (`hooks.cpp`). Only the VTable hooks are actually active — the wrappers exist but the factory wrapping that would install them is disabled.

- [ ] **Make a decision**: Either go all-in on VTable hooks (simpler, less code) or all-in on COM wrappers (more robust, more code). Delete the other
- [ ] If keeping VTable hooks: delete `WrappedID3D12Device`, `WrappedID3D12CommandQueue`, `WrappedID3D12GraphicsCommandList` (~500 lines of dead boilerplate)
- [ ] If keeping COM wrappers: fix the factory wrapping that was disabled due to crashes, remove the VTable hook logic
- [ ] **Recommendation**: Keep VTable hooks (proven working), delete wrappers. Less surface area = fewer bugs

---

## Phase 4: Thread Safety & Concurrency Overhaul 🔒

*Goal: Stop the data race roulette. Every mutex should be justified and documented.*

### 4.1 Audit All Shared State
Current state: 7 different `std::mutex` objects scattered across anonymous namespaces with no documentation on lock ordering.

- [ ] Create a lock hierarchy document:
  ```
  Level 0 (outermost): g_swapChainMutex
  Level 1: g_HookMutex
  Level 2: g_cameraMutex, g_cbvMutex, g_descriptorMutex, g_samplerMutex
  Level 3: m_callbackMutex (InputHandler)
  ```
- [ ] Add `[[nodiscard]]` lock guards and annotate with Clang Thread Safety Analysis attributes
- [ ] Replace `std::mutex` with `std::shared_mutex` for read-heavy data (resource candidates, camera data)

### 4.2 Lock-Free Hot Path
The per-frame path (`HookedExecuteCommandLists` → `ResourceDetector::NewFrame` → `StreamlineIntegration::TagResources`) acquires multiple mutexes per frame.

- [ ] Make `ResourceDetector` candidates use `std::atomic` for the "best" pointers and scores
- [ ] Use a lock-free SPSC queue for CBV address tracking (producer = hook thread, consumer = camera scanner)
- [ ] Use `std::atomic<uint64_t>` for frame counters (already done in some places, inconsistent in others)
- [ ] Profile mutex contention with `tracy` or equivalent

### 4.3 Fix the Timer Thread
`dxgi_wrappers.cpp` runs a background thread at 60Hz that calls into StreamlineIntegration, ImGuiOverlay, InputHandler, and ConfigManager. This is a nightmare for thread safety.

- [ ] The timer thread should NOT call `ImGuiOverlay::Render()` — ImGui rendering must happen on the same thread as D3D12 command list submission
- [ ] Move ImGui rendering into the Present hook (where it belongs)
- [ ] The timer thread should only handle: FPS calculation, config hot-reload, input polling
- [ ] Use `std::jthread` with `std::stop_token` instead of manual `std::atomic<bool>` + `condition_variable`

---

## Phase 5: Build System & CI/CD Hardening 🏗️

*Goal: A single `cmake --preset release` and everything works.*

### 5.1 Fix CMakeLists.txt Anti-Patterns
- [ ] Replace `file(GLOB_RECURSE SRC_FILES ...)` with explicit file lists — GLOB doesn't trigger reconfiguration when files are added/removed
- [ ] Replace `include_directories()` with `target_include_directories(${PROJECT_NAME} PRIVATE ...)` — global include dirs leak to consumers
- [ ] Add compile warning flags:
  ```cmake
  target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX /permissive- /Zc:__cplusplus)
  ```
- [ ] Add Address Sanitizer support for debug builds:
  ```cmake
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      target_compile_options(${PROJECT_NAME} PRIVATE /fsanitize=address)
  endif()
  ```
- [ ] Create CMake presets (`CMakePresets.json`) for Debug, Release, RelWithDebInfo, and CI configurations
- [ ] Remove `_CRT_SECURE_NO_WARNINGS` — fix the actual warnings instead of suppressing them

### 5.2 Dependency Hardening
- [ ] Pin vcpkg dependency versions in `vcpkg.json` (currently floating):
  ```json
  { "name": "spdlog", "version>=": "1.14.0" }
  ```
- [ ] Add `vcpkg-configuration.json` to pin the vcpkg baseline
- [ ] Consider replacing `nlohmann-json` with `glaze` for 10x faster JSON (if JSON is ever needed alongside TOML)
- [ ] Add `magic_enum` to replace manual enum-to-string conversions

### 5.3 CI/CD Pipeline
- [ ] **GitHub Actions** workflow:
  - Build matrix: MSVC 2022 + ClangCL
  - Static analysis: `clang-tidy` with Core Guidelines profile
  - Artifact: Upload `dxgi.dll` as release asset
- [ ] Add `.clang-format` (LLVM or Microsoft style)
- [ ] Add `.clang-tidy` with these checks:
  ```
  Checks: 'modernize-*, performance-*, readability-*, bugprone-*, cppcoreguidelines-*'
  ```
- [ ] Pre-commit hook: format check + include-what-you-use

---

## Phase 6: Testing — From Zero to Confidence ✅

*Goal: You should be able to refactor anything and know in 30 seconds if it's broken.*

### 6.1 Unit Test Framework
- [ ] Add `catch2` to vcpkg.json and CMake
- [ ] Create `tests/` directory with CMake target `tensor-curie-tests`

### 6.2 Testable Components (No GPU Required)
- [ ] **ConfigManager tests**: Load → Save → Load roundtrip, schema validation, corrupt file handling, legacy INI migration
- [ ] **PatternScanner tests**: Scan known patterns in synthetic byte arrays, wildcard handling, cache invalidation
- [ ] **Camera heuristics tests**: Feed known view/projection matrices, verify scoring, test edge cases (identity matrix, all-zeros, NaN)
- [ ] **InputHandler tests**: Register/unregister hotkeys, key name resolution, duplicate key handling

### 6.3 Integration Test Harness (Mock GPU)
- [ ] Create a minimal D3D12 "null device" using WARP adapter
- [ ] Test that the proxy DLL loads, hooks install, and basic Present calls succeed
- [ ] Test resource detection with synthetic render targets
- [ ] Test overlay initialization and teardown

### 6.4 Fuzz Testing
- [ ] Fuzz `PatternScanner::ParsePattern` with random strings
- [ ] Fuzz `ConfigManager::Load` with corrupt/adversarial TOML files
- [ ] Fuzz VTable index access with bounds checking enabled

---

## Phase 7: Performance — Zero Wasted Cycles ⚡

*Goal: The proxy should be invisible in frame time graphs.*

### 7.1 SIMD Pattern Scanning
`pattern_scanner.cpp` currently scans byte-by-byte. For a module that can be 100MB+, this is painfully slow.

- [ ] Implement SSE4.2/AVX2 pattern matching using `_mm256_cmpeq_epi8` + bitmask
- [ ] Add multi-threaded scanning with `std::async` / thread pool (split module into chunks)
- [ ] The `<immintrin.h>` include already exists in `d3d12_wrappers.cpp` but is never used — put it to work
- [ ] Target: 10x speedup on first-time scans (no cache)

### 7.2 Hot Path Allocation Elimination
- [ ] `ResourceDetector::RegisterResource` creates `ComPtr` copies on every call — use a fixed-size ring buffer instead of `std::vector`
- [ ] `std::vector<sl::ResourceTag> tags; tags.reserve(4)` in `TagResources()` — make it `std::array<sl::ResourceTag, 4>` (always exactly 4 tags)
- [ ] `std::vector<std::function<void()>> callbacksToRun` in `InputHandler::HandleKey` — use a small-buffer-optimized `static_vector` or `inplace_vector` (C++26)
- [ ] Pre-allocate `g_cbvInfos` and `g_rootCbvAddrs` to reasonable capacity at init time

### 7.3 Reduce Mutex Pressure
- [ ] Profile with `tracy` to identify the actual contention hotspots
- [ ] Use `std::atomic` for simple counters instead of mutex-guarded increments (some are already atomic, some aren't — inconsistent)
- [ ] Consider `folly::SharedMutex` or C++17 `std::shared_mutex` for read-heavy data paths
- [ ] Batch descriptor tracking updates per-frame instead of per-call

### 7.4 Precompiled Shaders
`heuristic_scanner.cpp` calls `D3DCompile()` at runtime. This adds latency to the first frame and requires shipping `d3dcompiler_47.dll`.

- [ ] Precompile `resource_analysis.hlsl` to a header file at build time:
  ```cmake
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/scanner_cs.h
      COMMAND dxc /T cs_6_0 /E CSMain /Fh ${CMAKE_BINARY_DIR}/scanner_cs.h src/shaders/resource_analysis.hlsl)
  ```
- [ ] Embed the bytecode as `constexpr std::array<uint8_t, N>` — zero runtime compilation
- [ ] Upgrade from `cs_5_0` to `cs_6_0` (SM 6.0 minimum for DLSS-capable GPUs)
- [ ] Remove `#pragma comment(lib, "d3dcompiler.lib")` link dependency

---

## Phase 8: Safety & Robustness Hardening 🛡️

*Goal: The proxy should never be the reason a game crashes.*

### 8.1 Crash Handler Rewrite
`crash_handler.cpp` has fundamental issues: it calls `fopen_s`, `fprintf`, `time()`, `GetModuleInformation`, and `MiniDumpWriteDump` inside an exception handler — many of these are **not async-signal-safe** and can deadlock if the crash occurred while a CRT lock was held.

- [ ] Use only async-signal-safe operations in the VEH handler: `WriteFile` (Win32 API, not CRT), pre-allocated buffers
- [ ] Pre-allocate the crash log buffer at startup (fixed-size `char[4096]`)
- [ ] Move minidump writing to a **separate watchdog process** that the crash handler signals via a named event
- [ ] Replace `CryptProtectData` encryption (requires CRT) with a simple XOR scramble for the hot path, real encryption on the watchdog side

### 8.2 SEH/C++ Exception Boundary
`hooks.cpp` line 117 uses `__try/__except` (SEH) mixed with C++ code. This is undefined behavior with destructors/RAII.

- [ ] Isolate all `__try/__except` blocks into separate `extern "C"` functions (no C++ objects on stack)
- [ ] Or replace with `IsBadReadPtr`-free validation: `VirtualQuery` + `ReadProcessMemory` into a local buffer
- [ ] Add `noexcept` to all hook callbacks — an uncaught exception in a hook kills the entire game

### 8.3 Bounds-Checked VTable Access
Magic numbers like `vtable[8]`, `vtable[9]`, `vtable[26]`, `vtable[27]`, `vtable[31]`, `vtable[32]` are used without any validation. If a game uses a custom D3D12 implementation with a different VTable layout, this is an instant crash.

- [ ] Validate VTable size before indexing (check memory region size with `VirtualQuery`)
- [ ] Create named constants (see Phase 2.4 strong types) for every VTable slot
- [ ] Add runtime validation: call `QueryInterface` to verify the object actually supports the expected interface before hooking its VTable
- [ ] Log a diagnostic message if a VTable index doesn't point to executable memory

### 8.4 Graceful Degradation Chain
Currently, if any single subsystem fails, the whole proxy is in a broken state. Build a proper fallback chain:

```
Full DLSS 4.5 (all features)
  └─ DLSS SR only (no frame gen)
       └─ Overlay only (no DLSS, just ImGui)
            └─ Pure passthrough (proxy does nothing, game runs normally)
```

- [ ] Each level should be independently functional
- [ ] If `slInit` fails, skip Streamline entirely but still show the overlay with "DLSS Unavailable" status
- [ ] If ImGui init fails, still proxy DXGI correctly
- [ ] If the original DXGI load fails, show a `MessageBox` and return `FALSE` from `DllMain`

---

## Phase 9: Feature Engineering — 2026-Tier Features 🧠

*Goal: Features that push beyond "it works" into "it's intelligent."*

### 9.1 Machine-Learning Resource Detection
The current heuristic scanner uses simple variance analysis. Upgrade to a tiny ML classifier.

- [ ] Train a small decision tree (< 1KB) on texture metadata features: `(width, height, format, flags, mipLevels, sampleCount)` → `{color, depth, motionVector, other}`
- [ ] Ship the trained model as a `constexpr` lookup table — no runtime ML framework needed
- [ ] Use the compute shader results (variance, range, uniformity) as secondary features
- [ ] Fall back to the current heuristic if the classifier is uncertain (confidence < 0.7)

### 9.2 Adaptive Frame Generation
- [ ] Monitor base FPS over a rolling window (already partially implemented in `dxgi_wrappers.cpp`)
- [ ] Dynamically adjust multiplier: if base FPS > 60 → 2×, if base FPS 30-60 → 3×, if base FPS < 30 → 4×
- [ ] Detect camera cuts via motion vector magnitude spikes → temporarily disable FG for 2 frames
- [ ] Detect loading screens (all-black or static frames) → disable FG entirely
- [ ] Expose "target FPS" slider in overlay instead of raw multiplier

### 9.3 HUDless Rendering Pipeline
- [ ] Detect UI draw calls by monitoring blend state changes (UI typically uses alpha blending with `SrcAlpha/InvSrcAlpha`)
- [ ] Capture the pre-UI render target as the "HUDless" input for DLSS
- [ ] Composite UI at native resolution post-upscale
- [ ] This eliminates the #1 visual artifact of DLSS mods (blurry HUD text)

### 9.4 Multi-Game Support Framework
Currently hardcoded for AC Valhalla. Generalize:

- [ ] Create `GameProfile` structs with per-game overrides:
  ```cpp
  struct GameProfile {
      std::string_view exeName;
      std::optional<DXGI_FORMAT> expectedDepthFormat;
      std::optional<DXGI_FORMAT> expectedMVFormat;
      float defaultMVScaleX, defaultMVScaleY;
      bool invertDepth;
  };
  ```
- [ ] Auto-detect game by checking `GetModuleFileName(nullptr, ...)` against known EXE names
- [ ] Ship a `profiles/` directory with TOML files per-game
- [ ] Community-contributed profiles via PR

### 9.5 Reflex Low-Latency Pipeline
- [ ] Inject `NvAPI_D3D_SetSleepMode` in the Present hook
- [ ] Add `slReflexSleep` call between CPU work completion and Present
- [ ] Expose "Max Frame Queue" setting in overlay (1-3 frames)
- [ ] Show latency overlay (render latency, input latency, total pipeline latency)

---

## Phase 10: Developer Experience & Tooling 🛠️

*Goal: Make contributing to this project a joy, not a mystery.*

### 10.1 Visual Debugger Overlay Tab
- [ ] Add an "🔧 Internals" tab to ImGui overlay showing:
  - Hook status for each intercepted function (✅ Hooked / ❌ Failed / ⏳ Pending)
  - Resource detection confidence scores with live bar graphs
  - Camera matrix visualization (extracted view/proj as readable numbers)
  - CBV scan statistics (count, hit rate, scan time)
  - Streamline feature status per-feature
  - Memory usage breakdown (proxy overhead vs game)

### 10.2 Flight Recorder
- [ ] Implement a lock-free ring buffer (8KB) that records the last N log messages
- [ ] On crash, dump the ring buffer to `flight_recorder.log` — shows exactly what happened in the last ~100 operations before the crash
- [ ] Much more useful than the current `startup_trace.log` which is append-only forever

### 10.3 Developer Documentation
- [ ] Write `ARCHITECTURE.md` explaining the hook chain: `DllMain` → `InitializeProxy` → `InstallD3D12Hooks` → `VTable hooks` → `Present path`
- [ ] Document every VTable index and its corresponding D3D12 method
- [ ] Document the resource detection scoring algorithm
- [ ] Add inline Doxygen comments to all public APIs

### 10.4 Hot-Reload Development Loop
- [ ] Support reloading the proxy DLL without restarting the game (detach hooks → unload → reload → reattach)
- [ ] File watcher on the DLL itself — when it changes, trigger a reload
- [ ] This turns the edit-compile-test cycle from ~60 seconds to ~5 seconds

### 10.5 Profiling Integration
- [ ] Add optional Tracy profiler integration:
  ```cpp
  #ifdef TRACY_ENABLE
  #include <tracy/Tracy.hpp>
  #define TC_ZONE ZoneScoped
  #else
  #define TC_ZONE
  #endif
  ```
- [ ] Annotate every hook entry/exit, mutex lock, and Streamline API call
- [ ] Visualize exactly where frame time is spent: proxy overhead vs game vs GPU

---

## Appendix A: Quick Wins (Can Be Done in < 1 Hour Each)

These don't fit neatly into a phase but are high-value, low-effort:

| # | File | Issue | Fix |
|---|------|-------|-----|
| 1 | `proxy.cpp:84` | `sprintf_s(handleBuf, ...)` for debug logging | Use `std::format` |
| 2 | `config_manager.cpp:236` | `char buf[32]` + `atof` for INI parsing | Use `GetPrivateProfileInt` return or remove legacy support entirely |
| 3 | `hooks.cpp:189-199` | `static uint64_t s_lastScanFrame` as function-local static in `HookedResourceBarrier` | Move to a proper per-frame state struct |
| 4 | `hooks.cpp:252-263` | VTable indices are magic numbers | Define as named `constexpr` constants |
| 5 | `input_handler.cpp:55-65` | `GetKeyName` modifies `mutable m_lastKeyName` (not thread-safe) | Return by value, or cache with `std::atomic` flag |
| 6 | `heuristic_scanner.cpp:88` | `D3DCompile` with `cs_5_0` | Upgrade to `cs_6_0` (or precompile) |
| 7 | `CMakeLists.txt:20` | `file(GLOB_RECURSE SRC_FILES ...)` | List files explicitly |
| 8 | `dxgi_wrappers.cpp:59` | `uint64_t g_frameCount` shadows `ResourceDetector::m_frameCount` | Rename or unify frame counting |
| 9 | `streamline_integration.h:49-117` | 30+ near-identical setter methods | Template or macro-generate, or use a single `ApplyConfig()` |
| 10 | `pattern_scanner.cpp:34-35` | Pattern hash → filename for cache creates filesystem clutter | Use a single `pattern_cache.db` file with all patterns |

---

## Appendix B: File-by-File Severity Map

| File | Lines | Severity | Top Issue |
|------|-------|----------|-----------|
| `d3d12_wrappers.cpp` | ~1700 | 🔴 CRITICAL | God file, needs splitting into 4-5 modules |
| `config_manager.h` | 162 | 🔴 CRITICAL | 50 reference members make struct non-copyable |
| `hooks.cpp` | 391 | 🟠 HIGH | Magic VTable numbers, SEH/C++ mixing |
| `dxgi_wrappers.cpp` | 399 | 🟠 HIGH | Missing SwapChain implementation, timer thread on wrong thread |
| `streamline_integration.h` | 283 | 🟠 HIGH | 60+ member variables, no state machine |
| `proxy.cpp` | 331 | 🟡 MEDIUM | Dead code, unsafe LogStartup |
| `crash_handler.cpp` | 212 | 🟡 MEDIUM | Non-async-safe operations in VEH |
| `imgui_overlay.h` | 106 | 🟡 MEDIUM | Raw pointers for D3D12 objects |
| `dlss4_config.h` | 94 | 🟢 LOW | #define macros → constexpr |
| `pattern_scanner.cpp` | 174 | 🟢 LOW | No SIMD, but functional |
| `heuristic_scanner.cpp` | 362 | 🟢 LOW | Runtime shader compilation |
| `input_handler.cpp` | 137 | 🟢 LOW | Thread-safety nit in GetKeyName |

---

## Appendix C: Dependency Upgrade Path

| Current | Version | Upgrade To | Reason |
|---------|---------|------------|--------|
| spdlog | (latest) | Keep | Already modern, async-capable |
| nlohmann-json | (latest) | **glaze 3.x** | 10x faster parse/serialize, compile-time reflection |
| tomlplusplus | (latest) | Keep | Best TOML library for C++ |
| minhook | (latest) | Keep, or evaluate **safetyhook** | safetyhook has better x64 support and inline hooks |
| imgui | (latest) | Keep, add **implot** | For live performance graphs in overlay |
| — | — | Add **catch2** | Unit testing |
| — | — | Add **tracy** | Profiling |
| — | — | Add **magic_enum** | Enum reflection for logging/serialization |

---

**Total Improvements Identified: 87**
**Estimated Complexity: 10 phases, roughly ordered by dependency and impact.**

*Start with Phase 1 — everything else builds on top of a clean foundation.*

