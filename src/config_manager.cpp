#include "config_manager.h"
#include "logger.h"
#include <windows.h>
#include <stdio.h>
#include <string>

ConfigManager& ConfigManager::Get() {
    static ConfigManager instance;
    return instance;
}

void ConfigManager::Load() {
    char buf[32];
    
    m_config.dlssMode = GetPrivateProfileIntA("Settings", "DLSSMode", 5, m_filePath.c_str());
    m_config.frameGenMultiplier = GetPrivateProfileIntA("Settings", "FrameGenMultiplier", 4, m_filePath.c_str());
    m_config.dlssPreset = GetPrivateProfileIntA("Settings", "DLSSPreset", 0, m_filePath.c_str());
    m_config.reflexEnabled = GetPrivateProfileIntA("Settings", "Reflex", 1, m_filePath.c_str()) != 0;
    
    GetPrivateProfileStringA("Settings", "Sharpness", "0.5", buf, 32, m_filePath.c_str());
    m_config.sharpness = (float)atof(buf);
    
    GetPrivateProfileStringA("Settings", "LODBias", "-1.0", buf, 32, m_filePath.c_str());
    m_config.lodBias = (float)atof(buf);

    GetPrivateProfileStringA("Settings", "MVecScaleX", "1.0", buf, 32, m_filePath.c_str());
    m_config.mvecScaleX = (float)atof(buf);

    GetPrivateProfileStringA("Settings", "MVecScaleY", "1.0", buf, 32, m_filePath.c_str());
    m_config.mvecScaleY = (float)atof(buf);
    
    LOG_INFO("Config Loaded: DLSS=%d, FG=%dx, Preset=%d, Scale=%.2fx%.2f", 
        m_config.dlssMode, m_config.frameGenMultiplier, m_config.dlssPreset, m_config.mvecScaleX, m_config.mvecScaleY);
}

void ConfigManager::Save() {
    char buf[32];
    
    WritePrivateProfileStringA("Settings", "DLSSMode", std::to_string(m_config.dlssMode).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "FrameGenMultiplier", std::to_string(m_config.frameGenMultiplier).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "DLSSPreset", std::to_string(m_config.dlssPreset).c_str(), m_filePath.c_str());
    WritePrivateProfileStringA("Settings", "Reflex", std::to_string(m_config.reflexEnabled ? 1 : 0).c_str(), m_filePath.c_str());
    
    sprintf_s(buf, "%.2f", m_config.sharpness);
    WritePrivateProfileStringA("Settings", "Sharpness", buf, m_filePath.c_str());
    
    sprintf_s(buf, "%.2f", m_config.lodBias);
    WritePrivateProfileStringA("Settings", "LODBias", buf, m_filePath.c_str());

    sprintf_s(buf, "%.2f", m_config.mvecScaleX);
    WritePrivateProfileStringA("Settings", "MVecScaleX", buf, m_filePath.c_str());

    sprintf_s(buf, "%.2f", m_config.mvecScaleY);
    WritePrivateProfileStringA("Settings", "MVecScaleY", buf, m_filePath.c_str());
}