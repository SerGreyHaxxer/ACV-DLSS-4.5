#pragma once
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>
#include <wrl/client.h>

#include "error_types.h"


struct SwapChainHookState {
  Microsoft::WRL::ComPtr<IDXGISwapChain> pSwapChain;
};

// Function pointer types for original functions
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present1)(
    IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_ResizeBuffers)(IDXGISwapChain *, UINT,
                                                      UINT, UINT, DXGI_FORMAT,
                                                      UINT);
typedef HRESULT(WINAPI *PFN_D3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL,
                                               REFIID, void **);
typedef void(STDMETHODCALLTYPE *PFN_ExecuteCommandLists)(
    ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCommandQueue)(
    ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCommittedResource)(
    ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE *, REFIID, void **);

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateDescriptorHeap)(
    ID3D12Device *, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **);
typedef void(STDMETHODCALLTYPE *PFN_CreateShaderResourceView)(
    ID3D12Device *, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *,
    D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CreateUnorderedAccessView)(
    ID3D12Device *, ID3D12Resource *, ID3D12Resource *,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CreateRenderTargetView)(
    ID3D12Device *, ID3D12Resource *, const D3D12_RENDER_TARGET_VIEW_DESC *,
    D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CreateDepthStencilView)(
    ID3D12Device *, ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *,
    D3D12_CPU_DESCRIPTOR_HANDLE);

// RAII Hook Manager using MinHook
class HookManager {
public:
  static HookManager &Get();

  // Non-copyable, non-movable singleton
  HookManager(const HookManager&) = delete;
  HookManager& operator=(const HookManager&) = delete;
  HookManager(HookManager&&) = delete;
  HookManager& operator=(HookManager&&) = delete;

  bool Initialize();
  void Shutdown();

  template <typename T>
  HookResult CreateHook(void *target, void *detour, T **original) {
    return CreateHookInternal(target, detour,
                              reinterpret_cast<void **>(original));
  }

private:
  HookManager() = default;
  ~HookManager() = default;
  HookResult CreateHookInternal(void *target, void *detour, void **original);
  bool m_initialized = false;
};

extern SwapChainHookState g_SwapChainState;
extern PFN_Present g_OriginalPresent;
extern PFN_Present1 g_OriginalPresent1;
extern PFN_ResizeBuffers g_OriginalResizeBuffers;

// Function pointer types for new hooks
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreatePlacedResource)(
    ID3D12Device *, ID3D12Heap *, UINT64, const D3D12_RESOURCE_DESC *,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef void(STDMETHODCALLTYPE *PFN_ClearDepthStencilView)(
    ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT *);
typedef void(STDMETHODCALLTYPE *PFN_ClearRenderTargetView)(
    ID3D12GraphicsCommandList *, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4],
    UINT, const D3D12_RECT *);

void WrapCreatedD3D12Device(REFIID riid, void **ppDevice,
                            bool takeOwnership = true);
void InstallD3D12Hooks();
bool InitializeHooks();
void CleanupHooks();
void EnsureD3D12VTableHooks(ID3D12Device *device);
void NotifyWrappedCommandListUsed();
bool IsWrappedCommandListUsed();
void SetPatternJitterAddress(uintptr_t address);
bool TryGetPatternJitter(float &jitterX, float &jitterY);
