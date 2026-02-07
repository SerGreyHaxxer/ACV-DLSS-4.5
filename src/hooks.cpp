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
#include "render_passes/ray_tracing_pass.h"
#include "resource_detector.h"
#include "resource_state_tracker.h"
#include "sampler_interceptor.h"
#include "streamline_integration.h"
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

  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;

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
PFN_CreateDescriptorHeap g_OriginalCreateDescriptorHeap = nullptr;
PFN_CreateCommandList g_OriginalCreateCommandList = nullptr;

// ============================================================================
// SLOT ROTATION SCHEDULER - shares 4 HW breakpoints across 14+ hooks
// ============================================================================
// Dr0: Present       (pinned - always active)
// Dr1: ExecCmdLists  (pinned - always active)
// Dr2: Rotating slot A
// Dr3: Rotating slot B
//
// Each frame, slots A/B rotate through the remaining hooks:
//   Device hooks:    CreateSRV, CreateUAV, CreateRTV, CreateDSV, CreateCBV
//   CmdList hooks:   ResourceBarrier, Close, SetComputeRootCBV,
//                    SetGraphicsRootCBV, ClearDSV, ClearRTV
// ============================================================================

struct RotatingHookEntry {
  uintptr_t address;
  Ghost::HookCallback callback;
  const char* name;
};

static std::vector<RotatingHookEntry> g_RotatingHooks;
static std::mutex g_RotationMutex;
static std::atomic<uint32_t> g_RotationIndex{0};
static int g_PinnedSlot0 = -1; // Present
static int g_PinnedSlot1 = -1; // ExecuteCommandLists
static int g_RotatingSlotA = -1;
static int g_RotatingSlotB = -1;

static void AdvanceSlotRotation() {
  // Phase 3 perf: Throttle rotation — only swap every N frames.
  // With 18 hooks and 2 slots, a full cycle takes 9*N frames.
  // N=4 gives a ~36 frame cycle (~0.6s @ 60fps) which is fine since
  // most rotating hooks (CreateSampler, CreateDescriptorHeap, etc.)
  // don't need per-frame coverage.
  constexpr uint32_t kRotateEveryNFrames = 4;
  static std::atomic<uint32_t> s_frameCounter{0};
  uint32_t frame = s_frameCounter.fetch_add(1, std::memory_order_relaxed);
  if ((frame % kRotateEveryNFrames) != 0) return;

  std::lock_guard<std::mutex> lock(g_RotationMutex);
  if (g_RotatingHooks.empty()) return;

  auto& ghost = Ghost::HookManager::Get();
  uint32_t count = static_cast<uint32_t>(g_RotatingHooks.size());
  uint32_t idx = g_RotationIndex.fetch_add(2, std::memory_order_relaxed);
  uint32_t idxA = idx % count;
  uint32_t idxB = (idx + 1) % count;

  auto& entryA = g_RotatingHooks[idxA];
  uintptr_t addrA = entryA.address;
  Ghost::HookCallback cbA = entryA.callback;

  uintptr_t addrB = 0;
  Ghost::HookCallback cbB = nullptr;
  if (idxA != idxB) {
    auto& entryB = g_RotatingHooks[idxB];
    addrB = entryB.address;
    cbB = entryB.callback;
  }

  // Batched swap: ONE thread enumeration instead of FOUR
  ghost.SwapRotatingSlots(2, addrA, std::move(cbA), 3, addrB, std::move(cbB));
}

static void RegisterRotatingHook(uintptr_t address, Ghost::HookCallback callback, const char* name) {
  if (!address) return;
  std::lock_guard<std::mutex> lock(g_RotationMutex);
  for (auto& entry : g_RotatingHooks) {
    if (entry.address == address) return;
  }
  g_RotatingHooks.push_back({address, std::move(callback), name});
  LOG_INFO("[GHOST] Registered rotating hook: {} @ {:p}", name, reinterpret_cast<void*>(address));
}

