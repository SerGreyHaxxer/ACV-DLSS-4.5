#pragma once
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <vector>

struct ImGui_ImplDX12_InitInfo;

class ImGuiOverlay {
public:
    static ImGuiOverlay& Get();
    void Initialize(IDXGISwapChain* swapChain);
    void Shutdown();
    void OnResize(UINT width, UINT height);
    void Render();
    void SetFPS(float gameFps, float totalFps);
    void SetCameraStatus(bool hasCamera, float jitterX, float jitterY);
    void ToggleVisibility();
    void ToggleFPS();
    void ToggleVignette();
    void ToggleDebugMode(bool enabled);
    void CaptureNextHotkey(int* target);
    void UpdateControls();
    bool IsVisible() const { return m_visible; }
private:
    ImGuiOverlay() = default;
    bool EnsureDeviceResources();
    void ReleaseDeviceResources();
    void RecreateRenderTargets();
    void ReleaseRenderTargets();
    void ApplyStyle();
    void BuildMainMenu();
    void BuildSetupWizard();
    void BuildFPSOverlay();
    void BuildVignette();
    void BuildDebugWindow();
    void BuildTexturesIfNeeded();
    void UpdateCursorState();
    void UpdateInputState();
    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
public:
    void AllocateSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu);
    void FreeSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);

private:

    bool m_initialized = false;
    bool m_visible = false;
    bool m_showFPS = false;
    bool m_showVignette = false;
    bool m_showDebug = false;
    int* m_pendingHotkeyTarget = nullptr;
    bool m_needRebuildTextures = true;
    bool m_cursorUnlocked = false;
    UINT m_width = 0;
    UINT m_height = 0;
    RECT m_prevClip = {};
    bool m_showSetupWizard = false;

    float m_cachedTotalFPS = 0.0f;
    float m_cachedJitterX = 0.0f;
    float m_cachedJitterY = 0.0f;
    bool m_cachedCamera = false;
    static constexpr int kFpsHistorySize = 120;
    float m_fpsHistory[kFpsHistorySize] = {};
    int m_fpsHistoryIndex = 0;

    HWND m_hwnd = nullptr;
    WNDPROC m_prevWndProc = nullptr;
    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    IDXGISwapChain3* m_swapChain = nullptr;
    ID3D12DescriptorHeap* m_srvHeap = nullptr;
    ID3D12DescriptorHeap* m_rtvHeap = nullptr;
    std::vector<ID3D12GraphicsCommandList*> m_commandLists;
    std::vector<ID3D12CommandAllocator*> m_commandAllocators;
    std::vector<UINT64> m_frameFenceValues;
    ID3D12Fence* m_fence = nullptr;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT m_backBufferCount = 0;
    ID3D12Resource** m_backBuffers = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandles[8] = {};

    ID3D12Resource* m_vignetteTexture = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE m_vignetteSrv = {};
    UINT m_srvDescriptorSize = 0;
    UINT m_srvDescriptorCount = 0;
    UINT m_srvDescriptorNext = 0;
};
