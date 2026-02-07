/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <type_traits>

#include "cpp26/reflection.h"

// ============================================================================
// CONFIGURATION STRUCTURES
// ============================================================================

struct DLSSConfig {
  int mode = 5; // DLAA
  int preset = 0;
  float sharpness = 0.5f;
  float lodBias = -1.0f;
};

struct FrameGenConfig {
  int multiplier = 4;
  bool smartEnabled = false;
  bool autoDisable = true;
  float autoDisableFps = 120.0f;
  bool sceneChangeEnabled = true;
  float sceneChangeThreshold = 0.25f;
  float interpolationQuality = 0.5f;
};

struct MotionVectorsConfig {
  bool autoScale = true;
  float scaleX = 1.0f;
  float scaleY = 1.0f;
};

struct RayReconstructionConfig {
  bool enabled = true;
  int preset = 0;
  float denoiserStrength = 0.5f;
};

struct DeepDVCConfig {
  bool enabled = false;
  float intensity = 0.5f;
  float saturation = 0.25f;
  bool adaptiveEnabled = false;
  float adaptiveStrength = 0.6f;
  float adaptiveMin = 0.2f;
  float adaptiveMax = 0.9f;
  float adaptiveSmoothing = 0.15f;
};

struct HDRConfig {
  bool enabled = false;
  float peakNits = 1000.0f;
  float paperWhiteNits = 200.0f;
  float exposure = 1.0f;
  float gamma = 2.2f;
  float tonemapCurve = 0.0f;
  float saturation = 1.0f;
};

struct UIConfig {
  bool visible = false;     // Control panel hidden on startup; use hotkey to open
  bool showFPS = false;
  bool showVignette = false;
  int menuHotkey = 0x74;     // F5
  int fpsHotkey = 0x75;      // F6
  int vignetteHotkey = 0x76; // F7
  float vignetteIntensity = 0.35f;
  float vignetteRadius = 0.78f;
  float vignetteSoftness = 0.55f;
  float vignetteColorR = 0.01f;
  float vignetteColorG = 0.73f;
  float vignetteColorB = 0.93f;
};

struct CustomizationConfig {
  // Panel animation
  int animationType = 0;         // 0=SlideLeft,1=SlideRight,2=SlideTop,3=SlideBottom,4=Fade,5=Scale,6=Bounce,7=Elastic
  float animSpeed = 1.0f;        // 0.25x - 3.0x multiplier
  // Panel appearance
  float panelOpacity = 0.94f;    // 0.3 - 1.0
  float panelWidth = 520.0f;     // 360 - 720
  float cornerRadius = 6.0f;    // 0 - 20
  bool panelShadow = true;
  // Panel position (drag)
  float panelX = -1.0f;         // -1 = default (left edge)
  float panelY = -1.0f;         // -1 = default (top)
  bool snapToEdges = true;
  float snapDistance = 20.0f;
  // FPS counter
  int fpsPosition = 0;           // 0=TopRight,1=TopLeft,2=BottomRight,3=BottomLeft
  int fpsStyle = 0;              // 0=Standard,1=Minimal,2=Detailed
  float fpsOpacity = 0.85f;
  float fpsScale = 1.0f;
  // Accent color
  float accentR = 0.831f;
  float accentG = 0.686f;
  float accentB = 0.216f;
  // Effects
  bool backgroundDim = true;
  float backgroundDimAmount = 0.3f;
  bool widgetGlow = true;
  bool statusPulse = true;
  bool smoothFPS = true;
  // Layout
  int layoutMode = 1;            // 0=Compact, 1=Normal, 2=Expanded
  float fontScale = 1.0f;        // 0.75 - 1.5
  // Mini mode
  bool miniMode = false;
};

struct SystemConfig {
  int logVerbosity = 1;
  bool debugMode = false;
  bool setupWizardCompleted = false;
  bool quietResourceScan = true;
  bool setupWizardForceShow = false;
  bool hudFixEnabled = false;
};

