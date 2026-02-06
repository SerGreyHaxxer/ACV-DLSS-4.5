#pragma once
#include <chrono>
#include <filesystem>
#include <string>
#include <type_traits>


struct ModConfig {
  struct DLSS {
    int mode = 5; // DLAA
    int preset = 0;
    float sharpness = 0.5f;
    float lodBias = -1.0f;
  } dlss;

  struct FrameGen {
    int multiplier = 4;
    bool smartEnabled = false;
    bool autoDisable = true;
    float autoDisableFps = 120.0f;
    bool sceneChangeEnabled = true;
    float sceneChangeThreshold = 0.25f;
    float interpolationQuality = 0.5f;
  } fg;

  struct MotionVectors {
    bool autoScale = true;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
  } mvec;

  struct RayReconstruction {
    bool enabled = true;
    int preset = 0;
    float denoiserStrength = 0.5f;
  } rr;

  struct DeepDVC {
    bool enabled = false;
    float intensity = 0.5f;
    float saturation = 0.25f;
    bool adaptiveEnabled = false;
    float adaptiveStrength = 0.6f;
    float adaptiveMin = 0.2f;
    float adaptiveMax = 0.9f;
    float adaptiveSmoothing = 0.15f;
  } dvc;

  struct HDR {
    bool enabled = false;
    float peakNits = 1000.0f;
    float paperWhiteNits = 200.0f;
    float exposure = 1.0f;
    float gamma = 2.2f;
    float tonemapCurve = 0.0f;
    float saturation = 1.0f;
  } hdr;

  struct UI {
    bool visible = false;
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
  } ui;

  struct Customization {
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
  } customization;

  struct System {
    int logVerbosity = 1;
    bool debugMode = false;
    bool setupWizardCompleted = false;
    bool quietResourceScan = true;
    bool setupWizardForceShow = false;
    bool hudFixEnabled = false;
  } system;
};

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

  ModConfig &Data() { return m_config; }

private:
  ConfigManager() = default;
  std::filesystem::path GetConfigPath();
  void ImportLegacyIni(const std::filesystem::path &iniPath);

  ModConfig m_config;
  bool m_dirty = false;
  std::filesystem::file_time_type m_lastWriteTime;
};
