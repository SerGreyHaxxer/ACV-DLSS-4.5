# 🪓 Tensor-Curie: Comprehensive Roadmap 2026-2027

**Project:** Tensor-Curie (DLSS 4.5 Graphics Injection Framework)
**Version:** 5.0.0 (In Development)
**Standard:** C++23 / C++26 Polyfills
**Status:** 🚧 Active Development - Major Restructuring Phase

---

## 📊 Executive Summary

Tensor-Curie is evolving from a game-specific mod into a **general-purpose graphics injection framework**. This roadmap addresses architectural debt, adds missing testing infrastructure, improves code quality, and expands platform support.

**Current State Assessment:**
- ✅ **Strengths:** Advanced resource detection, C++26 polyfills, reflection-based config
- ⚠️ **Weaknesses:** No testing, tight coupling, large files, inconsistent error handling
- 🎯 **Goal:** Production-ready framework suitable for multiple games

---

## 🏗️ Architecture Overview

### Current Module Structure

```
┌─────────────────────────────────────────────────────────────────────┐
│                         TENSOR-CURIE ARCHITECTURE                    │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐             │
│  │   DXGI      │    │    D3D12    │    │   Hooks     │             │
│  │   Proxy     │───▶│   Wrappers  │───▶│   System    │             │
│  └─────────────┘    └─────────────┘    └─────────────┘             │
│         │                   │                   │                   │
│         ▼                   ▼                   ▼                   │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐             │
│  │  Resource   │    │ Streamline  │    │   Crash     │             │
│  │  Detector   │───▶│ Integration │───▶│  Handler    │             │
│  └─────────────┘    └─────────────┘    └─────────────┘             │
│         │                   │                                       │
│         ▼                   ▼                                       │
│  ┌─────────────┐    ┌─────────────┐                               │
│  │    UI       │    │   Config    │                               │
│  │  Overlay    │◀───│  Manager    │                               │
│  └─────────────┘    └─────────────┘                               │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Lock Hierarchy (Documented)
```
Level 1: SwapChain (highest priority)
Level 2: Hooks / Initialization
Level 3: Resources
Level 4: Configuration
Level 5: Logging (lowest priority)
```

---

## 📅 Phase 0: Foundation & Stability (Weeks 1-4)

### 0.1 Sentinel Crash Handler Integration
**Status:** ✅ Implemented | 🚧 Integration Pending

| Task | Priority | Effort | Status |
|------|----------|--------|--------|
| Integration testing with game crashes | High | 2 days | 📝 Pending |
| Symbol server setup for release builds | Medium | 1 day | 📝 Pending |
| Minidump upload to GitHub Issues | Low | 2 days | 📝 Pending |
| Crash telemetry (opt-in) | Low | 3 days | 📝 Pending |

**Deliverables:**
- [ ] Automated crash test harness
- [ ] Symbol publishing workflow in CI
- [ ] User-friendly crash reporting dialog

### 0.2 Ghost Hook System Completion
**Status:** 🚧 Prototype | ✅ Core Done

| Task | Priority | Effort | Status |
|------|----------|--------|--------|
| Migrate Present() to Ghost Hook | High | 1 day | 📝 Pending |
| Migrate ResizeBuffers() to Ghost Hook | High | 1 day | 📝 Pending |
| Recursion guard verification | High | 1 day | 📝 Pending |
| Context sanitization (Dr6/Dr7) | High | 2 days | 📝 Pending |
| Anti-cheat detection testing | Critical | 3 days | 📝 Pending |

**Deliverables:**
- [ ] Full Ghost Hook migration
- [ ] Anti-cheat bypass verification
- [ ] Performance benchmarks vs MinHook

### 0.3 TensorBoot Launcher
**Status:** 🚧 Prototype

| Task | Priority | Effort | Status |
|------|----------|--------|--------|
| Crash loop detection | High | 1 day | 📝 Pending |
| Safe mode auto-activation | High | 1 day | 📝 Pending |
| GUI for user feedback | Medium | 2 days | 📝 Pending |
| Auto-repair missing DLLs | Low | 3 days | 📝 Pending |

**Deliverables:**
- [ ] Production-ready TensorBoot.exe
- [ ] Configuration backup/restore
- [ ] One-click repair functionality

### 0.4 Dependency Hardening
**Status:** ✅ Static CRT | 📝 Version Pinning Pending

| Task | Priority | Effort | Status |
|------|----------|--------|--------|
| Lock vcpkg.json to commit hash | High | 0.5 day | 📝 Pending |
| Submodule external dependencies | Medium | 1 day | 📝 Pending |
| Symbol stripping for Release | Medium | 1 day | 📝 Pending |
| DLL redirection safety | High | 2 days | 📝 Pending |

**Deliverables:**
- [ ] Reproducible builds
- [ ] Dependency vulnerability scanning
- [ ] Automated dependency update testing

---

## 🧪 Phase 1: Testing Infrastructure (Weeks 5-8)

### 1.1 Unit Testing Framework
**Status:** ❌ Missing

**Framework Choice:** Catch2 v3 (lightweight, header-only option available)

| Module | Test Coverage Target | Effort | Priority |
|--------|---------------------|--------|----------|
| Config Manager | 95% | 2 days | High |
| Reflection System | 90% | 2 days | High |
| inplace_vector | 100% | 1 day | High |
| hive (Colony) | 90% | 1 day | Medium |
| Pattern Scanner | 85% | 2 days | High |
| Resource Scoring | 80% | 3 days | Medium |

**Deliverables:**
- [ ] `tests/` directory with Catch2 integration
- [ ] CI test pipeline
- [ ] Coverage reporting (Codecov)

### 1.2 Mock D3D12 Device
**Status:** ❌ Not Started

| Component | Effort | Priority |
|-----------|--------|----------|
| ID3D12Device stub | 2 days | High |
| ID3D12Resource stub | 1 day | High |
| ID3D12CommandList stub | 1 day | Medium |
| SwapChain mock | 2 days | Medium |

**Deliverables:**
- [ ] `tests/mocks/d3d12_mocks.h`
- [ ] Resource detector integration tests
- [ ] Streamline integration tests (with mocked SL)

### 1.3 Property-Based Testing
**Status:** ❌ Not Started

Using **RapidCheck** for C++:

| Test Property | Effort | Priority |
|---------------|--------|----------|
| Config serialization roundtrip | 1 day | Medium |
| inplace_vector invariants | 1 day | Medium |
| Reflection field access | 1 day | Low |

### 1.4 Continuous Integration
**Status:** 🚧 Partial (build only)

| Check | Effort | Priority |
|-------|--------|----------|
| Linux build (WSL) | 2 days | Medium |
| Static analysis (clang-tidy) | 1 day | High |
| AddressSanitizer in CI | 1 day | High |
| UndefinedBehaviorSanitizer | 1 day | Medium |
| Code coverage reporting | 1 day | Medium |

**Deliverables:**
- [ ] Full CI/CD pipeline
- [ ] Automated release generation
- [ ] Performance regression detection

---

## 🔧 Phase 2: Code Quality & Refactoring (Weeks 9-14)

### 2.1 File Splitting & Modularization
**Status:** ⚠️ Several files >500 lines

| File | Current Lines | Target | Action |
|------|---------------|--------|--------|
| imgui_overlay.cpp | ~768 | <400 | Split into widget files |
| resource_detector.cpp | ~638 | <400 | Extract scoring logic |
| streamline_integration.cpp | ~566 | <400 | Split by feature |
| config_manager.cpp | ~319 | Keep | Acceptable |
| valhalla_gui.cpp | TBD | <400 | Split renderer |

**New Structure:**
```
src/ui/
├── widgets/
│   ├── button.cpp
│   ├── slider.cpp
│   ├── checkbox.cpp
│   ├── combobox.cpp
│   └── colorpicker.cpp
├── panels/
│   ├── main_panel.cpp
│   ├── customization_panel.cpp
│   └── setup_wizard.cpp
└── overlay.cpp (coordination)

