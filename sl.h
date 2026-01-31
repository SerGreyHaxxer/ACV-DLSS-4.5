// ============================================================================
// NVIDIA STREAMLINE SDK - COMPATIBILITY HEADER
// ============================================================================
// This header mimics the Streamline SDK API to allow compilation.
// Users should replace this with the actual 'sl.h' from the NVIDIA SDK.
// ============================================================================

#pragma once

#include <cstdint>

namespace sl {

// Result codes
enum class Result {
    eOk = 0,
    eError = 1,
    eNotSupported = 2,
    eNotInitialized = 3
};

// Log levels
enum class LogLevel {
    eOff = 0,
    eDefault = 1,
    eVerbose = 2,
    eInfo = 3
};

// Engine Type
enum class EngineType {
    eCustom = 0,
    eUnreal = 1,
    eUnity = 2
};

// Render API
enum class RenderAPI {
    eD3D11 = 0,
    eD3D12 = 1,
    eVulkan = 2
};

using Feature = uint32_t;

// SDK version (stub)
constexpr uint32_t kSDKVersion = 1;

// Feature IDs
constexpr Feature kSDKDLSS = 0;
constexpr Feature kFeatureDLSS = 1;
constexpr Feature kFeatureDLSS_G = 2;      // Frame Generation (DLSS-G)
constexpr Feature kFeatureDLSS_MFG = 3;    // Multi-Frame Generation (DLSS 4)
constexpr Feature kFeatureDLSS_RR = 4;     // Ray Reconstruction

// Buffer types for tagging
enum BufferType {
    kBufferTypeColor = 0,
    kBufferTypeDepth = 1,
    kBufferTypeMotionVectors = 2,
    kBufferTypeHUDLessColor = 3,
    kBufferTypeExposure = 4,
    kBufferTypeOutput = 5,
    kBufferTypeScalingInputColor = 6,
    kBufferTypeScalingOutputColor = 7
};

// Base structure for inputs
struct BaseStructure {
    uint64_t next;
};

// Preference flags
enum PreferenceFlags : uint32_t {
    eNone = 0,
    eUseManualHooking = 1u << 0,
    eUseFrameBasedResourceTagging = 1u << 1
};

// Preferences for initialization
struct Preferences {
    bool showConsole;
    LogLevel logLevel;
    int numPathsToPlugins;
    const char** pathsToPlugins;
    EngineType engine;
    RenderAPI renderAPI;
    uint32_t flags;
    uint64_t applicationId;
    const wchar_t* engineVersion;
    const wchar_t* projectId;
    const Feature* featuresToLoad;
    uint32_t numFeaturesToLoad;
};

// Viewport handle
struct ViewportHandle : public BaseStructure {
    unsigned int id;
    ViewportHandle(unsigned int value = 0) : id(value) {}
};

// Frame token
struct FrameToken {};

// Feature requirement flags
enum class FeatureRequirementFlags : uint32_t {
    eRequirementNone = 0,
    eD3D12Supported = 1u << 0
};

// Resource wrapper
enum class ResourceType {
    eTex2d = 0
};

enum class ResourceLifecycle {
    eValidUntilPresent = 0
};

struct Extent {
    uint32_t left;
    uint32_t top;
    uint32_t width;
    uint32_t height;
};

struct Resource {
    ResourceType type;
    void* native;          // ID3D12Resource*
    uint32_t state;        // D3D12_RESOURCE_STATES
    void* view;            // Optional view
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    uint32_t arraySize;
    uint32_t nativeFormat; // DXGI_FORMAT

    Resource(ResourceType resourceType, void* nativeResource, uint32_t resourceState)
        : type(resourceType),
          native(nativeResource),
          state(resourceState),
          view(nullptr),
          width(0),
          height(0),
          mipLevels(1),
          arraySize(1),
          nativeFormat(0) {}
};

// Resource tag for marking buffers
struct ResourceTag {
    BufferType type;
    Resource* resource;
    ResourceLifecycle lifecycle;
    const Extent* extent;

    ResourceTag(Resource* res, BufferType bufferType, ResourceLifecycle life, const Extent* ext)
        : type(bufferType), resource(res), lifecycle(life), extent(ext) {}
};

// Feature constants (for GetFeatureSupported)
struct FeatureConstants {
    bool supported;
    unsigned int flags;
    unsigned int minDriverVersion;
    unsigned int maxFrameGeneration;  // For MFG: 2, 3, or 4
};

struct FeatureRequirements {
    uint32_t flags;
};

enum class Boolean : uint32_t {
    eFalse = 0,
    eTrue = 1
};

struct float2 {
    float x;
    float y;
    float2(float a = 0.0f, float b = 0.0f) : x(a), y(b) {}
};

// Camera/Scene Constants
struct Constants {
    float cameraViewToClip[16];
    float cameraClipToView[16];
    float cameraViewToWorld[16];
    float cameraWorldToView[16];
    float2 jitterOffset;
    float2 mvecScale;
    Boolean depthInverted;
    Boolean cameraMotionIncluded;
    Boolean motionVectors3D;
    Boolean reset;
    float motionVectorScale[2];
};

// ============================================================================
// FUNCTION DECLARATIONS (STUBS)
// ============================================================================

// These would normally be imported from sl.interposer.lib

inline Result slInit(const Preferences& pref, uint32_t sdkVersion) { return Result::eOk; }
inline Result slShutdown() { return Result::eOk; }
inline Result slSetD3DDevice(void* device) { return Result::eOk; }

inline Result slGetFeatureRequirements(Feature feature, FeatureRequirements& outReq) {
    outReq.flags = static_cast<uint32_t>(FeatureRequirementFlags::eD3D12Supported);
    return Result::eOk;
}

inline Result slGetFeatureConstants(Feature feature, FeatureConstants* consts) {
    if (consts) consts->supported = true;
    return Result::eOk;
}

inline Result slGetNewFrameToken(FrameToken*& token, uint32_t* frameIndex) {
    static FrameToken dummy;
    token = &dummy;
    if (frameIndex) {
        static uint32_t idx = 0;
        *frameIndex = ++idx;
    }
    return Result::eOk;
}

inline Result slSetTagForFrame(FrameToken& token, ViewportHandle viewport, const ResourceTag* tags, uint32_t numTags, const void* outInfo) {
    return Result::eOk;
}

inline Result slEvaluateFeature(Feature feature, FrameToken& token, const BaseStructure* const* inputs, uint32_t numInputs, void* cmdList) {
    return Result::eOk;
}

inline Result slSetConstants(const Constants& consts, FrameToken& token, ViewportHandle viewport) { return Result::eOk; }

inline Result slSetFeatureOptions(Feature feature, const void* options) { return Result::eOk; }

} // namespace sl
