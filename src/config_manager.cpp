#include "config_manager.h"
#include "logger.h"
#include <array>
#include <charconv>
#include <fstream>
#include <toml++/toml.hpp>
#include <windows.h>


ConfigManager &ConfigManager::Get() {
  static ConfigManager instance;
  return instance;
}

std::filesystem::path ConfigManager::GetConfigPath() {
  std::array<wchar_t, MAX_PATH> path{};
  if (GetModuleFileNameW(nullptr, path.data(), MAX_PATH) > 0) {
    std::filesystem::path p(path.data());
    return p.parent_path() / "config.toml";
  }
  return "config.toml";
}

void ConfigManager::Load() {
  auto path = GetConfigPath();

  if (!std::filesystem::exists(path)) {
    auto iniPath = path.parent_path() / "dlss_settings.ini";
    if (std::filesystem::exists(iniPath)) {
      LOG_INFO("Migrating legacy .ini config to TOML...");
      ImportLegacyIni(iniPath);
      Save();
    } else {
      LOG_INFO("No config found, using defaults.");
      Save();
    }
  }

  try {
    auto tbl = toml::parse_file(path.string());

    // Parse into a temporary config, then swap under lock to avoid tearing.
    ModConfig parsed = m_config; // start from current defaults

    // DLSS
    parsed.dlss.mode = tbl["DLSS"]["mode"].value_or(parsed.dlss.mode);
    parsed.dlss.preset = tbl["DLSS"]["preset"].value_or(parsed.dlss.preset);
    parsed.dlss.sharpness =
        tbl["DLSS"]["sharpness"].value_or(parsed.dlss.sharpness);
    parsed.dlss.lodBias =
        tbl["DLSS"]["lod_bias"].value_or(parsed.dlss.lodBias);

    // FrameGen
    parsed.fg.multiplier =
        tbl["FrameGen"]["multiplier"].value_or(parsed.fg.multiplier);
    parsed.fg.smartEnabled =
        tbl["FrameGen"]["smart_enabled"].value_or(parsed.fg.smartEnabled);
    parsed.fg.autoDisable =
        tbl["FrameGen"]["auto_disable"].value_or(parsed.fg.autoDisable);
    parsed.fg.autoDisableFps = tbl["FrameGen"]["auto_disable_fps"].value_or(
        parsed.fg.autoDisableFps);
    parsed.fg.sceneChangeEnabled =
        tbl["FrameGen"]["scene_change_enabled"].value_or(
            parsed.fg.sceneChangeEnabled);
    parsed.fg.sceneChangeThreshold =
        tbl["FrameGen"]["scene_change_threshold"].value_or(
            parsed.fg.sceneChangeThreshold);
    parsed.fg.interpolationQuality =
        tbl["FrameGen"]["interpolation_quality"].value_or(
            parsed.fg.interpolationQuality);

    // MotionVectors
    parsed.mvec.autoScale =
        tbl["MotionVectors"]["auto_scale"].value_or(parsed.mvec.autoScale);
    parsed.mvec.scaleX =
        tbl["MotionVectors"]["scale_x"].value_or(parsed.mvec.scaleX);
    parsed.mvec.scaleY =
        tbl["MotionVectors"]["scale_y"].value_or(parsed.mvec.scaleY);

    // RayReconstruction
    parsed.rr.enabled =
        tbl["RayReconstruction"]["enabled"].value_or(parsed.rr.enabled);
    parsed.rr.preset =
        tbl["RayReconstruction"]["preset"].value_or(parsed.rr.preset);
    parsed.rr.denoiserStrength =
        tbl["RayReconstruction"]["denoiser_strength"].value_or(
            parsed.rr.denoiserStrength);

    // DeepDVC
    parsed.dvc.enabled =
        tbl["DeepDVC"]["enabled"].value_or(parsed.dvc.enabled);
    parsed.dvc.intensity =
        tbl["DeepDVC"]["intensity"].value_or(parsed.dvc.intensity);
    parsed.dvc.saturation =
        tbl["DeepDVC"]["saturation"].value_or(parsed.dvc.saturation);
    parsed.dvc.adaptiveEnabled = tbl["DeepDVC"]["adaptive_enabled"].value_or(
        parsed.dvc.adaptiveEnabled);
    parsed.dvc.adaptiveStrength =
        tbl["DeepDVC"]["adaptive_strength"].value_or(
            parsed.dvc.adaptiveStrength);
    parsed.dvc.adaptiveMin =
        tbl["DeepDVC"]["adaptive_min"].value_or(parsed.dvc.adaptiveMin);
    parsed.dvc.adaptiveMax =
        tbl["DeepDVC"]["adaptive_max"].value_or(parsed.dvc.adaptiveMax);
    parsed.dvc.adaptiveSmoothing =
        tbl["DeepDVC"]["adaptive_smoothing"].value_or(
            parsed.dvc.adaptiveSmoothing);

    // HDR
    parsed.hdr.enabled = tbl["HDR"]["enabled"].value_or(parsed.hdr.enabled);
    parsed.hdr.peakNits =
        tbl["HDR"]["peak_nits"].value_or(parsed.hdr.peakNits);
    parsed.hdr.paperWhiteNits =
        tbl["HDR"]["paper_white_nits"].value_or(parsed.hdr.paperWhiteNits);
    parsed.hdr.exposure =
        tbl["HDR"]["exposure"].value_or(parsed.hdr.exposure);
    parsed.hdr.gamma = tbl["HDR"]["gamma"].value_or(parsed.hdr.gamma);
    parsed.hdr.tonemapCurve =
        tbl["HDR"]["tonemap_curve"].value_or(parsed.hdr.tonemapCurve);
    parsed.hdr.saturation =
        tbl["HDR"]["saturation"].value_or(parsed.hdr.saturation);

    // UI
    parsed.ui.visible = tbl["UI"]["visible"].value_or(parsed.ui.visible);
    parsed.ui.showFPS = tbl["UI"]["show_fps"].value_or(parsed.ui.showFPS);
    parsed.ui.showVignette =
        tbl["UI"]["show_vignette"].value_or(parsed.ui.showVignette);
    parsed.ui.menuHotkey =
        tbl["UI"]["menu_hotkey"].value_or(parsed.ui.menuHotkey);
    parsed.ui.fpsHotkey =
        tbl["UI"]["fps_hotkey"].value_or(parsed.ui.fpsHotkey);
    parsed.ui.vignetteHotkey =
        tbl["UI"]["vignette_hotkey"].value_or(parsed.ui.vignetteHotkey);
    parsed.ui.vignetteIntensity =
        tbl["UI"]["vignette_intensity"].value_or(parsed.ui.vignetteIntensity);
    parsed.ui.vignetteRadius =
        tbl["UI"]["vignette_radius"].value_or(parsed.ui.vignetteRadius);
    parsed.ui.vignetteSoftness =
        tbl["UI"]["vignette_softness"].value_or(parsed.ui.vignetteSoftness);
    parsed.ui.vignetteColorR =
        tbl["UI"]["vignette_color_r"].value_or(parsed.ui.vignetteColorR);
    parsed.ui.vignetteColorG =
        tbl["UI"]["vignette_color_g"].value_or(parsed.ui.vignetteColorG);
    parsed.ui.vignetteColorB =
        tbl["UI"]["vignette_color_b"].value_or(parsed.ui.vignetteColorB);

    // Customization
    parsed.customization.animationType = tbl["Customization"]["anim_type"].value_or(parsed.customization.animationType);
    parsed.customization.animSpeed = tbl["Customization"]["anim_speed"].value_or(parsed.customization.animSpeed);
    parsed.customization.panelOpacity = tbl["Customization"]["panel_opacity"].value_or(parsed.customization.panelOpacity);
    parsed.customization.panelWidth = tbl["Customization"]["panel_width"].value_or(parsed.customization.panelWidth);
    parsed.customization.cornerRadius = tbl["Customization"]["corner_radius"].value_or(parsed.customization.cornerRadius);
    parsed.customization.panelShadow = tbl["Customization"]["panel_shadow"].value_or(parsed.customization.panelShadow);
    parsed.customization.panelX = tbl["Customization"]["panel_x"].value_or(parsed.customization.panelX);
    parsed.customization.panelY = tbl["Customization"]["panel_y"].value_or(parsed.customization.panelY);
    parsed.customization.snapToEdges = tbl["Customization"]["snap_to_edges"].value_or(parsed.customization.snapToEdges);
    parsed.customization.snapDistance = tbl["Customization"]["snap_distance"].value_or(parsed.customization.snapDistance);
    parsed.customization.fpsPosition = tbl["Customization"]["fps_position"].value_or(parsed.customization.fpsPosition);
    parsed.customization.fpsStyle = tbl["Customization"]["fps_style"].value_or(parsed.customization.fpsStyle);
    parsed.customization.fpsOpacity = tbl["Customization"]["fps_opacity"].value_or(parsed.customization.fpsOpacity);
    parsed.customization.fpsScale = tbl["Customization"]["fps_scale"].value_or(parsed.customization.fpsScale);
    parsed.customization.accentR = tbl["Customization"]["accent_r"].value_or(parsed.customization.accentR);
    parsed.customization.accentG = tbl["Customization"]["accent_g"].value_or(parsed.customization.accentG);
    parsed.customization.accentB = tbl["Customization"]["accent_b"].value_or(parsed.customization.accentB);
    parsed.customization.backgroundDim = tbl["Customization"]["background_dim"].value_or(parsed.customization.backgroundDim);
    parsed.customization.backgroundDimAmount = tbl["Customization"]["background_dim_amount"].value_or(parsed.customization.backgroundDimAmount);
    parsed.customization.widgetGlow = tbl["Customization"]["widget_glow"].value_or(parsed.customization.widgetGlow);
    parsed.customization.statusPulse = tbl["Customization"]["status_pulse"].value_or(parsed.customization.statusPulse);
    parsed.customization.smoothFPS = tbl["Customization"]["smooth_fps"].value_or(parsed.customization.smoothFPS);
    parsed.customization.layoutMode = tbl["Customization"]["layout_mode"].value_or(parsed.customization.layoutMode);
    parsed.customization.fontScale = tbl["Customization"]["font_scale"].value_or(parsed.customization.fontScale);
    parsed.customization.miniMode = tbl["Customization"]["mini_mode"].value_or(parsed.customization.miniMode);

    // System
    parsed.system.logVerbosity =
        tbl["System"]["log_verbosity"].value_or(parsed.system.logVerbosity);
    parsed.system.debugMode =
        tbl["System"]["debug_mode"].value_or(parsed.system.debugMode);
    parsed.system.setupWizardCompleted =
        tbl["System"]["wizard_completed"].value_or(
            parsed.system.setupWizardCompleted);
    parsed.system.quietResourceScan =
        tbl["System"]["quiet_resource_scan"].value_or(
            parsed.system.quietResourceScan);
    parsed.system.setupWizardForceShow =
        tbl["System"]["wizard_force_show"].value_or(
            parsed.system.setupWizardForceShow);
    parsed.system.hudFixEnabled =
        tbl["System"]["hud_fix_enabled"].value_or(
            parsed.system.hudFixEnabled);

    // Swap the parsed config into m_config atomically (under lock) so that
    // cross-thread readers via DataSnapshot() never observe a half-written state.
    {
      std::lock_guard<std::mutex> lock(m_configMutex);
      m_config = parsed;
    }
    m_lastWriteTime = std::filesystem::last_write_time(path);

    LOG_INFO("Configuration loaded from TOML.");
  } catch (const std::exception &ex) {
    LOG_ERROR("Failed to parse config.toml: {}", ex.what());
  }
}

