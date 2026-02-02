#include "config_manager.h"
#include "logger.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <algorithm>

ConfigManager& ConfigManager::Get() {
    static ConfigManager instance;
    return instance;
}

void ConfigManager::Load() {
    char buf[32];
    
    m_config.dlssMode = GetPrivateProfileIntA("Settings", "DLSSMode", 6, m_filePath.c_str());
    m_config.frameGenMultiplier = GetPrivateProfileIntA("Settings", "FrameGenMultiplier", 4, m_filePath.c_str());
    m_config.dlssPreset = GetPrivateProfileIntA("Settings", "DLSSPreset", 6, m_filePath.c_str());
    m_config.reflexEnabled = GetPrivateProfileIntA("Settings", "Reflex", 1, m_filePath.c_str()) != 0;
    m_config.smartFgEnabled = GetPrivateProfileIntA("Settings", "SmartFG", 0, m_filePath.c_str()) != 0;
    m_config.smartFgAutoDisable = GetPrivateProfileIntA("Settings", "SmartFGAutoDisable", 1, m_filePath.c_str()) != 0;
    m_config.smartFgSceneChangeEnabled = GetPrivateProfileIntA("Settings", "SmartFGSceneChange", 1, m_filePath.c_str()) != 0;
    m_config.logVerbosity = GetPrivateProfileIntA("Settings", "LogVerbosity", 1, m_filePath.c_str());
    m_config.quietResourceScan = GetPrivateProfileIntA("Settings", "QuietResourceScan", 0, m_filePath.c_str()) != 0;
    m_config.debugMode = GetPrivateProfileIntA("Settings", "DebugMode", 0, m_filePath.c_str()) != 0;
    m_config.uiVisible = GetPrivateProfileIntA("Settings", "UIVisible", 0, m_filePath.c_str()) != 0;
    m_config.uiExpanded = GetPrivateProfileIntA("Settings", "UIExpanded", 0, m_filePath.c_str()) != 0;
    m_config.showFPS = GetPrivateProfileIntA("Settings", "ShowFPS", 0, m_filePath.c_str()) != 0;
    m_config.showVignette = GetPrivateProfileIntA("Settings", "ShowVignette", 0, m_filePath.c_str()) != 0;
    m_config.menuHotkey = GetPrivateProfileIntA("Settings", "MenuHotkey", 0x74, m_filePath.c_str());
    m_config.fpsHotkey = GetPrivateProfileIntA("Settings", "FPSHotkey", 0x75, m_filePath.c_str());
    m_config.vignetteHotkey = GetPrivateProfileIntA("Settings", "VignetteHotkey", 0x76, m_filePath.c_str());
    m_config.uiPosX = GetPrivateProfileIntA("Settings", "UIPosX", 50, m_filePath.c_str());
    m_config.uiPosY = GetPrivateProfileIntA("Settings", "UIPosY", 50, m_filePath.c_str());
    
    GetPrivateProfileStringA("Settings", "Sharpness", "0.5", buf, 32, m_filePath.c_str());
    m_config.sharpness = (float)atof(buf);
    
    GetPrivateProfileStringA("Settings", "LODBias", "-1.0", buf, 32, m_filePath.c_str());
    m_config.lodBias = (float)atof(buf);

    GetPrivateProfileStringA("Settings", "MVecScaleX", "1.0", buf, 32, m_filePath.c_str());
    m_config.mvecScaleX = (float)atof(buf);

    GetPrivateProfileStringA("Settings", "MVecScaleY", "1.0", buf, 32, m_filePath.c_str());
    m_config.mvecScaleY = (float)atof(buf);

    m_config.rayReconstructionEnabled = GetPrivateProfileIntA("Settings", "RayReconstruction", 1, m_filePath.c_str()) != 0;

    GetPrivateProfileStringA("Settings", "SmartFGAutoDisableFPS", "120.0", buf, 32, m_filePath.c_str());
    m_config.smartFgAutoDisableFps = (float)atof(buf);
    GetPrivateProfileStringA("Settings", "SmartFGSceneChangeThreshold", "0.25", buf, 32, m_filePath.c_str());
    m_config.smartFgSceneChangeThreshold = (float)atof(buf);
    GetPrivateProfileStringA("Settings", "SmartFGInterpolationQuality", "0.50", buf, 32, m_filePath.c_str());
    m_config.smartFgInterpolationQuality = (float)atof(buf);

    GetPrivateProfileStringA("Settings", "VignetteIntensity", "0.35", buf, 32, m_filePath.c_str());
    m_config.vignetteIntensity = (float)atof(buf);

    GetPrivateProfileStringA("Settings", "VignetteRadius", "0.78", buf, 32, m_filePath.c_str());
    m_config.vignetteRadius = (float)atof(buf);

    GetPrivateProfileStringA("Settings", "VignetteSoftness", "0.55", buf, 32, m_filePath.c_str());
    m_config.vignetteSoftness = (float)atof(buf);

    GetPrivateProfileStringA("Settings", "VignetteColorR", "0.01", buf, 32, m_filePath.c_str());
    m_config.vignetteColorR = (float)atof(buf);
    GetPrivateProfileStringA("Settings", "VignetteColorG", "0.73", buf, 32, m_filePath.c_str());
    m_config.vignetteColorG = (float)atof(buf);
    GetPrivateProfileStringA("Settings", "VignetteColorB", "0.93", buf, 32, m_filePath.c_str());
    m_config.vignetteColorB = (float)atof(buf);

    m_config.rrPreset = GetPrivateProfileIntA("Settings", "RRPreset", 0, m_filePath.c_str());
    GetPrivateProfileStringA("Settings", "RRDenoiserStrength", "0.50", buf, 32, m_filePath.c_str());
    m_config.rrDenoiserStrength = (float)atof(buf);

    if (m_config.dlssMode < 0 || m_config.dlssMode > 6) {
        LOG_WARN("Config: DLSSMode out of range (%d), clamping to 6", m_config.dlssMode);
        m_config.dlssMode = 6;
    }
    if (m_config.frameGenMultiplier < 0 || m_config.frameGenMultiplier > 4 || m_config.frameGenMultiplier == 1) {
        int original = m_config.frameGenMultiplier;
        if (original < 0) m_config.frameGenMultiplier = 0;
        else if (original == 1) m_config.frameGenMultiplier = 2;
        else m_config.frameGenMultiplier = 4;
        LOG_WARN("Config: FrameGenMultiplier %d invalid, clamped to %d", original, m_config.frameGenMultiplier);
    }
    if (m_config.dlssPreset < 0 || m_config.dlssPreset > 7) {
        LOG_WARN("Config: DLSSPreset out of range (%d), clamping to 0..7", m_config.dlssPreset);
        m_config.dlssPreset = (m_config.dlssPreset < 0) ? 0 : 7;
    }
    if (m_config.sharpness < 0.0f || m_config.sharpness > 1.0f) {
        LOG_WARN("Config: Sharpness out of range (%.2f), clamping to 0..1", m_config.sharpness);
        if (m_config.sharpness < 0.0f) m_config.sharpness = 0.0f;
        if (m_config.sharpness > 1.0f) m_config.sharpness = 1.0f;
    }
    if (m_config.lodBias < -2.0f || m_config.lodBias > 0.0f) {
        LOG_WARN("Config: LODBias out of range (%.2f), clamping to -2..0", m_config.lodBias);
        if (m_config.lodBias < -2.0f) m_config.lodBias = -2.0f;
        if (m_config.lodBias > 0.0f) m_config.lodBias = 0.0f;
    }
    if (m_config.mvecScaleX < 0.0f || m_config.mvecScaleX > 4.0f) {
        LOG_WARN("Config: MVecScaleX out of range (%.2f), clamping to 0..4", m_config.mvecScaleX);
        if (m_config.mvecScaleX < 0.0f) m_config.mvecScaleX = 0.0f;
        if (m_config.mvecScaleX > 4.0f) m_config.mvecScaleX = 4.0f;
    }
    if (m_config.mvecScaleY < 0.0f || m_config.mvecScaleY > 4.0f) {
        LOG_WARN("Config: MVecScaleY out of range (%.2f), clamping to 0..4", m_config.mvecScaleY);
        if (m_config.mvecScaleY < 0.0f) m_config.mvecScaleY = 0.0f;
        if (m_config.mvecScaleY > 4.0f) m_config.mvecScaleY = 4.0f;
    }
    if (m_config.smartFgAutoDisableFps < 30.0f || m_config.smartFgAutoDisableFps > 300.0f) {
        LOG_WARN("Config: SmartFGAutoDisableFPS out of range (%.1f), clamping to 30..300", m_config.smartFgAutoDisableFps);
        m_config.smartFgAutoDisableFps = std::clamp(m_config.smartFgAutoDisableFps, 30.0f, 300.0f);
    }
    if (m_config.smartFgSceneChangeThreshold < 0.05f || m_config.smartFgSceneChangeThreshold > 1.0f) {
        LOG_WARN("Config: SmartFGSceneChangeThreshold out of range (%.2f), clamping to 0.05..1", m_config.smartFgSceneChangeThreshold);
        m_config.smartFgSceneChangeThreshold = std::clamp(m_config.smartFgSceneChangeThreshold, 0.05f, 1.0f);
    }
    if (m_config.smartFgInterpolationQuality < 0.0f || m_config.smartFgInterpolationQuality > 1.0f) {
        LOG_WARN("Config: SmartFGInterpolationQuality out of range (%.2f), clamping to 0..1", m_config.smartFgInterpolationQuality);
        m_config.smartFgInterpolationQuality = std::clamp(m_config.smartFgInterpolationQuality, 0.0f, 1.0f);
    }
    if (m_config.rrPreset < 0 || m_config.rrPreset > 12) {
        LOG_WARN("Config: RRPreset out of range (%d), clamping to 0..12", m_config.rrPreset);
        m_config.rrPreset = (m_config.rrPreset < 0) ? 0 : 12;
    }
    if (m_config.rrDenoiserStrength < 0.0f || m_config.rrDenoiserStrength > 1.0f) {
        LOG_WARN("Config: RRDenoiserStrength out of range (%.2f), clamping to 0..1", m_config.rrDenoiserStrength);
        m_config.rrDenoiserStrength = std::clamp(m_config.rrDenoiserStrength, 0.0f, 1.0f);
    }
    if (m_config.logVerbosity < 0 || m_config.logVerbosity > 2) {
        LOG_WARN("Config: LogVerbosity out of range (%d), clamping to 0..2", m_config.logVerbosity);
        if (m_config.logVerbosity < 0) m_config.logVerbosity = 0;
        if (m_config.logVerbosity > 2) m_config.logVerbosity = 2;
    }
    if (m_config.vignetteIntensity < 0.0f || m_config.vignetteIntensity > 1.0f) {
        LOG_WARN("Config: VignetteIntensity out of range (%.2f), clamping to 0..1", m_config.vignetteIntensity);
        if (m_config.vignetteIntensity < 0.0f) m_config.vignetteIntensity = 0.0f;
        if (m_config.vignetteIntensity > 1.0f) m_config.vignetteIntensity = 1.0f;
    }
    if (m_config.vignetteRadius < 0.2f || m_config.vignetteRadius > 1.0f) {
        LOG_WARN("Config: VignetteRadius out of range (%.2f), clamping to 0.2..1", m_config.vignetteRadius);
        if (m_config.vignetteRadius < 0.2f) m_config.vignetteRadius = 0.2f;
        if (m_config.vignetteRadius > 1.0f) m_config.vignetteRadius = 1.0f;
    }
    if (m_config.vignetteSoftness < 0.05f || m_config.vignetteSoftness > 1.0f) {
        LOG_WARN("Config: VignetteSoftness out of range (%.2f), clamping to 0.05..1", m_config.vignetteSoftness);
        if (m_config.vignetteSoftness < 0.05f) m_config.vignetteSoftness = 0.05f;
        if (m_config.vignetteSoftness > 1.0f) m_config.vignetteSoftness = 1.0f;
    }
    if (m_config.vignetteColorR < 0.0f || m_config.vignetteColorR > 1.0f ||
        m_config.vignetteColorG < 0.0f || m_config.vignetteColorG > 1.0f ||
        m_config.vignetteColorB < 0.0f || m_config.vignetteColorB > 1.0f) {
        LOG_WARN("Config: VignetteColor out of range, clamping to 0..1");
        m_config.vignetteColorR = std::clamp(m_config.vignetteColorR, 0.0f, 1.0f);
        m_config.vignetteColorG = std::clamp(m_config.vignetteColorG, 0.0f, 1.0f);
        m_config.vignetteColorB = std::clamp(m_config.vignetteColorB, 0.0f, 1.0f);
    }
    
    LOG_INFO("Config Loaded: DLSS=%d, FG=%dx, Preset=%d, Scale=%.2fx%.2f", 
        m_config.dlssMode, m_config.frameGenMultiplier, m_config.dlssPreset, m_config.mvecScaleX, m_config.mvecScaleY);
}