struct ModConfig {
  DLSSConfig dlss;
  FrameGenConfig fg;
  MotionVectorsConfig mvec;
  RayReconstructionConfig rr;
  DeepDVCConfig dvc;
  HDRConfig hdr;
  UIConfig ui;
  CustomizationConfig customization;
  SystemConfig system;
};

// ============================================================================
// REFLECTION REGISTRATION
// ============================================================================

namespace cpp26::reflect {

REFLECT_STRUCT_BEGIN(DLSSConfig)
  REFLECT_FIELD(int, mode, 5, ui::dropdown(nullptr, 0), "General") // Dropdown handled by custom UI for now
  REFLECT_FIELD(int, preset, 0, ui::dropdown(nullptr, 0), "General")
  REFLECT_FIELD(float, sharpness, 0.5f, ui::slider_float(0.0f, 1.0f), "Quality")
  REFLECT_FIELD(float, lodBias, -1.0f, ui::slider_float(-3.0f, 3.0f), "Quality")
REFLECT_STRUCT_END()

REFLECT_STRUCT_BEGIN(FrameGenConfig)
  REFLECT_FIELD(int, multiplier, 4, ui::dropdown(nullptr, 0), "Frame Generation")
  REFLECT_FIELD(bool, smartEnabled, false, ui::checkbox(), "Smart FG")
  REFLECT_FIELD(bool, autoDisable, true, ui::checkbox(), "Smart FG")
  REFLECT_FIELD(float, autoDisableFps, 120.0f, ui::slider_float(30.0f, 300.0f), "Smart FG")
  REFLECT_FIELD(bool, sceneChangeEnabled, true, ui::checkbox(), "Smart FG")
  REFLECT_FIELD(float, sceneChangeThreshold, 0.25f, ui::slider_float(0.0f, 1.0f), "Smart FG")
  REFLECT_FIELD(float, interpolationQuality, 0.5f, ui::slider_float(0.0f, 1.0f), "Smart FG")
REFLECT_STRUCT_END()

REFLECT_STRUCT_BEGIN(MotionVectorsConfig)
  REFLECT_FIELD(bool, autoScale, true, ui::checkbox(), "Quality")
  REFLECT_FIELD(float, scaleX, 1.0f, ui::slider_float(0.1f, 3.0f), "Quality")
  REFLECT_FIELD(float, scaleY, 1.0f, ui::slider_float(0.1f, 3.0f), "Quality")
REFLECT_STRUCT_END()

REFLECT_STRUCT_BEGIN(RayReconstructionConfig)
  REFLECT_FIELD(bool, enabled, true, ui::checkbox(), "Ray Reconstruction")
  REFLECT_FIELD(int, preset, 0, ui::dropdown(nullptr, 0), "Ray Reconstruction")
  REFLECT_FIELD(float, denoiserStrength, 0.5f, ui::slider_float(0.0f, 1.0f), "Ray Reconstruction")
REFLECT_STRUCT_END()

REFLECT_STRUCT_BEGIN(DeepDVCConfig)
  REFLECT_FIELD(bool, enabled, false, ui::checkbox(), "DeepDVC")
  REFLECT_FIELD(float, intensity, 0.5f, ui::slider_float(0.0f, 1.0f), "DeepDVC")
  REFLECT_FIELD(float, saturation, 0.25f, ui::slider_float(0.0f, 1.0f), "DeepDVC")
  REFLECT_FIELD(bool, adaptiveEnabled, false, ui::checkbox(), "DeepDVC")
  REFLECT_FIELD(float, adaptiveStrength, 0.6f, ui::slider_float(0.0f, 1.0f), "DeepDVC")
  REFLECT_FIELD(float, adaptiveMin, 0.2f, ui::slider_float(0.0f, 1.0f), "DeepDVC")
  REFLECT_FIELD(float, adaptiveMax, 0.9f, ui::slider_float(0.0f, 1.0f), "DeepDVC")
  REFLECT_FIELD(float, adaptiveSmoothing, 0.15f, ui::slider_float(0.0f, 1.0f), "DeepDVC")
REFLECT_STRUCT_END()

REFLECT_STRUCT_BEGIN(HDRConfig)
  REFLECT_FIELD(bool, enabled, false, ui::checkbox(), "HDR")
  REFLECT_FIELD(float, peakNits, 1000.0f, ui::slider_float(100.0f, 10000.0f), "HDR")
  REFLECT_FIELD(float, paperWhiteNits, 200.0f, ui::slider_float(50.0f, 1000.0f), "HDR")
  REFLECT_FIELD(float, exposure, 1.0f, ui::slider_float(0.1f, 10.0f), "HDR")
  REFLECT_FIELD(float, gamma, 2.2f, ui::slider_float(1.0f, 3.0f), "HDR")
  REFLECT_FIELD(float, tonemapCurve, 0.0f, ui::slider_float(-1.0f, 1.0f), "HDR")
  REFLECT_FIELD(float, saturation, 1.0f, ui::slider_float(0.0f, 2.0f), "HDR")
REFLECT_STRUCT_END()

REFLECT_STRUCT_BEGIN(UIConfig)
  REFLECT_FIELD(bool, visible, false, ui::hidden(), "")
  REFLECT_FIELD(bool, showFPS, false, ui::checkbox(), "Overlay")
  REFLECT_FIELD(bool, showVignette, false, ui::checkbox(), "Overlay")
  REFLECT_FIELD(int, menuHotkey, 0x74, ui::hidden(), "Hotkeys")
  REFLECT_FIELD(int, fpsHotkey, 0x75, ui::hidden(), "Hotkeys")
  REFLECT_FIELD(int, vignetteHotkey, 0x76, ui::hidden(), "Hotkeys")
  REFLECT_FIELD(float, vignetteIntensity, 0.35f, ui::slider_float(0.0f, 1.0f), "Overlay")
  REFLECT_FIELD(float, vignetteRadius, 0.78f, ui::slider_float(0.0f, 1.0f), "Overlay")
  REFLECT_FIELD(float, vignetteSoftness, 0.55f, ui::slider_float(0.0f, 1.0f), "Overlay")
  REFLECT_FIELD(float, vignetteColorR, 0.01f, ui::color_rgb(), "Overlay")
  REFLECT_FIELD(float, vignetteColorG, 0.73f, ui::color_rgb(), "Overlay")
  REFLECT_FIELD(float, vignetteColorB, 0.93f, ui::color_rgb(), "Overlay")
REFLECT_STRUCT_END()

REFLECT_STRUCT_BEGIN(CustomizationConfig)
  REFLECT_FIELD(int, animationType, 0, ui::dropdown(nullptr, 0), "Customization")
  REFLECT_FIELD(float, animSpeed, 1.0f, ui::slider_float(0.1f, 5.0f), "Customization")
  REFLECT_FIELD(float, panelOpacity, 0.94f, ui::slider_float(0.0f, 1.0f), "Customization")
  REFLECT_FIELD(float, panelWidth, 520.0f, ui::slider_float(300.0f, 1000.0f), "Customization")
  REFLECT_FIELD(float, cornerRadius, 6.0f, ui::slider_float(0.0f, 20.0f), "Customization")
  REFLECT_FIELD(bool, panelShadow, true, ui::checkbox(), "Customization")
  REFLECT_FIELD(float, panelX, -1.0f, ui::hidden(), "")
  REFLECT_FIELD(float, panelY, -1.0f, ui::hidden(), "")
  REFLECT_FIELD(bool, snapToEdges, true, ui::checkbox(), "Customization")
  REFLECT_FIELD(float, snapDistance, 20.0f, ui::slider_float(0.0f, 100.0f), "Customization")
  REFLECT_FIELD(int, fpsPosition, 0, ui::dropdown(nullptr, 0), "Customization")
  REFLECT_FIELD(int, fpsStyle, 0, ui::dropdown(nullptr, 0), "Customization")
  REFLECT_FIELD(float, fpsOpacity, 0.85f, ui::slider_float(0.0f, 1.0f), "Customization")
  REFLECT_FIELD(float, fpsScale, 1.0f, ui::slider_float(0.5f, 2.0f), "Customization")
  REFLECT_FIELD(float, accentR, 0.831f, ui::color_rgb(), "Customization")
  REFLECT_FIELD(float, accentG, 0.686f, ui::color_rgb(), "Customization")
  REFLECT_FIELD(float, accentB, 0.216f, ui::color_rgb(), "Customization")
  REFLECT_FIELD(bool, backgroundDim, true, ui::checkbox(), "Customization")
  REFLECT_FIELD(float, backgroundDimAmount, 0.3f, ui::slider_float(0.0f, 1.0f), "Customization")
  REFLECT_FIELD(bool, widgetGlow, true, ui::checkbox(), "Customization")
  REFLECT_FIELD(bool, statusPulse, true, ui::checkbox(), "Customization")
  REFLECT_FIELD(bool, smoothFPS, true, ui::checkbox(), "Customization")
  REFLECT_FIELD(int, layoutMode, 1, ui::dropdown(nullptr, 0), "Customization")
  REFLECT_FIELD(float, fontScale, 1.0f, ui::slider_float(0.5f, 2.0f), "Customization")
  REFLECT_FIELD(bool, miniMode, false, ui::checkbox(), "Customization")
REFLECT_STRUCT_END()

REFLECT_STRUCT_BEGIN(SystemConfig)
  REFLECT_FIELD(int, logVerbosity, 1, ui::hidden(), "")
  REFLECT_FIELD(bool, debugMode, false, ui::checkbox(), "System")
  REFLECT_FIELD(bool, setupWizardCompleted, false, ui::hidden(), "")
  REFLECT_FIELD(bool, quietResourceScan, true, ui::checkbox(), "System")
  REFLECT_FIELD(bool, setupWizardForceShow, false, ui::hidden(), "")
  REFLECT_FIELD(bool, hudFixEnabled, false, ui::checkbox(), "System")
REFLECT_STRUCT_END()

// Initialize reflection for all structs
inline void InitReflection() {
  REFLECT_INIT(DLSSConfig);
  REFLECT_INIT(FrameGenConfig);
  REFLECT_INIT(MotionVectorsConfig);
  REFLECT_INIT(RayReconstructionConfig);
  REFLECT_INIT(DeepDVCConfig);
  REFLECT_INIT(HDRConfig);
  REFLECT_INIT(UIConfig);
  REFLECT_INIT(CustomizationConfig);
  REFLECT_INIT(SystemConfig);
}

} // namespace cpp26::reflect

