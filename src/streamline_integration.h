#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <vector>
#include <windows.h>
#include <wrl/client.h> // Added for ComPtr
#include <chrono>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_dlss_d.h>
#include <sl_deepdvc.h>
#include "sl_reflex.h"

// ============================================================================
// STREAMLINE INTEGRATION WRAPPER
// ============================================================================
// This class manages the NVIDIA Streamline SDK lifecycle.
// It handles initialization, resource tagging, and feature evaluation.
// ============================================================================

class StreamlineIntegration {
public:
    static StreamlineIntegration& Get();

    // Lifecycle
    bool Initialize(ID3D12Device* pDevice);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }
    
    // Per-Frame Updates
    void NewFrame(IDXGISwapChain* pSwapChain);
    void SetCommandQueue(ID3D12CommandQueue* pQueue);
    ID3D12CommandQueue* GetCommandQueue() const { return m_pCommandQueue.Get(); }
    
    // Resource Tagging (Call before Evaluate)
    void TagColorBuffer(ID3D12Resource* pResource);
    void TagDepthBuffer(ID3D12Resource* pResource);
    void TagMotionVectors(ID3D12Resource* pResource);
    
    // Camera Constants (Required for DLSS)
    void SetCameraData(const float* viewMatrix, const float* projMatrix, float jitterX, float jitterY);

    // Feature Execution
    void EvaluateDLSS(ID3D12GraphicsCommandList* pCmdList);
    void EvaluateFrameGen(IDXGISwapChain* pSwapChain);
    void EvaluateDeepDVC(IDXGISwapChain* pSwapChain);

    // Configuration
    void SetDLSSMode(int mode); // 0=Off, 1=Perf, 2=Bal, 3=Qual...
    void SetDLSSPreset(int preset); // 0=Default, 1=A...
    void SetFrameGenMultiplier(int multiplier); // 0=Off, 2, 3, 4
    int GetFrameGenMultiplier() const { return m_frameGenMultiplier; }
    void SetSharpness(float sharpness); // 0.0 to 1.0
    void SetLODBias(float bias); // 0.0 to -3.0
    void SetMVecScale(float x, float y);
    void SetReflexEnabled(bool enabled);
    void SetHUDFixEnabled(bool enabled);
    void SetRayReconstructionEnabled(bool enabled);
    bool IsRayReconstructionSupported() const { return m_rayReconstructionSupported; }
    bool IsRayReconstructionEnabled() const { return m_rayReconstructionEnabled; }
    void SetRRPreset(int preset);
    int GetRRPresetIndex() const { return m_rrPresetIndex; }
    void SetRRDenoiserStrength(float strength);
    float GetRRDenoiserStrength() const { return m_rrDenoiserStrength; }
    bool IsDLSSRRActive() const { return m_rrLoaded && m_rayReconstructionEnabled; }
    void UpdateFrameTiming(float baseFps);
    void SetDeepDVCEnabled(bool enabled);
    void SetDeepDVCIntensity(float intensity);
    void SetDeepDVCSaturation(float saturation);
    void SetDeepDVCAdaptiveEnabled(bool enabled);
    void SetDeepDVCAdaptiveStrength(float strength);
    void SetDeepDVCAdaptiveMin(float minValue);
    void SetDeepDVCAdaptiveMax(float maxValue);
    void SetDeepDVCAdaptiveSmoothing(float smoothing);
    bool IsDeepDVCSupported() const { return m_deepDvcSupported; }
    bool IsDeepDVCEnabled() const { return m_deepDvcEnabled; }
    float GetDeepDVCIntensity() const { return m_deepDvcIntensity; }
    float GetDeepDVCSaturation() const { return m_deepDvcSaturation; }
    bool IsDeepDVCAdaptiveEnabled() const { return m_deepDvcAdaptiveEnabled; }
    float GetDeepDVCAdaptiveStrength() const { return m_deepDvcAdaptiveStrength; }
    float GetDeepDVCAdaptiveMin() const { return m_deepDvcAdaptiveMin; }
    float GetDeepDVCAdaptiveMax() const { return m_deepDvcAdaptiveMax; }
    float GetDeepDVCAdaptiveSmoothing() const { return m_deepDvcAdaptiveSmoothing; }
    void SetSmartFGEnabled(bool enabled);
    void SetSmartFGAutoDisable(bool enabled);
    void SetSmartFGAutoDisableThreshold(float fps);
    void SetSmartFGSceneChangeEnabled(bool enabled);
    void SetSmartFGSceneChangeThreshold(float threshold);
    void SetSmartFGInterpolationQuality(float quality);
    bool IsSmartFGEnabled() const { return m_smartFgEnabled; }
    bool IsSmartFGAutoDisableEnabled() const { return m_smartFgAutoDisable; }
    float GetSmartFGAutoDisableThreshold() const { return m_smartFgAutoDisableFps; }
    bool IsSmartFGSceneChangeEnabled() const { return m_smartFgSceneChangeEnabled; }
    float GetSmartFGSceneChangeThreshold() const { return m_smartFgSceneChangeThreshold; }
    float GetSmartFGInterpolationQuality() const { return m_smartFgInterpolationQuality; }
    bool IsSmartFGTemporarilyDisabled() const { return m_smartFgForceDisable; }
    bool IsFrameGenDisabledDueToInvalidParam() const { return m_disableFGDueToInvalidParam; }
    sl::DLSSGStatus GetFrameGenStatus() const { return m_dlssgStatus; }

    // DLSS mode mapping (UI index to SDK enum)
    void SetDLSSModeIndex(int modeIndex);
    int GetDLSSModeIndex() const;
    int GetDLSSPresetIndex() const { return static_cast<int>(m_dlssPreset); }

    // Runtime Toggles (For Hotkeys)
    void CycleDLSSMode();
    void CycleDLSSPreset();
    void CycleFrameGen();
    void CycleLODBias(); // Cycle between 0, -1, -2 (Sharpening)
    
    float GetLODBias() const { return m_lodBias; }

    // Resource Management
    void ReleaseResources();
    void NotifySwapChainResize(UINT width, UINT height);
    bool HasCameraData() const { return m_hasCameraData; }
    void GetLastCameraJitter(float& x, float& y) const { x = m_lastJitterX; y = m_lastJitterY; }
    float GetLastCameraDelta() const { return m_lastCameraDelta; }
    UINT GetDescriptorSize() const { return m_cbvSrvUavDescriptorSize; }
    float GetFgActualMultiplier() const { return m_fgActualMultiplier; }
    float GetLastBaseFps() const { return m_lastBaseFps; }
    
    // Frame Generation Debug (DLSS-G)
    void PrintDLSSGStatus();

    // Feature Support Getters
    bool IsDLSSSupported() const { return m_dlssSupported; }
    bool IsFrameGenSupported() const { return m_dlssgSupported; }
    bool IsReflexSupported() const { return m_reflexSupported; }
    bool IsDeepDVCLoaded() const { return m_deepDvcLoaded; }
    
    // Frame Tracking
    uint32_t GetFrameCount() const { return m_frameIndex; }