src/detection/
├── scoring.cpp
├── heuristics.cpp
└── pattern_matcher.cpp

src/streamline/
├── dlss.cpp
├── framegen.cpp
├── deepdvc.cpp
├── ray_reconstruction.cpp
├── reflex.cpp
└── hdr.cpp
```

### 2.2 Error Handling Standardization
**Status:** ⚠️ Inconsistent (HRESULT, bool, exceptions)

**Approach:** `Result<T>` type (Rust-inspired)

```cpp
template<typename T>
class Result {
    std::variant<T, Error> m_value;
public:
    bool IsOk() const;
    bool IsErr() const;
    T& Unwrap();
    const Error& GetError() const;
};
```

| Module | Effort | Priority |
|--------|--------|----------|
| Define Result<T> type | 1 day | High |
| Migrate hooks.cpp | 2 days | High |
| Migrate resource_detector.cpp | 2 days | High |
| Migrate streamline_integration.cpp | 3 days | High |
| Migrate config_manager.cpp | 1 day | Medium |

### 2.3 Smart Pointer Migration
**Status:** ⚠️ Mixed raw/COM smart pointers

| Area | Current | Target | Effort |
|------|---------|--------|--------|
| COM objects | ComPtr<> | Keep | N/A |
| Owned resources | Raw/new | unique_ptr | 3 days |
| Shared resources | Raw* | shared_ptr | 2 days |
| Arrays | new[] | vector/span | 2 days |

### 2.4 Singleton Elimination
**Status:** ⚠️ Extensive singleton usage

**Target Pattern:** Dependency Injection

| Component | Effort | Priority |
|-----------|--------|----------|
| Service Locator base | 2 days | High |
| ConfigManager refactor | 2 days | High |
| ResourceDetector refactor | 3 days | Medium |
| StreamlineIntegration refactor | 3 days | Medium |
| ImGuiOverlay refactor | 2 days | Low |

### 2.5 Header Cleanup
**Status:** ⚠️ Some implementation in headers

| Action | Effort | Priority |
|--------|--------|----------|
| Move template impl to .inl | 1 day | Low |
| Reduce include dependencies | 2 days | Medium |
| Add forward declarations | 1 day | Medium |
| Precompiled headers | 1 day | Low |

### 2.6 Magic Number Extraction
**Status:** ⚠️ Scattered throughout

**Create:** `src/game_offsets.h` with all game-specific values

| Category | Items | Effort |
|----------|-------|--------|
| DLSS constants | ~15 | 1 day |
| Resource detection | ~25 | 1 day |
| UI constants | ~40 | 1 day |
| HDR defaults | ~10 | 0.5 day |

---

## 🚀 Phase 3: C++26 Features & Performance (Weeks 15-20)

### 3.1 std::hive (Colony) Integration
**Status:** ✅ Polyfill exists | 📝 Integration Pending

| Use Case | Current Container | Target | Effort |
|----------|------------------|--------|--------|
| DescriptorTracker | unordered_map | hive | 2 days |
| Resource candidates | inplace_vector | hive | 2 days |
| UI widgets | unordered_map | hive | 1 day |

**Expected Impact:** 15-30% reduction in memory fragmentation

### 3.2 std::mdspan Integration
**Status:** ❌ Not used

| Application | Effort | Priority |
|-------------|--------|----------|
| Matrix operations | 2 days | Medium |
| Image processing | 2 days | Low |
| Shader bindings | 1 day | Low |

### 3.3 std::generator (Coroutines)
**Status:** ❌ Not used

| Use Case | Effort | Priority |
|----------|--------|----------|
| Resource iteration | 2 days | Medium |
| Config traversal | 1 day | Low |

### 3.4 std::flat_map
**Status:** ❌ Not used

| Use Case | Current | Target | Effort |
|----------|---------|--------|--------|
| UI state maps | unordered_map | flat_map | 1 day |
| Small caches | unordered_map | flat_map | 1 day |

### 3.5 Memory Pool Optimization
**Status:** ❌ Not implemented

| Pool Type | Target | Effort | Priority |
|-----------|--------|--------|----------|
| Command lists | Per-frame pool | 3 days | High |
| UI widgets | Fixed-size pool | 2 days | Medium |
| Resources | Arena allocator | 3 days | Medium |

### 3.6 Lock Contention Reduction
**Status:** ⚠️ Potential bottlenecks

| Optimization | Impact | Effort |
|--------------|--------|--------|
| Lock-free resource queues | High | 3 days |
| RCU for config reads | Medium | 2 days |
| Per-frame command allocators | High | 2 days |
| Atomic flag replacements | Low | 1 day |

---

## 🎨 Phase 4: Render Pipeline & Graphics (Weeks 21-28)

### 4.1 Ray Tracing Pass
**Status:** 🚧 Prototype

| Component | Effort | Priority |
|-----------|--------|----------|
| Complete SSRT shader | 3 days | High |
| Denoiser integration | 5 days | High |
| Temporal accumulation | 3 days | Medium |
| Quality presets | 2 days | Medium |

**Deliverables:**
- [ ] Production-ready SSRT
- [ ] NRD or custom denoiser
- [ ] Configurable quality settings

### 4.2 Multi-Upscaler Support
**Status:** 🚧 Interface defined

| Upscaler | Status | Effort | Priority |
|----------|--------|--------|----------|
| DLSS 4.5 | ✅ Complete | N/A | N/A |
| XeSS 2 | ❌ Not started | 5 days | Medium |
| FSR 3.1 | ❌ Not started | 4 days | Medium |
| RIS | ❌ Not started | 2 days | Low |

### 4.3 HDR Pipeline
**Status:** 🚧 Basic support

| Feature | Effort | Priority |
|---------|--------|----------|
| HDR10+ metadata | 2 days | Medium |
| Tone mapping presets | 2 days | Medium |
| Heatmap overlay | 1 day | Low |
| Color space converter | 3 days | Medium |

### 4.4 Advanced Post-Processing
**Status:** ❌ Not started

| Effect | Effort | Priority |
|--------|--------|----------|
| CAS (Sharpening) | 2 days | Low |
| LUT color grading | 3 days | Low |
| Film grain | 1 day | Low |
| Chromatic aberration | 2 days | Low |

---

## 🧠 Phase 5: AI & Heuristics (Weeks 29-36)

### 5.1 Neural Resource Detection
**Status:** 💭 Research

| Component | Effort | Priority |
|-----------|--------|----------|
| Data collection mode | 2 days | Medium |
| ONNX Runtime integration | 3 days | Medium |
| Model training pipeline | 5 days | Low |
| Inference thread | 2 days | Medium |

**Model Architecture:**
- Input: 64x64 depth patch + motion vectors
- Output: Classification (Menu/Gameplay/Cutscene)
- Framework: ONNX Runtime
- Target: <1ms inference time

### 5.2 Dynamic Quality Auto-Tuner
**Status:** 📝 Planned

| Component | Effort | Priority |
|-----------|--------|----------|
| Frametime monitor | 2 days | High |
| PID controller | 2 days | Medium |
| Hysteresis logic | 1 day | Medium |
| User preference learning | 3 days | Low |

**Control Loop:**
```
Target FPS (60/120/144)
    ↓
