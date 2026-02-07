/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "imgui_overlay.h"
#include "config_manager.h"
#include "input_handler.h"
#include "logger.h"
#include "nvapi.h"
#include "resource_detector.h"
#include "sampler_interceptor.h"
#include "streamline_integration.h"
#include "auto_ui_generator.h" // Phase 1: Auto-UI
#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <cmath>

// ============================================================================
// NvAPI Metrics (reused from original — independent of GUI library)
// ============================================================================
namespace {

uint64_t GetTimeMs() {
  static LARGE_INTEGER freq = []() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<uint64_t>((counter.QuadPart * 1000ULL) / freq.QuadPart);
}

float GetTimeSec() {
  static uint64_t startMs = GetTimeMs();
  return static_cast<float>(GetTimeMs() - startMs) / 1000.0f;
}

struct NvApiMetrics {
  bool initialized = false;
  bool hasGpu = false;
  NvPhysicalGpuHandle gpu = nullptr;
  char gpuName[NVAPI_SHORT_STRING_MAX] = {};
  char dxgiName[128] = {};
  bool dxgiNameReady = false;
} g_nvapiMetrics;

bool InitNvApi() {
  if (g_nvapiMetrics.initialized)
    return g_nvapiMetrics.hasGpu;
  g_nvapiMetrics.initialized = true;
  if (NvAPI_Initialize() != NVAPI_OK) return false;
  NvU32 gpuCount = 0;
  NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
  if (NvAPI_EnumPhysicalGPUs(handles, &gpuCount) != NVAPI_OK || gpuCount == 0) return false;
  g_nvapiMetrics.gpu = handles[0];
  NvAPI_ShortString name{};
  if (NvAPI_GPU_GetFullName(g_nvapiMetrics.gpu, name) == NVAPI_OK)
    strncpy_s(g_nvapiMetrics.gpuName, name, _TRUNCATE);
  g_nvapiMetrics.hasGpu = true;
  return true;
}

void EnsureDxgiName(ID3D12Device* device) {
  if (g_nvapiMetrics.dxgiNameReady || !device) return;
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgiDevice->GetAdapter(&adapter))) return;
  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc))) return;
  WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_nvapiMetrics.dxgiName,
                      sizeof(g_nvapiMetrics.dxgiName), nullptr, nullptr);
  g_nvapiMetrics.dxgiNameReady = true;
}

bool QueryGpuUtilization(uint32_t& outPercent) {
  static uint64_t s_lastInitAttempt = 0;
  uint64_t now = GetTimeMs();
  if (!g_nvapiMetrics.initialized && now - s_lastInitAttempt < 5000) return false;
  if (!g_nvapiMetrics.initialized) s_lastInitAttempt = now;
  if (!InitNvApi()) return false;
  NV_GPU_DYNAMIC_PSTATES_INFO_EX info{};
  info.version = NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER;
  if (NvAPI_GPU_GetDynamicPstatesInfoEx(g_nvapiMetrics.gpu, &info) != NVAPI_OK) return false;
  if (!info.utilization[0].bIsPresent) return false;
  outPercent = info.utilization[0].percentage;
  return true;
}

bool QueryVramUsageMB(ID3D12Device* device, uint32_t& outUsedMB, uint32_t& outBudgetMB) {
  if (!device) return false;
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (FAILED(adapter.As(&adapter3))) return false;
  DXGI_QUERY_VIDEO_MEMORY_INFO minfo{};
  if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &minfo))) return false;
  outUsedMB = static_cast<uint32_t>(minfo.CurrentUsage / (1024 * 1024));
  outBudgetMB = static_cast<uint32_t>(minfo.Budget / (1024 * 1024));
  return true;
}

struct MetricsCache {
  std::atomic<uint64_t> lastUpdateMs{0};
  std::atomic<bool> gpuOk{false};
  std::atomic<uint32_t> gpuPercent{0};
  std::atomic<bool> vramOk{false};
  std::atomic<uint32_t> vramUsed{0};
  std::atomic<uint32_t> vramBudget{0};
} g_metricsCache;

void UpdateMetrics(ID3D12Device* device) {
  uint64_t now = GetTimeMs();
  uint64_t lastUpdate = g_metricsCache.lastUpdateMs.load(std::memory_order_relaxed);
  if (now - lastUpdate < 500) return;
  g_metricsCache.lastUpdateMs.store(now, std::memory_order_relaxed);
  uint32_t gpuPercent = 0;
  bool gpuOk = QueryGpuUtilization(gpuPercent);
  g_metricsCache.gpuPercent.store(gpuPercent, std::memory_order_relaxed);
  g_metricsCache.gpuOk.store(gpuOk, std::memory_order_relaxed);
  uint32_t vramUsed = 0, vramBudget = 0;
  bool vramOk = QueryVramUsageMB(device, vramUsed, vramBudget);
  g_metricsCache.vramUsed.store(vramUsed, std::memory_order_relaxed);
  g_metricsCache.vramBudget.store(vramBudget, std::memory_order_relaxed);
  g_metricsCache.vramOk.store(vramOk, std::memory_order_relaxed);
}

} // namespace

// ============================================================================
// Singleton
// ============================================================================

ImGuiOverlay& ImGuiOverlay::Get() {
  static ImGuiOverlay instance;
  return instance;
}

// ============================================================================
// Initialize / Shutdown
// ============================================================================

void ImGuiOverlay::Initialize(IDXGISwapChain* swapChain) {
  if (m_initialized || !swapChain) return;
  m_shuttingDown.store(false, std::memory_order_release);

  if (FAILED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&m_swapChain)) || !m_swapChain)
    return;

  DXGI_SWAP_CHAIN_DESC desc{};
  if (FAILED(m_swapChain->GetDesc(&desc))) return;
  if (desc.BufferCount == 0 || desc.BufferCount > 16) return;
  m_backBufferCount = desc.BufferCount;
  m_hwnd = desc.OutputWindow;
  m_width = desc.BufferDesc.Width;
  m_height = desc.BufferDesc.Height;

  if (FAILED(m_swapChain->GetDevice(__uuidof(ID3D12Device), (void**)&m_device))) return;
  m_queue = StreamlineIntegration::Get().GetCommandQueue();
  if (!m_queue) {
    LOG_WARN("[ValhallaOverlay] Command queue not available yet.");
    return;
  }

  // Initialize custom D2D renderer
  if (!m_renderer.Initialize(m_device, m_queue, m_swapChain, m_backBufferCount)) {
    LOG_ERROR("[ValhallaOverlay] Failed to initialize D2D renderer");
    return;
  }

  // Install WndProc hook for mouse/scroll input
  if (m_hwnd && !m_prevWndProc) {
    m_prevWndProc = (WNDPROC)SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
  }

  // Initialize animation state
  m_panelSlide.current = 0.0f;
  m_panelSlide.target = 0.0f;
  m_panelAlpha.current = 0.0f;
  m_panelAlpha.target = 0.0f;
  m_time = GetTimeSec();
  m_firstFrame = true;

  // Start metrics polling thread
  m_metricsThreadRunning.store(true, std::memory_order_release);
  m_metricsThread = std::thread([this]() {
    ID3D12Device* device = m_device;
    if (device) device->AddRef();
    while (m_metricsThreadRunning.load(std::memory_order_acquire)) {
      if (!device) break;
      UpdateMetrics(device);
      EnsureDxgiName(device);
      Sleep(100);
    }
    if (device) device->Release();
  });

  m_initialized = true;
  UpdateControls();
  LOG_INFO("[ValhallaOverlay] Custom Valhalla GUI initialized");
}

void ImGuiOverlay::Shutdown() {
  if (!m_initialized) return;
  m_shuttingDown.store(true, std::memory_order_release);

  if (m_cursorUnlocked) {
    ClipCursor(&m_prevClip);
    while (ShowCursor(TRUE) < 0) {}
    m_cursorUnlocked = false;
  }

  if (m_metricsThreadRunning.exchange(false, std::memory_order_acq_rel)) {
    if (m_metricsThread.joinable()) m_metricsThread.join();
  }

  m_renderer.Shutdown();

  if (m_hwnd && m_prevWndProc) {
    SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)m_prevWndProc);
    m_prevWndProc = nullptr;
  }

  if (m_device) { m_device->Release(); m_device = nullptr; }
  if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
  m_initialized = false;
  LOG_INFO("[ValhallaOverlay] Shutdown complete");
}

// ============================================================================
// WndProc hook — captures mouse wheel scroll
// ============================================================================

LRESULT CALLBACK ImGuiOverlay::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  ImGuiOverlay& overlay = ImGuiOverlay::Get();

  // Capture scroll wheel when overlay is active
  if ((overlay.m_visible || overlay.m_showSetupWizard) && msg == WM_MOUSEWHEEL) {
    short delta = GET_WHEEL_DELTA_WPARAM(wParam);
    overlay.m_scrollAccum += static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) * 40.0f;
    return 0; // consume the message
  }

  // Block mouse input from reaching the game when overlay is visible
  if (overlay.m_visible || overlay.m_showSetupWizard) {
    switch (msg) {
      case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
      case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEMOVE:
        return 0; // consume
    }
  }

  if (overlay.m_prevWndProc)
    return CallWindowProcW(overlay.m_prevWndProc, hwnd, msg, wParam, lParam);
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Public state methods
// ============================================================================

void ImGuiOverlay::OnResize(UINT width, UINT height) {
  if (width == 0 || height == 0) return; // Ignore zero-sized resize (minimized)
  m_width = width; m_height = height;
  if (m_initialized) {
    m_renderer.OnResize();
    // Render targets were released — they will be recreated on the next
    // BeginFrame call inside Render().  No action needed here.
  }
}

void ImGuiOverlay::Render() {
  if (!m_initialized) return;
  if (!m_swapChain || !m_renderer.IsValid()) return;

  UINT backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
  if (!m_renderer.BeginFrame(backBufferIndex)) return;

  // Update timing
  float now = GetTimeSec();
  float dt = m_firstFrame ? (1.0f / 60.0f) : (now - m_lastFrameTime);
  dt = std::clamp(dt, 0.0001f, 0.1f);
  m_lastFrameTime = now;
  m_time = now;
  m_firstFrame = false;

  // Update animations
  m_panelSlide.Update(m_time);
  m_panelAlpha.Update(m_time);

  // Smooth FPS interpolation
  if (ConfigManager::Get().Data().customization.smoothFPS) {
    float target = m_fpsHistory[(m_fpsHistoryIndex + kFpsHistorySize - 1) % kFpsHistorySize];
    m_smoothFPS = vanim::SmoothDamp(m_smoothFPS, target, 8.0f, dt);
  } else {
    m_smoothFPS = m_fpsHistory[(m_fpsHistoryIndex + kFpsHistorySize - 1) % kFpsHistorySize];
  }

  // Update GPU metrics
  UpdateMetrics(m_device);

  // Begin widget frame (poll mouse, manage cursor)
  BeginWidgetFrame();

  // --- Render layers ---
  BuildVignette();
  BuildBackgroundDim();
  BuildMiniMode();

  if (m_visible) {
    BuildMainPanel();
  }

  BuildFPSOverlay();

  // Draw custom Valhalla cursor when overlay is active
  if (m_visible || m_showSetupWizard) {
    m_renderer.DrawValhallaCursor(m_input.mouseX, m_input.mouseY, 1.0f, m_accent, vtheme::hex(0x000000, 0.6f));
  }

  m_renderer.EndFrame();
}

