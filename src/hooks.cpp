/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "hooks.h"

#include "camera_scanner.h"
#include "d3d12_wrappers.h"
#include "descriptor_tracker.h"
#include "dlss4_config.h"
#include "ghost_hook.h"
#include "imgui_overlay.h"
#include "input_handler.h"
#include "jitter_engine.h"
#include "logger.h"
#include "pattern_scanner.h"
#include "reactive_mask.h"
#include "render_passes/ray_tracing_pass.h"
#include "resource_detector.h"
#include "resource_lifetime_tracker.h"
#include "resource_state_tracker.h"
#include "sampler_interceptor.h"
#include "shadow_vtable.h"
#include "streamline_integration.h"
#include "dxgi_wrappers.h" // StopFrameTimer()
#include "vtable_utils.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <psapi.h>
#include <unordered_map>
#include <vector>
#include <wchar.h>
#include <winternl.h>

// Forward declarations from dxgi_wrappers
extern void OnPresentThread(IDXGISwapChain* pSwapChain);

// ============================================================================
// GLOBAL STATE
// ============================================================================

SwapChainHookState g_SwapChainState;
PFN_Present g_OriginalPresent = nullptr;
PFN_Present1 g_OriginalPresent1 = nullptr;
PFN_ResizeBuffers g_OriginalResizeBuffers = nullptr;

// Lock hierarchy level 2 (SwapChain=1 > Hooks=2 > Resources=3 > Config=4 > Logging=5).
static std::mutex g_HookMutex;
static std::atomic<bool> g_HooksInitialized(false);
static std::atomic<bool> g_PatternScanDone(false);
static std::atomic<uintptr_t> g_JitterAddress(0);
static std::atomic<bool> g_JitterValid(false);
static std::atomic<bool> g_WrappedCommandListUsed(false);

// Resettable hook state flags (allow re-hooking after device recreation)
static std::atomic<bool> s_cmdListHooked{false};
static std::atomic<bool> s_cmdQueueHooked{false};
static std::atomic<bool> s_deviceHooked{false};

extern "C" void LogStartup(const char* msg);

void NotifyWrappedCommandListUsed() {
  g_WrappedCommandListUsed.store(true);
}

bool IsWrappedCommandListUsed() {
  return g_WrappedCommandListUsed.load();
}

void SetPatternJitterAddress(uintptr_t address) {
  g_JitterAddress.store(address);
  g_JitterValid.store(address != 0);
}

