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
};

class ConfigManager {
public:
    static ConfigManager& Get();
    
    void Load();
    void Save();
    
    ModConfig& Data() { return m_config; }

private:
    ConfigManager() = default;
    ModConfig m_config;
    std::string m_filePath = "dlss_settings.ini";
};
