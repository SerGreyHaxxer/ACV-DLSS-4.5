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
#include "dxgi_wrappers.h"
#include "config_manager.h"
#include "dlss4_config.h"
#include "hooks.h"
#include "imgui_overlay.h"
#include "input_handler.h"
#include "logger.h"
#include "resource_detector.h"
#include "shadow_vtable.h"
#include "streamline_integration.h"
#include "vtable_utils.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Global tracking
ComPtr<IDXGISwapChain> g_pRealSwapChain;
ComPtr<ID3D12CommandQueue> g_pRealCommandQueue;
// Lock hierarchy level 1 â€” highest priority.  Never hold a lower-level lock
// when acquiring this.  Order: SwapChain(1) > Hooks(2) > Resources(3) >
// Config(4) > Logging(5).
static std::mutex g_swapChainMutex;

static std::unique_ptr<std::jthread> g_timerThread;
static std::condition_variable_any g_timerCV;
// Timer mutex â€” used only for the condition variable; not part of the
// hierarchical ordering (never held while acquiring another lock).
static std::mutex g_timerMutex;

// Unified frame counter â€” single source of truth across the proxy
static std::atomic<uint64_t> g_unifiedFrameCount(0);

static void RegisterHotkeys() {
  // P1 FIX: Use snapshot â€” this runs on the timer thread, not the render thread.
  // RCU: DataSnapshot() returns shared_ptr<const ModConfig> â€” wait-free read.
  auto cfg = ConfigManager::Get().DataSnapshot();
  InputHandler::Get().RegisterHotkey(
      cfg.ui.menuHotkey, []() { ImGuiOverlay::Get().ToggleVisibility(); },
      "Toggle Menu");
  InputHandler::Get().RegisterHotkey(
      cfg.ui.fpsHotkey, []() { ImGuiOverlay::Get().ToggleFPS(); }, "Toggle FPS");
  InputHandler::Get().RegisterHotkey(
      cfg.ui.vignetteHotkey, []() { ImGuiOverlay::Get().ToggleVignette(); },
      "Toggle Vignette");
  InputHandler::Get().RegisterHotkey(
      VK_F8,
      []() {
        float jx = 0.0f, jy = 0.0f;
        StreamlineIntegration::Get().GetLastCameraJitter(jx, jy);
        bool hasCam = StreamlineIntegration::Get().HasCameraData();
        LOG_INFO("F8 Debug: Camera={} Jitter=({:.4f}, {:.4f})",
                 hasCam ? "OK" : "MISSING", jx, jy);
        ImGuiOverlay::Get().SetCameraStatus(hasCam, jx, jy);
      },
      "Debug Camera Status");
}

static void TimerThreadProc(std::stop_token stoken) {
  LOG_INFO("[TIMER] Thread started");

  // Register hotkey callbacks (idempotent â€” callbacks are only added once)
  bool hotkeysRegistered = false;

  auto lastFpsTime = std::chrono::steady_clock::now();
  uint64_t lastFrameCount = 0;
  int hotReloadCounter = 0;

  while (!stoken.stop_requested()) {
    // --- Hotkey registration (one-time) ---
    if (!hotkeysRegistered) {
      RegisterHotkeys();
      hotkeysRegistered = true;
    }

    // P1 FIX: WH_KEYBOARD_LL requires a message pump on the installing
    // thread.  This timer thread only uses condition_variable::wait_for
    // and never calls GetMessage/PeekMessage, so a LL hook installed here
    // would be dead.  Hotkeys are reliably handled via polling in
    // ProcessInput() below (called every ~16 ms).

    {
      std::unique_lock<std::mutex> lock(g_timerMutex);
      // condition_variable_any waits directly on the stop_token!
      // Instantly interrupts the 16ms sleep on shutdown.
      g_timerCV.wait_for(lock, stoken, std::chrono::milliseconds(16),
                         [] { return false; });
    }

    if (stoken.stop_requested())
      break;

    ComPtr<IDXGISwapChain> pSwapChain;
    {
      std::scoped_lock scLock(g_swapChainMutex);
      pSwapChain = g_pRealSwapChain;
    }

    if (!pSwapChain)
      continue;

    uint64_t currentCount = g_unifiedFrameCount.load(std::memory_order_relaxed);
    auto currentTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       currentTime - lastFpsTime)
                       .count();

    if (elapsed >= 1000) {
      float fps = static_cast<float>(currentCount - lastFrameCount) * 1000.0f /
                  static_cast<float>(elapsed);
      StreamlineIntegration::Get().UpdateFrameTiming(fps);
      ImGuiOverlay::Get().SetFPS(
          fps, fps * StreamlineIntegration::Get().GetFrameGenMultiplier());
      lastFpsTime = currentTime;
      lastFrameCount = currentCount;
    }

    // Timer thread handles ONLY: FPS calculation, config hot-reload, input polling.
    // GUI init and rendering are done on the Present/D3D12 submission thread.

    if (++hotReloadCounter >= 100) {
      ConfigManager::Get().CheckHotReload();
      hotReloadCounter = 0;
    }

    // ProcessInput() is the polling fallback â€” works even when the global hook
    // failed to install, ensuring hotkeys always function.
    InputHandler::Get().ProcessInput();
  }

  LOG_INFO("[TIMER] Thread exiting");
}