// Isolated SEH helper - no C++ objects on stack (extern "C" avoids UB with destructors)
extern "C" static bool SafeReadFloatPair(uintptr_t addr, float* outX, float* outY) {
  const float* vals = reinterpret_cast<const float*>(addr);
  __try {
    *outX = vals[0];
    *outY = vals[1];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool TryGetPatternJitter(float& jitterX, float& jitterY) {
  uintptr_t addr = g_JitterAddress.load();
  if (!addr) return false;

  // SEH in SafeReadFloatPair handles invalid memory Ã¢â‚¬â€ no VirtualQuery syscall needed.
  float jx = 0.0f;
  float jy = 0.0f;
  if (!SafeReadFloatPair(addr, &jx, &jy)) return false;

  if (!std::isfinite(jx) || !std::isfinite(jy)) return false;

  jitterX = jx;
  jitterY = jy;
  return true;
}

// ============================================================================
// D3D12 ORIGINAL FUNCTION POINTERS - captured from vtables, called by hooks
// ============================================================================

PFN_ExecuteCommandLists g_OriginalExecuteCommandLists = nullptr;
PFN_CreateCommandQueue g_OriginalCreateCommandQueue = nullptr;
PFN_CreateCommittedResource g_OriginalCreateCommittedResource = nullptr;
PFN_D3D12CreateDevice g_OriginalD3D12CreateDevice = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* PFN_Close)(ID3D12GraphicsCommandList*);
typedef void(STDMETHODCALLTYPE* PFN_ResourceBarrier)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
typedef void(STDMETHODCALLTYPE* PFN_SetGraphicsRootConstantBufferView)(ID3D12GraphicsCommandList*, UINT,
                                                                       D3D12_GPU_VIRTUAL_ADDRESS);
typedef void(STDMETHODCALLTYPE* PFN_SetComputeRootConstantBufferView)(ID3D12GraphicsCommandList*, UINT,
                                                                      D3D12_GPU_VIRTUAL_ADDRESS);

PFN_Close g_OriginalClose = nullptr;
PFN_ResourceBarrier g_OriginalResourceBarrier = nullptr;
PFN_SetGraphicsRootConstantBufferView g_OriginalSetGraphicsRootCbv = nullptr;
PFN_SetComputeRootConstantBufferView g_OriginalSetComputeRootCbv = nullptr;

PFN_CreatePlacedResource g_OriginalCreatePlacedResource = nullptr;
PFN_CreateShaderResourceView g_OriginalCreateSRV = nullptr;
PFN_CreateUnorderedAccessView g_OriginalCreateUAV = nullptr;
PFN_CreateRenderTargetView g_OriginalCreateRTV = nullptr;
PFN_CreateDepthStencilView g_OriginalCreateDSV = nullptr;
PFN_ClearDepthStencilView g_OriginalClearDSV = nullptr;
PFN_ClearRenderTargetView g_OriginalClearRTV = nullptr;
PFN_CreateConstantBufferView g_OriginalCreateCBV = nullptr;
PFN_CreateSampler g_OriginalCreateSampler = nullptr;
// Fix: Raw original CreateSampler pointer captured from pristine vtable
// BEFORE shadow patching. g_OriginalCreateSampler points to the shadow
// vtable slot which redirects back to Hooked_CreateSampler → infinite
// recursion. This pointer bypasses the shadow vtable entirely.
static PFN_CreateSampler g_RawOriginalCreateSampler = nullptr;
PFN_CreateDescriptorHeap g_OriginalCreateDescriptorHeap = nullptr;
PFN_CreateCommandList g_OriginalCreateCommandList = nullptr;
PFN_ResourceMap g_OriginalResourceMap = nullptr;
static std::atomic<bool> s_resourceHooked{false};

// Fix 1: Release hook for deterministic resource lifetime tracking
typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(IUnknown*);
static PFN_Release g_OriginalResourceRelease = nullptr;

// Fix 6: DrawIndexedInstanced hook for reactive mask
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
static PFN_DrawIndexedInstanced g_OriginalDrawIndexedInstanced = nullptr;

// ============================================================================
// SHADOW VTABLE ARCHITECTURE — All hooks via vtable patching
// ============================================================================
// Every hooked COM interface (Device, CommandList, CommandQueue, SwapChain)
// uses shadow vtables. This gives:
//   - Expected full call capture (all calls route through shadow vtable)
//   - Low overhead per call (indirect branch through copied vtable)
//   - No VEH overhead for device/cmdlist/queue/swapchain hooks
//   - Patches heap-allocated COM objects, not .text sections
// ==================================================================================================

// ============================================================================
// SWAPCHAIN SHADOW VTABLE HOOKS (stdcall wrappers)
// ============================================================================

static HRESULT STDMETHODCALLTYPE Hooked_Present(
    IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags) {
  try {
    OnPresentThread(pThis);
  } catch (...) {
    static std::atomic<uint32_t> s_errs{0};
    if (s_errs.fetch_add(1) % 300 == 0) LOG_ERROR("[ShadowVT] Exception in Present callback");
  }
  // Per-frame tick for Phase 3 subsystems
  DescriptorTracker_NewFrame();
  SamplerInterceptor_NewFrame();
  if (!g_OriginalPresent) return E_FAIL;
  return g_OriginalPresent(pThis, SyncInterval, Flags);
}

static HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers(
    IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
  try {
    if (Width > 0 && Height > 0) {
      ImGuiOverlay::Get().OnResize(Width, Height);
      ResourceDetector::Get().SetExpectedDimensions(Width, Height);
      LOG_INFO("[ShadowVT] ResizeBuffers: {}x{}", Width, Height);
    } else {
      LOG_DEBUG("[ShadowVT] ResizeBuffers: auto-size (0x0)");
    }
    ResetCameraScanCache();
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in ResizeBuffers");
  }
  if (!g_OriginalResizeBuffers) return E_FAIL;
  return g_OriginalResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// ============================================================================
// COMMAND QUEUE SHADOW VTABLE HOOKS (stdcall wrappers)
// ============================================================================

static void STDMETHODCALLTYPE Hooked_ExecuteCommandLists(
    ID3D12CommandQueue* pThis, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
  // Call original first
  if (g_OriginalExecuteCommandLists)
    g_OriginalExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);

  try {
    if (!StreamlineIntegration::Get().IsInitialized()) {
      Microsoft::WRL::ComPtr<ID3D12Device> pDevice;
      if (SUCCEEDED(pThis->GetDevice(IID_PPV_ARGS(&pDevice)))) {
        StreamlineIntegration::Get().Initialize(pDevice.Get());
      }
    }

    D3D12_COMMAND_QUEUE_DESC desc = pThis->GetDesc();
    if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
      // Fix 7: Merge all TLS barrier batches before any code reads resource states
      ResourceStateTracker_FlushTLS();

      ResourceDetector::Get().NewFrame();
      StreamlineIntegration::Get().SetCommandQueue(pThis);

      ID3D12Resource* pColor = ResourceDetector::Get().GetBestColorCandidate();
      ID3D12Resource* pDepth = ResourceDetector::Get().GetBestDepthCandidate();
      ID3D12Resource* pMVs = ResourceDetector::Get().GetBestMotionVectorCandidate();

      if (pColor) StreamlineIntegration::Get().TagColorBuffer(pColor);
      if (pDepth) StreamlineIntegration::Get().TagDepthBuffer(pDepth);
      if (pMVs) StreamlineIntegration::Get().TagMotionVectors(pMVs);

      Microsoft::WRL::ComPtr<ID3D12Device> pDevice;
      if (SUCCEEDED(pThis->GetDevice(IID_PPV_ARGS(&pDevice)))) {
        RayTracingPass::Get().Initialize(pDevice.Get());
      }

      float jitterX = 0.0f, jitterY = 0.0f;
      TryGetPatternJitter(jitterX, jitterY);
      // Fix: Don't attempt cached camera fallback here — Hooked_Close already
      // handles camera data with proper scoring and caching. Setting cached data
      // here can override better quality data that Hooked_Close will provide.
      // Pass nullptr as a preliminary call; Hooked_Close will fill in real matrices.
      StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitterX, jitterY);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in ExecuteCommandLists callback");
  }
}

// ============================================================================
// DEVICE SHADOW VTABLE HOOKS (stdcall wrappers)
// ============================================================================
// These replace the Ghost HW breakpoint rotating hooks. Each wrapper calls
// the original vtable function, then executes hook logic.  Installed via
// ShadowVTable::PatchEntry — 100% call capture, ~2ns overhead.
// ============================================================================