// ============================================================================
// GHOST HOOK CALLBACKS - executed from VEH on hardware breakpoint
// ============================================================================

// --- Present (pinned Dr0) ---
static bool GhostCB_Present(CONTEXT* ctx, void* /*userData*/) {
  auto* pSwapChain = reinterpret_cast<IDXGISwapChain*>(Ghost::GetArg1(ctx));
  try {
    OnPresentThread(pSwapChain);
  } catch (...) {
    static std::atomic<uint32_t> s_errs{0};
    if (s_errs.fetch_add(1) % 300 == 0) LOG_ERROR("[GHOST] Exception in Present callback");
  }
  // Per-frame tick for Phase 3 subsystems
  DescriptorTracker_NewFrame();
  SamplerInterceptor_NewFrame();
  AdvanceSlotRotation();
  return true;
}

// --- ExecuteCommandLists (pinned Dr1) ---
static bool GhostCB_ExecuteCommandLists(CONTEXT* ctx, void* /*userData*/) {
  auto* pThis = reinterpret_cast<ID3D12CommandQueue*>(Ghost::GetArg1(ctx));

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
      StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitterX, jitterY);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in ExecuteCommandLists callback");
  }
  return true;
}

// ============================================================================
// PHASE 2: COMMAND LIST GHOST HOOK CALLBACKS (Rotating)
// ============================================================================

static bool GhostCB_ResourceBarrier(CONTEXT* ctx, void* /*userData*/) {
  auto NumBarriers = static_cast<UINT>(Ghost::GetArg2(ctx));
  auto* pBarriers = reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(Ghost::GetArg3(ctx));

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
            ResourceStateTracker_RecordTransition(pRes, pBarriers[i].Transition.StateBefore,
                                                  pBarriers[i].Transition.StateAfter);
          }
        }
      }
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in ResourceBarrier callback");
  }
  return true;
}

static bool GhostCB_Close(CONTEXT* ctx, void* /*userData*/) {
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

      // Phase 4: Three-tier jitter with validation & smoothing
      float pX = hasPattern ? patternX : std::numeric_limits<float>::quiet_NaN();
      float pY = hasPattern ? patternY : std::numeric_limits<float>::quiet_NaN();
      JitterResult jitter = JitterEngine_Update(pX, pY, found ? proj : nullptr);

      if (found) {
        UpdateCameraCache(view, proj, jitter.x, jitter.y);
        StreamlineIntegration::Get().SetCameraData(view, proj, jitter.x, jitter.y);
      } else {
        StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitter.x, jitter.y);
      }
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in Close callback");
  }
  return true;
}

static bool GhostCB_SetGraphicsRootCbv(CONTEXT* ctx, void* /*userData*/) {
  auto BufferLocation = static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(Ghost::GetArg3(ctx));
  try {
    TrackRootCbvAddress(BufferLocation);
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in SetGraphicsRootCbv");
  }
  return true;
}

static bool GhostCB_SetComputeRootCbv(CONTEXT* ctx, void* /*userData*/) {
  auto BufferLocation = static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(Ghost::GetArg3(ctx));
  try {
    TrackRootCbvAddress(BufferLocation);
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in SetComputeRootCbv");
  }
  return true;
}

