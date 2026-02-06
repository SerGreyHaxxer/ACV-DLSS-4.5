/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "config_manager.h" // For ModConfig and its structs (Reflected)
#include "valhalla_gui.h"   // For ImGuiOverlay::Checkbox, SliderFloat, etc.
#include "imgui_overlay.h"  // For accessing ImGuiOverlay instance
#include <variant>

// ============================================================================
// AUTO-UI GENERATOR
// ============================================================================
// Generates ValhallaGUI widgets automatically from reflected structs.
// ============================================================================

namespace AutoUI {

// Helper to check if a variant holds a specific type
template <typename T, typename Variant>
bool holds_alternative(const Variant& v) {
  return std::holds_alternative<T>(v);
}

// Draw a single field
template <typename T>
void DrawField(ImGuiOverlay& gui, T& obj, const cpp26::reflect::FieldInfo& field, bool& changed) {
  using namespace cpp26::reflect;

  // Handle Hidden
  if (holds_alternative<ui::hidden>(field.annotation)) return;

  // Handle Boolean -> Checkbox
  if (field.type == FieldType::Bool) {
    bool val = field.getBool(&obj);
    if (gui.Checkbox(std::string(field.name).c_str(), &val)) {
      field.setBool(&obj, val);
      changed = true;
    }
    return;
  }

  // Handle Int
  if (field.type == FieldType::Int) {
    int val = field.getInt(&obj);
    
    // Dropdown
    if (auto* ann = std::get_if<ui::dropdown>(&field.annotation)) {
      if (ann->options && ann->count > 0) {
        if (gui.Combo(std::string(field.name).c_str(), &val, ann->options, ann->count)) {
          field.setInt(&obj, val);
          changed = true;
        }
      } else {
        // Fallback for int with no options: treated as slider if range implies it, or just generic
        // For now, no generic int slider in ValhallaGUI, so we use float slider cast
        float fVal = static_cast<float>(val);
        if (gui.SliderFloat(std::string(field.name).c_str(), &fVal, 0.0f, 10.0f, "%.0f")) {
             field.setInt(&obj, static_cast<int>(fVal));
             changed = true;
        }
      }
    } 
    // Int Slider
    else if (auto* ann = std::get_if<ui::slider_int>(&field.annotation)) {
      float fVal = static_cast<float>(val);
      if (gui.SliderFloat(std::string(field.name).c_str(), &fVal, static_cast<float>(ann->min), static_cast<float>(ann->max), "%.0f")) {
        field.setInt(&obj, static_cast<int>(fVal));
        changed = true;
      }
    }
    return;
  }

  // Handle Float
  if (field.type == FieldType::Float) {
    float val = field.getFloat(&obj);

    if (auto* ann = std::get_if<ui::slider_float>(&field.annotation)) {
      if (gui.SliderFloat(std::string(field.name).c_str(), &val, ann->min, ann->max)) {
        field.setFloat(&obj, val);
        changed = true;
      }
    }
    else if (std::holds_alternative<ui::color_rgb>(field.annotation)) {
       // Single float color component slider
       if (gui.SliderFloat(std::string(field.name).c_str(), &val, 0.0f, 1.0f)) {
         field.setFloat(&obj, val);
         changed = true;
       }
    }
    return;
  }
}

// Draw all fields of a struct
template <typename T>
bool DrawStruct(ImGuiOverlay& gui, T& obj) {
  bool changed = false;
  cpp26::reflect::forEachField<T>([&](const cpp26::reflect::FieldInfo& field) {
    DrawField(gui, obj, field, changed);
  });
  return changed;
}

// Draw fields belonging to a specific category
template <typename T>
bool DrawCategory(ImGuiOverlay& gui, T& obj, const char* category) {
  bool changed = false;
  cpp26::reflect::forEachFieldInCategory<T>(category, [&](const cpp26::reflect::FieldInfo& field) {
    DrawField(gui, obj, field, changed);
  });
  return changed;
}

} // namespace AutoUI