void ConfigManager::Save() {
  // Take a snapshot under lock so we serialize a consistent state
  ModConfig snapshot;
  {
    std::lock_guard<std::mutex> lock(m_configMutex);
    snapshot = m_config;
  }
  auto tbl = toml::table{
      {{"DLSS", toml::table{{{"mode", snapshot.dlss.mode},
                             {"preset", snapshot.dlss.preset},
                             {"sharpness", snapshot.dlss.sharpness},
                             {"lod_bias", snapshot.dlss.lodBias}}}},
       {"FrameGen",
        toml::table{
            {{"multiplier", snapshot.fg.multiplier},
             {"smart_enabled", snapshot.fg.smartEnabled},
             {"auto_disable", snapshot.fg.autoDisable},
             {"auto_disable_fps", snapshot.fg.autoDisableFps},
             {"scene_change_enabled", snapshot.fg.sceneChangeEnabled},
             {"scene_change_threshold", snapshot.fg.sceneChangeThreshold},
             {"interpolation_quality", snapshot.fg.interpolationQuality}}}},
       {"MotionVectors", toml::table{{{"auto_scale", snapshot.mvec.autoScale},
                                      {"scale_x", snapshot.mvec.scaleX},
                                      {"scale_y", snapshot.mvec.scaleY}}}},
       {"RayReconstruction",
        toml::table{{{"enabled", snapshot.rr.enabled},
                     {"preset", snapshot.rr.preset},
                     {"denoiser_strength", snapshot.rr.denoiserStrength}}}},
       {"DeepDVC",
        toml::table{{{"enabled", snapshot.dvc.enabled},
                     {"intensity", snapshot.dvc.intensity},
                     {"saturation", snapshot.dvc.saturation},
                     {"adaptive_enabled", snapshot.dvc.adaptiveEnabled},
                     {"adaptive_strength", snapshot.dvc.adaptiveStrength},
                     {"adaptive_min", snapshot.dvc.adaptiveMin},
                     {"adaptive_max", snapshot.dvc.adaptiveMax},
                     {"adaptive_smoothing", snapshot.dvc.adaptiveSmoothing}}}},
       {"HDR", toml::table{{{"enabled", snapshot.hdr.enabled},
                            {"peak_nits", snapshot.hdr.peakNits},
                            {"paper_white_nits", snapshot.hdr.paperWhiteNits},
                            {"exposure", snapshot.hdr.exposure},
                            {"gamma", snapshot.hdr.gamma},
                            {"tonemap_curve", snapshot.hdr.tonemapCurve},
                            {"saturation", snapshot.hdr.saturation}}}},
       {"UI",
        toml::table{{{"visible", snapshot.ui.visible},
                     {"show_fps", snapshot.ui.showFPS},
                     {"show_vignette", snapshot.ui.showVignette},
                     {"menu_hotkey", snapshot.ui.menuHotkey},
                     {"fps_hotkey", snapshot.ui.fpsHotkey},
                     {"vignette_hotkey", snapshot.ui.vignetteHotkey},
                     {"vignette_intensity", snapshot.ui.vignetteIntensity},
                     {"vignette_radius", snapshot.ui.vignetteRadius},
                     {"vignette_softness", snapshot.ui.vignetteSoftness},
                     {"vignette_color_r", snapshot.ui.vignetteColorR},
                     {"vignette_color_g", snapshot.ui.vignetteColorG},
                     {"vignette_color_b", snapshot.ui.vignetteColorB}}}},
       {"Customization",
        toml::table{{{"anim_type", snapshot.customization.animationType},
                     {"anim_speed", snapshot.customization.animSpeed},
                     {"panel_opacity", snapshot.customization.panelOpacity},
                     {"panel_width", snapshot.customization.panelWidth},
                     {"corner_radius", snapshot.customization.cornerRadius},
                     {"panel_shadow", snapshot.customization.panelShadow},
                     {"panel_x", snapshot.customization.panelX},
                     {"panel_y", snapshot.customization.panelY},
                     {"snap_to_edges", snapshot.customization.snapToEdges},
                     {"snap_distance", snapshot.customization.snapDistance},
                     {"fps_position", snapshot.customization.fpsPosition},
                     {"fps_style", snapshot.customization.fpsStyle},
                     {"fps_opacity", snapshot.customization.fpsOpacity},
                     {"fps_scale", snapshot.customization.fpsScale},
                     {"accent_r", snapshot.customization.accentR},
                     {"accent_g", snapshot.customization.accentG},
                     {"accent_b", snapshot.customization.accentB},
                     {"background_dim", snapshot.customization.backgroundDim},
                     {"background_dim_amount", snapshot.customization.backgroundDimAmount},
                     {"widget_glow", snapshot.customization.widgetGlow},
                     {"status_pulse", snapshot.customization.statusPulse},
                     {"smooth_fps", snapshot.customization.smoothFPS},
                     {"layout_mode", snapshot.customization.layoutMode},
                     {"font_scale", snapshot.customization.fontScale},
                     {"mini_mode", snapshot.customization.miniMode}}}},
       {"System", toml::table{{{"log_verbosity", snapshot.system.logVerbosity},
                               {"debug_mode", snapshot.system.debugMode},
                               {"wizard_completed",
                                snapshot.system.setupWizardCompleted},
                               {"quiet_resource_scan",
                                snapshot.system.quietResourceScan},
                               {"wizard_force_show",
                                snapshot.system.setupWizardForceShow},
                               {"hud_fix_enabled",
                                snapshot.system.hudFixEnabled}}}}}};

  std::ofstream file(GetConfigPath());
  file << tbl;
  m_dirty = false;
  m_lastWriteTime = std::filesystem::last_write_time(GetConfigPath());
}

