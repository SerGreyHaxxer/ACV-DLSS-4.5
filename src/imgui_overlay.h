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
#pragma once
// ============================================================================
// Valhalla Overlay â€” Custom AC Valhalla themed GUI overlay (replaces ImGui)
// Built entirely on D3D11On12 + Direct2D + DirectWrite. Zero third-party GUI.
// ============================================================================
#include "valhalla_gui.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class ImGuiOverlay {
public:
  static ImGuiOverlay& Get();

  // Non-copyable, non-movable singleton
  ImGuiOverlay(const ImGuiOverlay&) = delete;
  ImGuiOverlay& operator=(const ImGuiOverlay&) = delete;
  ImGuiOverlay(ImGuiOverlay&&) = delete;
  ImGuiOverlay& operator=(ImGuiOverlay&&) = delete;

  void Initialize(IDXGISwapChain* swapChain);
  void Shutdown();
  void OnResize(UINT width, UINT height);
  void Render();
  void SetFPS(float gameFps, float totalFps);
  void SetCameraStatus(bool hasCamera, float jitterX, float jitterY);
  void ToggleVisibility();
  void ToggleFPS();
  void ToggleVignette();
  void ToggleDebugMode(bool enabled);
  void CaptureNextHotkey(int* target);
  void UpdateControls();
  bool IsVisible() const { return m_visible; }

  // Widget drawing methods (public for auto_ui_generator)
  bool Checkbox(const char* label, bool* value, bool enabled = true);
  bool SliderFloat(const char* label, float* value, float vmin, float vmax, const char* fmt = "%.2f",
                   bool enabled = true);
  bool Combo(const char* label, int* selectedIndex, const char* const* items, int itemCount, bool enabled = true);
  bool ButtonGroup(const char* label, int* selectedIndex, const char* const* items, int itemCount, bool enabled = true);

private:
  ImGuiOverlay() = default;

  // --- WndProc hook for mouse/scroll input ---
  static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  // --- Build sections ---
  void BuildMainPanel();
  void BuildCustomization();
  void BuildSetupWizard();
  void BuildFPSOverlay();
  void BuildVignette();
  void BuildDebugWindow();
  void BuildBackgroundDim();
  void BuildPanelShadow(float x, float y, float w, float h, float alpha);
  void BuildMiniMode();

  // --- Widget system (immediate mode) ---
  void BeginWidgetFrame();
  bool PointInRect(float px, float py, float rx, float ry, float rw, float rh) const;

  void SectionHeader(const char* label, bool* open);
  void NorseSeparator();
  void Label(const char* text, const D2D1_COLOR_F& color = vtheme::kTextPrimary);
  void LabelValue(const char* label, const char* value);
  bool Button(const char* label, float w = 0.0f);
  bool ColorEdit3(const char* label, float* r, float* g, float* b);
  void StatusDot(const char* label, const D2D1_COLOR_F& color);
  void PlotLines(const char* label, const float* values, int count, int offset, float vmin, float vmax, float graphH);
  void Spacing(float height = vtheme::kSpacing);
  void SameLineButton(); // Makes next button render on same line

  // --- Layout state ---
  float m_cursorX = 0, m_cursorY = 0;
  float m_contentWidth = 0;
  float m_scrollOffset = 0;
  float m_scrollTarget = 0;
  float m_contentHeight = 0; // total content height for scroll
  float m_visibleHeight = 0; // visible scroll area height
  bool m_sameLine = false;
  float m_sameLineX = 0;
  float m_lastButtonEndX = 0;
  float m_lastButtonY = 0;

  // --- Widget interaction state ---
  uint32_t m_hotId = 0;
  uint32_t m_activeId = 0;
  uint32_t m_openComboId = 0;                       // which combo is currently expanded
  uint32_t m_openColorId = 0;                       // which color picker is currently expanded
  std::unordered_map<uint32_t, bool> m_sectionOpen; // section collapse state
  std::unordered_map<uint32_t, float> m_hoverAnim;  // hover animation progress [0..1]
  std::unordered_map<uint32_t, float> m_toggleAnim; // toggle switch animation [0..1]

  // --- Input ---
  VGuiInput m_input;
  bool m_mouseDownPrev = false;
  float m_scrollAccum = 0;

  // --- Panel state ---
  float m_panelX = 0, m_panelY = 0; // panel position (for drag)
  bool m_dragging = false;
  float m_dragOffsetX = 0, m_dragOffsetY = 0;
  vanim::AnimatedFloat m_panelSlide; // slide animation (0=closed, 1=open)
  vanim::AnimatedFloat m_panelAlpha; // fade animation
  float m_panelScale = 1.0f;         // for scale animation
  float m_smoothFPS = 0.0f;          // smoothly interpolated FPS display
  float m_statusPulsePhase = 0.0f;   // pulse animation for status dots

  // --- Accent color (dynamic, computed from config) ---
  D2D1_COLOR_F m_accent = vtheme::kGold;
  D2D1_COLOR_F m_accentBright = vtheme::kGoldBright;
  D2D1_COLOR_F m_accentDim = vtheme::kGoldDim;

  // --- Snap guides ---
  void SnapPanel(float screenW, float screenH);

  // --- Animation helpers ---
  float ComputeAnimProgress(float rawProgress, bool opening) const;
  void ComputePanelTransform(float progress, float screenW, float screenH, float panelW, float panelH, float& outX,
                             float& outY, float& outAlpha, float& outScale) const;

  // --- Core state ---
  bool m_initialized = false;
  bool m_visible = false;
  bool m_showFPS = false;
  bool m_showVignette = false;
  bool m_showDebug = false;
  bool m_showSetupWizard = false;
  int m_wizardStep = 0;       // 0=Welcome, 1=DLSS, 2=FG, 3=Extras, 4=Done
  int m_wizardDlssChoice = 3; // default: Max Quality
  int m_wizardFgChoice = 1;   // default: 2x
  bool m_wizardDvcEnabled = false;
  bool m_wizardHdrEnabled = false;
  bool m_cursorUnlocked = false;
  int* m_pendingHotkeyTarget = nullptr;

  HWND m_hwnd = nullptr;
  WNDPROC m_prevWndProc = nullptr;
  UINT m_width = 0, m_height = 0;
  RECT m_prevClip = {};

  // --- FPS data ---
  float m_cachedTotalFPS = 0.0f;
  float m_cachedJitterX = 0.0f;
  float m_cachedJitterY = 0.0f;
  bool m_cachedCamera = false;
  static constexpr int kFpsHistorySize = 120;
  float m_fpsHistory[kFpsHistorySize] = {};
  int m_fpsHistoryIndex = 0;

  // --- D3D12 resources (for vignette texture - now using D2D) ---
  IDXGISwapChain3* m_swapChain = nullptr;
  ID3D12Device* m_device = nullptr;
  ID3D12CommandQueue* m_queue = nullptr;
  UINT m_backBufferCount = 0;

  // --- Custom D2D renderer ---
  ValhallaRenderer m_renderer;

  // --- Timing ---
  float m_time = 0.0f; // current time in seconds
  float m_lastFrameTime = 0.0f;
  bool m_firstFrame = true;

  // --- Metrics thread ---
  std::thread m_metricsThread;
  std::atomic<bool> m_metricsThreadRunning{false};
  std::atomic<bool> m_shuttingDown{false};
};
