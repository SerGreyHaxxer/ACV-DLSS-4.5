// ============================================================================
// NVIDIA STREAMLINE DLSS SDK - STUB HEADERS
// ============================================================================

#pragma once
#include "sl.h"

namespace sl {

// DLSS Quality modes
enum class DLSSMode {
    eOff = 0,
    eMaxPerformance = 1,
    eBalanced = 2,
    eMaxQuality = 3,
    eUltraPerformance = 4,
    eUltraQuality = 5,
    eDLAA = 6
};

// DLSS Options
struct DLSSOptions {
    DLSSMode mode;
    unsigned int outputWidth;
    unsigned int outputHeight;
    float sharpness;
    bool useAutoExposure;
    bool colorBuffersHDR;
    float preExposure;
    unsigned int inputWidth;  // Added for explicit scaling support
    unsigned int inputHeight; // Added for explicit scaling support
};

// Get optimal settings for target resolution
inline void GetOptimalSettings(
    unsigned int targetWidth, 
    unsigned int targetHeight,
    DLSSMode mode,
    unsigned int* renderWidth,
    unsigned int* renderHeight,
    float* sharpness
) {
    float scale = 1.0f;
    switch (mode) {
        case DLSSMode::eMaxPerformance: scale = 0.5f; break;
        case DLSSMode::eBalanced: scale = 0.58f; break;
        case DLSSMode::eMaxQuality: scale = 0.67f; break;
        case DLSSMode::eUltraPerformance: scale = 0.33f; break;
        case DLSSMode::eUltraQuality: scale = 0.77f; break;
        case DLSSMode::eDLAA: scale = 1.0f; break;
        default: scale = 0.67f; break;
    }
    *renderWidth = (unsigned int)(targetWidth * scale);
    *renderHeight = (unsigned int)(targetHeight * scale);
    *sharpness = 0.0f;
}

} // namespace sl
