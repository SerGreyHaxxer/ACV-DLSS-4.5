// ============================================================================
// NVIDIA STREAMLINE DLSS-G (FRAME GENERATION) SDK - STUB HEADERS
// ============================================================================

#pragma once
#include "sl.h"

namespace sl {

enum class DLSSGMode {
    eOff = 0,
    eOn = 1
};

struct DLSSGOptions {
    DLSSGMode mode;
    unsigned int numFramesToGenerate;
    unsigned int numBackBuffers;
    unsigned int colorWidth;
    unsigned int colorHeight;
    unsigned int colorBufferFormat;
};

} // namespace sl
