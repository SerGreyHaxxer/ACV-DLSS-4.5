#include <MinHook.h>
#include <atomic>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mutex>
#include <psapi.h>
#include <unordered_map>
#include <wchar.h>
#include <windows.h>
#include <winternl.h>

#include "d3d12_wrappers.h"
#include "dlss4_config.h"
#include "hooks.h"
#include "imgui_overlay.h"
#include "input_handler.h"
#include "logger.h"
#include "pattern_scanner.h"
#include "resource_detector.h"
#include "streamline_integration.h"
#include "vtable_utils.h"

// ============================================================================
// HOOK MANAGER IMPLEMENTATION
// ============================================================================

HookManager &HookManager::Get() {
  static HookManager instance;
  return instance;
}

bool HookManager::Initialize() {
  if (m_initialized)
    return true;
  if (MH_Initialize() != MH_OK) {
    LOG_ERROR("Failed to initialize MinHook");
    return false;
  }
  m_initialized = true;
  LOG_INFO("MinHook initialized successfully");
  return true;
}

void HookManager::Shutdown() {
  if (!m_initialized)
    return;
  MH_Uninitialize();
  m_initialized = false;
  LOG_INFO("MinHook uninitialized");
}

HookResult HookManager::CreateHookInternal(void *target, void *detour,
                                     void **original) {
  if (!m_initialized && !Initialize())
    return std::unexpected(MH_ERROR_NOT_INITIALIZED);

  MH_STATUS status = MH_CreateHook(target, detour, original);
  if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED) {
    LOG_ERROR("Failed to create hook for address {:p}, status: {}", target,
              static_cast<int>(status));
    return std::unexpected(status);
  }

  status = MH_EnableHook(target);
  if (status != MH_OK) {
    LOG_ERROR("Failed to enable hook for address {:p}", target);
    return std::unexpected(status);
  }

  LOG_INFO("Successfully hooked address {:p} -> {:p}", target, detour);
  return {};
}

// ============================================================================
// GLOBAL STATE
// ============================================================================

SwapChainHookState g_SwapChainState;
PFN_Present g_OriginalPresent = nullptr;
PFN_Present1 g_OriginalPresent1 = nullptr;
PFN_ResizeBuffers g_OriginalResizeBuffers = nullptr;

static std::mutex g_HookMutex;
static std::atomic<bool> g_HooksInitialized(false);
static std::atomic<bool> g_PatternScanDone(false);
static std::atomic<uintptr_t> g_JitterAddress(0);
static std::atomic<bool> g_JitterValid(false);
static std::atomic<bool> g_WrappedCommandListUsed(false);

extern "C" void LogStartup(const char *msg);

void NotifyWrappedCommandListUsed() { g_WrappedCommandListUsed.store(true); }

bool IsWrappedCommandListUsed() { return g_WrappedCommandListUsed.load(); }

void SetPatternJitterAddress(uintptr_t address) {
  g_JitterAddress.store(address);
  g_JitterValid.store(address != 0);
}

