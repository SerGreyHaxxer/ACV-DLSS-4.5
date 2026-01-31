#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <vector>
#include <windows.h>
#include <wrl/client.h> // Added for ComPtr

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_dlss_d.h>
#include <sl_dlss_mfg.h>
#include "../sl_reflex.h" // Relative path since it's in root

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
    
    // Per-Frame Updates
    void NewFrame(IDXGISwapChain* pSwapChain);
    void SetCommandQueue(ID3D12CommandQueue* pQueue);
    
    // Resource Tagging (Call before Evaluate)
    void TagColorBuffer(ID3D12Resource* pResource);
    void TagDepthBuffer(ID3D12Resource* pResource);
    void TagMotionVectors(ID3D12Resource* pResource);
    
    // Camera Constants (Required for DLSS)
    void SetCameraData(const float* viewMatrix, const float* projMatrix, float jitterX, float jitterY);

    // Feature Execution
    void EvaluateDLSS(ID3D12GraphicsCommandList* pCmdList);
    void EvaluateFrameGen(IDXGISwapChain* pSwapChain);

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
    bool HasCameraData() const { return m_hasCameraData; }
    void GetLastCameraJitter(float& x, float& y) const { x = m_lastJitterX; y = m_lastJitterY; }
    UINT GetDescriptorSize() const { return m_cbvSrvUavDescriptorSize; }

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
    bool m_frameGenSupported = false;
    bool m_dlssgSupported = false;
    bool m_mfgSupported = false;
    bool m_rayReconstructionSupported = false;
    
    // Current viewport
    unsigned int m_viewportWidth = 0;
    unsigned int m_viewportHeight = 0;
    DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_UNKNOWN;

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
    ID3D12Resource* m_colorBuffer = nullptr;
    ID3D12Resource* m_depthBuffer = nullptr;
    ID3D12Resource* m_motionVectors = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backBuffer;

    // User configuration
    sl::DLSSMode m_dlssMode = sl::DLSSMode::eDLAA; // Default Highest Quality
    sl::DLSSPreset m_dlssPreset = sl::DLSSPreset::eDefault;
    bool m_useMfg = false;
    int m_frameGenMultiplier = 4; // Default 4x
    bool m_dlssEnabled = true;
    bool m_rayReconstructionEnabled = true;
    bool m_reflexEnabled = true;
    bool m_hudFixEnabled = false;
    bool m_hasCameraData = false;
    float m_lastJitterX = 0.0f;
    float m_lastJitterY = 0.0f;
    
    float m_sharpness = 0.5f; // Default sharpness
    float m_lodBias = -1.0f;  // Default Sharper Textures
    float m_mvecScaleX = 1.0f;
    float m_mvecScaleY = 1.0f;

    sl::Feature m_featuresToLoad[5] = {};
    uint32_t m_featureCount = 0;
    UINT m_cbvSrvUavDescriptorSize = 0;

    void UpdateSwapChain(IDXGISwapChain* pSwapChain);
    void UpdateOptions();
    void TagResources();
    void EnsureCommandList();
    void WaitForGpu();
    bool EnsureFrameToken();
};