void ImGuiOverlay::SetFPS(float gameFps, float totalFps) {
  m_cachedTotalFPS = totalFps;
  m_fpsHistory[m_fpsHistoryIndex] = gameFps;
  m_fpsHistoryIndex = (m_fpsHistoryIndex + 1) % kFpsHistorySize;
}

void ImGuiOverlay::SetCameraStatus(bool hasCamera, float jitterX, float jitterY) {
  m_cachedCamera = hasCamera;
  m_cachedJitterX = jitterX;
  m_cachedJitterY = jitterY;
}

void ImGuiOverlay::ToggleVisibility() {
  auto& cust = ConfigManager::Get().Data().customization;
  m_visible = !m_visible;
  ConfigManager::Get().Data().ui.visible = m_visible;
  ConfigManager::Get().MarkDirty();

  float speedMul = std::clamp(cust.animSpeed, 0.25f, 3.0f);
  float openDur = vtheme::kAnimOpenDuration / speedMul;
  float closeDur = vtheme::kAnimCloseDuration / speedMul;

  if (m_visible) {
    m_panelSlide.SetTarget(1.0f, openDur, true);
    m_panelAlpha.SetTarget(1.0f, openDur, true);
  } else {
    m_panelSlide.SetTarget(0.0f, closeDur, false);
    m_panelAlpha.SetTarget(0.0f, closeDur, false);
  }
}

void ImGuiOverlay::ToggleFPS() {
  m_showFPS = !m_showFPS;
  ConfigManager::Get().Data().ui.showFPS = m_showFPS;
  ConfigManager::Get().MarkDirty();
}

void ImGuiOverlay::ToggleVignette() {
  m_showVignette = !m_showVignette;
  ConfigManager::Get().Data().ui.showVignette = m_showVignette;
  ConfigManager::Get().MarkDirty();
}

void ImGuiOverlay::ToggleDebugMode(bool enabled) { m_showDebug = enabled; }

void ImGuiOverlay::CaptureNextHotkey(int* target) { m_pendingHotkeyTarget = target; }

void ImGuiOverlay::UpdateControls() {
  ModConfig& cfg = ConfigManager::Get().Data();
  m_showFPS = cfg.ui.showFPS;
  m_showVignette = cfg.ui.showVignette;
  m_showDebug = cfg.system.debugMode;
  m_visible = cfg.ui.visible;
  m_showSetupWizard = cfg.system.setupWizardForceShow || !cfg.system.setupWizardCompleted;

  // Set initial animation state
  m_panelSlide.current = m_visible ? 1.0f : 0.0f;
  m_panelSlide.target = m_panelSlide.current;
  m_panelAlpha.current = m_visible ? 1.0f : 0.0f;
  m_panelAlpha.target = m_panelAlpha.current;

  // Load accent color
  auto& cust = cfg.customization;
  m_accent = vtheme::rgba(cust.accentR, cust.accentG, cust.accentB, 1.0f);
  m_accentBright = vtheme::rgba(
    std::clamp(cust.accentR * 1.3f, 0.0f, 1.0f),
    std::clamp(cust.accentG * 1.3f, 0.0f, 1.0f),
    std::clamp(cust.accentB * 1.3f, 0.0f, 1.0f), 1.0f);
  m_accentDim = vtheme::rgba(cust.accentR * 0.65f, cust.accentG * 0.65f, cust.accentB * 0.65f, 0.6f);

  // Load panel position if saved
  if (cust.panelX >= 0.0f) m_panelX = cust.panelX;
  if (cust.panelY >= 0.0f) m_panelY = cust.panelY;
}

// ============================================================================
// Animation helpers — compute transform based on animation type
// ============================================================================

float ImGuiOverlay::ComputeAnimProgress(float rawProgress, bool opening) const {
  auto animType = static_cast<AnimType>(ConfigManager::Get().Data().customization.animationType);
  float t = std::clamp(rawProgress, 0.0f, 1.0f);
  switch (animType) {
    case AnimType::SlideLeft: case AnimType::SlideRight:
    case AnimType::SlideTop: case AnimType::SlideBottom:
      return opening ? vanim::EaseOutCubic(t) : vanim::EaseInCubic(t);
    case AnimType::Fade:
      return opening ? vanim::EaseOutQuint(t) : vanim::EaseInCubic(t);
    case AnimType::Scale:
      return opening ? vanim::EaseOutBack(t) : vanim::EaseInCubic(t);
    case AnimType::Bounce:
      return opening ? vanim::EaseBounce(t) : vanim::EaseInCubic(t);
    case AnimType::Elastic:
      return opening ? vanim::EaseElastic(t) : vanim::EaseInCubic(t);
    default:
      return opening ? vanim::EaseOutCubic(t) : vanim::EaseInCubic(t);
  }
}

void ImGuiOverlay::ComputePanelTransform(float progress, float screenW, float screenH,
                                          float panelW, float panelH,
                                          float& outX, float& outY, float& outAlpha, float& outScale) const {
  auto& cust = ConfigManager::Get().Data().customization;
  auto animType = static_cast<AnimType>(cust.animationType);
  float eased = ComputeAnimProgress(progress, m_panelSlide.opening);

  float targetX = m_panelX;
  float targetY = m_panelY;
  outAlpha = eased;
  outScale = 1.0f;

  switch (animType) {
    case AnimType::SlideLeft:
      outX = vanim::Lerp(targetX - panelW - 40.0f, targetX, eased);
      outY = targetY;
      break;
    case AnimType::SlideRight:
      outX = vanim::Lerp(screenW + 40.0f, targetX, eased);
      outY = targetY;
      break;
    case AnimType::SlideTop:
      outX = targetX;
      outY = vanim::Lerp(-panelH - 40.0f, targetY, eased);
      break;
    case AnimType::SlideBottom:
      outX = targetX;
      outY = vanim::Lerp(screenH + 40.0f, targetY, eased);
      break;
    case AnimType::Fade:
      outX = targetX;
      outY = targetY;
      break;
    case AnimType::Scale:
      outX = targetX;
      outY = targetY;
      outScale = vanim::Lerp(0.85f, 1.0f, eased);
      break;
    case AnimType::Bounce:
      outX = vanim::Lerp(targetX - panelW - 40.0f, targetX, eased);
      outY = targetY;
      break;
    case AnimType::Elastic:
      outX = vanim::Lerp(targetX - panelW - 40.0f, targetX, eased);
      outY = targetY;
      break;
    default:
      outX = vanim::Lerp(targetX - panelW, targetX, eased);
      outY = targetY;
      break;
  }
}

void ImGuiOverlay::SnapPanel(float screenW, float screenH) {
  auto& cust = ConfigManager::Get().Data().customization;
  if (!cust.snapToEdges) return;
  float snap = cust.snapDistance;
  float panelW = cust.panelWidth;

  if (m_panelX < snap) m_panelX = 0;
  if (m_panelY < snap) m_panelY = 0;
  if (m_panelX + panelW > screenW - snap) m_panelX = screenW - panelW;
  if (m_panelY > screenH - snap) m_panelY = screenH - 100.0f;
}

// ============================================================================
// New rendering features
// ============================================================================

void ImGuiOverlay::BuildBackgroundDim() {
  auto& cust = ConfigManager::Get().Data().customization;
  if (!cust.backgroundDim) return;
  float progress = m_panelAlpha.current;
  if (progress < 0.01f) return;
  float dimAlpha = cust.backgroundDimAmount * progress;
  m_renderer.FillRect(0, 0, static_cast<float>(m_width), static_cast<float>(m_height),
                      vtheme::hex(0x000000, dimAlpha));
}

void ImGuiOverlay::BuildPanelShadow(float x, float y, float w, float h, float alpha) {
  if (!ConfigManager::Get().Data().customization.panelShadow) return;
  // Subtle multi-layer shadow
  for (int i = 2; i >= 0; --i) {
    float offset = static_cast<float>(i + 1) * 3.0f;
    float shadowAlpha = 0.06f * alpha * (3 - i);
    m_renderer.FillRoundedRect(x + offset * 0.5f, y + offset, w, h, 8.0f, vtheme::hex(0x000000, shadowAlpha));
  }
}

void ImGuiOverlay::BuildMiniMode() {
  auto& cust = ConfigManager::Get().Data().customization;
  if (!cust.miniMode || m_visible) return;

  // Compact floating pill with mod name + FPS
  float barW = 140.0f, barH = 28.0f;
  float barX = 12.0f, barY = 12.0f;

  bool hovered = PointInRect(m_input.mouseX, m_input.mouseY, barX, barY, barW, barH);
  float bgAlpha = hovered ? 0.85f : 0.7f;

  m_renderer.FillRoundedRect(barX, barY, barW, barH, barH * 0.5f, vtheme::hex(0x0D1117, bgAlpha));
  m_renderer.OutlineRoundedRect(barX, barY, barW, barH, barH * 0.5f, vtheme::hex(0x30363D, 0.3f), 1.0f);

  // Small accent dot
  m_renderer.DrawCircle(barX + 14, barY + barH * 0.5f, 3.0f, m_accent);
  m_renderer.DrawTextA("DLSS", barX + 24, barY, 50.0f, barH, vtheme::kTextSecondary, 11.0f, ValhallaRenderer::TextAlign::Left, true);

  std::string fpsStr = std::format("{:.0f}", m_smoothFPS);
  m_renderer.DrawTextA(fpsStr.c_str(), barX + 80, barY, 50.0f, barH, vtheme::kTextPrimary, 13.0f, ValhallaRenderer::TextAlign::Right, true);

  // Click to open
  if (hovered && m_input.mouseClicked) {
    ToggleVisibility();
  }
}

// ============================================================================
// FPS Overlay — Valhalla-themed FPS counter (F6)
// ============================================================================

