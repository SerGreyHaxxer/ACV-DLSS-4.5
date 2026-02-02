#pragma once
#include <dxgi1_4.h>
#include <d3d12.h>
#include <cstdint>

struct SwapChainHookState {
    IDXGISwapChain* pSwapChain = nullptr;
};

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

extern SwapChainHookState g_SwapChainState;
extern PFN_Present g_OriginalPresent;
extern PFN_Present1 g_OriginalPresent1;
extern PFN_ResizeBuffers g_OriginalResizeBuffers;

void HookFactoryIfNeeded(void* pFactory);
void InstallD3D12Hooks();
void InstallGetProcAddressHook();
void InstallLoadLibraryHook();
bool InitializeHooks();
void CleanupHooks();
void InitDescriptorHooks();
void NotifyWrappedCommandListUsed();
bool IsWrappedCommandListUsed();
void SetPatternJitterAddress(uintptr_t address);
bool TryGetPatternJitter(float& jitterX, float& jitterY);
bool HookVirtualMethod(void* pObject, int index, void* pHook, void** ppOriginal);
bool HookVirtualMethodOnce(void* pObject, int index, void* pHook, void** ppOriginal);
