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
// C++20: Zero-cost RAII HMODULE wrapper using unique_ptr + custom deleter
// Replaces the 45-line ModuleHandle class with a 4-line zero-overhead abstraction
// ============================================================================
struct ModuleDeleter {
  void operator()(HMODULE h) const noexcept { if (h) ::FreeLibrary(h); }
};
using UniqueModule = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleDeleter>;

static std::atomic<bool> s_startupTraceEnabled(true);
// Lock hierarchy level 5 â€” lowest priority (Logging tier).
static std::mutex s_startupTraceMutex;
static std::ofstream s_startupTraceFile;

extern "C" void LogStartup(const char *msg) {
  if (!s_startupTraceEnabled.load(std::memory_order_relaxed))
    return;
  std::scoped_lock lock(s_startupTraceMutex);
  if (!s_startupTraceFile.is_open()) {
    s_startupTraceFile.open("startup_trace.log", std::ios::trunc);
  }
  if (s_startupTraceFile) {
    s_startupTraceFile << std::format("[PROXY] {}\n", msg);
    s_startupTraceFile.flush();
  }
}

int GetLogVerbosity() { return ConfigManager::Get().Data().system.logVerbosity; }

DXGIProxyState g_ProxyState;

static std::once_flag s_GlobalInitOnce;

void InitProxyGlobal() {
  std::call_once(s_GlobalInitOnce, []() {
    // Any global resource initialization
  });
}

void CleanupProxyGlobal() {
  std::scoped_lock lock(s_startupTraceMutex);
  if (s_startupTraceFile.is_open()) {
    s_startupTraceFile.close();
  }
}

bool InitializeProxy() {
  bool success = false;
  std::call_once(g_ProxyState.initFlag, [&]() {
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

    // Modern exception-safe loading via UniqueModule (unique_ptr<HMODULE>)
    UniqueModule dxgiModule(LoadLibraryW(dxgiPath.c_str()));
    if (!dxgiModule) {
      LOG_ERROR("Failed to load original dxgi.dll! Error: {}", GetLastError());
      LogStartup("Failed to load original dxgi.dll");
      return;
    }
    LogStartup("Original DXGI loaded");

    LogStartup("Loading function pointers...");

    LogStartup(std::format("hOriginalDXGI = {:p}",
               static_cast<void*>(dxgiModule.get())).c_str());

    // Test GetProcAddress directly
    FARPROC testProc =
        GetProcAddress(dxgiModule.get(), "CreateDXGIFactory");
    LogStartup(std::format("CreateDXGIFactory = {:p}",
               reinterpret_cast<void*>(testProc)).c_str());

    // Store function pointers in local variables first
    auto pfnCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(
        GetProcAddress(dxgiModule.get(), "CreateDXGIFactory"));
    auto pfnCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(
        GetProcAddress(dxgiModule.get(), "CreateDXGIFactory1"));
    auto pfnCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
        GetProcAddress(dxgiModule.get(), "CreateDXGIFactory2"));

    LogStartup("Got CreateDXGIFactory");

    if (!pfnCreateDXGIFactory || !pfnCreateDXGIFactory1 || !pfnCreateDXGIFactory2) {
      LOG_ERROR("Failed to get critical DXGI function pointers!");
      LogStartup("CRITICAL: Missing DXGI function pointers");
      return;
    }

    // Now commit the module and function pointers to global state
    // Release ownership from smart pointer — global state takes over lifetime
    g_ProxyState.hOriginalDXGI = dxgiModule.release();
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
    g_ProxyState.initComplete = true;
    success = true;
  });
  return g_ProxyState.initComplete;
}

