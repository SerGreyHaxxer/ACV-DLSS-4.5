// ============================================================================
// NVIDIA STREAMLINE DLSS-RR (RAY RECONSTRUCTION) SDK - STUB HEADERS
// ============================================================================

#pragma once
#include "sl.h"

namespace sl {

struct DLSSDOptions {
    DLSSMode mode;
    unsigned int outputWidth;
    unsigned int outputHeight;
    bool colorBuffersHDR;
};

} // namespace sl
