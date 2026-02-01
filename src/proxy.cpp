#include "proxy.h"
#include "logger.h"
#include "hooks.h"
#include "dxgi_wrappers.h"
#include <string>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h> // Added for startup logging
#include <atomic>

// Simple startup logger to debug early crashes (Duplicated from main.cpp)
// Force C linkage to be safe
extern "C" void LogStartup(const char* msg) {
    FILE* fp;
    if (fopen_s(&fp, "startup_trace.log", "a") == 0) {
        fprintf(fp, "[PROXY] %s\n", msg);
        fclose(fp);
    }
}

// Global proxy state instance
DXGIProxyState g_ProxyState;

// ============================================================================
// PROXY INITIALIZATION
// ============================================================================

static CRITICAL_SECTION s_InitCS;
static std::atomic<bool> s_CSInited(false);
static std::atomic<bool> s_LoggerInitAttempted(false);

void InitProxyGlobal() {
    bool expected = false;
    if (s_CSInited.compare_exchange_strong(expected, true)) {
        InitializeCriticalSection(&s_InitCS);
    }
}

void CleanupProxyGlobal() {
    bool expected = true;
    if (s_CSInited.compare_exchange_strong(expected, false)) {
        DeleteCriticalSection(&s_InitCS);
    }
}

bool InitializeProxy() {
    LogStartup("InitializeProxy Entry");
    if (!s_CSInited.load()) {
        LogStartup("CRITICAL: Proxy Global CS not initialized!");
        return false;
    }

    EnterCriticalSection(&s_InitCS);
    LogStartup("InitializeProxy Lock Acquired");
    
    // FORCE APP ID OVERRIDE via Environment Variables
    // This bypasses NGX constraints by pretending to be a Generic/Dev App
    SetEnvironmentVariableW(L"NVSDK_NGX_AppId_Override", L"0");
    SetEnvironmentVariableW(L"NVSDK_NGX_ProjectID_Override", L"0");
    LogStartup("Environment Variables Set: AppId=0");

    // CRITICAL: Check if d3d12.dll is already loaded (game created device before us)
    HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
    if (hD3D12) {
        LOG_ERROR("[MFG] CRITICAL: d3d12.dll already loaded! Game created D3D12 device before proxy initialized!");
        LOG_ERROR("[MFG] Resource detection will NOT work - timing issue!");
    } else {
        LOG_INFO("[MFG] d3d12.dll not yet loaded - proxy loaded first (good!)");
    }
    if (!s_LoggerInitAttempted.exchange(true)) {
        if (!Logger::Instance().Initialize(DLSS4_LOG_FILE)) {
            LogStartup("Logger Init Failed");
            OutputDebugStringA("DLSS4 Proxy: Failed to initialize logger\n");
        } else {
            LogStartup("Logger Initialized");
        }
    }

    if (g_ProxyState.initialized) {
        LeaveCriticalSection(&s_InitCS);
        LogStartup("InitializeProxy Already Init");
        return true;
    }

    // Get System32 path
    wchar_t systemPath[MAX_PATH];
    GetSystemDirectoryW(systemPath, MAX_PATH);
    
    std::wstring dxgiPath = std::wstring(systemPath) + L"\\dxgi.dll";
    
    LogStartup("Loading real DXGI...");
    LOG_INFO("Loading original DXGI from: %ls", dxgiPath.c_str());
    
    // Load the real DXGI.dll from System32
    g_ProxyState.hOriginalDXGI = LoadLibraryW(dxgiPath.c_str());
    if (!g_ProxyState.hOriginalDXGI) {
        LogStartup("Failed to load original DXGI!");
        LOG_ERROR("Failed to load original dxgi.dll! Error: %d", GetLastError());
        LeaveCriticalSection(&s_InitCS);
        return false;
    }
    
    LogStartup("GetProcAddress Start");
    
    // Get all the function pointers we need to forward
    g_ProxyState.pfnCreateDXGIFactory = (PFN_CreateDXGIFactory)
        GetProcAddress(g_ProxyState.hOriginalDXGI, "CreateDXGIFactory");
    g_ProxyState.pfnCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)
        GetProcAddress(g_ProxyState.hOriginalDXGI, "CreateDXGIFactory1");
    g_ProxyState.pfnCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)
        GetProcAddress(g_ProxyState.hOriginalDXGI, "CreateDXGIFactory2");
    g_ProxyState.pfnDXGIDeclareAdapterRemovalSupport = (PFN_DXGIDeclareAdapterRemovalSupport)
        GetProcAddress(g_ProxyState.hOriginalDXGI, "DXGIDeclareAdapterRemovalSupport");
    g_ProxyState.pfnDXGIGetDebugInterface1 = (PFN_DXGIGetDebugInterface1)
        GetProcAddress(g_ProxyState.hOriginalDXGI, "DXGIGetDebugInterface1");

    // Load additional passthroughs
    g_ProxyState.pfnApplyCompatResolutionQuirking = GetProcAddress(g_ProxyState.hOriginalDXGI, "ApplyCompatResolutionQuirking");
    g_ProxyState.pfnCompatString = GetProcAddress(g_ProxyState.hOriginalDXGI, "CompatString");
    g_ProxyState.pfnCompatValue = GetProcAddress(g_ProxyState.hOriginalDXGI, "CompatValue");
    g_ProxyState.pfnDXGIDumpJournal = GetProcAddress(g_ProxyState.hOriginalDXGI, "DXGIDumpJournal");
    g_ProxyState.pfnDXGIReportAdapterConfiguration = GetProcAddress(g_ProxyState.hOriginalDXGI, "DXGIReportAdapterConfiguration");
    g_ProxyState.pfnDXGIDisableVBlankVirtualization = GetProcAddress(g_ProxyState.hOriginalDXGI, "DXGIDisableVBlankVirtualization");
    g_ProxyState.pfnD3DKMTCloseAdapter = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTCloseAdapter");
    g_ProxyState.pfnD3DKMTDestroyAllocation = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTDestroyAllocation");
    g_ProxyState.pfnD3DKMTDestroyContext = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTDestroyContext");
    g_ProxyState.pfnD3DKMTDestroyDevice = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTDestroyDevice");
    g_ProxyState.pfnD3DKMTDestroySynchronizationObject = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTDestroySynchronizationObject");
    g_ProxyState.pfnD3DKMTQueryAdapterInfo = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTQueryAdapterInfo");
    g_ProxyState.pfnD3DKMTSetDisplayPrivateDriverFormat = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTSetDisplayPrivateDriverFormat");
    g_ProxyState.pfnD3DKMTSignalSynchronizationObject = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTSignalSynchronizationObject");
    g_ProxyState.pfnD3DKMTUnlock = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTUnlock");
    g_ProxyState.pfnD3DKMTWaitForSynchronizationObject = GetProcAddress(g_ProxyState.hOriginalDXGI, "D3DKMTWaitForSynchronizationObject");
    g_ProxyState.pfnOpenAdapter10 = GetProcAddress(g_ProxyState.hOriginalDXGI, "OpenAdapter10");
    g_ProxyState.pfnOpenAdapter10_2 = GetProcAddress(g_ProxyState.hOriginalDXGI, "OpenAdapter10_2");
    g_ProxyState.pfnSetAppCompatStringPointer = GetProcAddress(g_ProxyState.hOriginalDXGI, "SetAppCompatStringPointer");
    
    LogStartup("GetProcAddress End");
    if (!g_ProxyState.pfnDXGIDeclareAdapterRemovalSupport) LOG_INFO("Optional export missing: DXGIDeclareAdapterRemovalSupport");
    if (!g_ProxyState.pfnDXGIGetDebugInterface1) LOG_INFO("Optional export missing: DXGIGetDebugInterface1");
    if (!g_ProxyState.pfnApplyCompatResolutionQuirking) LOG_INFO("Optional export missing: ApplyCompatResolutionQuirking");
    if (!g_ProxyState.pfnCompatString) LOG_INFO("Optional export missing: CompatString");
    if (!g_ProxyState.pfnCompatValue) LOG_INFO("Optional export missing: CompatValue");
    if (!g_ProxyState.pfnDXGIDumpJournal) LOG_INFO("Optional export missing: DXGIDumpJournal");
    if (!g_ProxyState.pfnDXGIReportAdapterConfiguration) LOG_INFO("Optional export missing: DXGIReportAdapterConfiguration");
    if (!g_ProxyState.pfnDXGIDisableVBlankVirtualization) LOG_INFO("Optional export missing: DXGIDisableVBlankVirtualization");
    
    // Verify critical functions loaded
    if (!g_ProxyState.pfnCreateDXGIFactory || 
        !g_ProxyState.pfnCreateDXGIFactory1 || 
        !g_ProxyState.pfnCreateDXGIFactory2) {
        LogStartup("Failed to get critical pointers!");
        LOG_ERROR("Failed to get critical DXGI function pointers!");
        FreeLibrary(g_ProxyState.hOriginalDXGI);
        g_ProxyState.hOriginalDXGI = nullptr;
        LeaveCriticalSection(&s_InitCS);
        return false;
    }
    
    LOG_INFO("Original DXGI loaded successfully.");
    LOG_INFO("  CreateDXGIFactory:  0x%p", g_ProxyState.pfnCreateDXGIFactory);
    LOG_INFO("  CreateDXGIFactory1: 0x%p", g_ProxyState.pfnCreateDXGIFactory1);
    LOG_INFO("  CreateDXGIFactory2: 0x%p", g_ProxyState.pfnCreateDXGIFactory2);
    
    // Install IAT Hooks for D3D12
    InstallD3D12Hooks();
    
    g_ProxyState.initialized = true;
    LogStartup("InitializeProxy Success");
    LeaveCriticalSection(&s_InitCS);
    return true;
}