// Called from the D3D12 Present/submission thread â€” safe for GPU work
void OnPresentThread(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return;

  static std::once_flag s_imguiInitOnce;
  std::call_once(s_imguiInitOnce, [pSwapChain]() {
    ImGuiOverlay::Get().Initialize(pSwapChain);
  });

  g_unifiedFrameCount.fetch_add(1, std::memory_order_relaxed);

  StreamlineIntegration::Get().NewFrame(pSwapChain);
  StreamlineIntegration::Get().EvaluateDLSSFromPresent();
  StreamlineIntegration::Get().EvaluateFrameGen(pSwapChain);
  StreamlineIntegration::Get().EvaluateDeepDVC(pSwapChain);

  // Valhalla GUI rendering happens HERE on the GPU thread, not the timer thread
  ImGuiOverlay::Get().Render();

  StreamlineIntegration::Get().ReflexMarker(sl::PCLMarker::ePresentStart);
  StreamlineIntegration::Get().ReflexMarker(sl::PCLMarker::ePresentEnd);
}

// ============================================================================
// PRESENT HOOK â€” runs OnPresentThread on the GPU submission thread
// ============================================================================
// Present hook is now installed via Ghost Hook (hardware breakpoint) from hooks.cpp.
// Forward declaration of the ghost-hook-based installer.
extern void InstallPresentGhostHook(IDXGISwapChain *pSwapChain);

void StartFrameTimer() {
  if (g_timerThread)
    return; // already running
  g_timerThread = std::make_unique<std::jthread>(TimerThreadProc);
}

void StopFrameTimer() {
  if (g_timerThread) {
    g_timerThread->request_stop(); // Signals the stop token safely
    g_timerCV.notify_all();        // Wake any sleeping wait_for
    g_timerThread.reset();         // jthread automatically joins on destruction
  }
}

// ============================================================================
// Fix 1.3: DXGI Factory VTable Hooks (replaces WrappedIDXGIFactory)
// ============================================================================
// Only the 4 swap chain creation methods are hooked. All other factory
// methods execute natively through the real VTable — zero overhead.
// This preserves COM identity (game always sees the real pointer).
// ============================================================================

// Original function pointers for hooked factory methods
using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_CreateSwapChainForCoreWindow = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);
using PFN_CreateSwapChainForComposition = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
    IDXGIOutput*, IDXGISwapChain1**);

static PFN_CreateSwapChain g_OriginalFactoryCreateSwapChain = nullptr;
static PFN_CreateSwapChainForHwnd g_OriginalCreateSwapChainForHwnd = nullptr;
static PFN_CreateSwapChainForCoreWindow g_OriginalCreateSwapChainForCoreWindow = nullptr;
static PFN_CreateSwapChainForComposition g_OriginalCreateSwapChainForComposition = nullptr;

// Shared post-creation logic: store swap chain, install Present hook, start timer
static void OnSwapChainCreated(IUnknown* pDevice, IDXGISwapChain* pSwapChain) {
  if (!pSwapChain) return;

  InstallD3D12Hooks();

  ComPtr<ID3D12CommandQueue> pQueue;
  if (pDevice && SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
    StreamlineIntegration::Get().SetCommandQueue(pQueue.Get());
    g_pRealCommandQueue = pQueue;
    ComPtr<ID3D12Device> pD3DDevice;
    if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&pD3DDevice)))) {
      StreamlineIntegration::Get().Initialize(pD3DDevice.Get());
    }
  }

  {
    std::scoped_lock lock(g_swapChainMutex);
    g_pRealSwapChain = pSwapChain;
  }
  InstallPresentGhostHook(pSwapChain);
  StartFrameTimer();
}

