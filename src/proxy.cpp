#include "proxy.h"
#include "config_manager.h"
#include "crash_handler.h"
#include "dxgi_wrappers.h"
#include "hooks.h"
#include "logger.h"
#include <array>
#include <atomic>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

// ============================================================================
// RAII Wrapper for HMODULE - Ensures proper cleanup on initialization failure
// ============================================================================
class ModuleHandle {
public:
  ModuleHandle() = default;
  explicit ModuleHandle(HMODULE handle) : m_handle(handle) {}
  ~ModuleHandle() { Release(); }

  // Non-copyable
  ModuleHandle(const ModuleHandle&) = delete;
  ModuleHandle& operator=(const ModuleHandle&) = delete;

  // Movable
  ModuleHandle(ModuleHandle&& other) noexcept : m_handle(other.m_handle) {
    other.m_handle = nullptr;
  }
  ModuleHandle& operator=(ModuleHandle&& other) noexcept {
    if (this != &other) {
      Release();
      m_handle = other.m_handle;
      other.m_handle = nullptr;
    }
    return *this;
  }

  [[nodiscard]] HMODULE Get() const { return m_handle; }
  [[nodiscard]] bool IsValid() const { return m_handle != nullptr; }

  void Reset(HMODULE handle = nullptr) {
    Release();
    m_handle = handle;
  }

  HMODULE Detach() {
    HMODULE tmp = m_handle;
    m_handle = nullptr;
    return tmp;
  }

private:
  void Release() {
    if (m_handle) {
      FreeLibrary(m_handle);
      m_handle = nullptr;
    }
  }
  HMODULE m_handle = nullptr;
};

static std::atomic<bool> s_startupTraceEnabled(true);
// Lock hierarchy level 5 — lowest priority (Logging tier).
static std::mutex s_startupTraceMutex;
static std::ofstream s_startupTraceFile;

extern "C" void LogStartup(const char *msg) {
  if (!s_startupTraceEnabled.load(std::memory_order_relaxed))
    return;
  std::lock_guard<std::mutex> lock(s_startupTraceMutex);
  if (!s_startupTraceFile.is_open()) {
    s_startupTraceFile.open("startup_trace.log", std::ios::trunc);
  }
  if (s_startupTraceFile) {
    s_startupTraceFile << "[PROXY] " << msg << '\n';
    s_startupTraceFile.flush();
  }
}

int GetLogVerbosity() { return ConfigManager::Get().Data().system.logVerbosity; }

DXGIProxyState g_ProxyState;

static std::once_flag s_ProxyInitOnce;
static std::once_flag s_GlobalInitOnce;

void InitProxyGlobal() {
  std::call_once(s_GlobalInitOnce, []() {
    // Any global resource initialization
  });
}

void CleanupProxyGlobal() {
  std::lock_guard<std::mutex> lock(s_startupTraceMutex);
  if (s_startupTraceFile.is_open()) {
    s_startupTraceFile.close();
  }
}

