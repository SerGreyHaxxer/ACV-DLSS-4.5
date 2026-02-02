#pragma once

// ============================================================================
// DLSS 4 PROXY CONFIGURATION
// ============================================================================
// Adjust these parameters to tune the DLSS 4 behavior.
// Frame Generation multiplier: 2x, 3x, or 4x (DLSS-G where supported)
// ============================================================================

#define DLSS4_PROXY_VERSION "4.5"
#define DLSS4_LOG_FILE "dlss4_proxy.log"

// -------------------------------
// DLSS-G Frame Generation
// -------------------------------
// DLSS-G can generate up to 3 additional frames per rendered frame (4x total, GPU/driver dependent).
// Set to desired multiplier: 2, 3, or 4.
#define DLSS4_FRAME_GEN_MULTIPLIER 4

// Enable/Disable specific DLSS features
#define DLSS4_ENABLE_SUPER_RESOLUTION 1
#define DLSS4_ENABLE_RAY_RECONSTRUCTION 1
#define DLSS4_ENABLE_FRAME_GENERATION 1

// Super Resolution Quality Mode
// 0 = Performance, 1 = Balanced, 2 = Quality, 3 = Ultra Quality, 4 = DLAA
#define DLSS4_SR_QUALITY_MODE 2

// -------------------------------
// Hooking Configuration
// -------------------------------
#define HOOK_DIRECTX12 1       // AC Valhalla uses DX12
#define HOOK_DIRECTX11 0       // Fallback if needed

// -------------------------------
// Logging
// -------------------------------
#define ENABLE_LOGGING 1
#define LOG_VERBOSE 1          // Set to 1 for detailed frame-by-frame logging

// -------------------------------
// Motion Vector / Depth Buffer Offsets (GAME SPECIFIC)
// -------------------------------
// NOTE: These hardcoded offsets are unused in this version.
// The mod now uses Dynamic Pattern Scanning and Resource Sniffing
// to automatically detect Jitter, Color, Depth, and Motion Vectors at runtime.
// 
#define MOTION_VECTOR_OFFSET 0x0        // Unused (Dynamic Sniffer)
#define DEPTH_BUFFER_OFFSET  0x0        // Unused (Dynamic Sniffer)
#define JITTER_OFFSET        0x0        // Unused (Dynamic Pattern Scan)

// -------------------------------
// NGX SDK Paths
// -------------------------------
// The proxy will look for these DLLs in the game directory first,
// then fall back to the system path.
#define NGX_DLSS_DLL_NAME    L"nvngx_dlss.dll"
#define NGX_DLSSG_DLL_NAME   L"nvngx_dlssg.dll"  // Frame Generation module

// Application ID for NGX (Spoofing Generic/Dev ID to force DLSS enablement)
#define NGX_APP_ID 0        // Generic/Development AppID

// -------------------------------
// Resource Detector & Camera Heuristics
// -------------------------------
#define RESOURCE_CLEANUP_INTERVAL 900 // Frames between clearing resource cache
#define CAMERA_CBV_MIN_SIZE (sizeof(float) * 32) // Minimum size for camera constant buffer
#define CAMERA_POS_TOLERANCE 100000.0f // Max reasonable value for camera position
#define CAMERA_SCAN_MIN_INTERVAL_FRAMES 2
#define CAMERA_SCAN_STALE_FRAMES 120
#define CAMERA_SCAN_FORCE_FULL_FRAMES 300
#define CAMERA_SCAN_MAX_CBVS_PER_FRAME 64
#define CAMERA_SCAN_LOG_INTERVAL 600
#define RESOURCE_EXPECTED_MIN_RATIO 0.35f
#define RESOURCE_EXPECTED_MAX_RATIO 1.6f
#define RESOURCE_EXPECTED_MATCH_BONUS 0.2f
#define RESOURCE_MSAA_PENALTY 0.2f
#define RESOURCE_MIP_PENALTY 0.1f
#define RESOURCE_BARRIER_SCAN_MAX 16
#define MFG_INVALID_PARAM_FALLBACK_FRAMES 120
#define MFG_INVALID_PARAM_DISABLE_FRAMES 240