private:
    StreamlineIntegration() = default;
    ~StreamlineIntegration() = default;

    bool m_initialized = false;
    Microsoft::WRL::ComPtr<ID3D12Device> m_pDevice;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_pCommandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_pCommandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_pCommandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_pFence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_pSwapChain;
    
    // Feature availability tracking
    bool m_dlssSupported = false;
    bool m_dlssgSupported = false;
    bool m_rayReconstructionSupported = false;
    bool m_reflexSupported = false;
    bool m_deepDvcSupported = false;
    
    // Current viewport
    unsigned int m_viewportWidth = 0;
    unsigned int m_viewportHeight = 0;
    DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t m_swapChainBufferCount = 0;
    uint32_t m_dlssgMaxFramesToGenerate = 1;
    uint32_t m_dlssgMinWidthOrHeight = 0;
    sl::DLSSGStatus m_dlssgStatus = sl::DLSSGStatus::eOk;

    // Streamline runtime state
    sl::ViewportHandle m_viewport = sl::ViewportHandle(0);
    sl::FrameToken* m_frameToken = nullptr;
    uint32_t m_frameIndex = 0;
    uint32_t m_lastDlssEvalFrame = 0;
    bool m_loggedDlssEval = false;
    bool m_loggedFrameGenEval = false;
    bool m_optionsDirty = true;
    bool m_needNewFrameToken = true;

    // Cached resources
    Microsoft::WRL::ComPtr<ID3D12Resource> m_colorBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motionVectors;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lastTaggedColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lastTaggedDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lastTaggedMvec;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lastTaggedBackBuffer;
    D3D12_RESOURCE_DESC m_lastColorDesc = {};
    D3D12_RESOURCE_DESC m_lastDepthDesc = {};
    D3D12_RESOURCE_DESC m_lastMvecDesc = {};
    D3D12_RESOURCE_DESC m_lastBackBufferDesc = {};
    bool m_descInitialized = false;

    // User configuration
    sl::DLSSMode m_dlssMode = sl::DLSSMode::eDLAA; // Default Highest Quality
    sl::DLSSPreset m_dlssPreset = sl::DLSSPreset::eDefault;
    sl::DLSSDPreset m_rrPreset = sl::DLSSDPreset::eDefault;
    int m_rrPresetIndex = 0;
    float m_rrDenoiserStrength = 0.5f;
    int m_frameGenMultiplier = 4; // Default 4x
    bool m_dlssEnabled = true;
    bool m_rayReconstructionEnabled = true;
    bool m_reflexEnabled = true;
    bool m_hudFixEnabled = false;
    bool m_deepDvcEnabled = false;
    float m_deepDvcIntensity = 0.5f;
    float m_deepDvcSaturation = 0.25f;
    bool m_deepDvcAdaptiveEnabled = false;
    float m_deepDvcAdaptiveStrength = 0.6f;
    float m_deepDvcAdaptiveMin = 0.2f;
    float m_deepDvcAdaptiveMax = 0.9f;
    float m_deepDvcAdaptiveSmoothing = 0.15f;
    float m_deepDvcAdaptiveIntensity = 0.5f;
    float m_deepDvcAdaptiveSaturation = 0.25f;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lumaReadback;
    DXGI_FORMAT m_lumaReadbackFormat = DXGI_FORMAT_UNKNOWN;
    UINT m_lumaRowPitch = 0;
    uint64_t m_deepDvcLastSampleMs = 0;
    bool m_hasCameraData = false;
    float m_lastJitterX = 0.0f;
    float m_lastJitterY = 0.0f;
    float m_cachedView[16] = {};
    float m_cachedProj[16] = {};
    
    float m_sharpness = 0.5f; // Default sharpness
    float m_lodBias = -1.0f;  // Default Sharper Textures
    float m_mvecScaleX = 1.0f;
    float m_mvecScaleY = 1.0f;

    sl::Feature m_featuresToLoad[6] = {};
    uint32_t m_featureCount = 0;
    UINT m_cbvSrvUavDescriptorSize = 0;
    bool m_dlssgLoaded = false;
    bool m_reflexLoaded = false;
    bool m_rrLoaded = false;
    bool m_deepDvcLoaded = false;
    bool m_needFeatureReload = false;
    bool m_forceTagging = true;
    uint32_t m_dlssgInvalidParamFrames = 0;
    bool m_smartFgEnabled = false;
    bool m_smartFgAutoDisable = true;
    float m_smartFgAutoDisableFps = 120.0f;
    bool m_smartFgSceneChangeEnabled = true;
    float m_smartFgSceneChangeThreshold = 0.25f;
    float m_smartFgInterpolationQuality = 0.5f;
    float m_lastBaseFps = 0.0f;
    float m_fgActualMultiplier = 1.0f;
    bool m_smartFgForceDisable = false;
    int m_smartFgLastMultiplier = 0;
    std::chrono::steady_clock::time_point m_sceneChangeCooldownUntil{};
    bool m_sceneChangeCooldownActive = false;
    int m_sceneResetFrames = 0;
    float m_prevView[16] = {};
    float m_prevProj[16] = {};
    bool m_hasPrevMatrices = false;
    float m_lastCameraDelta = 0.0f;
    uint32_t m_rrInvalidParamFrames = 0;
    bool m_disableRRDueToInvalidParam = false;
    bool m_disableFGDueToInvalidParam = false;

    void UpdateSwapChain(IDXGISwapChain* pSwapChain);
    void UpdateOptions();
    void TagResources();
    void EnsureCommandList();
    bool EnsureFrameToken();
    void UpdateSmartFGState();
    void UpdateDeepDVCOptions();
    void UpdateDeepDVCAdaptive();
    float EstimateSceneLuma(ID3D12Resource* resource);
    
    // DLSS-G Debug tracking
    struct DLSSGStats {
        uint32_t baseFrames = 0;
        uint32_t generatedFrames = 0;
        uint32_t lastFrameIndex = 0;
        float peakMultiplier = 0.0f;
        uint64_t startTime = 0;
    } m_dlssgStats;
};