void ConfigManager::CheckHotReload() {
  auto path = GetConfigPath();
  if (!std::filesystem::exists(path))
    return;

  try {
    auto lastWrite = std::filesystem::last_write_time(path);
    if (lastWrite > m_lastWriteTime) {
      LOG_INFO("Hot-reloading configuration...");
      Load();
    }
  } catch (...) {
  }
}

void ConfigManager::ImportLegacyIni(const std::filesystem::path &iniPath) {
  std::array<char, 32> buf{};
  std::string path = iniPath.string();
  m_config.dlss.mode =
      GetPrivateProfileIntA("Settings", "DLSSMode", 5, path.c_str());
  m_config.fg.multiplier =
      GetPrivateProfileIntA("Settings", "FrameGenMultiplier", 4, path.c_str());
  GetPrivateProfileStringA("Settings", "Sharpness", "0.5", buf.data(),
                           static_cast<DWORD>(buf.size()), path.c_str());
  float val = 0.5f;
  std::from_chars(buf.data(), buf.data() + std::strlen(buf.data()), val);
  m_config.dlss.sharpness = val;
}

void ConfigManager::ResetToDefaults() {
  m_config = ModConfig{};
  m_dirty = true;
  Save();
}
void ConfigManager::MarkDirty() { m_dirty = true; }
void ConfigManager::SaveIfDirty() {
  if (m_dirty)
    Save();
}