void ShutdownProxy() {
  if (g_ProxyState.hOriginalDXGI) {
    FreeLibrary(g_ProxyState.hOriginalDXGI);
    g_ProxyState.hOriginalDXGI = nullptr;
  }
  g_ProxyState.initComplete = false;
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
    InstallDXGIFactoryVTableHooks(static_cast<IDXGIFactory*>(*ppFactory));
    LogStartup("Factory VTable Hooked");
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
    InstallDXGIFactoryVTableHooks(static_cast<IDXGIFactory*>(*ppFactory));
    LogStartup("Factory1 VTable Hooked");
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
    InstallDXGIFactoryVTableHooks(static_cast<IDXGIFactory*>(*ppFactory));
    LogStartup("Factory2 VTable Hooked");
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

// ============================================================================
// Type-Safe Forwarding Stubs (P0 Fix: Eliminates x64 ABI UB)
// ============================================================================
// The old GenericForward blindly cast all functions to a 4-arg void* signature.
// If the real function takes 0-2 args, calling with 4 trashes R8/R9 registers.
// Each stub now matches the exact argument count of the original function.
// ============================================================================

typedef HRESULT(WINAPI* PFN_2Arg)(void*, void*);
typedef HRESULT(WINAPI* PFN_3Arg)(void*, void*, void*);
typedef HRESULT(WINAPI* PFN_1Arg)(void*);
typedef HRESULT(WINAPI* PFN_0Arg)();

HRESULT WINAPI ApplyCompatResolutionQuirking(void *a, void *b) {
  if (!g_ProxyState.initComplete)
    InitializeProxy();
  auto fn = reinterpret_cast<PFN_2Arg>(g_ProxyState.pfnApplyCompatResolutionQuirking);
  return fn ? fn(a, b) : E_NOINTERFACE;
}
HRESULT WINAPI CompatString(void *a, void *b, void *c) {
  if (!g_ProxyState.initComplete)
    InitializeProxy();
  auto fn = reinterpret_cast<PFN_3Arg>(g_ProxyState.pfnCompatString);
  return fn ? fn(a, b, c) : E_NOINTERFACE;
}
HRESULT WINAPI CompatValue(void *a, void *b) {
  if (!g_ProxyState.initComplete)
    InitializeProxy();
  auto fn = reinterpret_cast<PFN_2Arg>(g_ProxyState.pfnCompatValue);
  return fn ? fn(a, b) : E_NOINTERFACE;
}
HRESULT WINAPI DXGIDumpJournal(void *a) {
  if (!g_ProxyState.initComplete)
    InitializeProxy();
  auto fn = reinterpret_cast<PFN_1Arg>(g_ProxyState.pfnDXGIDumpJournal);
  return fn ? fn(a) : E_NOINTERFACE;
}
HRESULT WINAPI DXGIReportAdapterConfiguration(void *a) {
  if (!g_ProxyState.initComplete)
    InitializeProxy();
  auto fn = reinterpret_cast<PFN_1Arg>(g_ProxyState.pfnDXGIReportAdapterConfiguration);
  return fn ? fn(a) : E_NOINTERFACE;
}
HRESULT WINAPI DXGIDisableVBlankVirtualization() {
  if (!g_ProxyState.initComplete)
    InitializeProxy();
  auto fn = reinterpret_cast<PFN_0Arg>(g_ProxyState.pfnDXGIDisableVBlankVirtualization);
  return fn ? fn() : E_NOINTERFACE;
}

// 1-arg D3DKMT forwarding stubs — each matches exact original signature
#define FORWARD_1ARG_SAFE(Name)                                                \
  HRESULT WINAPI Name(void *a) {                                               \
    if (!g_ProxyState.initComplete)             \
      InitializeProxy();                                                       \
    auto fn = reinterpret_cast<PFN_1Arg>(g_ProxyState.pfn##Name);              \
    return fn ? fn(a) : E_NOINTERFACE;                                         \
  }

FORWARD_1ARG_SAFE(D3DKMTCloseAdapter)
FORWARD_1ARG_SAFE(D3DKMTDestroyAllocation)
FORWARD_1ARG_SAFE(D3DKMTDestroyContext)
FORWARD_1ARG_SAFE(D3DKMTDestroyDevice)
FORWARD_1ARG_SAFE(D3DKMTDestroySynchronizationObject)
FORWARD_1ARG_SAFE(D3DKMTQueryAdapterInfo)
FORWARD_1ARG_SAFE(D3DKMTSetDisplayPrivateDriverFormat)
FORWARD_1ARG_SAFE(D3DKMTSignalSynchronizationObject)
FORWARD_1ARG_SAFE(D3DKMTUnlock)
FORWARD_1ARG_SAFE(D3DKMTWaitForSynchronizationObject)

HRESULT WINAPI OpenAdapter10(void *a) {
  if (!g_ProxyState.initComplete)
    InitializeProxy();
  auto fn = reinterpret_cast<PFN_1Arg>(g_ProxyState.pfnOpenAdapter10);
  return fn ? fn(a) : E_NOINTERFACE;
}
HRESULT WINAPI OpenAdapter10_2(void *a) {
  if (!g_ProxyState.initComplete)
    InitializeProxy();
  auto fn = reinterpret_cast<PFN_1Arg>(g_ProxyState.pfnOpenAdapter10_2);
  return fn ? fn(a) : E_NOINTERFACE;
}
HRESULT WINAPI SetAppCompatStringPointer(void *a, void *b) {
  if (!g_ProxyState.initComplete)
    InitializeProxy();
  auto fn = reinterpret_cast<PFN_2Arg>(g_ProxyState.pfnSetAppCompatStringPointer);
  return fn ? fn(a, b) : E_NOINTERFACE;
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

