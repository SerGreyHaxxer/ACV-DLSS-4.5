#pragma once

#include <windows.h>
#include <d3d12.h>
#include "dlss4_config.h"

// ============================================================================
// NVIDIA NGX / DLSS 4 WRAPPER
// ============================================================================
// This module provides the interface to NVIDIA's NGX SDK for DLSS 4.
// It handles initialization, feature creation, and frame generation.
//
// IMPORTANT: This is a WRAPPER/SKELETON. The actual NGX SDK requires:
//   1. nvngx_dlss.dll (Super Resolution)  
//   2. nvngx_dlssg.dll (Frame Generation)
//   3. Valid NGX SDK headers from NVIDIA Developer portal
// ============================================================================

// DLSS Quality Presets (matching NGX SDK)
enum class DLSS4_QualityMode {
    Performance = 0,     // Highest performance, lower quality
    Balanced = 1,        // Balance of performance and quality
    Quality = 2,         // Higher quality, moderate performance
    UltraQuality = 3,    // Best quality, lowest performance gain
    DLAA = 4,            // Native resolution anti-aliasing
    UltraPerformance = 5 // Maximum performance (DLSS 4 addition)
};

// Frame Generation modes (DLSS 4 Multi-Frame Generation)
enum class DLSS4_FrameGenMode {
    Off = 0,
    On_2x = 1,           // Generate 1 extra frame (2x total)
    On_3x = 2,           // Generate 2 extra frames (3x total)  
    On_4x = 3            // Generate 3 extra frames (4x total) - DLSS 4 max
};

// DLSS 4 State
struct DLSS4State {
    bool initialized = false;
    bool superResEnabled = false;
    bool rayReconEnabled = false;
    bool frameGenEnabled = false;
    
    DLSS4_QualityMode qualityMode = DLSS4_QualityMode::Quality;
    DLSS4_FrameGenMode frameGenMode = DLSS4_FrameGenMode::On_4x;
    
    // Resolution info
    UINT renderWidth = 0;   // Internal render resolution
    UINT renderHeight = 0;
    UINT displayWidth = 0;  // Output display resolution  
    UINT displayHeight = 0;
    
    // D3D12 resources (stored for NGX calls)
    ID3D12Device* pDevice = nullptr;
    ID3D12CommandQueue* pCommandQueue = nullptr;
    
    // NGX handles (opaque, would be real NGX types)
    void* hNGXContext = nullptr;
    void* hDLSSFeature = nullptr;
    void* hFrameGenFeature = nullptr;
    
    // Frame generation output
    void* pGeneratedFrameResource = nullptr;
    
    // Motion vectors and depth (game-specific, need to be hooked/captured)
    void* pMotionVectors = nullptr;
    void* pDepthBuffer = nullptr;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
};

extern DLSS4State g_DLSS4State;

// ============================================================================
// DLSS 4 API FUNCTIONS
// ============================================================================

// Initialize DLSS 4 with the game's D3D12 device
bool DLSS4_Initialize(ID3D12Device* pDevice, ID3D12CommandQueue* pCommandQueue,
    UINT displayWidth, UINT displayHeight);

// Shutdown and release resources
void DLSS4_Shutdown();

// Check if DLSS 4 is available on this system
bool DLSS4_IsAvailable();

// Set DLSS quality mode (affects internal render resolution)
void DLSS4_SetQualityMode(DLSS4_QualityMode mode);

// Enable/disable frame generation with multiplier
void DLSS4_SetFrameGeneration(DLSS4_FrameGenMode mode);

// Execute DLSS Super Resolution (call before Present)
void DLSS4_ExecuteSuperResolution();

// Execute Ray Reconstruction (if game uses RT)
void DLSS4_ExecuteRayReconstruction();

// Generate interpolated frame (call after Present for MFG)
void DLSS4_GenerateFrame(int frameIndex);

// Update motion vectors (must be called each frame by game hooks)
void DLSS4_SetMotionVectors(void* pMV, float jitterX, float jitterY);

// Update depth buffer 
void DLSS4_SetDepthBuffer(void* pDepth);
