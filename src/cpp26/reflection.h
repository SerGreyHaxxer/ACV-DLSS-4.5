/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

// ============================================================================
// C++26 POLYFILL: Compile-Time Reflection
// ============================================================================
// Macro-based reflection for automatic serialization and UI generation.
// Uses X-macros and constexpr introspection to enumerate struct fields.
//
// Usage:
//   REFLECT_STRUCT_BEGIN(MyConfig)
//     REFLECT_FIELD(int, myInt, 0, ui::slider{0, 100})
//     REFLECT_FIELD(float, myFloat, 1.0f, ui::slider{0.0f, 2.0f})
//     REFLECT_FIELD(bool, myBool, true, ui::checkbox{})
//   REFLECT_STRUCT_END()
//
// When MSVC adds std::meta support:
//   Use actual reflection instead of X-macros
// ============================================================================

#include <array>
#include <cstddef>
#include <functional>
#include <string_view>
#include <type_traits>
#include <variant>

namespace cpp26::reflect {

// ============================================================================
// UI ANNOTATIONS
// ============================================================================

namespace ui {

struct slider_int {
  int min = 0;
  int max = 100;
  constexpr slider_int() = default;
  constexpr slider_int(int mn, int mx) : min(mn), max(mx) {}
};

struct slider_float {
  float min = 0.0f;
  float max = 1.0f;
  constexpr slider_float() = default;
  constexpr slider_float(float mn, float mx) : min(mn), max(mx) {}
};

struct checkbox {
  constexpr checkbox() = default;
};

struct color_rgb {
  constexpr color_rgb() = default;
};

struct dropdown {
  const char* const* options = nullptr;
  int count = 0;
  constexpr dropdown() = default;
  constexpr dropdown(const char* const* opts, int cnt) : options(opts), count(cnt) {}
};

struct hidden {
  constexpr hidden() = default;
};

struct category {
  const char* name = "";
  constexpr category() = default;
  constexpr category(const char* n) : name(n) {}
};

using Annotation = std::variant<slider_int, slider_float, checkbox, color_rgb, dropdown, hidden, category>;

} // namespace ui

// ============================================================================
// FIELD METADATA
// ============================================================================

enum class FieldType { Int, Float, Bool, String, Struct, Unknown };

template <typename T> constexpr FieldType getFieldType() {
  if constexpr (std::is_same_v<T, int>)
    return FieldType::Int;
  else if constexpr (std::is_same_v<T, float>)
    return FieldType::Float;
  else if constexpr (std::is_same_v<T, bool>)
    return FieldType::Bool;
  else if constexpr (std::is_same_v<T, double>)
    return FieldType::Float;
  else
    return FieldType::Unknown;
}

struct FieldInfo {
  std::string_view name;
  std::string_view category;
  FieldType type;
  std::size_t offset;
  std::size_t size;
  ui::Annotation annotation;

