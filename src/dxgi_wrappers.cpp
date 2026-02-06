#include "dxgi_wrappers.h"
#include "config_manager.h"
#include "dlss4_config.h"
#include "hooks.h"
#include "imgui_overlay.h"
#include "input_handler.h"
#include "logger.h"
#include "resource_detector.h"
#include "streamline_integration.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Global tracking
ComPtr<IDXGISwapChain> g_pRealSwapChain;
ComPtr<ID3D12CommandQueue> g_pRealCommandQueue;
static std::mutex g_swapChainMutex;

static std::unique_ptr<std::thread> g_timerThread;
static std::atomic<bool> g_timerRunning(false);
static std::condition_variable g_timerCV;
static std::mutex g_timerMutex;

// Unified frame counter — single source of truth across the proxy
static std::atomic<uint64_t> g_unifiedFrameCount(0);

static void TimerThreadProc() {
  LOG_INFO("[TIMER] Thread started");

  static bool hotkeysRegistered = false;
  if (!hotkeysRegistered) {
    ModConfig &cfg = ConfigManager::Get().Data();
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
    InputHandler::Get().InstallHook();
    hotkeysRegistered = true;
  }

  auto lastFpsTime = std::chrono::steady_clock::now();
  uint64_t lastFrameCount = 0;

  while (g_timerRunning.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> lock(g_timerMutex);
    g_timerCV.wait_for(lock, std::chrono::milliseconds(16), [] {
      return !g_timerRunning.load(std::memory_order_acquire);
    });

    if (!g_timerRunning.load(std::memory_order_acquire))
      break;

    ComPtr<IDXGISwapChain> pSwapChain;
    {
      std::lock_guard<std::mutex> scLock(g_swapChainMutex);
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

    static int hotReloadCounter = 0;
    if (++hotReloadCounter >= 100) {
      ConfigManager::Get().CheckHotReload();
      hotReloadCounter = 0;
    }

    InputHandler::Get().ProcessInput();
  }

  LOG_INFO("[TIMER] Thread exiting");
}

// Called from the D3D12 Present/submission thread — safe for GPU work
void OnPresentThread(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return;

  static bool imguiInit = false;
  if (!imguiInit) {
    ImGuiOverlay::Get().Initialize(pSwapChain);
    imguiInit = true;
  }

  g_unifiedFrameCount.fetch_add(1, std::memory_order_relaxed);

  StreamlineIntegration::Get().NewFrame(pSwapChain);
  StreamlineIntegration::Get().EvaluateFrameGen(pSwapChain);
  StreamlineIntegration::Get().EvaluateDeepDVC(pSwapChain);

  // Valhalla GUI rendering happens HERE on the GPU thread, not the timer thread
  ImGuiOverlay::Get().Render();

  StreamlineIntegration::Get().ReflexMarker(sl::PCLMarker::ePresentStart);
  StreamlineIntegration::Get().ReflexMarker(sl::PCLMarker::ePresentEnd);
}

// ============================================================================
// PRESENT HOOK — runs OnPresentThread on the GPU submission thread
// ============================================================================
static PFN_Present g_OrigPresent = nullptr;

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain *pThis, UINT SyncInterval, UINT Flags) noexcept {
  try {
    OnPresentThread(pThis);
  } catch (...) {
    // Never let an exception escape a COM callback
  }
  return g_OrigPresent(pThis, SyncInterval, Flags);
}

static void InstallPresentHook(IDXGISwapChain *pSwapChain) {
  static std::atomic<bool> installed(false);
  if (installed.exchange(true))
    return;
  if (!pSwapChain)
    return;

  // IDXGISwapChain::Present is VTable index 8
  void **vt = *reinterpret_cast<void***>(pSwapChain);
  HookManager::Get().Initialize();
  (void)HookManager::Get().CreateHook(
      vt[8], reinterpret_cast<void*>(HookedPresent), &g_OrigPresent);
  LOG_INFO("[HOOK] IDXGISwapChain::Present hook installed");
}

void StartFrameTimer() {
  if (g_timerRunning.exchange(true))
    return;
  g_timerThread = std::make_unique<std::thread>(TimerThreadProc);
}

void StopFrameTimer() {
  if (!g_timerRunning.exchange(false))
    return;
  g_timerCV.notify_all();
  if (g_timerThread && g_timerThread->joinable()) {
    g_timerThread->join();
  }
}