[PID Controller]
    ↓
Quality Adjustment (DLSS Preset)
    ↓
[5s Hysteresis]
    ↓
Apply Change
```

### 5.3 Scene Change Detection
**Status:** 🚧 Basic implementation

| Improvement | Effort | Priority |
|-------------|--------|----------|
| Histogram-based | 2 days | Medium |
| Motion vector variance | 1 day | Medium |
| Adaptive threshold | 1 day | Low |

---

## 🌐 Phase 6: Platform Expansion (Weeks 37-44)

### 6.1 Multi-Game Support
**Status:** ❌ AC Valhalla only

| Game | Effort | Priority |
|------|--------|----------|
| AC Odyssey | 3 days | Medium |
| AC Origins | 3 days | Medium |
| Watch Dogs: Legion | 5 days | Low |
| Generic D3D12 | 10 days | High |

### 6.2 Game Profiles
**Status:** ❌ Not implemented

**Profile Format:**
```toml
[game_detection]
executable = "ACValhalla.exe"
window_title = "Assassin's Creed Valhalla"

[dlss]
default_mode = 5  # DLAA
requires_hud_fix = true

[motion_vectors]
scale_x = 1.0
scale_y = 1.0
inverted = false

[depth]
format_override = "DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS"
```

| Feature | Effort | Priority |
|---------|--------|----------|
| Profile loader | 2 days | High |
| Profile editor UI | 3 days | Medium |
| Community profile sharing | 5 days | Low |

### 6.3 Linux/Proton Support
**Status:** ❌ Windows-only

| Component | Effort | Priority |
|-----------|--------|----------|
| Proton DXGI proxy | 5 days | Medium |
| VKD3D bridge | 10 days | Low |
| Wine compatibility testing | 3 days | Low |

### 6.4 AMD GPU Support
**Status:** ❌ NVIDIA only

| Feature | Effort | Priority |
|---------|--------|----------|
| FSR 3.1 integration | 4 days | High |
| AFMF support | 3 days | Medium |
| VRS API | 2 days | Low |

---

## 📚 Phase 7: Documentation & Developer Experience (Weeks 45-48)

### 7.1 API Documentation
**Status:** ⚠️ Minimal

| Format | Tool | Effort |
|--------|------|--------|
| Public API headers | Doxygen | 3 days |
| Architecture guides | Markdown | 5 days |
| Video tutorials | OBS | 8 days |

### 7.2 Contribution Guidelines
**Status:** ❌ Missing

| Document | Effort |
|----------|--------|
| CONTRIBUTING.md | 1 day |
| CODING_STYLE.md | 1 day |
| ARCHITECTURE.md | 2 days |
| RELEASE_CHECKLIST.md | 1 day |

### 7.3 Developer Tools
**Status:** 🚧 Basic

| Tool | Effort | Priority |
|------|--------|----------|
| Resource inspector | 3 days | Medium |
| Hook visualizer | 2 days | Low |
| Config validator | 1 day | Medium |
| Profile generator | 3 days | Low |

---

## 📊 Sprint Schedule

| Sprint | Duration | Focus | Key Deliverables |
|--------|----------|-------|------------------|
| **Sprint 0** | Week 1 | Planning | Project setup, task tracking |
| **Sprint 1** | Weeks 2-3 | Stability | Sentinel integration, Ghost hooks |
| **Sprint 2** | Weeks 4-5 | Safety | TensorBoot, dependency hardening |
| **Sprint 3** | Weeks 6-7 | Testing | Catch2 setup, core tests |
| **Sprint 4** | Weeks 8-9 | CI/CD | Full pipeline, coverage |
| **Sprint 5** | Weeks 10-12 | Refactoring | File splits, Result<T> |
| **Sprint 6** | Weeks 13-15 | Architecture | DI, smart pointers |
| **Sprint 7** | Weeks 16-18 | Performance | C++26 features, optimization |
| **Sprint 8** | Weeks 19-21 | Graphics | Ray tracing, denoiser |
| **Sprint 9** | Weeks 22-24 | Upscalers | XeSS, FSR integration |
| **Sprint 10** | Weeks 25-27 | AI | ONNX, auto-tuner |
| **Sprint 11** | Weeks 28-30 | Platform | Multi-game, profiles |
| **Sprint 12** | Weeks 31-32 | Polish | Docs, bug fixes |
| **Sprint 13** | Weeks 33-36 | Release | v5.0.0 launch |

---

## 🎯 Success Metrics

### Code Quality
- [ ] Test coverage > 80%
- [ ] Zero warnings in Release build
- [ ] Max file size < 500 lines
- [ ] Cyclomatic complexity < 15 per function

### Performance
- [ ] Frame overhead < 0.5ms
- [ ] Memory overhead < 100MB
- [ ] Load time < 100ms
- [ ] No frame drops during hook installation

### Stability
- [ ] Crash rate < 0.1% of sessions
- [ ] Memory leak free (24h soak test)
- [ ] No deadlocks (thread sanitizer)
- [ ] Graceful degradation on all errors

### User Experience
- [ ] Setup time < 2 minutes
- [ ] Config changes apply instantly
- [ ] Clear error messages
- [ ] Comprehensive FAQ

---

## 🔄 Release Strategy

### v5.0.0 - "Foundation" (Target: Q2 2026)
- Complete refactoring
- Testing infrastructure
- Multi-game framework
- Full C++26 polyfills

### v5.1.0 - "Graphics" (Target: Q3 2026)
- Ray tracing injection
- Multi-upscaler support
- Advanced HDR

### v5.2.0 - "Intelligence" (Target: Q4 2026)
- AI resource detection
- Auto-quality tuning
- Scene detection

### v6.0.0 - "Universal" (Target: Q1 2027)
- Generic D3D12 support
- Linux/Proton
- AMD GPU support

---

## 📝 Notes

### Architectural Decisions
1. **Singletons vs DI**: Gradual migration to maintain stability
2. **Error Handling**: Result<T> for new code, gradual migration
3. **C++26**: Polyfills now, native when compiler support arrives
4. **Testing**: Test after refactoring to avoid churn

### Risk Mitigation
- Each phase has rollback points
- Feature flags for experimental features
- Extensive logging for diagnostics
- Beta testing group before releases

---

*Last Updated: 2026-02-06*
*Version: 2.0*
*Author: AI Assistant with human oversight*