static HRESULT STDMETHODCALLTYPE VTHook_CreateSwapChain(
    IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
    IDXGISwapChain** ppSwapChain) {
  LOG_INFO("[VTHook] CreateSwapChain");
  HRESULT hr = g_OriginalFactoryCreateSwapChain(pThis, pDevice, pDesc, ppSwapChain);
  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    OnSwapChainCreated(pDevice, *ppSwapChain);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE VTHook_CreateSwapChainForHwnd(
    IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFS, IDXGIOutput* pOutput,
    IDXGISwapChain1** ppSwapChain) {
  LOG_INFO("[VTHook] CreateSwapChainForHwnd");
  HRESULT hr = g_OriginalCreateSwapChainForHwnd(pThis, pDevice, hWnd, pDesc, pFS, pOutput, ppSwapChain);
  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    OnSwapChainCreated(pDevice, *ppSwapChain);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE VTHook_CreateSwapChainForCoreWindow(
    IDXGIFactory2* pThis, IUnknown* pDevice, IUnknown* pWindow,
    const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pOutput,
    IDXGISwapChain1** ppSwapChain) {
  LOG_INFO("[VTHook] CreateSwapChainForCoreWindow");
  HRESULT hr = g_OriginalCreateSwapChainForCoreWindow(pThis, pDevice, pWindow, pDesc, pOutput, ppSwapChain);
  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    OnSwapChainCreated(pDevice, *ppSwapChain);
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE VTHook_CreateSwapChainForComposition(
    IDXGIFactory2* pThis, IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    IDXGIOutput* pOutput, IDXGISwapChain1** ppSwapChain) {
  LOG_INFO("[VTHook] CreateSwapChainForComposition");
  HRESULT hr = g_OriginalCreateSwapChainForComposition(pThis, pDevice, pDesc, pOutput, ppSwapChain);
  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    OnSwapChainCreated(pDevice, *ppSwapChain);
  }
  return hr;
}

void InstallDXGIFactoryVTableHooks(IDXGIFactory* pFactory) {
  if (!pFactory) return;

  // Install shadow vtable — IDXGIFactory7 has ~30 methods
  constexpr size_t kFactoryVTableSize = 30;
  ShadowVTable::Install(pFactory, kFactoryVTableSize);

  // Hook CreateSwapChain (always available on IDXGIFactory)
  g_OriginalFactoryCreateSwapChain = reinterpret_cast<PFN_CreateSwapChain>(
      ShadowVTable::PatchEntry(pFactory,
          static_cast<size_t>(vtable::DXGIFactory::CreateSwapChain),
          reinterpret_cast<void*>(VTHook_CreateSwapChain)));
  LOG_INFO("[VTHook] Factory CreateSwapChain hooked");

  // Hook IDXGIFactory2+ methods if the factory supports them
  ComPtr<IDXGIFactory2> pFactory2;
  if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&pFactory2)))) {
    g_OriginalCreateSwapChainForHwnd = reinterpret_cast<PFN_CreateSwapChainForHwnd>(
        ShadowVTable::PatchEntry(pFactory,
            static_cast<size_t>(vtable::DXGIFactory::CreateSwapChainForHwnd),
            reinterpret_cast<void*>(VTHook_CreateSwapChainForHwnd)));

    g_OriginalCreateSwapChainForCoreWindow = reinterpret_cast<PFN_CreateSwapChainForCoreWindow>(
        ShadowVTable::PatchEntry(pFactory,
            static_cast<size_t>(vtable::DXGIFactory::CreateSwapChainForCoreWindow),
            reinterpret_cast<void*>(VTHook_CreateSwapChainForCoreWindow)));

    g_OriginalCreateSwapChainForComposition = reinterpret_cast<PFN_CreateSwapChainForComposition>(
        ShadowVTable::PatchEntry(pFactory,
            static_cast<size_t>(vtable::DXGIFactory::CreateSwapChainForComposition),
            reinterpret_cast<void*>(VTHook_CreateSwapChainForComposition)));

    LOG_INFO("[VTHook] Factory2+ swap chain hooks installed (Hwnd, CoreWindow, Composition)");
  }
}