WrappedIDXGIFactory::WrappedIDXGIFactory(IDXGIFactory *pReal)
    : m_pReal(pReal), m_refCount(1) {
  // Note: We take ownership of the reference - do NOT AddRef here
  // The caller is giving us their reference
}

WrappedIDXGIFactory::~WrappedIDXGIFactory() {
  if (m_pReal)
    m_pReal->Release();
}

HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;

  // Always support IUnknown, IDXGIObject, and base IDXGIFactory since we wrap
  // that
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
      riid == __uuidof(IDXGIFactory)) {
    *ppvObject = this;
    AddRef();
    return S_OK;
  }

  // For higher interfaces, only claim support if the real factory supports them
  if (riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2) ||
      riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory4) ||
      riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
      riid == __uuidof(IDXGIFactory7)) {
    // Check if real factory supports this interface
    ComPtr<IUnknown> testInterface;
    if (SUCCEEDED(m_pReal->QueryInterface(riid, &testInterface))) {
      *ppvObject = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  return m_pReal->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE WrappedIDXGIFactory::AddRef() {
  return m_refCount.fetch_add(1) + 1;
}
ULONG STDMETHODCALLTYPE WrappedIDXGIFactory::Release() {
  if (m_refCount.fetch_sub(1) == 1) {
    delete this;
    return 0;
  }
  return m_refCount.load();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumAdapters(UINT A,
                                                            IDXGIAdapter **P) {
  InstallD3D12Hooks();
  return m_pReal->EnumAdapters(A, P);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChain(
    IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
    IDXGISwapChain **ppSwapChain) {
  LOG_INFO("WrappedFactory::CreateSwapChain");
  InstallD3D12Hooks();

  ComPtr<ID3D12CommandQueue> pQueue;
  if (pDevice && SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
    StreamlineIntegration::Get().SetCommandQueue(pQueue.Get());
  }

  HRESULT hr = m_pReal->CreateSwapChain(pDevice, pDesc, ppSwapChain);

  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    if (pQueue) {
      g_pRealCommandQueue = pQueue;
      ComPtr<ID3D12Device> pD3DDevice;
      if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&pD3DDevice)))) {
        StreamlineIntegration::Get().Initialize(pD3DDevice.Get());
      }
    }

    {
      std::lock_guard<std::mutex> lock(g_swapChainMutex);
      g_pRealSwapChain = *ppSwapChain;
    }
    InstallPresentHook(*ppSwapChain);
    StartFrameTimer();
  }
  return hr;
}

// ... Additional IDXGIFactory methods forwarding (shortened for brevity but
// should be fully implemented) ...
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::EnumAdapters1(UINT A, IDXGIAdapter1 **P) {
  ComPtr<IDXGIFactory1> factory1;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&factory1))))
    return E_NOINTERFACE;
  return factory1->EnumAdapters1(A, P);
}
// ... repeat pattern for other methods ...

