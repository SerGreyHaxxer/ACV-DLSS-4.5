#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgiformat.h>

// Forward declarations for COM interfaces
struct IDXGISwapChain;
struct IDXGISwapChain1;
struct IDXGISwapChain4;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct DXGI_PRESENT_PARAMETERS;

// ============================================================================
// DIRECTX HOOKS
// ============================================================================
// Hooks for intercepting DirectX calls needed for DLSS integration.
// The main targets are:
//   - IDXGISwapChain::Present (to inject frame generation)
//   - ID3D12CommandQueue::ExecuteCommandLists (for render timing)
// ============================================================================

// Hook initialization (called after factory is created)
void HookFactoryIfNeeded(void* pFactory);

// SwapChain Present hook state
struct SwapChainHookState {
    IDXGISwapChain4* pSwapChain = nullptr;
    ID3D12Device* pDevice = nullptr;
    ID3D12CommandQueue* pCommandQueue = nullptr;
    HWND hWnd = nullptr;
    UINT width = 0;
    UINT height = 0;
    bool hooked = false;
};

extern SwapChainHookState g_SwapChainState;

// Command Queue hook types
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue* pThis, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);
extern PFN_ExecuteCommandLists g_OriginalExecuteCommandLists;
void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);

// D3D12CreateDevice Hook
typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
extern PFN_D3D12CreateDevice g_OriginalD3D12CreateDevice;
HRESULT WINAPI Hooked_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice);

// VMT hook helpers
bool HookVirtualMethod(void* pObject, int index, void* pHook, void** ppOriginal);

// Original function pointers (stored after hooking)
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1* pThis, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

extern PFN_Present g_OriginalPresent;
extern PFN_Present1 g_OriginalPresent1;
extern PFN_ResizeBuffers g_OriginalResizeBuffers;

// Our hook implementations
HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);
HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* pThis, UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

// Initialize/cleanup hooks
bool InitializeHooks();
void CleanupHooks();