bool InitializeProxy() {
  bool success = false;
  std::call_once(s_ProxyInitOnce, [&]() {
    LogStartup("InitializeProxy Execution");

    LogStartup("Setting env variables...");
    SetEnvironmentVariableW(L"NVSDK_NGX_AppId_Override", L"0");
    SetEnvironmentVariableW(L"NVSDK_NGX_ProjectID_Override", L"0");
    LogStartup("Env variables set");

    LogStartup("Initializing Logger...");
    if (!Logger::Initialize(std::string(dlss4::kLogFile))) {
      LogStartup("Logger Init Failed");
    }
    LogStartup("Logger initialized");

    // DISABLED: VEH handler may intercept game's expected exceptions
    // LogStartup("Installing Crash Handler...");
    // InstallCrashHandler();
    // LogStartup("Crash Handler installed");

    LogStartup("Loading original DXGI...");
    std::array<wchar_t, MAX_PATH> systemPath{};
    GetSystemDirectoryW(systemPath.data(), MAX_PATH);
    fs::path dxgiPath = fs::path(systemPath.data()) / "dxgi.dll";

    LOG_INFO("Loading original DXGI from: {}", dxgiPath.string());

    // Use RAII wrapper to ensure cleanup on failure
    ModuleHandle dxgiModule(LoadLibraryW(dxgiPath.c_str()));
    if (!dxgiModule.IsValid()) {
      LOG_ERROR("Failed to load original dxgi.dll! Error: {}", GetLastError());
      LogStartup("Failed to load original dxgi.dll");
      return;
    }
    LogStartup("Original DXGI loaded");

    LogStartup("Loading function pointers...");

    LogStartup(std::format("hOriginalDXGI = {:p}",
               static_cast<void*>(dxgiModule.Get())).c_str());

    // Test GetProcAddress directly
    FARPROC testProc =
        GetProcAddress(dxgiModule.Get(), "CreateDXGIFactory");
    LogStartup(std::format("CreateDXGIFactory = {:p}",
               reinterpret_cast<void*>(testProc)).c_str());

    // Store function pointers in local variables first
    auto pfnCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(
        GetProcAddress(dxgiModule.Get(), "CreateDXGIFactory"));
    auto pfnCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(
        GetProcAddress(dxgiModule.Get(), "CreateDXGIFactory1"));
    auto pfnCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
        GetProcAddress(dxgiModule.Get(), "CreateDXGIFactory2"));

    LogStartup("Got CreateDXGIFactory");

    if (!pfnCreateDXGIFactory || !pfnCreateDXGIFactory1 || !pfnCreateDXGIFactory2) {
      LOG_ERROR("Failed to get critical DXGI function pointers!");
      LogStartup("CRITICAL: Missing DXGI function pointers");
      return;
    }

    // Now commit the module and function pointers to global state
    g_ProxyState.hOriginalDXGI = dxgiModule.Detach();
    g_ProxyState.pfnCreateDXGIFactory = pfnCreateDXGIFactory;
    g_ProxyState.pfnCreateDXGIFactory1 = pfnCreateDXGIFactory1;
    g_ProxyState.pfnCreateDXGIFactory2 = pfnCreateDXGIFactory2;

    // Load remaining function pointers
#define LOAD_PROC(ptr, name)                                                   \
  g_ProxyState.ptr = reinterpret_cast<decltype(g_ProxyState.ptr)>(             \
      GetProcAddress(g_ProxyState.hOriginalDXGI, name))

    LOAD_PROC(pfnDXGIDeclareAdapterRemovalSupport,
              "DXGIDeclareAdapterRemovalSupport");
    LOAD_PROC(pfnDXGIGetDebugInterface1, "DXGIGetDebugInterface1");
    LOAD_PROC(pfnApplyCompatResolutionQuirking,
              "ApplyCompatResolutionQuirking");
    LOAD_PROC(pfnCompatString, "CompatString");
    LOAD_PROC(pfnCompatValue, "CompatValue");
    LOAD_PROC(pfnDXGIDumpJournal, "DXGIDumpJournal");
    LOAD_PROC(pfnDXGIReportAdapterConfiguration,
              "DXGIReportAdapterConfiguration");
    LOAD_PROC(pfnDXGIDisableVBlankVirtualization,
              "DXGIDisableVBlankVirtualization");
    LOAD_PROC(pfnD3DKMTCloseAdapter, "D3DKMTCloseAdapter");
    LOAD_PROC(pfnD3DKMTDestroyAllocation,
              "D3DKMTDestroyAllocation");
    LOAD_PROC(pfnD3DKMTDestroyContext, "D3DKMTDestroyContext");
    LOAD_PROC(pfnD3DKMTDestroyDevice, "D3DKMTDestroyDevice");
    LOAD_PROC(pfnD3DKMTDestroySynchronizationObject,
              "D3DKMTDestroySynchronizationObject");
    LOAD_PROC(pfnD3DKMTQueryAdapterInfo, "D3DKMTQueryAdapterInfo");
    LOAD_PROC(pfnD3DKMTSetDisplayPrivateDriverFormat,
              "D3DKMTSetDisplayPrivateDriverFormat");
    LOAD_PROC(pfnD3DKMTSignalSynchronizationObject,
              "D3DKMTSignalSynchronizationObject");
    LOAD_PROC(pfnD3DKMTUnlock, "D3DKMTUnlock");
    LOAD_PROC(pfnD3DKMTWaitForSynchronizationObject,
              "D3DKMTWaitForSynchronizationObject");
    LOAD_PROC(pfnOpenAdapter10, "OpenAdapter10");
    LOAD_PROC(pfnOpenAdapter10_2, "OpenAdapter10_2");
    LOAD_PROC(pfnSetAppCompatStringPointer,
              "SetAppCompatStringPointer");

#undef LOAD_PROC

    LogStartup("Function pointers loaded");

    LogStartup("Installing D3D12 Hooks...");
    InstallD3D12Hooks();
    LogStartup("D3D12 Hooks installed");
    g_ProxyState.initialized = true;
    success = true;
  });
  return g_ProxyState.initialized;
}

