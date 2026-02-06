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
using ScanResult = std::expected<T, ScanError>;

using ProxyResult = std::expected<void, ProxyError>;