void ShutdownProxy() {
    if (g_ProxyState.hOriginalDXGI) {
        FreeLibrary(g_ProxyState.hOriginalDXGI);
        g_ProxyState.hOriginalDXGI = nullptr;
    }
    g_ProxyState.initialized = false;
    LOG_INFO("DXGI Proxy shutdown complete.");
}

// Forward declaration from hooks.cpp (C++ Linkage)
extern HRESULT WINAPI Hooked_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice);

// ============================================================================
// EXPORTED PROXY FUNCTIONS
// ============================================================================

extern "C" {

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    LogStartup("CreateDXGIFactory Intercepted");
    
    if (!g_ProxyState.initialized && !InitializeProxy()) {
        return E_FAIL;
    }
    
    HRESULT hr = g_ProxyState.pfnCreateDXGIFactory(riid, ppFactory);
    
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        LOG_INFO("CreateDXGIFactory succeeded, factory at 0x%p", *ppFactory);
        *ppFactory = new WrappedIDXGIFactory((IDXGIFactory*)*ppFactory);
        LogStartup("Factory Wrapped");
    }
    
    return hr;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    LogStartup("CreateDXGIFactory1 Intercepted");
    
    if (!g_ProxyState.initialized && !InitializeProxy()) {
        return E_FAIL;
    }
    
    HRESULT hr = g_ProxyState.pfnCreateDXGIFactory1(riid, ppFactory);
    
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        LOG_INFO("CreateDXGIFactory1 succeeded, factory at 0x%p", *ppFactory);
        *ppFactory = new WrappedIDXGIFactory((IDXGIFactory*)*ppFactory);
        LogStartup("Factory1 Wrapped");
    }
    
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    LogStartup("CreateDXGIFactory2 Intercepted");
    
    if (!g_ProxyState.initialized && !InitializeProxy()) {
        return E_FAIL;
    }
    
    HRESULT hr = g_ProxyState.pfnCreateDXGIFactory2(Flags, riid, ppFactory);
    
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        LOG_INFO("CreateDXGIFactory2 succeeded, factory at 0x%p", *ppFactory);
        *ppFactory = new WrappedIDXGIFactory((IDXGIFactory*)*ppFactory);
        LogStartup("Factory2 Wrapped");
    }
    
    return hr;
}

HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    LogStartup("DXGIDeclareAdapterRemovalSupport Called");
    
    if (!g_ProxyState.initialized && !InitializeProxy()) {
        return E_FAIL;
    }
    
    if (g_ProxyState.pfnDXGIDeclareAdapterRemovalSupport) {
        return g_ProxyState.pfnDXGIDeclareAdapterRemovalSupport();
    }
    return S_OK;
}

HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug) {
    LogStartup("DXGIGetDebugInterface1 Called");
    
    if (!g_ProxyState.initialized && !InitializeProxy()) {
        return E_FAIL;
    }
    
    if (g_ProxyState.pfnDXGIGetDebugInterface1) {
        return g_ProxyState.pfnDXGIGetDebugInterface1(Flags, riid, pDebug);
    }
    return E_NOINTERFACE;
}

// Helper for passthroughs
typedef HRESULT (WINAPI *PFN_Generic)(void* a, void* b, void* c, void* d);

HRESULT WINAPI GenericForward(void* func, void* a, void* b, void* c, void* d) {
    if (!func) return E_NOINTERFACE;
    return ((PFN_Generic)func)(a, b, c, d);
}

#define FORWARD_1ARG(Name) \
    HRESULT WINAPI Name(void* a) { \
        LogStartup("Stub: " #Name); \
        if (!g_ProxyState.initialized) InitializeProxy(); \
        return GenericForward(g_ProxyState.pfn##Name, a, 0, 0, 0); \
    }

HRESULT WINAPI ApplyCompatResolutionQuirking(void* a, void* b) { LogStartup("Stub: ApplyCompat"); if(!g_ProxyState.initialized) InitializeProxy(); return GenericForward(g_ProxyState.pfnApplyCompatResolutionQuirking, a, b, 0, 0); }
HRESULT WINAPI CompatString(void* a, void* b, void* c) { LogStartup("Stub: CompatString"); if(!g_ProxyState.initialized) InitializeProxy(); return GenericForward(g_ProxyState.pfnCompatString, a, b, c, 0); }
HRESULT WINAPI CompatValue(void* a, void* b) { LogStartup("Stub: CompatValue"); if(!g_ProxyState.initialized) InitializeProxy(); return GenericForward(g_ProxyState.pfnCompatValue, a, b, 0, 0); }
HRESULT WINAPI DXGIDumpJournal(void* a) { LogStartup("Stub: DumpJournal"); if(!g_ProxyState.initialized) InitializeProxy(); return GenericForward(g_ProxyState.pfnDXGIDumpJournal, a, 0, 0, 0); }
HRESULT WINAPI DXGIReportAdapterConfiguration(void* a) { LogStartup("Stub: ReportAdapterCfg"); if(!g_ProxyState.initialized) InitializeProxy(); return GenericForward(g_ProxyState.pfnDXGIReportAdapterConfiguration, a, 0, 0, 0); }
HRESULT WINAPI DXGIDisableVBlankVirtualization() { LogStartup("Stub: DisableVBlank"); if(!g_ProxyState.initialized) InitializeProxy(); return GenericForward(g_ProxyState.pfnDXGIDisableVBlankVirtualization, 0, 0, 0, 0); }

// D3DKMT functions typically take 1 argument (struct pointer)
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

HRESULT WINAPI OpenAdapter10(void* a) { LogStartup("Stub: OpenAdapter10"); if(!g_ProxyState.initialized) InitializeProxy(); return GenericForward(g_ProxyState.pfnOpenAdapter10, a, 0, 0, 0); }
HRESULT WINAPI OpenAdapter10_2(void* a) { LogStartup("Stub: OpenAdapter10_2"); if(!g_ProxyState.initialized) InitializeProxy(); return GenericForward(g_ProxyState.pfnOpenAdapter10_2, a, 0, 0, 0); }
HRESULT WINAPI SetAppCompatStringPointer(void* a, void* b) { LogStartup("Stub: SetAppCompat"); if(!g_ProxyState.initialized) InitializeProxy(); return GenericForward(g_ProxyState.pfnSetAppCompatStringPointer, a, b, 0, 0); }

HRESULT WINAPI Proxy_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    LogStartup("Proxy_D3D12CreateDevice Called");
    return Hooked_D3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
}

} // extern "C"
