# Tensor-Curie Roadmap (2026)

## Phase 1: Foundation & Modernization
*Goal: Move from "Script-based" to "Engineering-grade" build environment.*

- [ ] **Build System Migration**
    - Replace `build.bat` with **CMake** (`CMakeLists.txt`).
    - Support multi-configuration (Debug/RelWithDebInfo/Release).
    - Create presets for VS2022 and Ninja generators.
- [ ] **Dependency Management**
    - Integrate **vcpkg** or **git submodules** for:
        - `spdlog` (Logging) - replacing `src/logger.h`.
        - `nlohmann/json` or `toml++` - replacing INI `GetPrivateProfileString`.
        - `minhook` - replacing custom IAT/VTable hook utils.
        - `imgui` - standardize version management.
- [ ] **CI/CD Pipeline**
    - Add **GitHub Actions** workflow.
    - Automated build checks on Pull Request.
    - Artifact uploading (DLL generation).

## Phase 2: Core Refactoring
*Goal: Reduce fragility and technical debt.*

- [ ] **Configuration Overhaul**
    - Rewrite `ConfigManager` to use a serialized struct approach (Reflection or Macro-based).
    - Switch to `.json` or `.toml` for config files (more robust than `.ini`).
    - Implement "Hot Reload" for settings (FileSystem Watcher).
- [ ] **Logging & Diagnostics**
    - Replace custom logging with **spdlog** (Async, thread-safe, structured).
    - Add "Trace" level logging for Hook entry/exit.
    - Implement a structured **Crash Reporter** (generating Minidumps + Context).
- [ ] **Architecture Decoupling**
    - **Dependency Injection**: Remove global singletons (`ConfigManager::Get()`, `StreamlineIntegration::Get()`). Pass context context objects to systems.
    - **Hooking Abstraction**: Create a `HookManager` class that abstracts the difference between IAT, VTable, and Trampoline hooks.

## Phase 3: Reliability & Testing
*Goal: Ensure stability before injecting into games.*

- [ ] **Unit Testing Suite**
    - Integrate **Catch2** or **GoogleTest**.
    - Test `ConfigManager` (Load/Save/Validation).
    - Test `PatternScanner` (against dummy binary data).
    - Test `MathUtils` (Motion vector scaling logic).
- [ ] **Integration Testing (The "Dummy Game")**
    - Create a minimal DirectX 12 "Hello World" application within the repo.
    - Test that the Proxy DLL loads correctly into it.
    - Verify Overlay rendering and Hook interception in a controlled environment.
- [ ] **Safety Mechanisms**
    - **Watchdog Thread**: Detect hangs in the specific proxied threads.
    - **Safe Detach**: Ensure `FreeLibrary` doesn't crash if the game is still calling hooks (ref counting).

## Phase 4: Advanced Features (Complex)
*Goal: Push the boundaries of what the Proxy can do.*

- [ ] **Dynamic Resource Detection 2.0**
    - Implement "Heuristic Scanning" to find Depth/MotionVectors without hardcoded patterns.
    - Use Compute Shaders to analyze potential texture candidates (variance analysis).
- [ ] **Telemetry & Auto-Update**
    - Optional "Check for Updates" in the Overlay.
    - Anonymous usage stats (Frame Gen usage, crash rates) - *Opt-in only*.
- [ ] **Plugin System**
    - Allow loading *other* DLLs via this Proxy (ASI Loader style).
    - Expose an API for other mods to draw to the ImGui Overlay.
- [ ] **Web Control Interface**
    - Embed a tiny HTTP server (e.g., `httplib`) to allow controlling DLSS settings from a phone/browser on the local network (Second Screen experience).