void ImGuiOverlay::BuildFPSOverlay() {
  if (!m_showFPS) return;

  auto& cust = ConfigManager::Get().Data().customization;
  float screenW = static_cast<float>(m_width);
  float screenH = static_cast<float>(m_height);
  float opacity = std::clamp(cust.fpsOpacity, 0.1f, 1.0f);
  float scale = std::clamp(cust.fpsScale, 0.5f, 2.0f);
  auto fpsPos = static_cast<FPSPosition>(std::clamp(cust.fpsPosition, 0, 3));
  auto fpsStyle = static_cast<FPSStyle>(std::clamp(cust.fpsStyle, 0, 2));

  // Compute FPS color — green/gold/red gradient based on performance
  D2D1_COLOR_F fpsColor;
  if (m_smoothFPS >= 55.0f) {
    fpsColor = vtheme::rgba(0.46f, 0.72f, 0.0f, opacity); // NVIDIA green — good
  } else if (m_smoothFPS >= 30.0f) {
    float t = (m_smoothFPS - 30.0f) / 25.0f;
    fpsColor = vtheme::rgba(
      vanim::Lerp(0.83f, 0.46f, t),
      vanim::Lerp(0.69f, 0.72f, t),
      vanim::Lerp(0.22f, 0.0f, t), opacity); // Gold to green
  } else {
    fpsColor = vtheme::rgba(0.97f, 0.32f, 0.29f, opacity); // Red — bad
  }

  if (fpsStyle == FPSStyle::Minimal) {
    // Minimal: just the FPS number, no background
    float fontSize = 24.0f * scale;
    float textW = 90.0f * scale;
    float textH = 30.0f * scale;
    float x = 0, y = 0;
    switch (fpsPos) {
      case FPSPosition::TopRight:    x = screenW - textW - 16; y = 16; break;
      case FPSPosition::TopLeft:     x = 16; y = 16; break;
      case FPSPosition::BottomRight: x = screenW - textW - 16; y = screenH - textH - 16; break;
      case FPSPosition::BottomLeft:  x = 16; y = screenH - textH - 16; break;
      default: x = screenW - textW - 16; y = 16; break;
    }
    std::string fpsStr = std::format("{:.0f}", m_smoothFPS);
    m_renderer.DrawTextA(fpsStr.c_str(), x, y, textW, textH, fpsColor, fontSize, ValhallaRenderer::TextAlign::Right, true);
    return;
  }

  // Standard / Detailed — Valhalla-themed panel
  float panelW = (fpsStyle == FPSStyle::Detailed) ? 180.0f * scale : 130.0f * scale;
  float panelH = (fpsStyle == FPSStyle::Detailed) ? 90.0f * scale : 54.0f * scale;
  float margin = 16.0f;
  float x = 0, y = 0;
  switch (fpsPos) {
    case FPSPosition::TopRight:    x = screenW - panelW - margin; y = margin; break;
    case FPSPosition::TopLeft:     x = margin; y = margin; break;
    case FPSPosition::BottomRight: x = screenW - panelW - margin; y = screenH - panelH - margin; break;
    case FPSPosition::BottomLeft:  x = margin; y = screenH - panelH - margin; break;
    default: x = screenW - panelW - margin; y = margin; break;
  }

  // Norse-inspired dark panel with gold accent border
  D2D1_COLOR_F panelBg = vtheme::hex(0x0D1117, 0.82f * opacity);
  m_renderer.FillRoundedRect(x, y, panelW, panelH, 6.0f * scale, panelBg);

  // Top accent stripe — gold Norse line
  D2D1_COLOR_F accentLine = vtheme::rgba(m_accent.r, m_accent.g, m_accent.b, 0.7f * opacity);
  m_renderer.FillRect(x + 8.0f * scale, y + 1.0f, panelW - 16.0f * scale, 2.0f, accentLine);

  // Subtle outer border
  m_renderer.OutlineRoundedRect(x, y, panelW, panelH, 6.0f * scale, vtheme::hex(0x30363D, 0.25f * opacity), 1.0f);

  // Norse diamond ornament on left
  float diamondX = x + 12.0f * scale;
  float diamondY = y + panelH * 0.5f - (fpsStyle == FPSStyle::Detailed ? 10.0f * scale : 0.0f);
  m_renderer.DrawDiamond(diamondX, diamondY, 4.0f * scale, accentLine);

  // FPS value — large bold
  float fpsNumX = x + 24.0f * scale;
  float fpsNumY = y + 6.0f * scale;
  float fpsNumW = panelW - 30.0f * scale;
  float fpsNumH = 32.0f * scale;
  std::string fpsStr = std::format("{:.0f}", m_smoothFPS);
  m_renderer.DrawTextA(fpsStr.c_str(), fpsNumX, fpsNumY, fpsNumW, fpsNumH,
                       fpsColor, vtheme::kFontFPS * scale, ValhallaRenderer::TextAlign::Left, true);

  // "FPS" label — muted gold, smaller
  D2D1_COLOR_F labelColor = vtheme::rgba(m_accent.r, m_accent.g, m_accent.b, 0.5f * opacity);
  float labelX = fpsNumX + m_renderer.MeasureTextA(fpsStr, vtheme::kFontFPS * scale, true).width + 4.0f * scale;
  m_renderer.DrawTextA("FPS", labelX, fpsNumY + 10.0f * scale, 40.0f * scale, 20.0f * scale,
                       labelColor, vtheme::kFontFPSLabel * scale, ValhallaRenderer::TextAlign::Left);

  if (fpsStyle == FPSStyle::Detailed) {
    // Thin separator
    float sepY = y + 48.0f * scale;
    m_renderer.DrawLine(x + 10.0f * scale, sepY, x + panelW - 10.0f * scale, sepY, vtheme::hex(0x30363D, 0.4f * opacity), 1.0f);

    // Frame time
    float frameMs = m_smoothFPS > 0.1f ? 1000.0f / m_smoothFPS : 0.0f;
    std::string ftStr = std::format("{:.1f} ms", frameMs);
    m_renderer.DrawTextA(ftStr.c_str(), x + 24.0f * scale, sepY + 4.0f * scale, 80.0f * scale, 16.0f * scale,
                         vtheme::kTextSecondary, 11.0f * scale);

    // GPU usage if available
    if (g_metricsCache.gpuOk.load(std::memory_order_relaxed)) {
      uint32_t gpuPct = g_metricsCache.gpuPercent.load(std::memory_order_relaxed);
      std::string gpuStr = std::format("GPU {}%", gpuPct);
      D2D1_COLOR_F gpuColor = gpuPct > 95 ? vtheme::kStatusWarn : vtheme::kTextSecondary;
      gpuColor.a *= opacity;
      m_renderer.DrawTextA(gpuStr.c_str(), x + 24.0f * scale, sepY + 20.0f * scale, 80.0f * scale, 16.0f * scale,
                           gpuColor, 11.0f * scale);
    }

    // Norse diamond ornament bottom-right
    m_renderer.DrawDiamond(x + panelW - 12.0f * scale, y + panelH - 12.0f * scale, 3.0f * scale, accentLine);
  }
}

// ============================================================================
// Vignette Overlay (F7)
// ============================================================================

void ImGuiOverlay::BuildVignette() {
  if (!m_showVignette) return;

  auto& ui = ConfigManager::Get().Data().ui;
  float screenW = static_cast<float>(m_width);
  float screenH = static_cast<float>(m_height);

  m_renderer.DrawVignette(screenW, screenH,
                          ui.vignetteColorR, ui.vignetteColorG, ui.vignetteColorB,
                          ui.vignetteIntensity, ui.vignetteRadius, ui.vignetteSoftness);
}

// ============================================================================
// Widget helpers
// ============================================================================

