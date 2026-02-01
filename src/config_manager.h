#pragma once
#include <string>

struct ModConfig {
    int dlssMode = 5; // DLAA default
    int frameGenMultiplier = 4;
    int dlssPreset = 0; // 0=Default, 1=A, 2=B... 6=F
    float sharpness = 0.5f;
    float lodBias = -1.0f;
    float mvecScaleX = 1.0f;
    float mvecScaleY = 1.0f;
    bool reflexEnabled = true;
    bool hudFixEnabled = false;
    int logVerbosity = 1; // 0=Quiet, 1=Normal, 2=Verbose
    bool quietResourceScan = false;
    bool debugMode = false;
    bool uiVisible = false;
    bool uiExpanded = false;
    bool showFPS = false;
    bool showVignette = false;
    int uiPosX = 50;
    int uiPosY = 50;
    float vignetteIntensity = 0.35f;
    float vignetteRadius = 0.78f;
    float vignetteSoftness = 0.55f;
};

class ConfigManager {
public:
    static ConfigManager& Get();
    
    void Load();
    void Save();
    void ResetToDefaults();
    
    ModConfig& Data() { return m_config; }

private:
    ConfigManager() = default;
    ModConfig m_config;
    std::string m_filePath = "dlss_settings.ini";
};