static bool GhostCB_ClearDSV(CONTEXT* ctx, void* /*userData*/) {
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;
  dsv.ptr = static_cast<SIZE_T>(Ghost::GetArg2(ctx));
  try {
    ID3D12Resource* pResource = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (TryResolveDescriptorResource(dsv, &pResource, &fmt) && pResource) {
      ResourceDetector::Get().RegisterDepthFromClear(pResource, 1.0f);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in ClearDSV");
  }
  return true;
}

static bool GhostCB_ClearRTV(CONTEXT* ctx, void* /*userData*/) {
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
  rtv.ptr = static_cast<SIZE_T>(Ghost::GetArg2(ctx));
  try {
    ID3D12Resource* pResource = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (TryResolveDescriptorResource(rtv, &pResource, &fmt) && pResource) {
      ResourceDetector::Get().RegisterColorFromClear(pResource);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in ClearRTV");
  }
  return true;
}

// ============================================================================
// DEVICE VIEW-CREATION GHOST CALLBACKS (Rotating)
// ============================================================================

static bool GhostCB_CreateSRV(CONTEXT* ctx, void* /*userData*/) {
  auto* pResource = reinterpret_cast<ID3D12Resource*>(Ghost::GetArg2(ctx));
  auto* pDesc = reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(Ghost::GetArg3(ctx));
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  handle.ptr = static_cast<SIZE_T>(Ghost::GetArg4(ctx));
  try {
    if (pResource) {
      DXGI_FORMAT fmt = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
      TrackDescriptorResource(handle, pResource, fmt);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateSRV");
  }
  return true;
}

static bool GhostCB_CreateUAV(CONTEXT* ctx, void* /*userData*/) {
  auto* pResource = reinterpret_cast<ID3D12Resource*>(Ghost::GetArg2(ctx));
  // UAV: pDesc is arg4 (R9), DestDescriptor is arg5 (stack RSP+0x28)
  auto* pDesc = reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(Ghost::GetArg4(ctx));
#ifdef _WIN64
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  handle.ptr = *reinterpret_cast<SIZE_T*>(ctx->Rsp + 0x28);
#else
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  handle.ptr = *reinterpret_cast<SIZE_T*>(ctx->Esp + 0x18);
#endif
  try {
    if (pResource) {
      DXGI_FORMAT fmt = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
      TrackDescriptorResource(handle, pResource, fmt);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateUAV");
  }
  return true;
}

static bool GhostCB_CreateRTV(CONTEXT* ctx, void* /*userData*/) {
  auto* pResource = reinterpret_cast<ID3D12Resource*>(Ghost::GetArg2(ctx));
  auto* pDesc = reinterpret_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(Ghost::GetArg3(ctx));
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  handle.ptr = static_cast<SIZE_T>(Ghost::GetArg4(ctx));
  try {
    if (pResource) {
      DXGI_FORMAT fmt = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
      TrackDescriptorResource(handle, pResource, fmt);
      ResourceDetector::Get().RegisterResource(pResource, true);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateRTV");
  }
  return true;
}

static bool GhostCB_CreateDSV(CONTEXT* ctx, void* /*userData*/) {
  auto* pResource = reinterpret_cast<ID3D12Resource*>(Ghost::GetArg2(ctx));
  auto* pDesc = reinterpret_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(Ghost::GetArg3(ctx));
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  handle.ptr = static_cast<SIZE_T>(Ghost::GetArg4(ctx));
  try {
    if (pResource) {
      DXGI_FORMAT fmt = pDesc ? pDesc->Format : DXGI_FORMAT_UNKNOWN;
      TrackDescriptorResource(handle, pResource, fmt);
      ResourceDetector::Get().RegisterDepthFromView(pResource, fmt);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateDSV");
  }
  return true;
}

// Phase 2.5: NEW CreateConstantBufferView hook
static bool GhostCB_CreateCBV(CONTEXT* ctx, void* /*userData*/) {
  auto* pDesc = reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(Ghost::GetArg2(ctx));
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  handle.ptr = static_cast<SIZE_T>(Ghost::GetArg3(ctx));
  try {
    if (pDesc) {
      TrackCbvDescriptor(handle, pDesc);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateCBV");
  }
  return true;
}

// ============================================================================
// PHASE 3: NEW GHOST HOOK CALLBACKS - completing roadmap 0.2
// ============================================================================

// --- CreateSampler (Rotating) ---
static bool GhostCB_CreateSampler(CONTEXT* ctx, void* /*userData*/) {
  auto* pDevice = reinterpret_cast<ID3D12Device*>(Ghost::GetArg1(ctx));
  auto* pDesc = reinterpret_cast<const D3D12_SAMPLER_DESC*>(Ghost::GetArg2(ctx));
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  handle.ptr = static_cast<SIZE_T>(Ghost::GetArg3(ctx));
  try {
    if (pDesc && pDevice && handle.ptr) {
      RegisterSampler(*pDesc, handle, pDevice);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateSampler");
  }
  return true;
}

// --- CreateDescriptorHeap (Rotating) ---
static bool GhostCB_CreateDescriptorHeap(CONTEXT* ctx, void* /*userData*/) {
  // We intercept at entry. The heap is not created yet, but we can track
  // descriptor heaps when they are used by view-creation hooks.
  // This callback is a pre-call observer: we log the creation request.
  auto* pDevice = reinterpret_cast<ID3D12Device*>(Ghost::GetArg1(ctx));
  auto* pDesc = reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(Ghost::GetArg2(ctx));
  try {
    if (pDesc && pDevice) {
      static std::atomic<uint32_t> s_heapCount{0};
      uint32_t count = s_heapCount.fetch_add(1, std::memory_order_relaxed);
      if (count < 50) { // Log first 50 heaps
        LOG_DEBUG("[GHOST] CreateDescriptorHeap: Type={}, NumDescriptors={}, Flags={}", static_cast<int>(pDesc->Type),
                  pDesc->NumDescriptors, static_cast<int>(pDesc->Flags));
      }
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateDescriptorHeap");
  }
  return true;
}

// --- CreateCommittedResource (Rotating) ---
static bool GhostCB_CreateCommittedResource(CONTEXT* ctx, void* /*userData*/) {
  // Pre-call observer: extract resource description for early classification.
  // Arg1=pDevice, Arg2=pHeapProperties, Arg3=HeapFlags, Arg4=pDesc(R9)
  auto* pDesc = reinterpret_cast<const D3D12_RESOURCE_DESC*>(Ghost::GetArg4(ctx));
  try {
    if (pDesc) {
      // Early classification: log large render targets and depth buffers
      bool isRT = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
      bool isDS = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
      bool isUAV = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
      if ((isRT || isDS || isUAV) && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        static std::atomic<uint32_t> s_logCount{0};
        if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 100) {
          LOG_DEBUG("[GHOST] CreateCommittedResource: {}x{} Fmt={} RT={} DS={} UAV={}",
                    static_cast<uint32_t>(pDesc->Width), pDesc->Height, static_cast<int>(pDesc->Format), isRT, isDS,
                    isUAV);
        }
      }
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateCommittedResource");
  }
  return true;
}

// --- CreatePlacedResource (Rotating) ---
static bool GhostCB_CreatePlacedResource(CONTEXT* ctx, void* /*userData*/) {
  // Pre-call observer: pDesc is arg4 (R9) for CreatePlacedResource
  // Arg1=pDevice, Arg2=pHeap, Arg3=HeapOffset, Arg4=pDesc(R9)
  auto* pDesc = reinterpret_cast<const D3D12_RESOURCE_DESC*>(Ghost::GetArg4(ctx));
  try {
    if (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
      bool isRT = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
      bool isDS = (pDesc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
      if (isRT || isDS) {
        static std::atomic<uint32_t> s_logCount{0};
        if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 50) {
          LOG_DEBUG("[GHOST] CreatePlacedResource: {}x{} Fmt={} RT={} DS={}", static_cast<uint32_t>(pDesc->Width),
                    pDesc->Height, static_cast<int>(pDesc->Format), isRT, isDS);
        }
      }
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreatePlacedResource");
  }
  return true;
}

// --- CreateCommandList (Rotating) ---
static bool GhostCB_CreateCommandList(CONTEXT* /*ctx*/, void* /*userData*/) {
  // Pre-call observer: command list vtables are already captured from
  // the initial temporary object in EnsureD3D12VTableHooks. This hook
  // serves as a diagnostic counter to track command list creation rate.
  try {
    static std::atomic<uint64_t> s_cmdListCreated{0};
    uint64_t count = s_cmdListCreated.fetch_add(1, std::memory_order_relaxed);
    if (count % 1000 == 0 && count > 0) {
      LOG_DEBUG("[GHOST] CommandList creation milestone: {} total", count);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateCommandList");
  }
  return true;
}

// --- CreateCommandQueue (Rotating) ---
static bool GhostCB_CreateCommandQueue(CONTEXT* /*ctx*/, void* /*userData*/) {
  // Pre-call observer: queue vtables are already captured from
  // the initial temporary object in EnsureD3D12VTableHooks. This hook
  // serves as a diagnostic counter.
  try {
    static std::atomic<uint64_t> s_queueCreated{0};
    uint64_t count = s_queueCreated.fetch_add(1, std::memory_order_relaxed);
    if (count > 0) {
      LOG_DEBUG("[GHOST] CommandQueue created (total: {})", count);
    }
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in CreateCommandQueue");
  }
  return true;
}

// --- ResizeBuffers (Rotating) ---
static bool GhostCB_ResizeBuffers(CONTEXT* ctx, void* /*userData*/) {
  // Arg1=pSwapChain, Arg2=BufferCount, Arg3=Width, Arg4=Height
  // Stack: NewFormat(RSP+0x28), Flags(RSP+0x30)
  auto Width = static_cast<UINT>(Ghost::GetArg3(ctx));
  auto Height = static_cast<UINT>(Ghost::GetArg4(ctx));
  try {
    // Notify overlay of pending resize
    if (Width > 0 && Height > 0) {
      ImGuiOverlay::Get().OnResize(Width, Height);
      ResourceDetector::Get().SetExpectedDimensions(Width, Height);
      LOG_INFO("[GHOST] ResizeBuffers: {}x{}", Width, Height);
    } else {
      LOG_DEBUG("[GHOST] ResizeBuffers: auto-size (0x0)");
    }

    // Reset camera scan cache on resolution change
    ResetCameraScanCache();
  } catch (...) {
    LOG_ERROR("[GHOST] Exception in ResizeBuffers");
  }
  return true;
}

// ============================================================================
// DEVICE HOOK INSTALLATION - captures vtable pointers + registers ghost hooks
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
  g_OriginalCreateDescriptorHeap = reinterpret_cast<PFN_CreateDescriptorHeap>(
      GetVTableEntry<void*, vtable::Device>(devVt, vtable::Device::CreateDescriptorHeap));

  // Register as rotating ghost hooks (existing)
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateSRV), GhostCB_CreateSRV, "CreateSRV");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateUAV), GhostCB_CreateUAV, "CreateUAV");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateRTV), GhostCB_CreateRTV, "CreateRTV");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateDSV), GhostCB_CreateDSV, "CreateDSV");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateCBV), GhostCB_CreateCBV, "CreateCBV");

  // Register new Phase 3 rotating hooks
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateSampler), GhostCB_CreateSampler, "CreateSampler");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateDescriptorHeap), GhostCB_CreateDescriptorHeap,
                       "CreateDescHeap");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateCommittedResource), GhostCB_CreateCommittedResource,
                       "CreateCommitted");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreatePlacedResource), GhostCB_CreatePlacedResource,
                       "CreatePlaced");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateCommandList), GhostCB_CreateCommandList,
                       "CreateCmdList");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalCreateCommandQueue), GhostCB_CreateCommandQueue,
                       "CreateCmdQueue");

  LOG_INFO("[GHOST] Device vtable pointers captured ({} rotating hooks)", g_RotatingHooks.size());
}