void ShutdownProxy() {
  if (g_ProxyState.hOriginalDXGI) {
    FreeLibrary(g_ProxyState.hOriginalDXGI);
    g_ProxyState.hOriginalDXGI = nullptr;
  }
  g_ProxyState.initialized = false;
  Logger::Shutdown();
}

extern "C" {

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **ppFactory) {
  LogStartup("CreateDXGIFactory called");
  if (!InitializeProxy())
    return E_FAIL;
  LogStartup("CreateDXGIFactory: Calling original");
  HRESULT hr = g_ProxyState.pfnCreateDXGIFactory(riid, ppFactory);
  LogStartup("CreateDXGIFactory: Original returned");
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
    *ppFactory = new WrappedIDXGIFactory(static_cast<IDXGIFactory*>(*ppFactory));
    LogStartup("Factory Wrapped");
  }
  return hr;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  LogStartup("CreateDXGIFactory1 called");
  if (!InitializeProxy())
    return E_FAIL;
  LogStartup("CreateDXGIFactory1: Calling original");
  HRESULT hr = g_ProxyState.pfnCreateDXGIFactory1(riid, ppFactory);
  LogStartup("CreateDXGIFactory1: Original returned");
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
    *ppFactory = new WrappedIDXGIFactory(static_cast<IDXGIFactory*>(*ppFactory));
    LogStartup("Factory1 Wrapped");
  }
  return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
  LogStartup("CreateDXGIFactory2 called");
  if (!InitializeProxy())
    return E_FAIL;
  LogStartup("CreateDXGIFactory2: Calling original");
  HRESULT hr = g_ProxyState.pfnCreateDXGIFactory2(Flags, riid, ppFactory);
  LogStartup("CreateDXGIFactory2: Original returned");
  if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
    *ppFactory = new WrappedIDXGIFactory(static_cast<IDXGIFactory*>(*ppFactory));
    LogStartup("Factory2 Wrapped");
  }
  return hr;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
  if (!InitializeProxy())
    return E_FAIL;
  return g_ProxyState.pfnDXGIDeclareAdapterRemovalSupport
             ? g_ProxyState.pfnDXGIDeclareAdapterRemovalSupport()
             : S_OK;
}

HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **pDebug) {
  if (!InitializeProxy())
    return E_FAIL;
  return g_ProxyState.pfnDXGIGetDebugInterface1
             ? g_ProxyState.pfnDXGIGetDebugInterface1(Flags, riid, pDebug)
             : E_NOINTERFACE;
}

typedef HRESULT(WINAPI *PFN_Generic)(void *a, void *b, void *c, void *d);

HRESULT WINAPI GenericForward(void *func, void *a, void *b, void *c, void *d) {
  return func ? reinterpret_cast<PFN_Generic>(func)(a, b, c, d) : E_NOINTERFACE;
}

