#pragma once
#include "sl.h"

namespace sl {

// Reflex Feature ID (Typically defined in SDK, reverse engineered or standard)
// kFeatureDLSS is 0, DLSS_G is 1... Reflex is usually a specific ID.
// Based on SDK v2.4+:
constexpr Feature kFeatureReflex = static_cast<Feature>(100); // Placeholder, need verification or dynamic query

// Enums
enum class ReflexMode : uint32_t {
    eOff = 0,
    eLowLatency = 1,
    eLowLatencyWithBoost = 2,
};

// Options Struct
struct ReflexOptions : public BaseStructure {
    ReflexMode mode = ReflexMode::eOff;
    bool useMarkersToOptimize = false;
    uint32_t virtualKey = 0;
    uint32_t idThread = 0;
};

} // namespace sl