static void CaptureCommandQueueVTable(ID3D12CommandQueue* pQueue) {
  if (s_cmdQueueHooked.exchange(true)) return;

  void** queueVt = GetVTable(pQueue);
  auto fnExecCmdLists = reinterpret_cast<uintptr_t>(
      GetVTableEntry<void*, vtable::CommandQueue>(queueVt, vtable::CommandQueue::ExecuteCommandLists));
  g_OriginalExecuteCommandLists = reinterpret_cast<PFN_ExecuteCommandLists>(fnExecCmdLists);

  auto& ghost = Ghost::HookManager::Get();
  g_PinnedSlot1 = ghost.InstallHook(fnExecCmdLists, GhostCB_ExecuteCommandLists);
  if (g_PinnedSlot1 >= 0)
    LOG_INFO("[GHOST] ExecuteCommandLists pinned to Dr{}", g_PinnedSlot1);
  else
    LOG_ERROR("[GHOST] Failed to pin ExecuteCommandLists");
}

static void CaptureCommandListVTable(ID3D12GraphicsCommandList* pList) {
  if (s_cmdListHooked.exchange(true)) return;

  void** cmdVt = GetVTable(pList);

  g_OriginalClose =
      reinterpret_cast<PFN_Close>(GetVTableEntry<void*, vtable::CommandList>(cmdVt, vtable::CommandList::Close));
  g_OriginalResourceBarrier = reinterpret_cast<PFN_ResourceBarrier>(
      GetVTableEntry<void*, vtable::CommandList>(cmdVt, vtable::CommandList::ResourceBarrier));
  g_OriginalSetGraphicsRootCbv = reinterpret_cast<PFN_SetGraphicsRootConstantBufferView>(
      GetVTableEntry<void*, vtable::CommandList>(cmdVt, vtable::CommandList::SetGraphicsRootConstantBufferView));
  g_OriginalSetComputeRootCbv = reinterpret_cast<PFN_SetComputeRootConstantBufferView>(
      GetVTableEntry<void*, vtable::CommandList>(cmdVt, vtable::CommandList::SetComputeRootConstantBufferView));
  g_OriginalClearDSV = reinterpret_cast<PFN_ClearDepthStencilView>(
      GetVTableEntry<void*, vtable::CommandList>(cmdVt, vtable::CommandList::ClearDepthStencilView));
  g_OriginalClearRTV = reinterpret_cast<PFN_ClearRenderTargetView>(
      GetVTableEntry<void*, vtable::CommandList>(cmdVt, vtable::CommandList::ClearRenderTargetView));

  // Register ALL command list hooks as rotating
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalResourceBarrier), GhostCB_ResourceBarrier,
                       "ResourceBarrier");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalClose), GhostCB_Close, "Close");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalSetGraphicsRootCbv), GhostCB_SetGraphicsRootCbv,
                       "SetGfxRootCBV");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalSetComputeRootCbv), GhostCB_SetComputeRootCbv,
                       "SetCmpRootCBV");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalClearDSV), GhostCB_ClearDSV, "ClearDSV");
  RegisterRotatingHook(reinterpret_cast<uintptr_t>(g_OriginalClearRTV), GhostCB_ClearRTV, "ClearRTV");

  LOG_INFO("[GHOST] CommandList vtable captured ({} total rotating hooks)", g_RotatingHooks.size());
}

