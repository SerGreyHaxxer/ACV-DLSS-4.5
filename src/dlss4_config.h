#pragma once

// ============================================================================
// DLSS 4 PROXY CONFIGURATION
// ============================================================================
// Adjust these parameters to tune the DLSS 4 behavior.
// Frame Generation multiplier: 2x, 3x, or 4x (DLSS 4 Multi-Frame Gen max)
// ============================================================================

#define DLSS4_PROXY_VERSION "4.5"
#define DLSS4_LOG_FILE "dlss4_proxy.log"

// -------------------------------
// DLSS 4 Multi Frame Generation
// -------------------------------
// DLSS 4 can generate up to 3 additional frames per rendered frame (4x total).
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
#define LOG_VERBOSE 0          // Set to 1 for detailed frame-by-frame logging

// -------------------------------
// Motion Vector / Depth Buffer Offsets (GAME SPECIFIC)
// -------------------------------
// These are PLACEHOLDER values. You must use a tool like Cheat Engine,
// x64dbg, or pattern scanning to find the correct offsets for your
// version of Assassin's Creed Valhalla.
// 
// Pattern hint (example, may vary):
// Motion Vectors: Search for float array near render target creation
// Depth Buffer:   Search for DXGI_FORMAT_D32_FLOAT resource
// 
#define MOTION_VECTOR_OFFSET 0x0        // PLACEHOLDER
#define DEPTH_BUFFER_OFFSET  0x0        // PLACEHOLDER
#define JITTER_OFFSET        0x0        // PLACEHOLDER for TAA jitter values

// -------------------------------
// NGX SDK Paths
// -------------------------------
// The proxy will look for these DLLs in the game directory first,
// then fall back to the system path.
#define NGX_DLSS_DLL_NAME    L"nvngx_dlss.dll"
#define NGX_DLSSG_DLL_NAME   L"nvngx_dlssg.dll"  // Frame Generation module

// Application ID for NGX (use game's Steam AppID or a custom one)
#define NGX_APP_ID 2208920  // AC Valhalla Steam AppID
