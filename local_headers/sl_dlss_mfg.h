// ============================================================================
// NVIDIA STREAMLINE DLSS 4 MULTI-FRAME GENERATION SDK - STUB HEADERS
// ============================================================================

#pragma once
#include "sl.h"

namespace sl {

// DLSS 4 Multi-Frame Generation Modes
enum class DLSSMFGMode {
    eOff = 0,
    e2x = 2,    // Generate 1 extra frame (2x total)
    e3x = 3,    // Generate 2 extra frames (3x total)  
    e4x = 4     // Generate 3 extra frames (4x total) - RTX 50 series only
};

// DLSS 4 MFG Options
struct DLSSMFGOptions {
    DLSSMFGMode mode;
    bool enableAsyncCompute;
    bool dynamicFramePacing;
    float targetFrameTime;      // Target frame time in ms (for pacing)
    unsigned int numBackBuffers;
    bool enableOFA;             // Optical Flow Accelerator (Blackwell)
};

// MFG Status
struct DLSSMFGStatus {
    bool available;
    bool active;
    DLSSMFGMode currentMode;
    unsigned int generatedFrames;
    float averageLatency;
    float interpolationQuality;
};

// Check if MFG is supported (requires RTX 40+ for 2x/3x, RTX 50+ for 4x)
inline bool IsMFGSupported(DLSSMFGMode mode) {
    // In real implementation, this checks GPU architecture
    // RTX 40 series: Ada Lovelace - supports up to 3x
    // RTX 50 series: Blackwell - supports 4x with OFA 2.0
    return true; // Assume RTX 5080
}

// Get maximum supported MFG multiplier for current GPU
inline DLSSMFGMode GetMaxMFGMode() {
    // For RTX 5080 (Blackwell), 4x is supported
    return DLSSMFGMode::e4x;
}

} // namespace sl