// ============================================================================
// DEVICE HOOKS ENTRY POINT
// ============================================================================

void EnsureD3D12VTableHooks(ID3D12Device* pDevice) {
  if (!pDevice) return;
  if (s_deviceHooked.exchange(true)) return;

  Ghost::HookManager::Get().Initialize();
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
      tmpList->Close();
    }
  }

  StreamlineIntegration::Get().Initialize(pDevice);
  AdvanceSlotRotation();

  LOG_INFO("[GHOST] All hooks installed - {} pinned, {} rotating",
           (g_PinnedSlot0 >= 0 ? 1 : 0) + (g_PinnedSlot1 >= 0 ? 1 : 0), g_RotatingHooks.size());
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
  }
  return g_OriginalGetProcAddress(hModule, lpProcName);
}

// ============================================================================
// PRESENT HOOK INSTALLATION - via Ghost Hook (Dr0, pinned) with vtable fallback
// ============================================================================

// Fallback Present function pointer (Phase 1.6: shadow vtable approach)
static PFN_Present g_OrigPresent_Fallback = nullptr;

static HRESULT STDMETHODCALLTYPE HookedPresent_Fallback(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags) noexcept {
  try {
    OnPresentThread(pThis);
  } catch (...) {
    static std::atomic<uint32_t> s_errs{0};
    if (s_errs.fetch_add(1) % 300 == 0) LOG_ERROR("[GHOST] Exception in fallback Present");
  }
  AdvanceSlotRotation();
  if (!g_OrigPresent_Fallback) return E_FAIL;
  return g_OrigPresent_Fallback(pThis, SyncInterval, Flags);
}