bool ImGuiOverlay::PointInRect(float px, float py, float rx, float ry, float rw, float rh) const {
  return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

void ImGuiOverlay::BeginWidgetFrame() {
  // Poll mouse
  POINT cursorPos{};
  GetCursorPos(&cursorPos);
  if (m_hwnd) ScreenToClient(m_hwnd, &cursorPos);
  m_input.mouseX = static_cast<float>(cursorPos.x);
  m_input.mouseY = static_cast<float>(cursorPos.y);
  bool mouseNow = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  m_input.mouseClicked = mouseNow && !m_mouseDownPrev;
  m_input.mouseReleased = !mouseNow && m_mouseDownPrev;
  m_input.mouseDown = mouseNow;
  m_mouseDownPrev = mouseNow;
  m_input.scrollDelta = m_scrollAccum;
  m_scrollAccum = 0;

  // Manage cursor visibility
  if (m_visible || m_showSetupWizard) {
    if (!m_cursorUnlocked) {
      GetClipCursor(&m_prevClip);
      ClipCursor(nullptr);
      // Force system cursor hidden — we draw our own Valhalla cursor.
      // ShowCursor uses an internal counter; drain it to ensure hidden.
      while (ShowCursor(FALSE) >= 0) {}
      SetCursor(nullptr);
      m_cursorUnlocked = true;
    }
  } else {
    if (m_cursorUnlocked) {
      ClipCursor(&m_prevClip);
      // Restore system cursor
      while (ShowCursor(TRUE) < 0) {}
      m_cursorUnlocked = false;
    }
  }
}

// ... [Existing manual widgets omitted for brevity, they remain] ...
// I'm modifying BuildCustomization to use AutoUI

void ImGuiOverlay::NorseSeparator() {
  float y = m_cursorY + 8.0f;
  float lineY = y + 1.0f;
  // Clean subtle separator line
  D2D1_COLOR_F lineColor = vtheme::hex(0x30363D, 0.5f);
  m_renderer.DrawLine(m_cursorX + 4.0f, lineY, m_cursorX + m_contentWidth - 4.0f, lineY, lineColor, 1.0f);
  m_cursorY += 14.0f;
}

void ImGuiOverlay::SectionHeader(const char* label, bool* open) {
  uint32_t id = VGuiHash(label);
  float x = m_cursorX;
  float y = m_cursorY;
  float w = m_contentWidth;
  float h = vtheme::kSectionHeight;

  // Hover animation
  float& hoverT = m_hoverAnim[id];
  bool hovered = PointInRect(m_input.mouseX, m_input.mouseY, x, y, w, h);
  hoverT += (hovered ? 1.0f : -1.0f) * (m_time - m_lastFrameTime) / vtheme::kAnimHoverDuration;
  hoverT = std::clamp(hoverT, 0.0f, 1.0f);

  // Subtle background on hover only
  if (hoverT > 0.01f) {
    D2D1_COLOR_F bg = vtheme::kBgHover;
    bg.a = hoverT * 0.5f;
    m_renderer.FillRoundedRect(x, y, w, h, 6.0f, bg);
  }

  // Chevron indicator (triangle pointing right when closed, down when open)
  float chevX = x + 10.0f;
  float chevY = y + h * 0.5f;
  D2D1_COLOR_F chevColor = m_accent;
  chevColor.a = vanim::Lerp(0.6f, 1.0f, hoverT);
  if (*open) {
    // Down chevron: small filled triangle
    m_renderer.DrawLine(chevX - 3.5f, chevY - 2.0f, chevX, chevY + 2.5f, chevColor, 1.8f);
    m_renderer.DrawLine(chevX, chevY + 2.5f, chevX + 3.5f, chevY - 2.0f, chevColor, 1.8f);
  } else {
    // Right chevron
    m_renderer.DrawLine(chevX - 1.5f, chevY - 4.0f, chevX + 2.5f, chevY, chevColor, 1.8f);
    m_renderer.DrawLine(chevX + 2.5f, chevY, chevX - 1.5f, chevY + 4.0f, chevColor, 1.8f);
  }

  // Label text — accent color when open, secondary when closed
  D2D1_COLOR_F textColor = *open ? m_accent : vtheme::kTextPrimary;
  textColor.r = vanim::Lerp(textColor.r, m_accentBright.r, hoverT * 0.3f);
  textColor.g = vanim::Lerp(textColor.g, m_accentBright.g, hoverT * 0.3f);
  textColor.b = vanim::Lerp(textColor.b, m_accentBright.b, hoverT * 0.3f);
  m_renderer.DrawTextA(label, x + 24.0f, y, w - 32.0f, h, textColor, vtheme::kFontSection, ValhallaRenderer::TextAlign::Left, true);

  // Click to toggle
  if (hovered && m_input.mouseClicked) {
    *open = !(*open);
  }

  m_cursorY += h + 2.0f;
}

void ImGuiOverlay::Label(const char* text, const D2D1_COLOR_F& color) {
  m_renderer.DrawTextA(text, m_cursorX + 4.0f, m_cursorY, m_contentWidth - 8.0f, vtheme::kWidgetHeight, color, vtheme::kFontBody);
  m_cursorY += vtheme::kWidgetHeight;
}

void ImGuiOverlay::LabelValue(const char* label, const char* value) {
  float w = m_contentWidth;
  m_renderer.DrawTextA(label, m_cursorX + 4.0f, m_cursorY, w * 0.55f, vtheme::kWidgetHeight, vtheme::kTextSecondary, vtheme::kFontSmall);
  m_renderer.DrawTextA(value, m_cursorX + w * 0.55f, m_cursorY, w * 0.45f - 4.0f, vtheme::kWidgetHeight, vtheme::kTextPrimary, vtheme::kFontBody, ValhallaRenderer::TextAlign::Right);
  m_cursorY += vtheme::kWidgetHeight;
}

void ImGuiOverlay::StatusDot(const char* label, const D2D1_COLOR_F& color) {
  float dotR = 3.5f;
  float cx = m_cursorX + dotR + 4.0f;
  float cy = m_cursorY + vtheme::kWidgetHeight * 0.5f;

  // Pulse animation for status dots
  float pulseScale = 1.0f;
  if (ConfigManager::Get().Data().customization.statusPulse) {
    pulseScale = 1.0f + std::sin(m_statusPulsePhase) * 0.1f;
  }

  // Subtle glow behind the dot
  D2D1_COLOR_F glow = color;
  glow.a = 0.15f;
  m_renderer.DrawCircle(cx, cy, (dotR + 2.5f) * pulseScale, glow);
  m_renderer.DrawCircle(cx, cy, dotR * pulseScale, color);

  m_renderer.DrawTextA(label, m_cursorX + dotR * 2 + 12.0f, m_cursorY, 100.0f, vtheme::kWidgetHeight, vtheme::kTextSecondary, vtheme::kFontSmall);
}

void ImGuiOverlay::Spacing(float height) {
  m_cursorY += height;
}

void ImGuiOverlay::SameLineButton() {
  m_sameLine = true;
  m_sameLineX = m_lastButtonEndX + 6.0f;
}

bool ImGuiOverlay::Button(const char* label, float w) {
  uint32_t id = VGuiHash(label);
  float h = vtheme::kWidgetHeight;
  float x = m_cursorX;
  float y = m_cursorY;

  if (m_sameLine) {
    auto ts = m_renderer.MeasureTextA(label, vtheme::kFontBody, false);
    if (w <= 0.0f) w = ts.width + 28.0f;
    x = m_sameLineX;
    y = m_lastButtonY;
    m_sameLine = false;
  } else {
    if (w <= 0.0f) w = m_contentWidth;
  }

  // Hover / active animation
  float& hoverT = m_hoverAnim[id];
  bool hovered = PointInRect(m_input.mouseX, m_input.mouseY, x, y, w, h);
  hoverT += (hovered ? 1.0f : -1.0f) * (m_time - m_lastFrameTime) / vtheme::kAnimHoverDuration;
  hoverT = std::clamp(hoverT, 0.0f, 1.0f);

  bool pressed = hovered && m_input.mouseDown;
  bool clicked = hovered && m_input.mouseClicked;

  float cr = 6.0f;

  // Button background — subtle fill with border
  D2D1_COLOR_F bg = vtheme::kBgWidget;
  bg.r = vanim::Lerp(bg.r, vtheme::kBgHover.r, hoverT);
  bg.g = vanim::Lerp(bg.g, vtheme::kBgHover.g, hoverT);
  bg.b = vanim::Lerp(bg.b, vtheme::kBgHover.b, hoverT);
  if (pressed) bg = vtheme::kBgActive;
  m_renderer.FillRoundedRect(x, y, w, h, cr, bg);

  // Thin border — brighter on hover
  D2D1_COLOR_F border = vtheme::hex(0x3D444D, 0.4f);
  border.r = vanim::Lerp(border.r, m_accent.r, hoverT * 0.5f);
  border.g = vanim::Lerp(border.g, m_accent.g, hoverT * 0.5f);
  border.b = vanim::Lerp(border.b, m_accent.b, hoverT * 0.5f);
  border.a = vanim::Lerp(0.35f, 0.7f, hoverT);
  m_renderer.OutlineRoundedRect(x, y, w, h, cr, border, 1.0f);

  // Text — accent color on hover
  D2D1_COLOR_F textColor = vtheme::kTextPrimary;
  textColor.r = vanim::Lerp(textColor.r, m_accentBright.r, hoverT * 0.6f);
  textColor.g = vanim::Lerp(textColor.g, m_accentBright.g, hoverT * 0.6f);
  textColor.b = vanim::Lerp(textColor.b, m_accentBright.b, hoverT * 0.6f);
  m_renderer.DrawTextA(label, x, y, w, h, textColor, vtheme::kFontBody, ValhallaRenderer::TextAlign::Center);

  m_lastButtonEndX = x + w;
  m_lastButtonY = y;
  m_cursorY = y + h + vtheme::kSpacing;
  return clicked;
}

bool ImGuiOverlay::Checkbox(const char* label, bool* value, bool enabled) {
  uint32_t id = VGuiHash(label);
  float x = m_cursorX;
  float y = m_cursorY;
  float rowH = vtheme::kWidgetHeight;
  float togW = vtheme::kToggleW;
  float togH = vtheme::kToggleH;

  bool hovered = enabled && PointInRect(m_input.mouseX, m_input.mouseY, x, y, m_contentWidth, rowH);
  bool clicked = hovered && m_input.mouseClicked;

  float& hoverT = m_hoverAnim[id];
  hoverT += (hovered ? 1.0f : -1.0f) * (m_time - m_lastFrameTime) / vtheme::kAnimHoverDuration;
  hoverT = std::clamp(hoverT, 0.0f, 1.0f);

  // Animated toggle position (0 = off, 1 = on)
  float& toggleT = m_toggleAnim[id];
  float targetT = *value ? 1.0f : 0.0f;
  float animSpeed = 10.0f;
  float dt = m_time - m_lastFrameTime;
  if (dt > 0 && dt < 1.0f) {
    toggleT = vanim::SmoothDamp(toggleT, targetT, animSpeed, dt);
  } else {
    toggleT = targetT;
  }

  // Label text (left side)
  D2D1_COLOR_F textColor = enabled ? vtheme::kTextPrimary : vtheme::kTextSecondary;
  m_renderer.DrawTextA(label, x + 4.0f, y, m_contentWidth - togW - 16.0f, rowH, textColor, vtheme::kFontBody);

  // Toggle switch (right-aligned)
  float togX = x + m_contentWidth - togW - 4.0f;
  float togY = y + (rowH - togH) * 0.5f;
  float togR = togH * 0.5f; // pill radius

  // Track background — interpolate between off/on colors
  D2D1_COLOR_F trackOff = vtheme::hex(0x30363D, 1.0f);
  D2D1_COLOR_F trackOn = m_accent;
  if (!enabled) { trackOff.a = 0.3f; trackOn.a = 0.3f; }
  D2D1_COLOR_F trackColor;
  trackColor.r = vanim::Lerp(trackOff.r, trackOn.r, toggleT);
  trackColor.g = vanim::Lerp(trackOff.g, trackOn.g, toggleT);
  trackColor.b = vanim::Lerp(trackOff.b, trackOn.b, toggleT);
  trackColor.a = vanim::Lerp(trackOff.a, trackOn.a, toggleT);
  m_renderer.FillRoundedRect(togX, togY, togW, togH, togR, trackColor);

  // Knob — slides left to right
  float knobPad = 2.0f;
  float knobD = togH - knobPad * 2.0f;
  float knobMinX = togX + knobPad;
  float knobMaxX = togX + togW - knobD - knobPad;
  float knobX = vanim::Lerp(knobMinX, knobMaxX, toggleT);
  float knobY = togY + knobPad;

  D2D1_COLOR_F knobColor = vtheme::hex(0xFFFFFF, enabled ? 1.0f : 0.4f);
  m_renderer.FillRoundedRect(knobX, knobY, knobD, knobD, knobD * 0.5f, knobColor);

  // Hover highlight on track
  if (hoverT > 0.01f && enabled) {
    D2D1_COLOR_F hoverGlow = m_accent;
    hoverGlow.a = hoverT * 0.12f;
    m_renderer.FillRoundedRect(togX - 2, togY - 2, togW + 4, togH + 4, togR + 2, hoverGlow);
  }

  m_cursorY += rowH + vtheme::kSpacing;

  if (clicked && enabled) {
    *value = !(*value);
    return true;
  }
  return false;
}

bool ImGuiOverlay::SliderFloat(const char* label, float* value, float vmin, float vmax, const char* fmt, bool enabled) {
  uint32_t id = VGuiHash(label);
  float x = m_cursorX;
  float y = m_cursorY;
  float w = m_contentWidth;
  float labelH = 20.0f;

  // Label row — label left, value right
  std::string valStr = std::vformat(fmt, std::make_format_args(*value));
  D2D1_COLOR_F textColor = enabled ? vtheme::kTextSecondary : vtheme::hex(0x484F58, 1.0f);
  m_renderer.DrawTextA(label, x + 4.0f, y, w * 0.65f, labelH, textColor, vtheme::kFontSmall);
  m_renderer.DrawTextA(valStr.c_str(), x + w * 0.65f, y, w * 0.35f - 4.0f, labelH,
                       enabled ? vtheme::kTextPrimary : vtheme::kTextSecondary, vtheme::kFontSmall, ValhallaRenderer::TextAlign::Right);
  y += labelH;

  // Track geometry
  float trackH = 4.0f;
  float trackX = x + 4.0f;
  float trackW = w - 8.0f;
  float trackY = y + 6.0f;

  float t = (vmax > vmin) ? std::clamp((*value - vmin) / (vmax - vmin), 0.0f, 1.0f) : 0.0f;
  float grabCenterX = trackX + t * trackW;

  // Track background (pill shape)
  D2D1_COLOR_F trackBg = enabled ? vtheme::hex(0x21262D, 1.0f) : vtheme::hex(0x1C2128, 0.5f);
  m_renderer.FillRoundedRect(trackX, trackY, trackW, trackH, trackH * 0.5f, trackBg);

  // Filled portion
  if (t > 0.002f) {
    D2D1_COLOR_F fillColor = enabled ? m_accent : vtheme::hex(0x30363D, 0.5f);
    m_renderer.FillRoundedRect(trackX, trackY, t * trackW, trackH, trackH * 0.5f, fillColor);
  }

  // Interaction
  float hitPad = 10.0f;
  bool trackHovered = enabled && PointInRect(m_input.mouseX, m_input.mouseY, trackX - 4.0f, trackY - hitPad, trackW + 8.0f, trackH + hitPad * 2.0f);

  if (trackHovered && m_input.mouseClicked && enabled) {
    m_activeId = id;
  }

  bool changed = false;
  if (m_activeId == id) {
    if (m_input.mouseDown) {
      float newT = std::clamp((m_input.mouseX - trackX) / trackW, 0.0f, 1.0f);
      float newVal = vmin + newT * (vmax - vmin);
      if (std::abs(newVal - *value) > 0.0001f) {
        *value = newVal;
        changed = true;
      }
    } else {
      m_activeId = 0;
    }
  }

  // Grab handle — circle style
  float grabR = 7.0f;
  bool isDragging = (m_activeId == id);
  bool grabHovered = enabled && PointInRect(m_input.mouseX, m_input.mouseY, grabCenterX - grabR - 2, trackY - grabR - 2, grabR * 2 + 4, grabR * 2 + 4);

  // Hover animation for grab
  float& hoverT = m_hoverAnim[id];
  hoverT += ((grabHovered || isDragging) ? 1.0f : -1.0f) * (m_time - m_lastFrameTime) / vtheme::kAnimHoverDuration;
  hoverT = std::clamp(hoverT, 0.0f, 1.0f);

  float grabDrawR = vanim::Lerp(grabR - 1.0f, grabR, hoverT);
  float grabCY = trackY + trackH * 0.5f;
  D2D1_COLOR_F grabColor = enabled ? vtheme::hex(0xFFFFFF, 1.0f) : vtheme::hex(0x484F58, 1.0f);
  m_renderer.DrawCircle(grabCenterX, grabCY, grabDrawR, grabColor);

  // Accent ring on hover/drag
  if (hoverT > 0.01f && enabled) {
    D2D1_COLOR_F ring = m_accent;
    ring.a = hoverT * 0.25f;
    m_renderer.DrawCircle(grabCenterX, grabCY, grabDrawR + 4.0f, ring);
  }

  m_cursorY = y + 20.0f;
  return changed;
}

bool ImGuiOverlay::Combo(const char* label, int* selectedIndex, const char* const* items, int itemCount, bool enabled) {
  uint32_t id = VGuiHash(label);
  float x = m_cursorX;
  float y = m_cursorY;
  float w = m_contentWidth;
  float h = vtheme::kComboHeight;

  // Label above
  D2D1_COLOR_F textColor = enabled ? vtheme::kTextSecondary : vtheme::hex(0x484F58, 1.0f);
  m_renderer.DrawTextA(label, x + 4.0f, y, w, 18.0f, textColor, vtheme::kFontSmall);
  y += 20.0f;

  // Combo header
  bool isOpen = (m_openComboId == id);
  bool headerHovered = enabled && PointInRect(m_input.mouseX, m_input.mouseY, x, y, w, h);

  float& hoverT = m_hoverAnim[id];
  hoverT += (headerHovered ? 1.0f : -1.0f) * (m_time - m_lastFrameTime) / vtheme::kAnimHoverDuration;
  hoverT = std::clamp(hoverT, 0.0f, 1.0f);

  // Background
  D2D1_COLOR_F bg = vtheme::kBgWidget;
  bg.r = vanim::Lerp(bg.r, vtheme::kBgHover.r, hoverT);
  bg.g = vanim::Lerp(bg.g, vtheme::kBgHover.g, hoverT);
  bg.b = vanim::Lerp(bg.b, vtheme::kBgHover.b, hoverT);
  if (!enabled) bg.a *= 0.5f;
  m_renderer.FillRoundedRect(x, y, w, h, 6.0f, bg);

  // Border
  D2D1_COLOR_F border = vtheme::hex(0x3D444D, 0.4f);
  border.r = vanim::Lerp(border.r, m_accent.r, hoverT * 0.4f);
  border.g = vanim::Lerp(border.g, m_accent.g, hoverT * 0.4f);
  border.b = vanim::Lerp(border.b, m_accent.b, hoverT * 0.4f);
  border.a = vanim::Lerp(0.35f, 0.6f, hoverT);
  m_renderer.OutlineRoundedRect(x, y, w, h, 6.0f, border, 1.0f);

  // Current selection text
  const char* currentText = (*selectedIndex >= 0 && *selectedIndex < itemCount) ? items[*selectedIndex] : "---";
  m_renderer.DrawTextA(currentText, x + 10.0f, y, w - 36.0f, h,
                       enabled ? vtheme::kTextPrimary : vtheme::kTextSecondary, vtheme::kFontBody);

  // Dropdown chevron
  float arrowX = x + w - 18.0f;
  float arrowY = y + h * 0.5f;
  D2D1_COLOR_F chevColor = isOpen ? m_accent : vtheme::kTextSecondary;
  if (isOpen) {
    m_renderer.DrawLine(arrowX - 4.0f, arrowY + 1.5f, arrowX, arrowY - 2.5f, chevColor, 1.5f);
    m_renderer.DrawLine(arrowX, arrowY - 2.5f, arrowX + 4.0f, arrowY + 1.5f, chevColor, 1.5f);
  } else {
    m_renderer.DrawLine(arrowX - 4.0f, arrowY - 1.5f, arrowX, arrowY + 2.5f, chevColor, 1.5f);
    m_renderer.DrawLine(arrowX, arrowY + 2.5f, arrowX + 4.0f, arrowY - 1.5f, chevColor, 1.5f);
  }

  // Toggle dropdown on click
  if (headerHovered && m_input.mouseClicked && enabled) {
    m_openComboId = isOpen ? 0 : id;
    isOpen = !isOpen;
  }

  float advanceY = h + 4.0f;
  bool changed = false;

  // Dropdown items
  if (isOpen && enabled) {
    float itemY = y + h + 2.0f;
    // Dropdown container background
    float dropH = static_cast<float>(itemCount) * (h - 2.0f) + static_cast<float>(itemCount - 1) * 1.0f;
    m_renderer.FillRoundedRect(x, itemY - 1.0f, w, dropH + 2.0f, 6.0f, vtheme::hex(0x1C2128, 0.98f));
    m_renderer.OutlineRoundedRect(x, itemY - 1.0f, w, dropH + 2.0f, 6.0f, vtheme::hex(0x3D444D, 0.3f), 1.0f);

    for (int i = 0; i < itemCount; ++i) {
      float ih = h - 2.0f;
      bool itemHov = PointInRect(m_input.mouseX, m_input.mouseY, x, itemY, w, ih);
      bool isSel = (i == *selectedIndex);

      // Item background
      if (isSel) {
        D2D1_COLOR_F selBg = m_accent;
        selBg.a = 0.15f;
        m_renderer.FillRoundedRect(x + 2, itemY, w - 4, ih, 4.0f, selBg);
      } else if (itemHov) {
        m_renderer.FillRoundedRect(x + 2, itemY, w - 4, ih, 4.0f, vtheme::hex(0x30363D, 0.6f));
      }

      // Selection accent dot
      if (isSel) {
        m_renderer.DrawCircle(x + 12.0f, itemY + ih * 0.5f, 3.0f, m_accent);
      }

      D2D1_COLOR_F itemText = isSel ? m_accent : vtheme::kTextPrimary;
      m_renderer.DrawTextA(items[i], x + (isSel ? 22.0f : 10.0f), itemY, w - 24.0f, ih, itemText, vtheme::kFontBody);

      if (itemHov && m_input.mouseClicked) {
        *selectedIndex = i;
        m_openComboId = 0;
        changed = true;
      }
      itemY += ih + 1.0f;
    }
    advanceY += dropH + 6.0f;
  }

  m_cursorY = y + advanceY;
  return changed;
}

bool ImGuiOverlay::ColorEdit3(const char* label, float* r, float* g, float* b) {
  uint32_t id = VGuiHash(label);
  float x = m_cursorX;
  float y = m_cursorY;
  float w = m_contentWidth;
  float h = vtheme::kWidgetHeight;
  bool isOpen = (m_openColorId == id);

  // Color swatch + label
  float swatchSize = 18.0f;
  float swatchX = x + 4.0f;
  float swatchY = y + (h - swatchSize) * 0.5f;
  m_renderer.FillRoundedRect(swatchX, swatchY, swatchSize, swatchSize, 4.0f, vtheme::rgba(*r, *g, *b, 1.0f));
  m_renderer.OutlineRoundedRect(swatchX, swatchY, swatchSize, swatchSize, 4.0f, vtheme::hex(0x484F58, 0.5f), 1.0f);

  m_renderer.DrawTextA(label, x + swatchSize + 12.0f, y, w - swatchSize - 16.0f, h, vtheme::kTextPrimary, vtheme::kFontBody);

  // Click to expand/collapse
  bool headerHovered = PointInRect(m_input.mouseX, m_input.mouseY, x, y, w, h);
  if (headerHovered && m_input.mouseClicked) {
    m_openColorId = isOpen ? 0 : id;
    isOpen = !isOpen;
  }

  // Expand indicator
  D2D1_COLOR_F chevColor = isOpen ? m_accent : vtheme::kTextSecondary;
  float chevX = x + w - 14.0f;
  float chevY = y + h * 0.5f;
  if (isOpen) {
    m_renderer.DrawLine(chevX - 3, chevY + 1.5f, chevX, chevY - 2, chevColor, 1.5f);
    m_renderer.DrawLine(chevX, chevY - 2, chevX + 3, chevY + 1.5f, chevColor, 1.5f);
  } else {
    m_renderer.DrawLine(chevX - 3, chevY - 1.5f, chevX, chevY + 2, chevColor, 1.5f);
    m_renderer.DrawLine(chevX, chevY + 2, chevX + 3, chevY - 1.5f, chevColor, 1.5f);
  }

  m_cursorY += h + 2.0f;
  bool changed = false;

  if (isOpen) {
    float tempR = *r, tempG = *g, tempB = *b;
    if (SliderFloat("  Red", &tempR, 0.0f, 1.0f, "%.2f")) { *r = tempR; changed = true; }
    if (SliderFloat("  Green", &tempG, 0.0f, 1.0f, "%.2f")) { *g = tempG; changed = true; }
    if (SliderFloat("  Blue", &tempB, 0.0f, 1.0f, "%.2f")) { *b = tempB; changed = true; }
  }

  return changed;
}

void ImGuiOverlay::PlotLines(const char* label, const float* values, int count, int offset,
                             float vmin, float vmax, float graphH) {
  float x = m_cursorX + 4.0f;
  float y = m_cursorY;
  float w = m_contentWidth - 8.0f;

  // Label
  m_renderer.DrawTextA(label, x, y, w, 16.0f, vtheme::kTextSecondary, vtheme::kFontSmall);
  y += 18.0f;

  // Graph background
  m_renderer.FillRoundedRect(x, y, w, graphH, 6.0f, vtheme::hex(0x161B22, 1.0f));
  m_renderer.OutlineRoundedRect(x, y, w, graphH, 6.0f, vtheme::hex(0x30363D, 0.25f), 1.0f);

  // Subtle horizontal grid lines
  for (int g = 1; g <= 3; ++g) {
    float gy = y + graphH * static_cast<float>(g) / 4.0f;
    m_renderer.DrawLine(x + 4.0f, gy, x + w - 4.0f, gy, vtheme::hex(0x21262D, 0.6f), 1.0f);
  }

  // Draw lines
  float range = vmax - vmin;
  if (range < 0.001f) range = 1.0f;
  float stepX = w / static_cast<float>(count > 1 ? count - 1 : 1);
  float pad = 4.0f;
  float drawH = graphH - pad * 2.0f;

  for (int i = 1; i < count; ++i) {
    int idx0 = (offset + i - 1) % count;
    int idx1 = (offset + i) % count;
    float t0 = std::clamp((values[idx0] - vmin) / range, 0.0f, 1.0f);
    float t1 = std::clamp((values[idx1] - vmin) / range, 0.0f, 1.0f);
    float x0 = x + static_cast<float>(i - 1) * stepX;
    float x1 = x + static_cast<float>(i) * stepX;
    float y0 = y + pad + drawH - t0 * drawH;
    float y1 = y + pad + drawH - t1 * drawH;
    m_renderer.DrawLine(x0, y0, x1, y1, m_accent, 1.5f);
  }

  m_cursorY = y + graphH + vtheme::kSpacing;
}

// ============================================================================
// Main panel content
// ============================================================================

void ImGuiOverlay::BuildMainPanel() {
  ModConfig& cfg = ConfigManager::Get().Data();
  auto& cust = cfg.customization;
  StreamlineIntegration& sli = StreamlineIntegration::Get();

  float screenW = static_cast<float>(m_width);
  float screenH = static_cast<float>(m_height);
  float panelW = std::clamp(cust.panelWidth, 360.0f, 720.0f);
  float panelH = screenH;
  // cornerRadius is available via cust.cornerRadius if ever needed for sub-panels
  float panelOpacity = std::clamp(cust.panelOpacity, 0.3f, 1.0f);
  float fontScl = std::clamp(cust.fontScale, 0.75f, 1.5f);

  // --- Animation-driven position, alpha, scale ---
  float panelDrawX = m_panelX, panelDrawY = m_panelY;
  float alpha = m_panelAlpha.current;
  float scale = 1.0f;
  ComputePanelTransform(m_panelSlide.current, screenW, screenH, panelW, panelH,
                        panelDrawX, panelDrawY, alpha, scale);
  m_panelScale = scale;

  if (alpha < 0.01f) return; // fully hidden

  // --- Update accent colors dynamically ---
  m_accent = vtheme::rgba(cust.accentR, cust.accentG, cust.accentB, 1.0f);
  m_accentBright = vtheme::rgba(
    std::clamp(cust.accentR * 1.3f, 0.0f, 1.0f),
    std::clamp(cust.accentG * 1.3f, 0.0f, 1.0f),
    std::clamp(cust.accentB * 1.3f, 0.0f, 1.0f), 1.0f);
  m_accentDim = vtheme::rgba(cust.accentR * 0.65f, cust.accentG * 0.65f, cust.accentB * 0.65f, 0.6f);

  // --- Panel shadow (multi-layer) ---
  BuildPanelShadow(panelDrawX, panelDrawY, panelW, panelH, alpha);

  // --- Panel background — clean solid with subtle top-to-bottom gradient ---
  D2D1_COLOR_F bgTop = vtheme::kBgPanel;
  D2D1_COLOR_F bgBottom = vtheme::kBgDeep;
  bgTop.a *= alpha * panelOpacity;
  bgBottom.a *= alpha * panelOpacity;
  m_renderer.FillGradientV(panelDrawX, panelDrawY, panelW, panelH, bgTop, bgBottom);

  // Subtle right border only
  D2D1_COLOR_F edgeColor = vtheme::hex(0x30363D, 0.3f * alpha);
  m_renderer.DrawLine(panelDrawX + panelW - 1, panelDrawY, panelDrawX + panelW - 1, panelDrawY + panelH, edgeColor, 1.0f);

  // --- Title bar ---
  float titleH = vtheme::kTitleBarHeight;
  D2D1_COLOR_F titleBg = vtheme::hex(0x0D1117, 0.98f * alpha);
  m_renderer.FillRect(panelDrawX, panelDrawY, panelW, titleH, titleBg);

  // Title text — clean and minimal
  m_renderer.DrawTextA("AC VALHALLA", panelDrawX + 16.0f, panelDrawY, panelW * 0.5f, titleH,
                       m_accent, vtheme::kFontTitle * fontScl, ValhallaRenderer::TextAlign::Left, true);
  m_renderer.DrawTextA("DLSS 4.5", panelDrawX + 16.0f, panelDrawY, panelW - 56.0f, titleH,
                       vtheme::kTextSecondary, vtheme::kFontSmall, ValhallaRenderer::TextAlign::Right);

  // --- Dragging on title bar ---
  bool onTitleBar = PointInRect(m_input.mouseX, m_input.mouseY, panelDrawX, panelDrawY, panelW - 40.0f, titleH);
  if (onTitleBar && m_input.mouseClicked && !m_dragging) {
    m_dragging = true;
    m_dragOffsetX = m_input.mouseX - m_panelX;
    m_dragOffsetY = m_input.mouseY - m_panelY;
  }
  if (m_dragging) {
    if (m_input.mouseDown) {
      m_panelX = m_input.mouseX - m_dragOffsetX;
      m_panelY = m_input.mouseY - m_dragOffsetY;
      m_panelX = std::clamp(m_panelX, -panelW + 60.0f, screenW - 60.0f);
      m_panelY = std::clamp(m_panelY, 0.0f, screenH - titleH);
      SnapPanel(screenW, screenH);
      cust.panelX = m_panelX;
      cust.panelY = m_panelY;
      ConfigManager::Get().MarkDirty();
    } else {
      m_dragging = false;
    }
  }

  // Close button [X] — minimal circle style
  float closeS = 24.0f;
  float closeX = panelDrawX + panelW - closeS - 12.0f;
  float closeY = panelDrawY + (titleH - closeS) * 0.5f;
  bool closeHovered = PointInRect(m_input.mouseX, m_input.mouseY, closeX, closeY, closeS, closeS);
  if (closeHovered) {
    m_renderer.FillRoundedRect(closeX, closeY, closeS, closeS, closeS * 0.5f, vtheme::hex(0xF85149, 0.2f));
  }
  D2D1_COLOR_F closeColor = closeHovered ? vtheme::kStatusBad : vtheme::hex(0x8B949E, 0.6f);
  float cx = closeX + closeS * 0.5f;
  float cy = closeY + closeS * 0.5f;
  m_renderer.DrawLine(cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f, closeColor, 1.5f);
  m_renderer.DrawLine(cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f, closeColor, 1.5f);
  if (closeHovered && m_input.mouseClicked) {
    ToggleVisibility();
    return;
  }

  // Title bar separator — subtle line
  m_renderer.DrawLine(panelDrawX, panelDrawY + titleH, panelDrawX + panelW, panelDrawY + titleH, vtheme::hex(0x30363D, 0.4f), 1.0f);

  // --- Status bar ---
  float statusY = panelDrawY + titleH + 2.0f;
  float statusH = vtheme::kStatusBarHeight;
  float dotX = panelDrawX + vtheme::kPadding;

  bool dlssOk = sli.IsDLSSSupported() && sli.GetDLSSModeIndex() > 0;
  bool dlssWarn = sli.IsDLSSSupported() && sli.GetDLSSModeIndex() == 0;
  bool fgDisabled = sli.IsFrameGenDisabledDueToInvalidParam();
  bool fgOk = sli.IsFrameGenSupported() && !fgDisabled && sli.GetFrameGenMultiplier() >= 2 &&
              !sli.IsSmartFGTemporarilyDisabled() && sli.GetFrameGenStatus() == sl::DLSSGStatus::eOk;
  bool fgWarn = sli.IsFrameGenSupported() && !fgDisabled &&
                (sli.GetFrameGenMultiplier() < 2 || sli.IsSmartFGTemporarilyDisabled());
  bool camOk = sli.HasCameraData();
  bool dvcOk = sli.IsDeepDVCSupported() && sli.IsDeepDVCEnabled();
  bool dvcWarn = sli.IsDeepDVCSupported() && !sli.IsDeepDVCEnabled();
  bool hdrOk = sli.IsHDRSupported() && sli.IsHDRActive();
  bool hdrWarn = sli.IsHDRSupported() && !sli.IsHDRActive() && sli.IsHDREnabled();

  // Backup cursor for status dots inline
  m_cursorX = dotX;
  m_cursorY = statusY;
  m_contentWidth = 90.0f;
  StatusDot("DLSS", dlssOk ? vtheme::kStatusOk : (dlssWarn ? vtheme::kStatusWarn : vtheme::kStatusBad));
  m_cursorX = dotX + 85.0f; m_cursorY = statusY;
  StatusDot("FG", fgOk ? vtheme::kStatusOk : (fgWarn ? vtheme::kStatusWarn : vtheme::kStatusBad));
  m_cursorX = dotX + 155.0f; m_cursorY = statusY;
  StatusDot("Camera", camOk ? vtheme::kStatusOk : vtheme::kStatusWarn);
  m_cursorX = dotX + 250.0f; m_cursorY = statusY;
  StatusDot("DVC", dvcOk ? vtheme::kStatusOk : (dvcWarn ? vtheme::kStatusWarn : vtheme::kStatusBad));
  m_cursorX = dotX + 330.0f; m_cursorY = statusY;
  StatusDot("HDR", hdrOk ? vtheme::kStatusOk : (hdrWarn ? vtheme::kStatusWarn : vtheme::kStatusBad));

  // --- Scrollable content area ---
  float contentStartY = panelDrawY + titleH + statusH + 8.0f;
  float contentH = panelH - (titleH + statusH + 8.0f) - 8.0f;
  m_visibleHeight = contentH;

  m_renderer.PushClip(panelDrawX, contentStartY, panelW, contentH);

  // Setup cursor for content
  m_cursorX = panelDrawX + vtheme::kPadding;
  m_cursorY = contentStartY - m_scrollOffset;
  m_contentWidth = panelW - vtheme::kPadding * 2.0f - vtheme::kScrollbarW;
  float contentStartCursorY = m_cursorY;

  // Hotkey capture overlay
  if (m_pendingHotkeyTarget) {
    int key = -1;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x1) key = VK_ESCAPE;
    else {
      for (int scanKey = 0x08; scanKey <= 0xFE; ++scanKey) {
        if (GetAsyncKeyState(scanKey) & 0x1) { key = scanKey; break; }
      }
    }
    if (key != -1) {
      if (key == VK_ESCAPE) { m_pendingHotkeyTarget = nullptr; }
      else {
        *m_pendingHotkeyTarget = key;
        m_pendingHotkeyTarget = nullptr;
        InputHandler::Get().UpdateHotkey("Toggle Menu", cfg.ui.menuHotkey);
        InputHandler::Get().UpdateHotkey("Toggle FPS", cfg.ui.fpsHotkey);
        InputHandler::Get().UpdateHotkey("Toggle Vignette", cfg.ui.vignetteHotkey);
        ConfigManager::Get().MarkDirty();
      }
    }
    Label(">> Press a key to rebind (Esc to cancel) <<", vtheme::kGoldBright);
  }

  // ---- WIZARD BUTTON ----
  if (Button("Run Setup Wizard")) {
    m_showSetupWizard = true;
    cfg.system.setupWizardForceShow = true;
    ConfigManager::Get().MarkDirty();
  }

  NorseSeparator();

  // ---- PRESETS ----
  {
    bool presetOpen = m_sectionOpen[VGuiHash("presets_section")];
    SectionHeader("Quick Presets", &presetOpen);
    m_sectionOpen[VGuiHash("presets_section")] = presetOpen;
    if (presetOpen) {
      if (Button("Quality")) {
        cfg.dlss.mode = 5; cfg.dlss.preset = 0; cfg.fg.multiplier = 2; cfg.dlss.sharpness = 0.2f;
        cfg.dlss.lodBias = -1.0f; cfg.rr.enabled = true; cfg.dvc.enabled = false;
        sli.SetDLSSModeIndex(cfg.dlss.mode); sli.SetDLSSPreset(cfg.dlss.preset);
        sli.SetFrameGenMultiplier(cfg.fg.multiplier); sli.SetSharpness(cfg.dlss.sharpness);
        sli.SetLODBias(cfg.dlss.lodBias); ApplySamplerLodBias(cfg.dlss.lodBias);
        sli.SetReflexEnabled(cfg.rr.enabled); sli.SetRayReconstructionEnabled(cfg.rr.enabled);
        sli.SetDeepDVCEnabled(cfg.dvc.enabled); ConfigManager::Get().MarkDirty();
      }
      SameLineButton();
      if (Button("Balanced")) {
        cfg.dlss.mode = 2; cfg.dlss.preset = 0; cfg.fg.multiplier = 3; cfg.dlss.sharpness = 0.35f;
        cfg.dlss.lodBias = -1.0f; cfg.rr.enabled = true; cfg.dvc.enabled = false; cfg.dvc.adaptiveEnabled = false;
        sli.SetDLSSModeIndex(cfg.dlss.mode); sli.SetDLSSPreset(cfg.dlss.preset);
        sli.SetFrameGenMultiplier(cfg.fg.multiplier); sli.SetSharpness(cfg.dlss.sharpness);
        sli.SetLODBias(cfg.dlss.lodBias); ApplySamplerLodBias(cfg.dlss.lodBias);
        sli.SetReflexEnabled(cfg.rr.enabled); sli.SetRayReconstructionEnabled(cfg.rr.enabled);
        sli.SetDeepDVCEnabled(cfg.dvc.enabled); sli.SetDeepDVCAdaptiveEnabled(cfg.dvc.adaptiveEnabled);
        ConfigManager::Get().MarkDirty();
      }
      SameLineButton();
      if (Button("Performance")) {
        cfg.dlss.mode = 1; cfg.dlss.preset = 0; cfg.fg.multiplier = 4; cfg.dlss.sharpness = 0.5f;
        cfg.dlss.lodBias = -1.2f; cfg.rr.enabled = true; cfg.dvc.enabled = false; cfg.dvc.adaptiveEnabled = false;
        sli.SetDLSSModeIndex(cfg.dlss.mode); sli.SetDLSSPreset(cfg.dlss.preset);
        sli.SetFrameGenMultiplier(cfg.fg.multiplier); sli.SetSharpness(cfg.dlss.sharpness);
        sli.SetLODBias(cfg.dlss.lodBias); ApplySamplerLodBias(cfg.dlss.lodBias);
        sli.SetReflexEnabled(cfg.rr.enabled); sli.SetRayReconstructionEnabled(cfg.rr.enabled);
        sli.SetDeepDVCEnabled(cfg.dvc.enabled); sli.SetDeepDVCAdaptiveEnabled(cfg.dvc.adaptiveEnabled);
        ConfigManager::Get().MarkDirty();
      }
      Spacing();
    }
  }

  // ---- GENERAL ----
  {
    auto genIt = m_sectionOpen.find(VGuiHash("general_section"));
    bool open = (genIt != m_sectionOpen.end()) ? genIt->second : true; // default open
    SectionHeader("General", &open);
    m_sectionOpen[VGuiHash("general_section")] = open;
    if (open) {
      // Manual combo handling as they depend on runtime values (modes)
      const char* dlssModes[] = {"Off", "Max Performance", "Balanced", "Max Quality", "Ultra Quality", "DLAA"};
      int dlssMode = sli.GetDLSSModeIndex();
      if (Combo("DLSS Quality Mode", &dlssMode, dlssModes, 6, sli.IsDLSSSupported())) {
        sli.SetDLSSModeIndex(dlssMode); cfg.dlss.mode = dlssMode; ConfigManager::Get().MarkDirty();
      }
      const char* presets[] = {"Default", "Preset A", "Preset B", "Preset C", "Preset D", "Preset E", "Preset F", "Preset G"};
      int preset = sli.GetDLSSPresetIndex();
      if (Combo("DLSS Preset", &preset, presets, 8)) {
        sli.SetDLSSPreset(preset); cfg.dlss.preset = preset; ConfigManager::Get().MarkDirty();
      }
      // Auto-UI for other general settings
      if (AutoUI::DrawStruct(*this, cfg.dlss)) ConfigManager::Get().MarkDirty();
      Spacing();
    }
  }

  // ---- RAY RECONSTRUCTION ----
  {
    bool open = m_sectionOpen[VGuiHash("rr_section")];
    SectionHeader("Ray Reconstruction", &open);
    m_sectionOpen[VGuiHash("rr_section")] = open;
    if (open) {
      // Manual handling for now due to complex dependencies
      bool rrEnabled = cfg.rr.enabled;
      if (Checkbox("Enable DLSS Ray Reconstruction", &rrEnabled, sli.IsRayReconstructionSupported())) {
        cfg.rr.enabled = rrEnabled; sli.SetRayReconstructionEnabled(rrEnabled); ConfigManager::Get().MarkDirty();
      }
      bool rrActive = sli.IsRayReconstructionSupported() && cfg.rr.enabled;
      const char* rrPresets[] = {"Default", "Preset D", "Preset E", "Preset F", "Preset G", "Preset H", "Preset I", "Preset J", "Preset K", "Preset L", "Preset M", "Preset N", "Preset O"};
      int rrPreset = cfg.rr.preset;
      if (Combo("RR Preset", &rrPreset, rrPresets, 13, rrActive)) {
        cfg.rr.preset = rrPreset; sli.SetRRPreset(rrPreset); ConfigManager::Get().MarkDirty();
      }
      float rrStr = cfg.rr.denoiserStrength;
      if (SliderFloat("RR Denoiser Strength", &rrStr, 0.0f, 1.0f, "%.2f", rrActive)) {
        cfg.rr.denoiserStrength = rrStr; sli.SetRRDenoiserStrength(rrStr); ConfigManager::Get().MarkDirty();
      }
      Spacing();
    }
  }

  // ---- DEEP DVC ----
  {
    bool open = m_sectionOpen[VGuiHash("dvc_section")];
    SectionHeader("DeepDVC (RTX Dynamic Vibrance)", &open);
    m_sectionOpen[VGuiHash("dvc_section")] = open;
    if (open) {
      if (AutoUI::DrawStruct(*this, cfg.dvc)) ConfigManager::Get().MarkDirty();
      Spacing();
    }
  }

  // ---- FRAME GENERATION ----
  {
    bool open = m_sectionOpen[VGuiHash("fg_section")];
    SectionHeader("Frame Generation", &open);
    m_sectionOpen[VGuiHash("fg_section")] = open;
    if (open) {
      const char* fgModes[] = {"Off", "2x (DLSS-G)", "3x (DLSS-G)", "4x (DLSS-G)", "5x (DLSS-G)", "6x (DLSS-G)", "7x (DLSS-G)", "8x (DLSS-G)"};
      int fgMult = sli.GetFrameGenMultiplier();
      int fgIndex = (fgMult >= 2 && fgMult <= 8) ? (fgMult - 1) : 0;
      if (Combo("Frame Generation", &fgIndex, fgModes, 8, sli.IsFrameGenSupported())) {
        int mult = fgIndex > 0 ? (fgIndex + 1) : 0;
        sli.SetFrameGenMultiplier(mult); cfg.fg.multiplier = mult; ConfigManager::Get().MarkDirty();
      }
      if (AutoUI::DrawStruct(*this, cfg.fg)) ConfigManager::Get().MarkDirty();
      Spacing();
    }
  }

  // ---- SMART FG ----
  // (Merged into Frame Generation struct in Reflection but UI separated, kept manual or we need categories)
  // Since we don't have categories fully set up for separation in `fg` struct (all "Smart FG"),
  // we can skip this or use AutoUI filtered by category.
  // Actually, FrameGenConfig reflection has "Smart FG" category.
  // The above AutoUI::DrawStruct(*this, cfg.fg) draws EVERYTHING in FrameGenConfig, including SmartFG.
  // So I should remove the separate Smart FG section if I want to rely on AutoUI, OR
  // use DrawCategory.
  
  // Let's rely on the previous section handling it via DrawStruct(cfg.fg) which iterates all fields.
  
  // ---- QUALITY ----
  {
    bool open = m_sectionOpen[VGuiHash("quality_section")];
    SectionHeader("Quality", &open);
    m_sectionOpen[VGuiHash("quality_section")] = open;
    if (open) {
      if (AutoUI::DrawStruct(*this, cfg.mvec)) ConfigManager::Get().MarkDirty();
      Spacing();
    }
  }

  // ---- HDR ----
  {
    bool open = m_sectionOpen[VGuiHash("hdr_section")];
    SectionHeader("HDR", &open);
    m_sectionOpen[VGuiHash("hdr_section")] = open;
    if (open) {
      if (AutoUI::DrawStruct(*this, cfg.hdr)) ConfigManager::Get().MarkDirty();
      Spacing();
    }
  }

  // ---- OVERLAY ----
  {
    bool open = m_sectionOpen[VGuiHash("overlay_section")];
    SectionHeader("Overlay", &open);
    m_sectionOpen[VGuiHash("overlay_section")] = open;
    if (open) {
      if (AutoUI::DrawStruct(*this, cfg.ui)) ConfigManager::Get().MarkDirty();
      Spacing();
    }
  }

  // ---- VIGNETTE ----
  {
    bool open = m_sectionOpen[VGuiHash("vignette_section")];
    SectionHeader("Vignette (F7)", &open);
    m_sectionOpen[VGuiHash("vignette_section")] = open;
    if (open) {
      bool vigOn = cfg.ui.showVignette;
      if (Checkbox("Enable Vignette", &vigOn)) {
        cfg.ui.showVignette = vigOn; m_showVignette = vigOn; ConfigManager::Get().MarkDirty();
      }
      bool vigActive = cfg.ui.showVignette;
      if (SliderFloat("Intensity", &cfg.ui.vignetteIntensity, 0.0f, 1.0f, "%.2f", vigActive)) {
        ConfigManager::Get().MarkDirty();
      }
      if (SliderFloat("Radius", &cfg.ui.vignetteRadius, 0.0f, 1.0f, "%.2f", vigActive)) {
        ConfigManager::Get().MarkDirty();
      }
      if (SliderFloat("Softness", &cfg.ui.vignetteSoftness, 0.0f, 1.0f, "%.2f", vigActive)) {
        ConfigManager::Get().MarkDirty();
      }
      if (ColorEdit3("Vignette Color", &cfg.ui.vignetteColorR, &cfg.ui.vignetteColorG, &cfg.ui.vignetteColorB)) {
        ConfigManager::Get().MarkDirty();
      }
      Spacing();
    }
  }

  // ---- HOTKEYS ----
  {
    bool open = m_sectionOpen[VGuiHash("hotkeys_section")];
    SectionHeader("Hotkeys", &open);
    m_sectionOpen[VGuiHash("hotkeys_section")] = open;
    if (open) {
      Label("Click a button to rebind. Press Esc to cancel.", vtheme::kTextSecondary);
      std::string menuKey = std::format("Menu: {}", InputHandler::Get().GetKeyName(cfg.ui.menuHotkey));
      if (Button(menuKey.c_str())) CaptureNextHotkey(&cfg.ui.menuHotkey);
      std::string fpsKey = std::format("FPS: {}", InputHandler::Get().GetKeyName(cfg.ui.fpsHotkey));
      if (Button(fpsKey.c_str())) CaptureNextHotkey(&cfg.ui.fpsHotkey);
      std::string vigKey = std::format("Vignette: {}", InputHandler::Get().GetKeyName(cfg.ui.vignetteHotkey));
      if (Button(vigKey.c_str())) CaptureNextHotkey(&cfg.ui.vignetteHotkey);
      Spacing();
    }
  }

  // ---- CUSTOMIZATION ----
  BuildCustomization();

  // ---- PERFORMANCE ----
  {
    bool open = m_sectionOpen[VGuiHash("perf_section")];
    SectionHeader("Performance", &open);
    m_sectionOpen[VGuiHash("perf_section")] = open;
    if (open) {
      // FPS graph
      float minFps = m_fpsHistory[0], maxFps = m_fpsHistory[0];
      for (int i = 1; i < kFpsHistorySize; ++i) {
        minFps = (std::min)(minFps, m_fpsHistory[i]);
        maxFps = (std::max)(maxFps, m_fpsHistory[i]);
      }
      float graphMax = maxFps > 1.0f ? maxFps * 1.15f : 60.0f;
      std::string graphLabel = std::format("FPS (min {:.0f} / max {:.0f})", minFps, maxFps);
      PlotLines(graphLabel.c_str(), m_fpsHistory, kFpsHistorySize, m_fpsHistoryIndex, 0.0f, graphMax, 70.0f);

      NorseSeparator();

      // Metrics
      if (g_metricsCache.gpuOk.load(std::memory_order_relaxed)) {
        std::string gpuStr = std::format("{}%", g_metricsCache.gpuPercent.load(std::memory_order_relaxed));
        LabelValue("GPU Utilization", gpuStr.c_str());
      } else {
        LabelValue("GPU Utilization", "N/A");
      }
      if (g_metricsCache.vramOk.load(std::memory_order_relaxed)) {
        uint32_t used = g_metricsCache.vramUsed.load(std::memory_order_relaxed);
        uint32_t budget = g_metricsCache.vramBudget.load(std::memory_order_relaxed);
        std::string vramStr = std::format("{} / {} MB", (std::min)(used, budget), budget);
        LabelValue("VRAM", vramStr.c_str());
      } else {
        LabelValue("VRAM", "N/A");
      }
      float fgActual = sli.GetFgActualMultiplier();
      std::string fgStr = fgActual > 1.01f ? std::format("{:.2f}x", fgActual) : std::string("Off");
      LabelValue("FG Actual", fgStr.c_str());

      NorseSeparator();

      // Camera info
      std::string camStr = std::format("{} (J {:.3f}, {:.3f})",
                 m_cachedCamera ? "OK" : "Missing", m_cachedJitterX, m_cachedJitterY);
      LabelValue("Camera", camStr.c_str());
      std::string camDeltaStr = std::format("{:.3f}", sli.GetLastCameraDelta());
      LabelValue("Camera Delta", camDeltaStr.c_str());

      Spacing();
      if (Button("Reset to Defaults")) {
        ConfigManager::Get().ResetToDefaults();
        ConfigManager::Get().Load();
        ModConfig& reset = ConfigManager::Get().Data();
        sli.SetDLSSModeIndex(reset.dlss.mode); sli.SetDLSSPreset(reset.dlss.preset);
        sli.SetFrameGenMultiplier(reset.fg.multiplier); sli.SetSharpness(reset.dlss.sharpness);
        sli.SetLODBias(reset.dlss.lodBias); ApplySamplerLodBias(reset.dlss.lodBias);
        sli.SetReflexEnabled(reset.rr.enabled); sli.SetHUDFixEnabled(reset.system.hudFixEnabled);
        sli.SetRayReconstructionEnabled(reset.rr.enabled); sli.SetRRPreset(reset.rr.preset);
        sli.SetRRDenoiserStrength(reset.rr.denoiserStrength); sli.SetDeepDVCEnabled(reset.dvc.enabled);
        sli.SetDeepDVCIntensity(reset.dvc.intensity); sli.SetDeepDVCSaturation(reset.dvc.saturation);
        sli.SetDeepDVCAdaptiveEnabled(reset.dvc.adaptiveEnabled);
        sli.SetDeepDVCAdaptiveStrength(reset.dvc.adaptiveStrength);
        sli.SetDeepDVCAdaptiveMin(reset.dvc.adaptiveMin); sli.SetDeepDVCAdaptiveMax(reset.dvc.adaptiveMax);
        sli.SetDeepDVCAdaptiveSmoothing(reset.dvc.adaptiveSmoothing);
        sli.SetSmartFGEnabled(reset.fg.smartEnabled); sli.SetSmartFGAutoDisable(reset.fg.autoDisable);
        sli.SetSmartFGAutoDisableThreshold(reset.fg.autoDisableFps);
        sli.SetSmartFGSceneChangeEnabled(reset.fg.sceneChangeEnabled);
        sli.SetSmartFGSceneChangeThreshold(reset.fg.sceneChangeThreshold);
        sli.SetSmartFGInterpolationQuality(reset.fg.interpolationQuality);
        sli.SetHDREnabled(reset.hdr.enabled); sli.SetHDRPeakNits(reset.hdr.peakNits);
        sli.SetHDRPaperWhiteNits(reset.hdr.paperWhiteNits); sli.SetHDRExposure(reset.hdr.exposure);
        sli.SetHDRGamma(reset.hdr.gamma); sli.SetHDRTonemapCurve(reset.hdr.tonemapCurve);
        sli.SetHDRSaturation(reset.hdr.saturation);
        sli.SetMVecScale(reset.mvec.scaleX, reset.mvec.scaleY);
        m_showFPS = reset.ui.showFPS; m_showVignette = reset.ui.showVignette;
      }
      Spacing();
    }
  }

  // ---- Internals Section (Debug mode only) ----
  if (cfg.system.debugMode) {
    bool internalsOpen = m_sectionOpen[VGuiHash("internals_section")];
    SectionHeader("Internals", &internalsOpen);
    m_sectionOpen[VGuiHash("internals_section")] = internalsOpen;
    if (internalsOpen) {
      // --- Hook status ---
      Label("HOOK STATUS");
      LabelValue("Streamline", sli.IsInitialized() ? "OK" : "Not Initialized");
      LabelValue("DLSS", sli.IsDLSSSupported() ? "Supported" : "Unsupported");
      LabelValue("Frame Gen", sli.IsFrameGenSupported() ? "Supported" : "Unsupported");
      LabelValue("Ray Recon", sli.IsRayReconstructionSupported() ? "Supported" : "Unsupported");
      LabelValue("DeepDVC", sli.IsDeepDVCSupported() ? "Supported" : "Unsupported");
      LabelValue("HDR", sli.IsHDRSupported() ? "Supported" : "Unsupported");
      LabelValue("Keyboard Hook", InputHandler::Get().HasHookInstalled() ? "Installed" : "Polling");

      NorseSeparator();

      // --- Resource detection confidence ---
      Label("RESOURCE DETECTION");
      auto& det = ResourceDetector::Get();
      {
        std::string frameStr = std::format("{}", det.GetFrameCount());
        LabelValue("Detector Frame", frameStr.c_str());
      }

      ID3D12Resource* bestColor = det.GetBestColorCandidate();
      ID3D12Resource* bestDepth = det.GetBestDepthCandidate();
      ID3D12Resource* bestMV = det.GetBestMotionVectorCandidate();

      if (bestColor) {
        D3D12_RESOURCE_DESC d = bestColor->GetDesc();
        std::string s = std::format("{}x{} fmt:{}", d.Width, d.Height, static_cast<int>(d.Format));
        LabelValue("Color Buffer", s.c_str());
        StatusDot("color_ok", vtheme::kStatusOk);
      } else {
        LabelValue("Color Buffer", "Not found");
        StatusDot("color_missing", vtheme::kStatusBad);
      }

      if (bestDepth) {
        D3D12_RESOURCE_DESC d = bestDepth->GetDesc();
        std::string s = std::format("{}x{} fmt:{}", d.Width, d.Height, static_cast<int>(d.Format));
        LabelValue("Depth Buffer", s.c_str());
        StatusDot("depth_ok", vtheme::kStatusOk);
        std::string depthType = det.IsDepthInverted() ? "Inverted (0=far)" : "Standard (1=far)";
        LabelValue("Depth Type", depthType.c_str());
      } else {
        LabelValue("Depth Buffer", "Not found");
        StatusDot("depth_missing", vtheme::kStatusBad);
      }

      if (bestMV) {
        D3D12_RESOURCE_DESC d = bestMV->GetDesc();
        std::string s = std::format("{}x{} fmt:{}", d.Width, d.Height, static_cast<int>(d.Format));
        LabelValue("Motion Vectors", s.c_str());
        StatusDot("mv_ok", vtheme::kStatusOk);
      } else {
        LabelValue("Motion Vectors", "Not found");
        StatusDot("mv_missing", vtheme::kStatusBad);
      }

      ID3D12Resource* exposure = det.GetExposureResource();
      LabelValue("Exposure", exposure ? "Detected" : "Not found");

      NorseSeparator();

      // --- Streamline feature states ---
      Label("FRAME GENERATION");
      {
        std::string statusStr;
        auto fgStatus = sli.GetFrameGenStatus();
        switch (fgStatus) {
        case sl::DLSSGStatus::eOk: statusStr = "OK"; break;
        default: statusStr = std::format("Error ({})", static_cast<int>(fgStatus)); break;
        }
        LabelValue("FG Status", statusStr.c_str());
      }
      {
        std::string multStr = std::format("{}x (effective {:.1f}x)",
                                          sli.GetFrameGenMultiplier(),
                                          sli.GetFgActualMultiplier());
        LabelValue("FG Multiplier", multStr.c_str());
      }
      if (cfg.fg.smartEnabled) {
        std::string avgStr = std::format("{:.1f} FPS", sli.GetSmartFgRollingAvgFps());
        LabelValue("SmartFG Avg", avgStr.c_str());
        std::string compStr = std::format("{}x", sli.GetSmartFgComputedMultiplier());
        LabelValue("SmartFG Target", compStr.c_str());
        LabelValue("SmartFG Paused", sli.IsSmartFGTemporarilyDisabled() ? "Yes" : "No");
      }
      {
        std::string frameStr = std::format("{}", sli.GetFrameCount());
        LabelValue("SL Frame Index", frameStr.c_str());
      }
      LabelValue("Camera Data", sli.HasCameraData() ? "Available" : "Missing");
      {
        std::string deltaStr = std::format("{:.4f}", sli.GetLastCameraDelta());
        LabelValue("Camera Delta", deltaStr.c_str());
      }

      NorseSeparator();

      // --- Descriptor / sampler stats ---
      Label("SYSTEM");
      {
        std::string verStr =
#ifdef ACV_DLSS_VERSION
            ACV_DLSS_VERSION;
#else
            "dev";
#endif
        LabelValue("Build Version", verStr.c_str());
      }

      Spacing();
    }
  }

  // Record content height for scrolling
  m_contentHeight = m_cursorY - contentStartCursorY + m_scrollOffset;

  m_renderer.PopClip();

  // --- Scrollbar — minimal thin rail ---
  if (m_contentHeight > m_visibleHeight) {
    float sbW = vtheme::kScrollbarW;
    float sbX = panelDrawX + panelW - sbW - 3.0f;
    float sbY = contentStartY;
    float sbH = m_visibleHeight;
    float thumbH = (std::max)(24.0f, sbH * (m_visibleHeight / m_contentHeight));
    float maxScroll = m_contentHeight - m_visibleHeight;
    float thumbY = sbY + (m_scrollOffset / maxScroll) * (sbH - thumbH);

    bool sbHovered = PointInRect(m_input.mouseX, m_input.mouseY, sbX - 6, thumbY, sbW + 12, thumbH);
    D2D1_COLOR_F thumbColor = sbHovered ? vtheme::hex(0x8B949E, 0.7f) : vtheme::hex(0x484F58, 0.5f);
    m_renderer.FillRoundedRect(sbX, thumbY, sbW, thumbH, sbW * 0.5f, thumbColor);
  }

  // --- Scroll input ---
  if (m_contentHeight > m_visibleHeight) {
    float maxScroll = m_contentHeight - m_visibleHeight;
    m_scrollOffset -= m_input.scrollDelta;
    m_scrollOffset = std::clamp(m_scrollOffset, 0.0f, maxScroll);
  }
}

// ============================================================================
// Customization Section — Full UI customization panel (Auto-UI Powered)
// ============================================================================

void ImGuiOverlay::BuildCustomization() {
  ModConfig& cfg = ConfigManager::Get().Data();
  auto& cust = cfg.customization;

  bool open = m_sectionOpen[VGuiHash("cust_section")];
  SectionHeader("Customization", &open);
  m_sectionOpen[VGuiHash("cust_section")] = open;
  if (!open) return;

  // Use Auto-UI to generate widgets based on reflection categories
  // This replaces the manual calls
  
  bool subOpen = m_sectionOpen[VGuiHash("cust_anim")];
  SectionHeader("  Animation & Panel", &subOpen);
  m_sectionOpen[VGuiHash("cust_anim")] = subOpen;
  if (subOpen) {
    if (AutoUI::DrawCategory(*this, cust, "Customization")) {
      ConfigManager::Get().MarkDirty();
    }
    Spacing();
  }

  NorseSeparator();
}
