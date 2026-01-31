// ============================================================================
// DLSS 4 PROXY DLL - SIMPLIFIED MAIN ENTRY POINT
// ============================================================================
// Simplified version for debugging - minimal initialization to isolate issues
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>

// ============================================================================
// SIMPLE LOGGING (no C++ dependencies)
// ============================================================================

static FILE* g_LogFile = nullptr;

void SimpleLog(const char* format, ...) {
    if (!g_LogFile) {
        fopen_s(&g_LogFile, "dlss4_proxy.log", "a");
    }
    if (g_LogFile) {
        va_list args;
        va_start(args, format);
        vfprintf(g_LogFile, format, args);
        fprintf(g_LogFile, "\n");
        fflush(g_LogFile);
        va_end(args);
    }
}

// ============================================================================
// PROXY STATE
// ============================================================================

static HMODULE g_hOriginalDXGI = nullptr;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void** ppFactory);

static PFN_CreateDXGIFactory g_pfnCreateDXGIFactory = nullptr;
static PFN_CreateDXGIFactory1 g_pfnCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 g_pfnCreateDXGIFactory2 = nullptr;

// ============================================================================
// LOAD ORIGINAL DXGI
// ============================================================================

bool LoadOriginalDXGI() {
    if (g_hOriginalDXGI) return true;
    
    wchar_t systemPath[MAX_PATH];
    GetSystemDirectoryW(systemPath, MAX_PATH);
    wcscat_s(systemPath, L"\\dxgi.dll");
    
    SimpleLog("Loading original DXGI from: %ls", systemPath);
    
    g_hOriginalDXGI = LoadLibraryW(systemPath);
    if (!g_hOriginalDXGI) {
        SimpleLog("ERROR: Failed to load original dxgi.dll! Error: %d", GetLastError());
        return false;
    }
    
    g_pfnCreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(g_hOriginalDXGI, "CreateDXGIFactory");
    g_pfnCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(g_hOriginalDXGI, "CreateDXGIFactory1");
    g_pfnCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_hOriginalDXGI, "CreateDXGIFactory2");
    
    SimpleLog("Original DXGI loaded successfully");
    SimpleLog("  CreateDXGIFactory:  %p", g_pfnCreateDXGIFactory);
    SimpleLog("  CreateDXGIFactory1: %p", g_pfnCreateDXGIFactory1);
    SimpleLog("  CreateDXGIFactory2: %p", g_pfnCreateDXGIFactory2);
    
    return true;
}

// ============================================================================
// EXPORTED FUNCTIONS
// ============================================================================

extern "C" {

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    SimpleLog("CreateDXGIFactory called");
    if (!LoadOriginalDXGI() || !g_pfnCreateDXGIFactory) {
        SimpleLog("ERROR: Original function not available");
        return E_FAIL;
    }
    return g_pfnCreateDXGIFactory(riid, ppFactory);
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    SimpleLog("CreateDXGIFactory1 called");
    if (!LoadOriginalDXGI() || !g_pfnCreateDXGIFactory1) {
        SimpleLog("ERROR: Original function not available");
        return E_FAIL;
    }
    return g_pfnCreateDXGIFactory1(riid, ppFactory);
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    SimpleLog("CreateDXGIFactory2 called");
    if (!LoadOriginalDXGI() || !g_pfnCreateDXGIFactory2) {
        SimpleLog("ERROR: Original function not available");
        return E_FAIL;
    }
    return g_pfnCreateDXGIFactory2(Flags, riid, ppFactory);
}

// Additional exports that games commonly need
__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    if (!LoadOriginalDXGI()) return E_FAIL;
    typedef HRESULT(WINAPI* PFN)();
    static PFN pfn = nullptr;
    if (!pfn) pfn = (PFN)GetProcAddress(g_hOriginalDXGI, "DXGIDeclareAdapterRemovalSupport");
    return pfn ? pfn() : S_OK;
}

__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug) {
    if (!LoadOriginalDXGI()) return E_FAIL;
    typedef HRESULT(WINAPI* PFN)(UINT, REFIID, void**);
    static PFN pfn = nullptr;
    if (!pfn) pfn = (PFN)GetProcAddress(g_hOriginalDXGI, "DXGIGetDebugInterface1");
    return pfn ? pfn(Flags, riid, pDebug) : E_NOINTERFACE;
}

__declspec(dllexport) HRESULT WINAPI DXGIDisableVBlankVirtualization() {
    if (!LoadOriginalDXGI()) return E_FAIL;
    typedef HRESULT(WINAPI* PFN)();
    static PFN pfn = nullptr;
    if (!pfn) pfn = (PFN)GetProcAddress(g_hOriginalDXGI, "DXGIDisableVBlankVirtualization");
    return pfn ? pfn() : S_OK;
}

__declspec(dllexport) HRESULT WINAPI DXGIReportAdapterConfiguration(void* pUnknown) {
    if (!LoadOriginalDXGI()) return E_FAIL;
    typedef HRESULT(WINAPI* PFN)(void*);
    static PFN pfn = nullptr;
    if (!pfn) pfn = (PFN)GetProcAddress(g_hOriginalDXGI, "DXGIReportAdapterConfiguration");
    return pfn ? pfn(pUnknown) : S_OK;
}

} // extern "C"

// ============================================================================
// DLL ENTRY POINT
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            SimpleLog("=== DLSS 4 Proxy DLL Loaded ===");
            SimpleLog("Version: 1.0.0 (Debug Build)");
            // Don't load DXGI here - wait for first function call
            break;
            
        case DLL_PROCESS_DETACH:
            SimpleLog("DLSS 4 Proxy DLL Unloading");
            if (g_hOriginalDXGI) {
                FreeLibrary(g_hOriginalDXGI);
                g_hOriginalDXGI = nullptr;
            }
            if (g_LogFile) {
                fclose(g_LogFile);
                g_LogFile = nullptr;
            }
            break;
    }
    return TRUE;
}