static void STDMETHODCALLTYPE Hooked_CreateSRV(
    ID3D12Device* pThis, ID3D12Resource* pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  if (g_OriginalCreateSRV) g_OriginalCreateSRV(pThis, pResource, pDesc, handle);
  try {
    if (pResource) {
      DXGI_FORMAT fmt = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
      TrackDescriptorResource(handle, pResource, fmt);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateSRV");
  }
}

static void STDMETHODCALLTYPE Hooked_CreateUAV(
    ID3D12Device* pThis, ID3D12Resource* pResource,
    ID3D12Resource* pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  if (g_OriginalCreateUAV) g_OriginalCreateUAV(pThis, pResource, pCounterResource, pDesc, handle);
  try {
    if (pResource) {
      DXGI_FORMAT fmt = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
      TrackDescriptorResource(handle, pResource, fmt);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateUAV");
  }
}

static void STDMETHODCALLTYPE Hooked_CreateRTV(
    ID3D12Device* pThis, ID3D12Resource* pResource,
    const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  if (g_OriginalCreateRTV) g_OriginalCreateRTV(pThis, pResource, pDesc, handle);
  try {
    if (pResource) {
      DXGI_FORMAT fmt = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
      TrackDescriptorResource(handle, pResource, fmt);
      ResourceDetector::Get().RegisterResource(pResource, true);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateRTV");
  }
}

static void STDMETHODCALLTYPE Hooked_CreateDSV(
    ID3D12Device* pThis, ID3D12Resource* pResource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  if (g_OriginalCreateDSV) g_OriginalCreateDSV(pThis, pResource, pDesc, handle);
  try {
    if (pResource) {
      DXGI_FORMAT fmt = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
      TrackDescriptorResource(handle, pResource, fmt);
      ResourceDetector::Get().RegisterDepthFromView(pResource, fmt);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateDSV");
  }
}

static void STDMETHODCALLTYPE Hooked_CreateCBV(
    ID3D12Device* pThis, const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  if (g_OriginalCreateCBV) g_OriginalCreateCBV(pThis, pDesc, handle);
  try {
    if (pDesc) {
      TrackCbvDescriptor(handle, pDesc);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateCBV");
  }
}

static void STDMETHODCALLTYPE Hooked_CreateSampler(
    ID3D12Device* pThis, const D3D12_SAMPLER_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  // Fix: TLS recursion guard — if we're already inside this hook (e.g. via
  // a shadow vtable cycle), skip hook logic and call the raw original directly.
  thread_local bool t_inCreateSampler = false;
  if (t_inCreateSampler) {
    if (g_RawOriginalCreateSampler) g_RawOriginalCreateSampler(pThis, pDesc, handle);
    return;
  }
  t_inCreateSampler = true;
  try {
    if (pDesc && pThis && handle.ptr) {
      D3D12_SAMPLER_DESC modifiedDesc = ApplyLodBias(*pDesc);
      // Fix: Call through g_RawOriginalCreateSampler which points to the
      // pristine vtable entry, NOT the shadow vtable slot. This prevents
      // the infinite recursion: Hooked → shadow → Hooked → shadow → ...
      if (g_RawOriginalCreateSampler) g_RawOriginalCreateSampler(pThis, &modifiedDesc, handle);
    } else {
      if (g_RawOriginalCreateSampler) g_RawOriginalCreateSampler(pThis, pDesc, handle);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateSampler");
  }
  t_inCreateSampler = false;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateDescriptorHeap(
    ID3D12Device* pThis, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc,
    REFIID riid, void** ppvHeap) {
  HRESULT hr = E_FAIL;
  if (g_OriginalCreateDescriptorHeap) hr = g_OriginalCreateDescriptorHeap(pThis, pDesc, riid, ppvHeap);
  try {
    if (pDesc && pThis) {
      static std::atomic<uint32_t> s_heapCount{0};
      uint32_t count = s_heapCount.fetch_add(1, std::memory_order_relaxed);
      if (count < 50) {
        LOG_DEBUG("[ShadowVT] CreateDescriptorHeap: Type={}, NumDescriptors={}, Flags={}",
                  static_cast<int>(pDesc->Type), pDesc->NumDescriptors, static_cast<int>(pDesc->Flags));
      }
      // Item 22: Wire descriptor heap tracking for resource/descriptor resolution
      if (SUCCEEDED(hr) && ppvHeap && *ppvHeap) {
        auto* pHeap = static_cast<ID3D12DescriptorHeap*>(*ppvHeap);
        UINT incSize = pThis->GetDescriptorHandleIncrementSize(pDesc->Type);
        TrackDescriptorHeap(pHeap, incSize);
      }
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateDescriptorHeap");
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateCommittedResource(
    ID3D12Device* pThis, const D3D12_HEAP_PROPERTIES* pHeapProperties,
    D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource,
    void** ppvResource) {
  HRESULT hr = E_FAIL;
  if (g_OriginalCreateCommittedResource)
    hr = g_OriginalCreateCommittedResource(pThis, pHeapProperties, HeapFlags, pDesc,
                                            InitialResourceState, pOptimizedClearValue,
                                            riidResource, ppvResource);
  try {
    if (pDesc) {
      bool isRT = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
      bool isDS = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
      bool isUAV = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
      if ((isRT || isDS || isUAV) && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        static std::atomic<uint32_t> s_logCount{0};
        if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 100) {
          LOG_DEBUG("[ShadowVT] CreateCommittedResource: {}x{} Fmt={} RT={} DS={} UAV={}",
                    static_cast<uint32_t>(pDesc->Width), pDesc->Height,
                    static_cast<int>(pDesc->Format), isRT, isDS, isUAV);
        }
      }
    }

    // Install Resource shadow vtable if it's an upload buffer (for Map tracking)
    if (SUCCEEDED(hr) && ppvResource && *ppvResource && pHeapProperties && pHeapProperties->Type == D3D12_HEAP_TYPE_UPLOAD) {
      ID3D12Resource* pRes = static_cast<ID3D12Resource*>(*ppvResource);
      constexpr size_t kResourceVTableSize = 12;
      ShadowVTable::Install(pRes, kResourceVTableSize);
      if (g_OriginalResourceMap) {
        ShadowVTable::PatchEntry(pRes, static_cast<size_t>(vtable::Resource::Map),
                                 reinterpret_cast<void*>(Hooked_ResourceMap));
      }
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateCommittedResource");
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateCommandQueue(
    ID3D12Device* pThis, const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) {
  HRESULT hr = E_FAIL;
  if (g_OriginalCreateCommandQueue) {
    hr = g_OriginalCreateCommandQueue(pThis, pDesc, riid, ppCommandQueue);
  }
  try {
    if (SUCCEEDED(hr) && ppCommandQueue && *ppCommandQueue) {
      ID3D12CommandQueue* pQueue = static_cast<ID3D12CommandQueue*>(*ppCommandQueue);
      if (pDesc && pDesc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        constexpr size_t kQueueVTableSize = 19;
        ShadowVTable::Install(pQueue, kQueueVTableSize);
        ShadowVTable::PatchEntry(pQueue, static_cast<size_t>(vtable::CommandQueue::ExecuteCommandLists),
                                 reinterpret_cast<void*>(Hooked_ExecuteCommandLists));
        LOG_INFO("[ShadowVT] CommandQueue ExecuteCommandLists installed on newly created queue");
      }
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateCommandQueue");
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateCommandList(
    ID3D12Device* pThis, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState,
    REFIID riid, void** ppCommandList) {
  HRESULT hr = E_FAIL;
  if (g_OriginalCreateCommandList) {
    hr = g_OriginalCreateCommandList(pThis, nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
  }
  try {
    if (SUCCEEDED(hr) && ppCommandList && *ppCommandList) {
      ID3D12GraphicsCommandList* pList = static_cast<ID3D12GraphicsCommandList*>(*ppCommandList);
      if (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        InstallCommandListShadowVTable(pList);
      }
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreateCommandList");
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreatePlacedResource(
    ID3D12Device* pThis, ID3D12Heap* pHeap, UINT64 HeapOffset,
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState,
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
  HRESULT hr = E_FAIL;
  if (g_OriginalCreatePlacedResource)
    hr = g_OriginalCreatePlacedResource(pThis, pHeap, HeapOffset, pDesc,
                                         InitialState, pOptimizedClearValue, riid, ppvResource);
  try {
    if (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
      bool isRT = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
      bool isDS = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
      if (isRT || isDS) {
        static std::atomic<uint32_t> s_logCount{0};
        if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 50) {
          LOG_DEBUG("[ShadowVT] CreatePlacedResource: {}x{} Fmt={} RT={} DS={}",
                    static_cast<uint32_t>(pDesc->Width), pDesc->Height,
                    static_cast<int>(pDesc->Format), isRT, isDS);
        }
      }
    }
    // Item 23: Install Map hook on placed upload resources (mirrors CreateCommittedResource)
    if (SUCCEEDED(hr) && ppvResource && *ppvResource && pHeap) {
      D3D12_HEAP_DESC heapDesc = pHeap->GetDesc();
      if (heapDesc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
        ID3D12Resource* pRes = static_cast<ID3D12Resource*>(*ppvResource);
        constexpr size_t kResourceVTableSize = 12;
        ShadowVTable::Install(pRes, kResourceVTableSize);
        if (g_OriginalResourceMap) {
          ShadowVTable::PatchEntry(pRes, static_cast<size_t>(vtable::Resource::Map),
                                   reinterpret_cast<void*>(Hooked_ResourceMap));
        }
      }
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in CreatePlacedResource");
  }
  return hr;
}

// Resource::Map hook — captures upload buffer CPU pointers for camera scanning
static HRESULT STDMETHODCALLTYPE Hooked_ResourceMap(
    ID3D12Resource* pThis, UINT Subresource, const D3D12_RANGE* pReadRange,
    void** ppData) {
  HRESULT hr = E_FAIL;
  if (g_OriginalResourceMap) hr = g_OriginalResourceMap(pThis, Subresource, pReadRange, ppData);
  try {
    if (!pThis || Subresource != 0) return hr;
    D3D12_RESOURCE_DESC desc = pThis->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return hr;
    if (desc.Width < camera_config::kCbvMinSize) return hr;

    if (SUCCEEDED(hr) && ppData && *ppData) {
      RegisterCbv(pThis, static_cast<UINT64>(desc.Width),
                  reinterpret_cast<uint8_t*>(*ppData));
      static std::atomic<uint32_t> s_logCountMap{0};
      if (s_logCountMap.fetch_add(1, std::memory_order_relaxed) < 10) {
        LOG_INFO("[CAM] Registered upload buffer GPU:0x{:x} Size:{} via Map hook",
                 pThis->GetGPUVirtualAddress(), desc.Width);
      }
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in ResourceMap");
  }
  return hr;
}

// Fix 1.4: Hooked_ResourceRelease REMOVED — no longer needed.
// With SetPrivateDataInterface, the D3D12 runtime automatically calls
// GhostTrackerTag::Release() when a resource is destroyed. The old approach
// of intercepting IUnknown::Release and probing refcounts was fragile and
// required the global mutex-based ResourceLifetimeTracker.

static void** g_CmdListShadowVTable = nullptr;
static size_t g_CmdListVTableSize = 0;
static std::mutex g_CmdListShadowMutex;

static void STDMETHODCALLTYPE Hooked_ResourceBarrier(
    ID3D12GraphicsCommandList* pThis, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
  // Call original first
  if (g_OriginalResourceBarrier) g_OriginalResourceBarrier(pThis, NumBarriers, pBarriers);

  // Hook logic — resource barrier interception
  try {
    if (pBarriers && NumBarriers > 0) {
      UINT scanCount = (std::min)(NumBarriers, static_cast<UINT>(resource_config::kBarrierScanMax));
      for (UINT i = 0; i < scanCount; i++) {
        if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
          ID3D12Resource* pRes = pBarriers[i].Transition.pResource;
          if (pRes) {
            ResourceDetector::Get().RegisterResource(pRes, true);
            D3D12_RESOURCE_STATES after = pBarriers[i].Transition.StateAfter;
            if (after == D3D12_RESOURCE_STATE_DEPTH_WRITE || after == D3D12_RESOURCE_STATE_DEPTH_READ) {
              ResourceDetector::Get().RegisterDepthFromView(pRes, DXGI_FORMAT_UNKNOWN);
            }
            if (after == D3D12_RESOURCE_STATE_RENDER_TARGET) {
              ResourceDetector::Get().RegisterHUDLessCandidate(pRes);
            }
            ResourceStateTracker_RecordTransition(pRes, pBarriers[i].Transition.StateBefore,
                                                  pBarriers[i].Transition.StateAfter);
          }
        }
      }
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in Hooked_ResourceBarrier");
  }
}

static HRESULT STDMETHODCALLTYPE Hooked_Close(ID3D12GraphicsCommandList* pThis) {
  // Execute hook logic BEFORE closing
  try {
    float patternX = 0.0f, patternY = 0.0f;
    bool hasPattern = TryGetPatternJitter(patternX, patternY);

    uint64_t currentFrame = ResourceDetector::Get().GetFrameCount();
    static std::atomic<uint64_t> s_lastScanFrame{0};

    uint64_t lastScan = s_lastScanFrame.load(std::memory_order_relaxed);
    if (currentFrame > lastScan &&
        s_lastScanFrame.compare_exchange_strong(lastScan, currentFrame, std::memory_order_relaxed)) {
      float view[16], proj[16], score = 0.0f;
      bool found = TryScanAllCbvsForCamera(view, proj, &score, false, true);
      if (!found) found = TryScanDescriptorCbvsForCamera(view, proj, &score, false);
      if (!found) found = TryScanRootCbvsForCamera(view, proj, &score, false);

      float pX = hasPattern ? patternX : std::numeric_limits<float>::quiet_NaN();
      float pY = hasPattern ? patternY : std::numeric_limits<float>::quiet_NaN();
      JitterResult jitter = JitterEngine_Update(pX, pY, found ? proj : nullptr);

      if (found) {
        UpdateCameraCache(view, proj, jitter.x, jitter.y);
        StreamlineIntegration::Get().CacheCameraData(view, proj);
        StreamlineIntegration::Get().SetCameraData(view, proj, jitter.x, jitter.y);
      } else {
        float cachedView[16], cachedProj[16];
        if (StreamlineIntegration::Get().GetCachedCameraData(cachedView, cachedProj)) {
          StreamlineIntegration::Get().SetCameraData(cachedView, cachedProj, jitter.x, jitter.y);
        } else {
          StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitter.x, jitter.y);
        }
      }
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in Hooked_Close");
  }

  // Call original
  if (g_OriginalClose) return g_OriginalClose(pThis);
  return E_FAIL;
}

static void STDMETHODCALLTYPE Hooked_SetGraphicsRootCbv(
    ID3D12GraphicsCommandList* pThis, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
  // Call original first
  if (g_OriginalSetGraphicsRootCbv) g_OriginalSetGraphicsRootCbv(pThis, RootParameterIndex, BufferLocation);

  try {
    if (RootParameterIndex <= 1) {
      TrackRootCbvAddress(BufferLocation);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in Hooked_SetGraphicsRootCbv");
  }
}

static void STDMETHODCALLTYPE Hooked_SetComputeRootCbv(
    ID3D12GraphicsCommandList* pThis, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
  // Call original first
  if (g_OriginalSetComputeRootCbv) g_OriginalSetComputeRootCbv(pThis, RootParameterIndex, BufferLocation);

  try {
    if (RootParameterIndex <= 1) {
      TrackRootCbvAddress(BufferLocation);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in Hooked_SetComputeRootCbv");
  }
}

static void STDMETHODCALLTYPE Hooked_ClearDSV(
    ID3D12GraphicsCommandList* pThis, D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView,
    D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) {
  // Call original first
  if (g_OriginalClearDSV) g_OriginalClearDSV(pThis, DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);

  try {
    ID3D12Resource* pResource = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (TryResolveDescriptorResource(DepthStencilView, &pResource, &fmt) && pResource) {
      ResourceDetector::Get().RegisterDepthFromClear(pResource, Depth);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in Hooked_ClearDSV");
  }
}

static void STDMETHODCALLTYPE Hooked_ClearRTV(
    ID3D12GraphicsCommandList* pThis, D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView,
    const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects) {
  // Call original first
  if (g_OriginalClearRTV) g_OriginalClearRTV(pThis, RenderTargetView, ColorRGBA, NumRects, pRects);

  try {
    ID3D12Resource* pResource = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (TryResolveDescriptorResource(RenderTargetView, &pResource, &fmt) && pResource) {
      ResourceDetector::Get().RegisterColorFromClear(pResource);
    }
  } catch (...) {
    LOG_ERROR("[ShadowVT] Exception in Hooked_ClearRTV");
  }
}

// Patch a command list's vtable with our shadow hooks.
// Called for every newly created command list. Thread-safe.
void InstallCommandListShadowVTable(ID3D12GraphicsCommandList* pList) {
  if (!pList) return;

  std::lock_guard<std::mutex> lock(g_CmdListShadowMutex);

  // Lazily create the shared shadow vtable on first call
  if (!g_CmdListShadowVTable) {
    // DX12 ID3D12GraphicsCommandList has ~65 methods
    constexpr size_t kCmdListVTableSize = 65;
    g_CmdListVTableSize = kCmdListVTableSize;

    // Clone the original vtable
    void** originalVTable = GetVTable(pList);
    g_CmdListShadowVTable = new (std::nothrow) void*[kCmdListVTableSize];
    if (!g_CmdListShadowVTable) {
      LOG_ERROR("[ShadowVT] Failed to allocate CmdList shadow vtable");
      return;
    }
    std::copy_n(originalVTable, kCmdListVTableSize, g_CmdListShadowVTable);

    // Capture originals from the pristine vtable BEFORE patching
    g_OriginalClose = reinterpret_cast<PFN_Close>(
        g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::Close)]);
    g_OriginalResourceBarrier = reinterpret_cast<PFN_ResourceBarrier>(
        g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::ResourceBarrier)]);
    g_OriginalSetGraphicsRootCbv = reinterpret_cast<PFN_SetGraphicsRootConstantBufferView>(
        g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::SetGraphicsRootConstantBufferView)]);
    g_OriginalSetComputeRootCbv = reinterpret_cast<PFN_SetComputeRootConstantBufferView>(
        g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::SetComputeRootConstantBufferView)]);
    g_OriginalClearDSV = reinterpret_cast<PFN_ClearDepthStencilView>(
        g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::ClearDepthStencilView)]);
    g_OriginalClearRTV = reinterpret_cast<PFN_ClearRenderTargetView>(
        g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::ClearRenderTargetView)]);

    // Patch all 6 entries in the shared shadow vtable
    g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::ResourceBarrier)] =
        reinterpret_cast<void*>(Hooked_ResourceBarrier);
    g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::Close)] =
        reinterpret_cast<void*>(Hooked_Close);
    g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::SetGraphicsRootConstantBufferView)] =
        reinterpret_cast<void*>(Hooked_SetGraphicsRootCbv);
    g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::SetComputeRootConstantBufferView)] =
        reinterpret_cast<void*>(Hooked_SetComputeRootCbv);
    g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::ClearDepthStencilView)] =
        reinterpret_cast<void*>(Hooked_ClearDSV);
    g_CmdListShadowVTable[static_cast<size_t>(vtable::CommandList::ClearRenderTargetView)] =
        reinterpret_cast<void*>(Hooked_ClearRTV);

    LOG_INFO("[ShadowVT] CmdList shadow vtable created ({} entries, 6 hooks patched)", kCmdListVTableSize);
  }

  // Swap this command list's vptr to point to our shared shadow vtable
  *reinterpret_cast<void**>(pList) = g_CmdListShadowVTable;
}


// ============================================================================
// DEVICE HOOK INSTALLATION — captures vtable pointers + patches shadow vtable
// ============================================================================

static void CaptureDeviceVTablePointers(ID3D12Device* pDevice) {
  void** devVt = GetVTable(pDevice);

  g_OriginalCreateCommandQueue = reinterpret_cast<PFN_CreateCommandQueue>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateCommandQueue));
  g_OriginalCreateCommittedResource = reinterpret_cast<PFN_CreateCommittedResource>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateCommittedResource));
  g_OriginalCreatePlacedResource = reinterpret_cast<PFN_CreatePlacedResource>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreatePlacedResource));
  g_OriginalCreateSRV = reinterpret_cast<PFN_CreateShaderResourceView>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateShaderResourceView));
  g_OriginalCreateUAV = reinterpret_cast<PFN_CreateUnorderedAccessView>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateUnorderedAccessView));
  g_OriginalCreateRTV = reinterpret_cast<PFN_CreateRenderTargetView>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateRenderTargetView));
  g_OriginalCreateDSV = reinterpret_cast<PFN_CreateDepthStencilView>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateDepthStencilView));
  g_OriginalCreateCBV = reinterpret_cast<PFN_CreateConstantBufferView>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateConstantBufferView));
  g_OriginalCreateCommandList = reinterpret_cast<PFN_CreateCommandList>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateCommandList));
  g_OriginalCreateSampler =
      reinterpret_cast<PFN_CreateSampler>(GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateSampler));
  // Fix: Capture raw original BEFORE shadow vtable is installed.
  // g_OriginalCreateSampler will later point to the shadow vtable slot
  // (which redirects to Hooked_CreateSampler), causing infinite recursion.
  // g_RawOriginalCreateSampler always points to the real D3D12 implementation.
  g_RawOriginalCreateSampler = g_OriginalCreateSampler;
  g_OriginalCreateDescriptorHeap = reinterpret_cast<PFN_CreateDescriptorHeap>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateDescriptorHeap));

  // Install shadow vtable on device — all hooks fire 100% reliably.
  constexpr size_t kDeviceVTableSize = 45;
  ShadowVTable::Install(pDevice, kDeviceVTableSize);

  // Patch each hook into the shadow vtable via stdcall wrappers
  auto patchDevice = [pDevice](auto vtIdx, void* hookFn, const char* name) {
    size_t idx = static_cast<size_t>(vtIdx);
    void* orig = ShadowVTable::PatchEntry(pDevice, idx, hookFn);
    if (orig) LOG_INFO("[ShadowVT] Device hook installed: {} (slot {})", name, idx);
    return orig;
  };

  patchDevice(vtable::Device::CreateShaderResourceView,
              reinterpret_cast<void*>(Hooked_CreateSRV), "CreateSRV");
  patchDevice(vtable::Device::CreateUnorderedAccessView,
              reinterpret_cast<void*>(Hooked_CreateUAV), "CreateUAV");
  patchDevice(vtable::Device::CreateRenderTargetView,
              reinterpret_cast<void*>(Hooked_CreateRTV), "CreateRTV");
  patchDevice(vtable::Device::CreateDepthStencilView,
              reinterpret_cast<void*>(Hooked_CreateDSV), "CreateDSV");
  patchDevice(vtable::Device::CreateConstantBufferView,
              reinterpret_cast<void*>(Hooked_CreateCBV), "CreateCBV");
  patchDevice(vtable::Device::CreateSampler,
              reinterpret_cast<void*>(Hooked_CreateSampler), "CreateSampler");
  patchDevice(vtable::Device::CreateDescriptorHeap,
              reinterpret_cast<void*>(Hooked_CreateDescriptorHeap), "CreateDescHeap");
  patchDevice(vtable::Device::CreateCommittedResource,
              reinterpret_cast<void*>(Hooked_CreateCommittedResource), "CreateCommitted");
  patchDevice(vtable::Device::CreatePlacedResource,
              reinterpret_cast<void*>(Hooked_CreatePlacedResource), "CreatePlaced");
  patchDevice(vtable::Device::CreateCommandQueue,
              reinterpret_cast<void*>(Hooked_CreateCommandQueue), "CreateCommandQueue");
  patchDevice(vtable::Device::CreateCommandList,
              reinterpret_cast<void*>(Hooked_CreateCommandList), "CreateCommandList");

}