void ConfigManager::Save() {
    char buf[32];
    
    WritePrivateProfileStringA("Settings", "DLSSMode", std::to_string(m_config.dlssMode).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "FrameGenMultiplier", std::to_string(m_config.frameGenMultiplier).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "DLSSPreset", std::to_string(m_config.dlssPreset).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "Reflex", std::to_string(m_config.reflexEnabled ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "SmartFG", std::to_string(m_config.smartFgEnabled ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "SmartFGAutoDisable", std::to_string(m_config.smartFgAutoDisable ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "SmartFGSceneChange", std::to_string(m_config.smartFgSceneChangeEnabled ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "LogVerbosity", std::to_string(m_config.logVerbosity).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "QuietResourceScan", std::to_string(m_config.quietResourceScan ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "DebugMode", std::to_string(m_config.debugMode ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "UIVisible", std::to_string(m_config.uiVisible ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "UIExpanded", std::to_string(m_config.uiExpanded ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "ShowFPS", std::to_string(m_config.showFPS ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "ShowVignette", std::to_string(m_config.showVignette ? 1 : 0).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "MenuHotkey", std::to_string(m_config.menuHotkey).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "FPSHotkey", std::to_string(m_config.fpsHotkey).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "VignetteHotkey", std::to_string(m_config.vignetteHotkey).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "UIPosX", std::to_string(m_config.uiPosX).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "UIPosY", std::to_string(m_config.uiPosY).c_str(), m_filePath.c_str());
    
    sprintf_s(buf, "%.2f", m_config.sharpness);
    WritePrivateProfileStringA("Settings", "Sharpness", buf, m_filePath.c_str());
    
    sprintf_s(buf, "%.2f", m_config.lodBias);
    WritePrivateProfileStringA("Settings", "LODBias", buf, m_filePath.c_str());

    sprintf_s(buf, "%.2f", m_config.mvecScaleX);
    WritePrivateProfileStringA("Settings", "MVecScaleX", buf, m_filePath.c_str());

    sprintf_s(buf, "%.2f", m_config.mvecScaleY);
    WritePrivateProfileStringA("Settings", "MVecScaleY", buf, m_filePath.c_str());

    WritePrivateProfileStringA("Settings", "RayReconstruction", std::to_string(m_config.rayReconstructionEnabled ? 1 : 0).c_str(), m_filePath.c_str());

    sprintf_s(buf, "%.1f", m_config.smartFgAutoDisableFps);
    WritePrivateProfileStringA("Settings", "SmartFGAutoDisableFPS", buf, m_filePath.c_str());
    sprintf_s(buf, "%.2f", m_config.smartFgSceneChangeThreshold);
    WritePrivateProfileStringA("Settings", "SmartFGSceneChangeThreshold", buf, m_filePath.c_str());
    sprintf_s(buf, "%.2f", m_config.smartFgInterpolationQuality);
    WritePrivateProfileStringA("Settings", "SmartFGInterpolationQuality", buf, m_filePath.c_str());

    sprintf_s(buf, "%.2f", m_config.vignetteIntensity);
    WritePrivateProfileStringA("Settings", "VignetteIntensity", buf, m_filePath.c_str());

    sprintf_s(buf, "%.2f", m_config.vignetteRadius);
    WritePrivateProfileStringA("Settings", "VignetteRadius", buf, m_filePath.c_str());

    sprintf_s(buf, "%.2f", m_config.vignetteSoftness);
    WritePrivateProfileStringA("Settings", "VignetteSoftness", buf, m_filePath.c_str());

    sprintf_s(buf, "%.2f", m_config.vignetteColorR);
    WritePrivateProfileStringA("Settings", "VignetteColorR", buf, m_filePath.c_str());
    sprintf_s(buf, "%.2f", m_config.vignetteColorG);
    WritePrivateProfileStringA("Settings", "VignetteColorG", buf, m_filePath.c_str());
    sprintf_s(buf, "%.2f", m_config.vignetteColorB);
    WritePrivateProfileStringA("Settings", "VignetteColorB", buf, m_filePath.c_str());

    WritePrivateProfileStringA("Settings", "RRPreset", std::to_string(m_config.rrPreset).c_str(), m_filePath.c_str());
    sprintf_s(buf, "%.2f", m_config.rrDenoiserStrength);
    WritePrivateProfileStringA("Settings", "RRDenoiserStrength", buf, m_filePath.c_str());
}

void ConfigManager::ResetToDefaults() {
    m_config = ModConfig{};
    Save();
}
