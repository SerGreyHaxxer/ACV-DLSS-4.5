#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// ============================================================================
// DLSS 4 PROXY CONFIGURATION — Modern C++23 Typed Constants
// ============================================================================

namespace dlss4 {

inline constexpr std::string_view kProxyVersion = "4.5";
inline constexpr std::string_view kLogFile      = "dlss4_proxy.log";

// DLSS-G Frame Generation (2x, 3x, or 4x — GPU/driver dependent)
inline constexpr int kDefaultFrameGenMultiplier = 4;

// Feature toggles
inline constexpr bool kEnableSuperResolution   = true;
inline constexpr bool kEnableRayReconstruction = true;
inline constexpr bool kEnableFrameGeneration   = true;

// Super Resolution Quality Mode (0=Perf, 1=Balanced, 2=Quality, 3=UltraQuality, 4=DLAA)
inline constexpr int kDefaultSRQualityMode = 2;

// Hooking
inline constexpr bool kHookDirectX12 = true;
inline constexpr bool kHookDirectX11 = false;

// Logging
inline constexpr bool kEnableLogging = true;
inline constexpr bool kLogVerbose    = true;

// NGX SDK
inline constexpr uint32_t kNgxAppId = 0; // Generic/Development AppID
inline constexpr const wchar_t* kNgxDlssDllName  = L"nvngx_dlss.dll";
inline constexpr const wchar_t* kNgxDlssgDllName = L"nvngx_dlssg.dll";

} // namespace dlss4

// ============================================================================
// Camera Scanning Heuristics
// ============================================================================

namespace camera_config {

inline constexpr size_t   kCbvMinSize            = sizeof(float) * 32;
inline constexpr float    kPosTolerance           = 100'000.0f;
inline constexpr uint32_t kScanMinIntervalFrames  = 2;
inline constexpr uint32_t kScanStaleFrames        = 120;
inline constexpr uint32_t kScanForceFullFrames    = 300;
inline constexpr uint32_t kScanMaxCbvsPerFrame    = 64;
inline constexpr uint32_t kDescriptorScanMax      = 32;
inline constexpr uint32_t kScanLogInterval        = 120;
inline constexpr uint32_t kScanExtendedMultiplier = 3;
inline constexpr uint32_t kScanFineStride         = 16;
inline constexpr uint32_t kScanMedStride          = 128;
inline constexpr uint32_t kGraceFrames            = 240;

} // namespace camera_config

// ============================================================================
// Resource Detection Heuristics
// ============================================================================

namespace resource_config {

inline constexpr uint32_t kCleanupInterval     = 900;
inline constexpr float    kExpectedMinRatio     = 0.35f;
inline constexpr float    kExpectedMaxRatio     = 1.6f;
inline constexpr float    kExpectedMatchBonus   = 0.2f;
inline constexpr float    kMsaaPenalty          = 0.2f;
inline constexpr float    kMipPenalty           = 0.1f;
inline constexpr uint32_t kBarrierScanMax       = 64;
inline constexpr uint32_t kStaleFrames          = 120;
inline constexpr uint32_t kRecencyFrames        = 60;
inline constexpr float    kRecencyBonus         = 0.25f;
inline constexpr float    kFrequencyBonus       = 0.2f;
inline constexpr uint32_t kFrequencyHitCap      = 30;

} // namespace resource_config

// ============================================================================
// DeepDVC & Streamline
// ============================================================================

namespace dvc_config {

inline constexpr uint32_t kLumaSampleIntervalMs = 250;
inline constexpr uint32_t kLumaSampleSize       = 64;

} // namespace dvc_config

namespace streamline_config {

inline constexpr uint32_t kInvalidParamFallbackFrames = 120;
inline constexpr uint32_t kInvalidParamDisableFrames  = 240;

} // namespace streamline_config