static void CaptureCommandQueueVTable(ID3D12CommandQueue* pQueue) {
  if (s_cmdQueueHooked.exchange(true)) return;

  void** queueVt = GetVTable(pQueue);
  g_OriginalExecuteCommandLists = reinterpret_cast<PFN_ExecuteCommandLists>(
      GetVTableEntry<void*, vtable::CommandQueue>(queueVt, vtable::CommandQueue::ExecuteCommandLists));

  // Install shadow vtable on CommandQueue — ExecuteCommandLists via stdcall wrapper
  constexpr size_t kQueueVTableSize = 19; // ID3D12CommandQueue has ~19 methods
  ShadowVTable::Install(pQueue, kQueueVTableSize);
  ShadowVTable::PatchEntry(pQueue, static_cast<size_t>(vtable::CommandQueue::ExecuteCommandLists),
                           reinterpret_cast<void*>(Hooked_ExecuteCommandLists));
  LOG_INFO("[ShadowVT] CommandQueue ExecuteCommandLists installed via shadow vtable");
}

static void CaptureCommandListVTable(ID3D12GraphicsCommandList* pList) {
  if (s_cmdListHooked.exchange(true)) return;

  // P0 Fix 1: Install shadow vtable on the first command list. This captures
  // the original function pointers AND patches the vtable in one step.
  // All subsequent command lists will use the same shared shadow vtable.
  // NO rotating hooks needed — 100% call capture, zero VEH overhead.
  InstallCommandListShadowVTable(pList);

  LOG_INFO("[ShadowVT] CommandList vtable captured via shadow vtable (6 hooks, 0 rotating)");
}