// Isolated SEH helper — no C++ objects on stack (extern "C" avoids UB with destructors)
extern "C" static bool SafeReadFloatPair(uintptr_t addr, float *outX, float *outY) {
  const float *vals = reinterpret_cast<const float *>(addr);
  __try {
    *outX = vals[0];
    *outY = vals[1];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool TryGetPatternJitter(float &jitterX, float &jitterY) {
  uintptr_t addr = g_JitterAddress.load();
  if (!addr)
    return false;

  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
    return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
    return false;

  float jx = 0.0f;
  float jy = 0.0f;
  if (!SafeReadFloatPair(addr, &jx, &jy))
    return false;

  if (!std::isfinite(jx) || !std::isfinite(jy))
    return false;

  jitterX = jx;
  jitterY = jy;
  return true;
}

// ============================================================================
// D3D12 HOOKS
// ============================================================================

PFN_ExecuteCommandLists g_OriginalExecuteCommandLists = nullptr;
PFN_CreateCommandQueue g_OriginalCreateCommandQueue = nullptr;
PFN_CreateCommittedResource g_OriginalCreateCommittedResource = nullptr;
PFN_D3D12CreateDevice g_OriginalD3D12CreateDevice = nullptr;

typedef HRESULT(STDMETHODCALLTYPE *PFN_Close)(ID3D12GraphicsCommandList *);
typedef void(STDMETHODCALLTYPE *PFN_ResourceBarrier)(
    ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *);
typedef void(STDMETHODCALLTYPE *PFN_SetGraphicsRootConstantBufferView)(
    ID3D12GraphicsCommandList *, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
typedef void(STDMETHODCALLTYPE *PFN_SetComputeRootConstantBufferView)(
    ID3D12GraphicsCommandList *, UINT, D3D12_GPU_VIRTUAL_ADDRESS);

PFN_Close g_OriginalClose = nullptr;
PFN_ResourceBarrier g_OriginalResourceBarrier = nullptr;
PFN_SetGraphicsRootConstantBufferView g_OriginalSetGraphicsRootCbv = nullptr;
PFN_SetComputeRootConstantBufferView g_OriginalSetComputeRootCbv = nullptr;

void STDMETHODCALLTYPE
HookedExecuteCommandLists(ID3D12CommandQueue *pThis, UINT NumCommandLists,
                          ID3D12CommandList *const *ppCommandLists) noexcept {
  try {
    if (!StreamlineIntegration::Get().IsInitialized()) {
      Microsoft::WRL::ComPtr<ID3D12Device> pDevice;
      if (SUCCEEDED(pThis->GetDevice(IID_PPV_ARGS(&pDevice)))) {
        StreamlineIntegration::Get().Initialize(pDevice.Get());
      }
    }

    D3D12_COMMAND_QUEUE_DESC desc = pThis->GetDesc();
    if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
      ResourceDetector::Get().NewFrame();
      StreamlineIntegration::Get().SetCommandQueue(pThis);

      ID3D12Resource *pColor = ResourceDetector::Get().GetBestColorCandidate();
      ID3D12Resource *pDepth = ResourceDetector::Get().GetBestDepthCandidate();
      ID3D12Resource *pMVs =
          ResourceDetector::Get().GetBestMotionVectorCandidate();

      if (pColor)
        StreamlineIntegration::Get().TagColorBuffer(pColor);
      if (pDepth)
        StreamlineIntegration::Get().TagDepthBuffer(pDepth);
      if (pMVs)
        StreamlineIntegration::Get().TagMotionVectors(pMVs);
    }
  } catch (...) {
    LOG_ERROR("[HOOK] Exception in HookedExecuteCommandLists");
  }

  g_OriginalExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
}

void STDMETHODCALLTYPE
HookedResourceBarrier(ID3D12GraphicsCommandList *pThis, UINT NumBarriers,
                      const D3D12_RESOURCE_BARRIER *pBarriers) noexcept {
  try {
    if (pBarriers) {
      static uint64_t s_lastScanFrame = 0;
      uint64_t currentFrame = StreamlineIntegration::Get().GetFrameCount();
      if (currentFrame != s_lastScanFrame) {
        s_lastScanFrame = currentFrame;
        for (UINT i = 0; i < NumBarriers && i < 16; i++) {
          if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            ResourceDetector::Get().RegisterResource(
                pBarriers[i].Transition.pResource, true);
          }
        }
      }
    }
  } catch (...) {
    LOG_ERROR("[HOOK] Exception in HookedResourceBarrier");
  }
  g_OriginalResourceBarrier(pThis, NumBarriers, pBarriers);
}

HRESULT STDMETHODCALLTYPE HookedClose(ID3D12GraphicsCommandList *pThis) noexcept {
  try {
    float jitterX = 0.0f, jitterY = 0.0f;
    TryGetPatternJitter(jitterX, jitterY);

    uint64_t currentFrame = ResourceDetector::Get().GetFrameCount();
    static uint64_t s_lastScanFrame = 0;

    if (currentFrame > s_lastScanFrame) {
      float view[16], proj[16], score = 0.0f;
      bool found = TryScanAllCbvsForCamera(view, proj, &score, false, true);
      if (!found)
        found = TryScanDescriptorCbvsForCamera(view, proj, &score, false);
      if (!found)
        found = TryScanRootCbvsForCamera(view, proj, &score, false);

      if (found) {
        UpdateCameraCache(view, proj, jitterX, jitterY);
        StreamlineIntegration::Get().SetCameraData(view, proj, jitterX, jitterY);
      } else {
        StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitterX,
                                                   jitterY);
      }
      s_lastScanFrame = currentFrame;
    } else {
      StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitterX,
                                                 jitterY);
    }

    StreamlineIntegration::Get().EvaluateDLSS(pThis);
  } catch (...) {
    LOG_ERROR("[HOOK] Exception in HookedClose");
  }
  return g_OriginalClose(pThis);
}