// Implement missing required methods for IDXGIFactory7
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::MakeWindowAssociation(HWND H,
                                                                     UINT F) {
  return m_pReal->MakeWindowAssociation(H, F);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetWindowAssociation(HWND *H) {
  return m_pReal->GetWindowAssociation(H);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::CreateSoftwareAdapter(HMODULE M, IDXGIAdapter **P) {
  return m_pReal->CreateSoftwareAdapter(M, P);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::SetPrivateData(REFGUID N, UINT S,
                                                              const void *D) {
  return m_pReal->SetPrivateData(N, S, D);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::SetPrivateDataInterface(REFGUID N, const IUnknown *U) {
  return m_pReal->SetPrivateDataInterface(N, U);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetPrivateData(REFGUID N,
                                                              UINT *S,
                                                              void *D) {
  return m_pReal->GetPrivateData(N, S, D);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetParent(REFIID R, void **P) {
  return m_pReal->GetParent(R, P);
}
BOOL STDMETHODCALLTYPE WrappedIDXGIFactory::IsCurrent() {
  ComPtr<IDXGIFactory1> f;
  return (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&f)))) ? f->IsCurrent()
                                                                : FALSE;
}
BOOL STDMETHODCALLTYPE WrappedIDXGIFactory::IsWindowedStereoEnabled() {
  ComPtr<IDXGIFactory2> f;
  return (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
             ? f->IsWindowedStereoEnabled()
             : FALSE;
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChainForHwnd(
    IUnknown *pD, HWND h, const DXGI_SWAP_CHAIN_DESC1 *d,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *o,
    IDXGISwapChain1 **s) {
  ComPtr<IDXGIFactory2> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  HRESULT hr = f->CreateSwapChainForHwnd(pD, h, d, fs, o, s);
  if (SUCCEEDED(hr) && s && *s) {
    {
      std::lock_guard<std::mutex> lock(g_swapChainMutex);
      g_pRealSwapChain = *s;
    }
    InstallPresentHook(*s);
    StartFrameTimer();
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChainForCoreWindow(
    IUnknown *pD, IUnknown *w, const DXGI_SWAP_CHAIN_DESC1 *d, IDXGIOutput *o,
    IDXGISwapChain1 **s) {
  ComPtr<IDXGIFactory2> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->CreateSwapChainForCoreWindow(pD, w, d, o, s);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::GetSharedResourceAdapterLuid(HANDLE h, LUID *l) {
  ComPtr<IDXGIFactory2> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->GetSharedResourceAdapterLuid(h, l);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::RegisterStereoStatusWindow(HWND h, UINT m, DWORD *c) {
  ComPtr<IDXGIFactory2> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->RegisterStereoStatusWindow(h, m, c);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::RegisterStereoStatusEvent(HANDLE h, DWORD *c) {
  ComPtr<IDXGIFactory2> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->RegisterStereoStatusEvent(h, c);
}
void STDMETHODCALLTYPE WrappedIDXGIFactory::UnregisterStereoStatus(DWORD c) {
  ComPtr<IDXGIFactory2> f;
  if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    f->UnregisterStereoStatus(c);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::RegisterOcclusionStatusWindow(HWND h, UINT m, DWORD *c) {
  ComPtr<IDXGIFactory2> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->RegisterOcclusionStatusWindow(h, m, c);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::RegisterOcclusionStatusEvent(HANDLE h, DWORD *c) {
  ComPtr<IDXGIFactory2> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->RegisterOcclusionStatusEvent(h, c);
}
void STDMETHODCALLTYPE WrappedIDXGIFactory::UnregisterOcclusionStatus(DWORD c) {
  ComPtr<IDXGIFactory2> f;
  if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    f->UnregisterOcclusionStatus(c);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChainForComposition(
    IUnknown *pD, const DXGI_SWAP_CHAIN_DESC1 *d, IDXGIOutput *o,
    IDXGISwapChain1 **s) {
  ComPtr<IDXGIFactory2> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->CreateSwapChainForComposition(pD, d, o, s);
}
UINT STDMETHODCALLTYPE WrappedIDXGIFactory::GetCreationFlags() {
  ComPtr<IDXGIFactory3> f;
  return (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
             ? f->GetCreationFlags()
             : 0;
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumAdapterByLuid(LUID l,
                                                                 REFIID r,
                                                                 void **p) {
  ComPtr<IDXGIFactory4> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->EnumAdapterByLuid(l, r, p);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumWarpAdapter(REFIID r,
                                                               void **p) {
  ComPtr<IDXGIFactory4> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->EnumWarpAdapter(r, p);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::CheckFeatureSupport(DXGI_FEATURE fe, void *d, UINT s) {
  ComPtr<IDXGIFactory5> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->CheckFeatureSupport(fe, d, s);
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumAdapterByGpuPreference(
    UINT a, DXGI_GPU_PREFERENCE g, REFIID r, void **p) {
  ComPtr<IDXGIFactory6> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->EnumAdapterByGpuPreference(a, g, r, p);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::RegisterAdaptersChangedEvent(HANDLE h, DWORD *c) {
  ComPtr<IDXGIFactory7> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->RegisterAdaptersChangedEvent(h, c);
}
HRESULT STDMETHODCALLTYPE
WrappedIDXGIFactory::UnregisterAdaptersChangedEvent(DWORD c) {
  ComPtr<IDXGIFactory7> f;
  if (FAILED(m_pReal->QueryInterface(IID_PPV_ARGS(&f))))
    return E_NOINTERFACE;
  return f->UnregisterAdaptersChangedEvent(c);
}
