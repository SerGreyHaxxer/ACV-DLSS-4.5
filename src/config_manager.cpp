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
    m_lastWriteTime = std::filesystem::last_write_time(path);

    // DLSS
    m_config.dlss.mode = tbl["DLSS"]["mode"].value_or(m_config.dlss.mode);
    m_config.dlss.preset = tbl["DLSS"]["preset"].value_or(m_config.dlss.preset);
    m_config.dlss.sharpness =
        tbl["DLSS"]["sharpness"].value_or(m_config.dlss.sharpness);
    m_config.dlss.lodBias =
        tbl["DLSS"]["lod_bias"].value_or(m_config.dlss.lodBias);

    // FrameGen
    m_config.fg.multiplier =
        tbl["FrameGen"]["multiplier"].value_or(m_config.fg.multiplier);
    m_config.fg.smartEnabled =
        tbl["FrameGen"]["smart_enabled"].value_or(m_config.fg.smartEnabled);
    m_config.fg.autoDisable =
        tbl["FrameGen"]["auto_disable"].value_or(m_config.fg.autoDisable);
    m_config.fg.autoDisableFps = tbl["FrameGen"]["auto_disable_fps"].value_or(
        m_config.fg.autoDisableFps);
    m_config.fg.sceneChangeEnabled =
        tbl["FrameGen"]["scene_change_enabled"].value_or(
            m_config.fg.sceneChangeEnabled);
    m_config.fg.sceneChangeThreshold =
        tbl["FrameGen"]["scene_change_threshold"].value_or(
            m_config.fg.sceneChangeThreshold);
    m_config.fg.interpolationQuality =
        tbl["FrameGen"]["interpolation_quality"].value_or(
            m_config.fg.interpolationQuality);

    // MotionVectors
    m_config.mvec.autoScale =
        tbl["MotionVectors"]["auto_scale"].value_or(m_config.mvec.autoScale);
    m_config.mvec.scaleX =
        tbl["MotionVectors"]["scale_x"].value_or(m_config.mvec.scaleX);
    m_config.mvec.scaleY =
        tbl["MotionVectors"]["scale_y"].value_or(m_config.mvec.scaleY);

    // RayReconstruction
    m_config.rr.enabled =
        tbl["RayReconstruction"]["enabled"].value_or(m_config.rr.enabled);
    m_config.rr.preset =
        tbl["RayReconstruction"]["preset"].value_or(m_config.rr.preset);
    m_config.rr.denoiserStrength =
        tbl["RayReconstruction"]["denoiser_strength"].value_or(
            m_config.rr.denoiserStrength);

    // DeepDVC
    m_config.dvc.enabled =
        tbl["DeepDVC"]["enabled"].value_or(m_config.dvc.enabled);
    m_config.dvc.intensity =
        tbl["DeepDVC"]["intensity"].value_or(m_config.dvc.intensity);
    m_config.dvc.saturation =
        tbl["DeepDVC"]["saturation"].value_or(m_config.dvc.saturation);
    m_config.dvc.adaptiveEnabled = tbl["DeepDVC"]["adaptive_enabled"].value_or(
        m_config.dvc.adaptiveEnabled);
    m_config.dvc.adaptiveStrength =
        tbl["DeepDVC"]["adaptive_strength"].value_or(
            m_config.dvc.adaptiveStrength);
    m_config.dvc.adaptiveMin =
        tbl["DeepDVC"]["adaptive_min"].value_or(m_config.dvc.adaptiveMin);
    m_config.dvc.adaptiveMax =
        tbl["DeepDVC"]["adaptive_max"].value_or(m_config.dvc.adaptiveMax);
    m_config.dvc.adaptiveSmoothing =
        tbl["DeepDVC"]["adaptive_smoothing"].value_or(
            m_config.dvc.adaptiveSmoothing);

    // HDR
    m_config.hdr.enabled = tbl["HDR"]["enabled"].value_or(m_config.hdr.enabled);
    m_config.hdr.peakNits =
        tbl["HDR"]["peak_nits"].value_or(m_config.hdr.peakNits);
    m_config.hdr.paperWhiteNits =
        tbl["HDR"]["paper_white_nits"].value_or(m_config.hdr.paperWhiteNits);
    m_config.hdr.exposure =
        tbl["HDR"]["exposure"].value_or(m_config.hdr.exposure);
    m_config.hdr.gamma = tbl["HDR"]["gamma"].value_or(m_config.hdr.gamma);
    m_config.hdr.tonemapCurve =
        tbl["HDR"]["tonemap_curve"].value_or(m_config.hdr.tonemapCurve);
    m_config.hdr.saturation =
        tbl["HDR"]["saturation"].value_or(m_config.hdr.saturation);

    // UI
    m_config.ui.visible = tbl["UI"]["visible"].value_or(m_config.ui.visible);
    m_config.ui.showFPS = tbl["UI"]["show_fps"].value_or(m_config.ui.showFPS);
    m_config.ui.showVignette =
        tbl["UI"]["show_vignette"].value_or(m_config.ui.showVignette);
    m_config.ui.menuHotkey =
        tbl["UI"]["menu_hotkey"].value_or(m_config.ui.menuHotkey);
    m_config.ui.fpsHotkey =
        tbl["UI"]["fps_hotkey"].value_or(m_config.ui.fpsHotkey);
    m_config.ui.vignetteHotkey =
        tbl["UI"]["vignette_hotkey"].value_or(m_config.ui.vignetteHotkey);
    m_config.ui.vignetteIntensity =
        tbl["UI"]["vignette_intensity"].value_or(m_config.ui.vignetteIntensity);
    m_config.ui.vignetteRadius =
        tbl["UI"]["vignette_radius"].value_or(m_config.ui.vignetteRadius);
    m_config.ui.vignetteSoftness =
        tbl["UI"]["vignette_softness"].value_or(m_config.ui.vignetteSoftness);
    m_config.ui.vignetteColorR =
        tbl["UI"]["vignette_color_r"].value_or(m_config.ui.vignetteColorR);
    m_config.ui.vignetteColorG =
        tbl["UI"]["vignette_color_g"].value_or(m_config.ui.vignetteColorG);
    m_config.ui.vignetteColorB =
        tbl["UI"]["vignette_color_b"].value_or(m_config.ui.vignetteColorB);

    // System
    m_config.system.logVerbosity =
        tbl["System"]["log_verbosity"].value_or(m_config.system.logVerbosity);
    m_config.system.debugMode =
        tbl["System"]["debug_mode"].value_or(m_config.system.debugMode);
    m_config.system.setupWizardCompleted =
        tbl["System"]["wizard_completed"].value_or(
            m_config.system.setupWizardCompleted);

    LOG_INFO("Configuration loaded from TOML.");
  } catch (const std::exception &ex) {
    LOG_ERROR("Failed to parse config.toml: {}", ex.what());
  }
}

