/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <MinHook.h>
#include <cstdint>
#include <expected>
#include <string_view>

// ============================================================================
// Typed Error Enums for std::expected Returns
// ============================================================================

enum class ProxyError {
  DXGILoadFailed,
  MissingFunctionPointers,
  HookInstallFailed,
};

constexpr std::string_view to_string(ProxyError e) {
  switch (e) {
  case ProxyError::DXGILoadFailed:          return "Failed to load original dxgi.dll";
  case ProxyError::MissingFunctionPointers: return "Missing critical DXGI function pointers";
  case ProxyError::HookInstallFailed:       return "D3D12 hook installation failed";
  }
  return "Unknown proxy error";
}

enum class ScanError {
  ModuleNotFound,
  ModuleInfoFailed,
  PatternNotFound,
  CacheInvalid,
};

constexpr std::string_view to_string(ScanError e) {
  switch (e) {
  case ScanError::ModuleNotFound:   return "Module handle not found";
  case ScanError::ModuleInfoFailed: return "Failed to get module info";
  case ScanError::PatternNotFound:  return "Pattern not found in memory";
  case ScanError::CacheInvalid:     return "Pattern cache invalid";
  }
  return "Unknown scan error";
}

// ============================================================================
// Convenience Aliases
// ============================================================================

using HookResult = std::expected<void, MH_STATUS>;

template<typename T>
using PatternScanResult = std::expected<T, ScanError>;

using ProxyResult = std::expected<void, ProxyError>;

