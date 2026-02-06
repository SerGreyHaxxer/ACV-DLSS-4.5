/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "config_manager.h"
#include "logger.h"
#include <array>
#include <charconv>
#include <fstream>
#include <toml++/toml.hpp>
#include <windows.h>

// ============================================================================
// REFLECTION HELPERS
// ============================================================================

template <typename T>
void SerializeStruct(toml::table& tbl, const T& obj, const char* sectionName) {
  toml::table section;
  cpp26::reflect::forEachField<T>([&](const cpp26::reflect::FieldInfo& field) {
    if (field.type == cpp26::reflect::FieldType::Int) {
      section.insert_or_assign(field.name, field.getInt(&obj));
    } else if (field.type == cpp26::reflect::FieldType::Float) {
      // Round to 4 decimal places for cleaner config files
      float val = field.getFloat(&obj);
      // toml++ handles serialization, but we can't easily force precision here without string conversion
      section.insert_or_assign(field.name, val);
    } else if (field.type == cpp26::reflect::FieldType::Bool) {
      section.insert_or_assign(field.name, field.getBool(&obj));
    }
  });
  tbl.insert_or_assign(sectionName, section);
}

template <typename T>
void DeserializeStruct(const toml::table& tbl, T& obj, const char* sectionName) {
  if (!tbl.contains(sectionName)) return;
  const auto* section = tbl[sectionName].as_table();
  if (!section) return;

  cpp26::reflect::forEachField<T>([&](const cpp26::reflect::FieldInfo& field) {
    if (field.type == cpp26::reflect::FieldType::Int) {
      if (auto val = section->get(field.name); val && val->is_integer()) {
        field.setInt(&obj, static_cast<int>(val->as_integer()->get()));
      }
    } else if (field.type == cpp26::reflect::FieldType::Float) {
      if (auto val = section->get(field.name); val && val->is_floating_point()) {
        field.setFloat(&obj, static_cast<float>(val->as_floating_point()->get()));
      } else if (val && val->is_integer()) {
        field.setFloat(&obj, static_cast<float>(val->as_integer()->get()));
      }
    } else if (field.type == cpp26::reflect::FieldType::Bool) {
      if (auto val = section->get(field.name); val && val->is_boolean()) {
        field.setBool(&obj, val->as_boolean()->get());
      }
    }
  });
}

// ============================================================================
// ConfigManager Implementation
// ============================================================================

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

    DeserializeStruct(tbl, parsed.dlss, "DLSS");
    DeserializeStruct(tbl, parsed.fg, "FrameGen");
    DeserializeStruct(tbl, parsed.mvec, "MotionVectors");
    DeserializeStruct(tbl, parsed.rr, "RayReconstruction");
    DeserializeStruct(tbl, parsed.dvc, "DeepDVC");
    DeserializeStruct(tbl, parsed.hdr, "HDR");
    DeserializeStruct(tbl, parsed.ui, "UI");
    DeserializeStruct(tbl, parsed.customization, "Customization");
    DeserializeStruct(tbl, parsed.system, "System");

    // Swap the parsed config into m_config atomically (under lock)
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

  auto tbl = toml::table{};

  SerializeStruct(tbl, snapshot.dlss, "DLSS");
  SerializeStruct(tbl, snapshot.fg, "FrameGen");
  SerializeStruct(tbl, snapshot.mvec, "MotionVectors");
  SerializeStruct(tbl, snapshot.rr, "RayReconstruction");
  SerializeStruct(tbl, snapshot.dvc, "DeepDVC");
  SerializeStruct(tbl, snapshot.hdr, "HDR");
  SerializeStruct(tbl, snapshot.ui, "UI");
  SerializeStruct(tbl, snapshot.customization, "Customization");
  SerializeStruct(tbl, snapshot.system, "System");

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