#define STUB_FORWARD(Name)                                                     \
  HRESULT WINAPI Name(void *a, void *b, void *c, void *d) {                    \
    if (!g_ProxyState.initialized)                                             \
      InitializeProxy();                                                       \
    return GenericForward(g_ProxyState.pfn##Name, a, b, c, d);                 \
  }

HRESULT WINAPI ApplyCompatResolutionQuirking(void *a, void *b) {
  if (!g_ProxyState.initialized)
    InitializeProxy();
  return GenericForward(g_ProxyState.pfnApplyCompatResolutionQuirking, a, b,
                        nullptr, nullptr);
}
HRESULT WINAPI CompatString(void *a, void *b, void *c) {
  if (!g_ProxyState.initialized)
    InitializeProxy();
  return GenericForward(g_ProxyState.pfnCompatString, a, b, c, nullptr);
}
HRESULT WINAPI CompatValue(void *a, void *b) {
  if (!g_ProxyState.initialized)
    InitializeProxy();
  return GenericForward(g_ProxyState.pfnCompatValue, a, b, nullptr, nullptr);
}
HRESULT WINAPI DXGIDumpJournal(void *a) {
  if (!g_ProxyState.initialized)
    InitializeProxy();
  return GenericForward(g_ProxyState.pfnDXGIDumpJournal, a, nullptr, nullptr,
                        nullptr);
}
HRESULT WINAPI DXGIReportAdapterConfiguration(void *a) {
  if (!g_ProxyState.initialized)
    InitializeProxy();
  return GenericForward(g_ProxyState.pfnDXGIReportAdapterConfiguration, a,
                        nullptr, nullptr, nullptr);
}
HRESULT WINAPI DXGIDisableVBlankVirtualization() {
  if (!g_ProxyState.initialized)
    InitializeProxy();
  return GenericForward(g_ProxyState.pfnDXGIDisableVBlankVirtualization,
                        nullptr, nullptr, nullptr, nullptr);
}

#define FORWARD_1ARG(Name)                                                     \
  HRESULT WINAPI Name(void *a) {                                               \
    if (!g_ProxyState.initialized)                                             \
      InitializeProxy();                                                       \
    return GenericForward(g_ProxyState.pfn##Name, a, nullptr, nullptr,         \
                          nullptr);                                            \
  }

FORWARD_1ARG(D3DKMTCloseAdapter)
FORWARD_1ARG(D3DKMTDestroyAllocation)
FORWARD_1ARG(D3DKMTDestroyContext)
FORWARD_1ARG(D3DKMTDestroyDevice)
FORWARD_1ARG(D3DKMTDestroySynchronizationObject)
FORWARD_1ARG(D3DKMTQueryAdapterInfo)
FORWARD_1ARG(D3DKMTSetDisplayPrivateDriverFormat)
FORWARD_1ARG(D3DKMTSignalSynchronizationObject)
FORWARD_1ARG(D3DKMTUnlock)
FORWARD_1ARG(D3DKMTWaitForSynchronizationObject)

HRESULT WINAPI OpenAdapter10(void *a) {
  if (!g_ProxyState.initialized)
    InitializeProxy();
  return GenericForward(g_ProxyState.pfnOpenAdapter10, a, nullptr, nullptr,
                        nullptr);
}
HRESULT WINAPI OpenAdapter10_2(void *a) {
  if (!g_ProxyState.initialized)
    InitializeProxy();
  return GenericForward(g_ProxyState.pfnOpenAdapter10_2, a, nullptr, nullptr,
                        nullptr);
}
HRESULT WINAPI SetAppCompatStringPointer(void *a, void *b) {
  if (!g_ProxyState.initialized)
    InitializeProxy();
  return GenericForward(g_ProxyState.pfnSetAppCompatStringPointer, a, b,
                        nullptr, nullptr);
}

HRESULT WINAPI Proxy_D3D12CreateDevice(IUnknown *pAdapter,
                                       D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                       REFIID riid, void **ppDevice) {
  extern HRESULT WINAPI Hooked_D3D12CreateDevice(
      IUnknown * pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid,
      void **ppDevice);
  return Hooked_D3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid,
                                  ppDevice);
}

} // extern "C"