// ============================================================================
// DEVICE HOOKS ENTRY POINT
// ============================================================================

void EnsureD3D12VTableHooks(ID3D12Device* pDevice) {
  if (!pDevice) return;
  if (s_deviceHooked.exchange(true)) return;

  // Shadow VTable architecture: no Ghost::HookManager needed.
  CaptureDeviceVTablePointers(pDevice);

  // Create temporary objects to capture their vtables
  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> tmpQueue;
  if (SUCCEEDED(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&tmpQueue)))) {
    CaptureCommandQueueVTable(tmpQueue.Get());
  }

  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> tmpAlloc;
  if (SUCCEEDED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmpAlloc)))) {
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> tmpList;
    if (SUCCEEDED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmpAlloc.Get(), nullptr,
                                             IID_PPV_ARGS(&tmpList)))) {
      CaptureCommandListVTable(tmpList.Get());
      // Note: Close() was already hooked via shadow vtable, use original directly
      if (g_OriginalClose) g_OriginalClose(tmpList.Get());
      else tmpList->Close();
    }
  }

  // Capture ID3D12Resource vtable by creating a temporary upload buffer
  if (!s_resourceHooked.exchange(true)) {
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = 256;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> tmpUpload;
    HRESULT hr = pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tmpUpload));
    if (SUCCEEDED(hr) && tmpUpload) {
      void** resVt = GetVTable(tmpUpload.Get());
      g_OriginalResourceMap =
          reinterpret_cast<PFN_ResourceMap>(GetVTableEntry<void*, vtable::Resource>(resVt, vtable::Resource::Map));
      g_OriginalResourceRelease = reinterpret_cast<PFN_Release>(resVt[2]);

      // Install shadow vtable on the resource to patch Map and Release
      constexpr size_t kResourceVTableSize = 12; // ID3D12Resource has ~12 methods
      ShadowVTable::Install(tmpUpload.Get(), kResourceVTableSize);

      if (g_OriginalResourceMap) {
        ShadowVTable::PatchEntry(tmpUpload.Get(), static_cast<size_t>(vtable::Resource::Map),
                                 reinterpret_cast<void*>(Hooked_ResourceMap));
        LOG_INFO("[ShadowVT] Resource::Map hook installed for camera CBV tracking");
      }

      // Fix 1.4: Resource::Release hook REMOVED — SetPrivateDataInterface
      // handles lifecycle automatically via GhostTrackerTag.
      // The old Hooked_ResourceRelease was a fragile refcount-probing
      // workaround that required the global mutex.
    } else {
      LOG_WARN("[ShadowVT] Failed to create temp upload buffer for Resource vtable capture");
    }
  }

  StreamlineIntegration::Get().Initialize(pDevice);
  SamplerInterceptor_InstallRootSigHook(); // P1 Fix 4: Static sampler LOD bias

  LOG_INFO("[ShadowVT] All hooks installed via shadow vtable architecture");
}