  // Accessor functions (set at registration)
  std::function<void(void* obj, int val)> setInt;
  std::function<int(const void* obj)> getInt;
  std::function<void(void* obj, float val)> setFloat;
  std::function<float(const void* obj)> getFloat;
  std::function<void(void* obj, bool val)> setBool;
  std::function<bool(const void* obj)> getBool;
};

// ============================================================================
// STRUCT REGISTRY
// ============================================================================

// Maximum fields per struct (increase if needed)
constexpr std::size_t kMaxFields = 128;

template <typename T> struct StructInfo {
  static inline std::array<FieldInfo, kMaxFields> fields{};
  static inline std::size_t fieldCount = 0;
  static inline std::string_view name;

  static void reset() { fieldCount = 0; }

  static std::size_t registerField(FieldInfo info) {
    if (fieldCount < kMaxFields) {
      fields[fieldCount] = std::move(info);
      return fieldCount++;
    }
    return static_cast<std::size_t>(-1);
  }

  static const FieldInfo* getField(std::string_view fieldName) {
    for (std::size_t i = 0; i < fieldCount; ++i) {
      if (fields[i].name == fieldName) return &fields[i];
    }
    return nullptr;
  }
};

// ============================================================================
// FIELD VALUE ACCESSORS
// ============================================================================

// Generic get/set for field value by type
template <typename T, typename FieldT> void setFieldValue(void* obj, std::size_t offset, FieldT value) {
  *reinterpret_cast<FieldT*>(static_cast<char*>(obj) + offset) = value;
}

template <typename T, typename FieldT> FieldT getFieldValue(const void* obj, std::size_t offset) {
  return *reinterpret_cast<const FieldT*>(static_cast<const char*>(obj) + offset);
}

// ============================================================================
// REFLECTION REGISTRATION HELPER
// ============================================================================

template <typename StructT, typename FieldT> struct FieldRegistrar {
  FieldRegistrar(std::string_view name, std::string_view category, FieldT StructT::* memberPtr,
                 ui::Annotation annotation) {
    FieldInfo info;
    info.name = name;
    info.category = category;
    info.type = getFieldType<FieldT>();
    info.offset = reinterpret_cast<std::size_t>(&(static_cast<StructT*>(nullptr)->*memberPtr));
    info.size = sizeof(FieldT);
    info.annotation = annotation;

    // Set up accessors based on type
    if constexpr (std::is_same_v<FieldT, int>) {
      info.setInt = [memberPtr](void* obj, int val) { static_cast<StructT*>(obj)->*memberPtr = val; };
      info.getInt = [memberPtr](const void* obj) -> int { return static_cast<const StructT*>(obj)->*memberPtr; };
    } else if constexpr (std::is_same_v<FieldT, float>) {
      info.setFloat = [memberPtr](void* obj, float val) { static_cast<StructT*>(obj)->*memberPtr = val; };
      info.getFloat = [memberPtr](const void* obj) -> float { return static_cast<const StructT*>(obj)->*memberPtr; };
    } else if constexpr (std::is_same_v<FieldT, bool>) {
      info.setBool = [memberPtr](void* obj, bool val) { static_cast<StructT*>(obj)->*memberPtr = val; };
      info.getBool = [memberPtr](const void* obj) -> bool { return static_cast<const StructT*>(obj)->*memberPtr; };
    }

    StructInfo<StructT>::registerField(std::move(info));
  }
};

// ============================================================================
// ITERATION HELPERS
// ============================================================================

template <typename T, typename Func> void forEachField(Func&& func) {
  for (std::size_t i = 0; i < StructInfo<T>::fieldCount; ++i) {
    func(StructInfo<T>::fields[i]);
  }
}

template <typename T, typename Func> void forEachFieldInCategory(std::string_view category, Func&& func) {
  for (std::size_t i = 0; i < StructInfo<T>::fieldCount; ++i) {
    if (StructInfo<T>::fields[i].category == category) {
      func(StructInfo<T>::fields[i]);
    }
  }
}

// ============================================================================
// MACROS FOR STRUCT DEFINITION
// ============================================================================

// Start reflecting a struct
#define REFLECT_STRUCT_BEGIN(StructName)                                                                               \
  namespace reflect_##StructName##_impl {                                                                              \
    using ReflectedType = StructName;                                                                                  \
    inline void registerFields() {                                                                                     \
      ::cpp26::reflect::StructInfo<ReflectedType>::name = #StructName;

// Define a reflected field with annotation
// REFLECT_FIELD(type, name, default_value, annotation, category)
#define REFLECT_FIELD(type, name, defaultVal, annotation, category)                                                    \
  static ::cpp26::reflect::FieldRegistrar<ReflectedType, type> _reg_##name(#name, category, &ReflectedType::name,      \
                                                                           annotation);

// Simpler macro without category (uses empty string)
#define REFLECT_FIELD_SIMPLE(type, name, defaultVal, annotation) REFLECT_FIELD(type, name, defaultVal, annotation, "")

// End struct reflection
#define REFLECT_STRUCT_END()                                                                                           \
  }                                                                                                                    \
  inline struct Initializer {                                                                                          \
    Initializer() { registerFields(); }                                                                                \
  } _init;                                                                                                             \
  }

// ============================================================================
// FORCED REGISTRATION (call at module init)
// ============================================================================

#define REFLECT_INIT(StructName) (void)reflect_##StructName##_impl::_init;

} // namespace cpp26::reflect