void ConfigManager::Save() {
  auto tbl = toml::table{
      {{"DLSS", toml::table{{{"mode", m_config.dlss.mode},
                             {"preset", m_config.dlss.preset},
                             {"sharpness", m_config.dlss.sharpness},
                             {"lod_bias", m_config.dlss.lodBias}}}},
       {"FrameGen",
        toml::table{
            {{"multiplier", m_config.fg.multiplier},
             {"smart_enabled", m_config.fg.smartEnabled},
             {"auto_disable", m_config.fg.autoDisable},
             {"auto_disable_fps", m_config.fg.autoDisableFps},
             {"scene_change_enabled", m_config.fg.sceneChangeEnabled},
             {"scene_change_threshold", m_config.fg.sceneChangeThreshold},
             {"interpolation_quality", m_config.fg.interpolationQuality}}}},
       {"MotionVectors", toml::table{{{"auto_scale", m_config.mvec.autoScale},
                                      {"scale_x", m_config.mvec.scaleX},
                                      {"scale_y", m_config.mvec.scaleY}}}},
       {"RayReconstruction",
        toml::table{{{"enabled", m_config.rr.enabled},
                     {"preset", m_config.rr.preset},
                     {"denoiser_strength", m_config.rr.denoiserStrength}}}},
       {"DeepDVC",
        toml::table{{{"enabled", m_config.dvc.enabled},
                     {"intensity", m_config.dvc.intensity},
                     {"saturation", m_config.dvc.saturation},
                     {"adaptive_enabled", m_config.dvc.adaptiveEnabled},
                     {"adaptive_strength", m_config.dvc.adaptiveStrength},
                     {"adaptive_min", m_config.dvc.adaptiveMin},
                     {"adaptive_max", m_config.dvc.adaptiveMax},
                     {"adaptive_smoothing", m_config.dvc.adaptiveSmoothing}}}},
       {"HDR", toml::table{{{"enabled", m_config.hdr.enabled},
                            {"peak_nits", m_config.hdr.peakNits},
                            {"paper_white_nits", m_config.hdr.paperWhiteNits},
                            {"exposure", m_config.hdr.exposure},
                            {"gamma", m_config.hdr.gamma},
                            {"tonemap_curve", m_config.hdr.tonemapCurve},
                            {"saturation", m_config.hdr.saturation}}}},
       {"UI",
        toml::table{{{"visible", m_config.ui.visible},
                     {"show_fps", m_config.ui.showFPS},
                     {"show_vignette", m_config.ui.showVignette},
                     {"menu_hotkey", m_config.ui.menuHotkey},
                     {"fps_hotkey", m_config.ui.fpsHotkey},
                     {"vignette_hotkey", m_config.ui.vignetteHotkey},
                     {"vignette_intensity", m_config.ui.vignetteIntensity},
                     {"vignette_radius", m_config.ui.vignetteRadius},
                     {"vignette_softness", m_config.ui.vignetteSoftness},
                     {"vignette_color_r", m_config.ui.vignetteColorR},
                     {"vignette_color_g", m_config.ui.vignetteColorG},
                     {"vignette_color_b", m_config.ui.vignetteColorB}}}},
       {"System", toml::table{{{"log_verbosity", m_config.system.logVerbosity},
                               {"debug_mode", m_config.system.debugMode},
                               {"wizard_completed",
                                m_config.system.setupWizardCompleted}}}}}};

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