void WrapCreatedD3D12Device(REFIID /*riid*/, void** ppDevice, bool /*takeOwnership*/) {
  if (!ppDevice || !*ppDevice) return;
  Microsoft::WRL::ComPtr<ID3D12Device> pRealDevice;
  if (FAILED((reinterpret_cast<IUnknown*>(*ppDevice))->QueryInterface(IID_PPV_ARGS(&pRealDevice)))) return;
  EnsureD3D12VTableHooks(pRealDevice.Get());
  StreamlineIntegration::Get().Initialize(pRealDevice.Get());
}

extern "C" HRESULT WINAPI Hooked_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                                   REFIID riid, void** ppDevice) {
  if (!g_OriginalD3D12CreateDevice) return E_FAIL;
  HRESULT hr = g_OriginalD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
  if (SUCCEEDED(hr) && ppDevice && *ppDevice) WrapCreatedD3D12Device(riid, ppDevice);
  return hr;
}

typedef FARPROC(WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);
PFN_GetProcAddress g_OriginalGetProcAddress = nullptr;

FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
  if (HIWORD(lpProcName) != 0) {
    if (strcmp(lpProcName, "D3D12CreateDevice") == 0) return reinterpret_cast<FARPROC>(Hooked_D3D12CreateDevice);
    if (strcmp(lpProcName, "D3D12SerializeVersionedRootSignature") == 0 && IsRootSigHookReady())
      return reinterpret_cast<FARPROC>(Hooked_D3D12SerializeVersionedRootSignature);
  }
  return g_OriginalGetProcAddress(hModule, lpProcName);
}