static_assert(std::is_trivially_copyable_v<ModConfig>,
              "ModConfig must be trivially copyable — no reference members");

class ConfigManager {
public:
  static ConfigManager &Get();

  // Non-copyable, non-movable singleton
  ConfigManager(const ConfigManager&) = delete;
  ConfigManager& operator=(const ConfigManager&) = delete;
  ConfigManager(ConfigManager&&) = delete;
  ConfigManager& operator=(ConfigManager&&) = delete;

  void Load();
  void Save();
  void ResetToDefaults();
  void MarkDirty();
  void SaveIfDirty();
  void CheckHotReload();

  // Returns a mutable reference to the config.  Safe ONLY when called from the
  // render / Present thread (the "owning" thread).  For cross-thread reads use
  // DataSnapshot() instead.
  ModConfig &Data() { return m_config; }

  // Returns a thread-safe copy of the current config.  Use this from any
  // non-render thread (e.g. timer thread, metrics thread).
  ModConfig DataSnapshot() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_config;
  }

private:
  ConfigManager() { cpp26::reflect::InitReflection(); }
  std::filesystem::path GetConfigPath();
  void ImportLegacyIni(const std::filesystem::path &iniPath);

  ModConfig m_config;
  // Lock hierarchy level 4 (SwapChain=1 > Hooks=2 > Resources=3 > Config=4 > Logging=5).
  // Protects m_config during Load/Save/CheckHotReload so that cross-thread
  // readers via DataSnapshot() observe a consistent state.
  mutable std::mutex m_configMutex;
  bool m_dirty = false;
  std::filesystem::file_time_type m_lastWriteTime;
};