void InstallPresentGhostHook(IDXGISwapChain* pSwapChain) {
  static std::atomic<bool> installed(false);
  if (installed.exchange(true)) return;
  if (!pSwapChain) return;

  auto& ghost = Ghost::HookManager::Get();
  if (!ghost.IsInitialized()) ghost.Initialize();

  void** vt = GetVTable(pSwapChain);
  auto fnPresent = reinterpret_cast<uintptr_t>(vt[static_cast<size_t>(vtable::SwapChain::Present)]);
  g_OriginalPresent = reinterpret_cast<PFN_Present>(fnPresent);

  // Phase 3: Capture ResizeBuffers vtable pointer and register as rotating hook
  auto fnResizeBuffers = reinterpret_cast<uintptr_t>(vt[static_cast<size_t>(vtable::SwapChain::ResizeBuffers)]);
  g_OriginalResizeBuffers = reinterpret_cast<PFN_ResizeBuffers>(fnResizeBuffers);
  RegisterRotatingHook(fnResizeBuffers, GhostCB_ResizeBuffers, "ResizeBuffers");
  LOG_INFO("[GHOST] ResizeBuffers registered as rotating hook ({:p})", reinterpret_cast<void*>(fnResizeBuffers));

  g_PinnedSlot0 = ghost.InstallHook(fnPresent, GhostCB_Present);
  if (g_PinnedSlot0 >= 0) {
    LOG_INFO("[GHOST] Present pinned to Dr{} (address {:p})", g_PinnedSlot0, reinterpret_cast<void*>(fnPresent));
  } else {
    // Phase 1.6: Fallback to vtable swap if ghost hook fails
    LOG_WARN("[GHOST] Present ghost hook failed - using vtable swap fallback");
    void** realVt = *reinterpret_cast<void***>(pSwapChain);
    DWORD oldProtect;
    if (VirtualProtect(&realVt[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
      g_OrigPresent_Fallback = reinterpret_cast<PFN_Present>(realVt[8]);
      realVt[8] = reinterpret_cast<void*>(HookedPresent_Fallback);
      VirtualProtect(&realVt[8], sizeof(void*), oldProtect, &oldProtect);
      LOG_WARN("[GHOST] Present installed via vtable swap fallback");
    } else {
      LOG_ERROR("[GHOST] Present hook failed entirely");
      installed.store(false);
    }
  }
}

// ============================================================================
// PUBLIC API
// ============================================================================

void InstallD3D12Hooks() {
  static std::atomic<bool> installed(false);
  if (installed.exchange(true)) return;
  // Initialize Ghost Hook system (VEH + hardware breakpoints).
  // Zero code modification. Zero vtable patching. Invisible to integrity checks.
  Ghost::HookManager::Get().Initialize();
  LOG_INFO("[GHOST] Hook system initialized (hardware breakpoints only - no MinHook)");
}

bool InitializeHooks() {
  return true;
}

void CleanupHooks() {
  {
    std::lock_guard<std::mutex> lock(g_RotationMutex);
    g_RotatingHooks.clear();
  }
  Ghost::HookManager::Get().Shutdown();
  InputHandler::Get().UninstallHook();
  ImGuiOverlay::Get().Shutdown();
  StreamlineIntegration::Get().Shutdown();
}