void STDMETHODCALLTYPE HookedSetGraphicsRootCbv(
    ID3D12GraphicsCommandList *pThis, UINT RootParameterIndex,
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) noexcept {
  try { TrackRootCbvAddress(BufferLocation); } catch (...) {}
  g_OriginalSetGraphicsRootCbv(pThis, RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE HookedSetComputeRootCbv(
    ID3D12GraphicsCommandList *pThis, UINT RootParameterIndex,
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) noexcept {
  try { TrackRootCbvAddress(BufferLocation); } catch (...) {}
  g_OriginalSetComputeRootCbv(pThis, RootParameterIndex, BufferLocation);
}

void EnsureCommandListHooks(ID3D12GraphicsCommandList *pList) {
  static std::once_flag listHookOnce;
  std::call_once(listHookOnce, [pList]() {
    void **vt = GetVTable(pList);
    (void)HookManager::Get().CreateHook(
        GetVTableEntry<void*, vtable::CommandList>(vt, vtable::CommandList::Close),
        reinterpret_cast<void*>(HookedClose), &g_OriginalClose);
    (void)HookManager::Get().CreateHook(
        GetVTableEntry<void*, vtable::CommandList>(vt, vtable::CommandList::ResourceBarrier),
        reinterpret_cast<void*>(HookedResourceBarrier), &g_OriginalResourceBarrier);
    (void)HookManager::Get().CreateHook(
        GetVTableEntry<void*, vtable::CommandList>(vt, vtable::CommandList::SetComputeRootConstantBufferView),
        reinterpret_cast<void*>(HookedSetComputeRootCbv), &g_OriginalSetComputeRootCbv);
    (void)HookManager::Get().CreateHook(
        GetVTableEntry<void*, vtable::CommandList>(vt, vtable::CommandList::SetGraphicsRootConstantBufferView),
        reinterpret_cast<void*>(HookedSetGraphicsRootCbv), &g_OriginalSetGraphicsRootCbv);
    LOG_INFO("[HOOK] Global D3D12 CommandList hooks installed");
  });
}

HRESULT STDMETHODCALLTYPE Hooked_CreateCommandQueue(
    ID3D12Device *pThis, const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid,
    void **ppCommandQueue) noexcept {
  HRESULT hr = g_OriginalCreateCommandQueue(pThis, pDesc, riid, ppCommandQueue);
  if (SUCCEEDED(hr) && ppCommandQueue && *ppCommandQueue && pDesc &&
      pDesc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
    static std::once_flag hookOnce;
    std::call_once(hookOnce, [ppCommandQueue]() {
      void **vt = GetVTable(static_cast<IUnknown*>(*ppCommandQueue));
      (void)HookManager::Get().CreateHook(
          GetVTableEntry<void*, vtable::CommandQueue>(vt, vtable::CommandQueue::ExecuteCommandLists),
          reinterpret_cast<void*>(HookedExecuteCommandLists),
          &g_OriginalExecuteCommandLists);
    });
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_CreateCommittedResource(
    ID3D12Device *pThis, const D3D12_HEAP_PROPERTIES *pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid,
    void **ppvResource) noexcept {
  HRESULT hr = g_OriginalCreateCommittedResource(
      pThis, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
      pOptimizedClearValue, riid, ppvResource);
  if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
    ResourceDetector::Get().RegisterResource(
        static_cast<ID3D12Resource *>(*ppvResource));
  }
  return hr;
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCommandList)(
    ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *,
    ID3D12PipelineState *, REFIID, void **);
PFN_CreateCommandList g_OriginalCreateCommandList = nullptr;

HRESULT STDMETHODCALLTYPE HookedCreateCommandList(
    ID3D12Device *pThis, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator *pCommandAllocator,
    ID3D12PipelineState *pInitialState, REFIID riid, void **ppCommandList) noexcept {
  HRESULT hr =
      g_OriginalCreateCommandList(pThis, nodeMask, type, pCommandAllocator,
                                  pInitialState, riid, ppCommandList);
  if (SUCCEEDED(hr) && ppCommandList && *ppCommandList &&
      type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
    EnsureCommandListHooks(
        static_cast<ID3D12GraphicsCommandList *>(*ppCommandList));
  }
  return hr;
}

void EnsureD3D12VTableHooks(ID3D12Device *pDevice) {
  if (!pDevice)
    return;
  static std::once_flag deviceHookOnce;
  std::call_once(deviceHookOnce, [pDevice]() {
    void **vt = GetVTable(pDevice);
    (void)HookManager::Get().CreateHook(
        GetVTableEntry<void*, vtable::Device>(vt, vtable::Device::CreateCommandQueue),
        reinterpret_cast<void*>(Hooked_CreateCommandQueue),
        &g_OriginalCreateCommandQueue);
    (void)HookManager::Get().CreateHook(
        GetVTableEntry<void*, vtable::Device>(vt, vtable::Device::CreateCommandList),
        reinterpret_cast<void*>(HookedCreateCommandList),
        &g_OriginalCreateCommandList);
    (void)HookManager::Get().CreateHook(
        GetVTableEntry<void*, vtable::Device>(vt, vtable::Device::CreateCommittedResource),
        reinterpret_cast<void*>(Hooked_CreateCommittedResource),
        &g_OriginalCreateCommittedResource);
    LOG_INFO("[HOOK] Global D3D12 Device hooks installed");
  });
}

void WrapCreatedD3D12Device(REFIID riid, void **ppDevice, bool takeOwnership) {
  if (!ppDevice || !*ppDevice)
    return;
  Microsoft::WRL::ComPtr<ID3D12Device> pRealDevice;
  if (FAILED(
          (reinterpret_cast<IUnknown*>(*ppDevice))->QueryInterface(IID_PPV_ARGS(&pRealDevice))))
    return;
  EnsureD3D12VTableHooks(pRealDevice.Get());
  StreamlineIntegration::Get().Initialize(pRealDevice.Get());
}

extern "C" HRESULT WINAPI Hooked_D3D12CreateDevice(
    IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid,
    void **ppDevice) {
  if (!g_OriginalD3D12CreateDevice)
    return E_FAIL;
  HRESULT hr = g_OriginalD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid,
                                           ppDevice);
  if (SUCCEEDED(hr) && ppDevice && *ppDevice)
    WrapCreatedD3D12Device(riid, ppDevice);
  return hr;
}

typedef FARPROC(WINAPI *PFN_GetProcAddress)(HMODULE, LPCSTR);
PFN_GetProcAddress g_OriginalGetProcAddress = nullptr;

FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
  if (HIWORD(lpProcName) != 0) {
    if (strcmp(lpProcName, "D3D12CreateDevice") == 0)
      return reinterpret_cast<FARPROC>(Hooked_D3D12CreateDevice);
  }
  return g_OriginalGetProcAddress(hModule, lpProcName);
}

void InstallD3D12Hooks() {
  static std::atomic<bool> installed(false);
  if (installed.exchange(true))
    return;
  HookManager::Get().Initialize();
  HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
  if (hD3D12) {
    void *target = GetProcAddress(hD3D12, "D3D12CreateDevice");
    if (target)
      (void)HookManager::Get().CreateHook(
          target, reinterpret_cast<void *>(Hooked_D3D12CreateDevice),
          &g_OriginalD3D12CreateDevice);
  }
  // Note: GetProcAddress hook disabled - too aggressive
}
bool InitializeHooks() { return true; }
void CleanupHooks() {
  HookManager::Get().Shutdown();
  InputHandler::Get().UninstallHook();
  ImGuiOverlay::Get().Shutdown();
  StreamlineIntegration::Get().Shutdown();
}