// ============================================================================
// PRESENT HOOK INSTALLATION — via Shadow VTable (unified with all other hooks)
// ============================================================================

void InstallPresentGhostHook(IDXGISwapChain* pSwapChain) {
  static std::atomic<bool> installed(false);
  if (installed.exchange(true)) return;
  if (!pSwapChain) return;

  // Capture original vtable pointers BEFORE installing shadow vtable
  void** vt = GetVTable(pSwapChain);
  g_OriginalPresent = reinterpret_cast<PFN_Present>(
      vt[static_cast<size_t>(vtable::SwapChain::Present)]);
  g_OriginalResizeBuffers = reinterpret_cast<PFN_ResizeBuffers>(
      vt[static_cast<size_t>(vtable::SwapChain::ResizeBuffers)]);

  // Install shadow vtable on swap chain — same approach as Device/CmdList/CmdQueue
  constexpr size_t kSwapChainVTableSize = 40; // IDXGISwapChain4 has ~40 methods
  ShadowVTable::Install(pSwapChain, kSwapChainVTableSize);

  ShadowVTable::PatchEntry(pSwapChain,
      static_cast<size_t>(vtable::SwapChain::Present),
      reinterpret_cast<void*>(Hooked_Present));
  ShadowVTable::PatchEntry(pSwapChain,
      static_cast<size_t>(vtable::SwapChain::ResizeBuffers),
      reinterpret_cast<void*>(Hooked_ResizeBuffers));

  LOG_INFO("[ShadowVT] SwapChain Present + ResizeBuffers installed via shadow vtable");
}

// ============================================================================
// HOOK STATE RESET — allows re-hooking after device recreation (Item 24)
// ============================================================================

void ResetHookState() {
  s_cmdListHooked.store(false);
  s_cmdQueueHooked.store(false);
  s_deviceHooked.store(false);
  s_resourceHooked.store(false);
  g_CmdListShadowVTable = nullptr;
  g_CmdListVTableSize = 0;
}

// ============================================================================
// PUBLIC API
// ============================================================================

void InstallD3D12Hooks() {
  static std::atomic<bool> installed(false);
  if (installed.exchange(true)) return;
  // Shadow vtable architecture — no Ghost HWBP needed at runtime.
  // Ghost hook system is retained for CI testing but not initialized here.
  LOG_INFO("[ShadowVT] Hook system ready (shadow vtable architecture)");
}

bool InitializeHooks() {
  return true;
}

void CleanupHooks() {
  // Stop timer thread first — it may be calling into hooked functions.
  StopFrameTimer();

  // Reset hook state flags so hooks can be re-installed on device recreation
  ResetHookState();

  InputHandler::Get().UninstallHook();
  ImGuiOverlay::Get().Shutdown();
  StreamlineIntegration::Get().Shutdown();
}
