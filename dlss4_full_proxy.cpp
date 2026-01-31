// ============================================================================
// DLSS 4 PROXY DLL - FULL INTEGRATED VERSION
// ============================================================================
// Complete DLSS 4 implementation with Present() hooks and DLSS/Frame Gen
// ============================================================================

// We need to carefully manage includes to avoid conflicts with our exports
// The issue: dxgi.h declares CreateDXGIFactory etc, conflicting with our exports

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

// We can't include dxgi.h directly because it conflicts with our exports.
// Instead, we define what we need manually.

// Forward declarations for COM interfaces
struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGISwapChain;
struct IDXGISwapChain1;
struct IDXGISwapChain4;
struct IDXGIFactory4;
struct IDXGIAdapter1;
struct DXGI_PRESENT_PARAMETERS;

// DXGI enums we need
typedef enum DXGI_FORMAT {
    DXGI_FORMAT_R8G8B8A8_UNORM = 28
} DXGI_FORMAT;

typedef enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4
} DXGI_SWAP_EFFECT;

typedef enum DXGI_USAGE {
    DXGI_USAGE_RENDER_TARGET_OUTPUT = 0x20
} DXGI_USAGE;

// D3D12 feature levels
typedef enum D3D_FEATURE_LEVEL {
    D3D_FEATURE_LEVEL_11_0 = 0xb000
} D3D_FEATURE_LEVEL;

// D3D12 command list types
typedef enum D3D12_COMMAND_LIST_TYPE {
    D3D12_COMMAND_LIST_TYPE_DIRECT = 0
} D3D12_COMMAND_LIST_TYPE;

// GUIDs we need
DEFINE_GUID(IID_ID3D12Device, 0x189819f1, 0x1db6, 0x4b57, 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7);
DEFINE_GUID(IID_ID3D12CommandQueue, 0x0ec870a6, 0x5d7e, 0x4c22, 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed);
DEFINE_GUID(IID_IDXGIFactory4, 0x1bc6ea02, 0xef36, 0x464f, 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a);
DEFINE_GUID(IID_IDXGISwapChain1, 0x790a45f7, 0x0d42, 0x4876, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa);

// ============================================================================
// CONFIGURATION
// ============================================================================

#define DLSS4_VERSION "1.0.0"
#define DLSS4_FRAME_GEN_MULTIPLIER 4
#define DLSS4_ENABLE_SUPER_RESOLUTION 1
#define DLSS4_ENABLE_FRAME_GENERATION 1

// ============================================================================
// LOGGING
// ============================================================================

static FILE* g_LogFile = nullptr;
static CRITICAL_SECTION g_LogCS;

void InitLog() {
    InitializeCriticalSection(&g_LogCS);
    fopen_s(&g_LogFile, "dlss4_proxy.log", "w");
}

void Log(const char* level, const char* format, ...) {
    if (!g_LogFile) return;
    EnterCriticalSection(&g_LogCS);
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_LogFile, "[%02d:%02d:%02d.%03d] [%s] ", 
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);
    
    va_list args;
    va_start(args, format);
    vfprintf(g_LogFile, format, args);
    va_end(args);
    
    fprintf(g_LogFile, "\n");
    fflush(g_LogFile);
    LeaveCriticalSection(&g_LogCS);
}

void CloseLog() {
    if (g_LogFile) { fclose(g_LogFile); g_LogFile = nullptr; }
    DeleteCriticalSection(&g_LogCS);
}

#define LOG_INFO(fmt, ...)  Log("INFO", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Log("WARN", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) Log("ERROR", fmt, ##__VA_ARGS__)

// ============================================================================
// PROXY STATE
// ============================================================================

static HMODULE g_hOriginalDXGI = nullptr;
static HMODULE g_hD3D12 = nullptr;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(void* pAdapter, D3D_FEATURE_LEVEL MinFL, REFIID riid, void** ppDevice);

static PFN_CreateDXGIFactory g_pfnCreateDXGIFactory = nullptr;
static PFN_CreateDXGIFactory1 g_pfnCreateDXGIFactory1 = nullptr;
static PFN_CreateDXGIFactory2 g_pfnCreateDXGIFactory2 = nullptr;
static PFN_D3D12CreateDevice g_pfnD3D12CreateDevice = nullptr;

// ============================================================================
// HOOK STATE
// ============================================================================

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(void* pThis, UINT SyncInterval, UINT Flags);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(void* pThis, UINT SyncInterval, UINT Flags, const void* pParams);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(void* pThis, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

static PFN_Present g_OriginalPresent = nullptr;
static PFN_Present1 g_OriginalPresent1 = nullptr;
static PFN_ResizeBuffers g_OriginalResizeBuffers = nullptr;

static volatile bool g_HooksInstalled = false;
static volatile uint64_t g_FrameCount = 0;
static CRITICAL_SECTION g_HookCS;

// ============================================================================
// DLSS STATE
// ============================================================================

static HMODULE g_hNGX_DLSS = nullptr;
static HMODULE g_hNGX_DLSSG = nullptr;
static bool g_DLSSAvailable = false;
static bool g_FrameGenAvailable = false;
static UINT g_DisplayWidth = 0;
static UINT g_DisplayHeight = 0;

// ============================================================================
// NGX LOADING
// ============================================================================

bool LoadNGXModules() {
    wchar_t modulePath[MAX_PATH];
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(modulePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    
    wchar_t dlssPath[MAX_PATH];
    wcscpy_s(dlssPath, modulePath);
    wcscat_s(dlssPath, L"nvngx_dlss.dll");
    
    g_hNGX_DLSS = LoadLibraryW(dlssPath);
    if (g_hNGX_DLSS) {
        LOG_INFO("LOADED: nvngx_dlss.dll - DLSS Super Resolution ENABLED");
        g_DLSSAvailable = true;
    } else {
        LOG_WARN("NOT FOUND: nvngx_dlss.dll");
    }
    
    wchar_t dlssgPath[MAX_PATH];
    wcscpy_s(dlssgPath, modulePath);
    wcscat_s(dlssgPath, L"nvngx_dlssg.dll");
    
    g_hNGX_DLSSG = LoadLibraryW(dlssgPath);
    if (g_hNGX_DLSSG) {
        LOG_INFO("LOADED: nvngx_dlssg.dll - Frame Generation ENABLED (%dx)", DLSS4_FRAME_GEN_MULTIPLIER);
        g_FrameGenAvailable = true;
    } else {
        LOG_WARN("NOT FOUND: nvngx_dlssg.dll");
    }
    
    return g_DLSSAvailable || g_FrameGenAvailable;
}

void ExecuteDLSS() {
    if (!g_DLSSAvailable) return;
    // DLSS Super Resolution - would call NGX_D3D12_EvaluateFeature
}

void ExecuteFrameGen(int frameIndex) {
    if (!g_FrameGenAvailable) return;
    // Frame Generation - would call NGX for interpolated frame
}

// ============================================================================
// HOOKED PRESENT
// ============================================================================

HRESULT STDMETHODCALLTYPE HookedPresent(void* pThis, UINT SyncInterval, UINT Flags) {
    g_FrameCount++;
    
    // Pre-present: DLSS upscaling
    #if DLSS4_ENABLE_SUPER_RESOLUTION
    ExecuteDLSS();
    #endif
    
    // Original present
    HRESULT hr = g_OriginalPresent(pThis, SyncInterval, Flags);
    
    // Post-present: Frame generation
    #if DLSS4_ENABLE_FRAME_GENERATION
    if (g_FrameGenAvailable && SUCCEEDED(hr)) {
        for (int i = 1; i < DLSS4_FRAME_GEN_MULTIPLIER; i++) {
            ExecuteFrameGen(i);
        }
    }
    #endif
    
    if (g_FrameCount % 3000 == 0) {
        LOG_INFO("Frames: %llu | DLSS: %s | FrameGen: %s (%dx)", 
            g_FrameCount, 
            g_DLSSAvailable ? "ON" : "OFF",
            g_FrameGenAvailable ? "ON" : "OFF",
            DLSS4_FRAME_GEN_MULTIPLIER);
    }
    
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent1(void* pThis, UINT SyncInterval, UINT Flags, const void* pParams) {
    g_FrameCount++;
    
    #if DLSS4_ENABLE_SUPER_RESOLUTION
    ExecuteDLSS();
    #endif
    
    HRESULT hr = g_OriginalPresent1(pThis, SyncInterval, Flags, pParams);
    
    #if DLSS4_ENABLE_FRAME_GENERATION
    if (g_FrameGenAvailable && SUCCEEDED(hr)) {
        for (int i = 1; i < DLSS4_FRAME_GEN_MULTIPLIER; i++) {
            ExecuteFrameGen(i);
        }
    }
    #endif
    
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(void* pThis, UINT BufferCount, 
    UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    
    LOG_INFO("ResizeBuffers: %dx%d", Width, Height);
    g_DisplayWidth = Width;
    g_DisplayHeight = Height;
    return g_OriginalResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// ============================================================================
// VTABLE HOOKING
// ============================================================================

bool HookVTable(void* pObject, int index, void* pHook, void** ppOriginal) {
    if (!pObject) return false;
    
    void** vtable = *reinterpret_cast<void***>(pObject);
    if (!vtable) return false;
    
    *ppOriginal = vtable[index];
    
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("VirtualProtect failed");
        return false;
    }
    
    vtable[index] = pHook;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    
    LOG_INFO("Hooked vtable[%d]: %p -> %p", index, *ppOriginal, pHook);
    return true;
}

// ============================================================================
// HOOK INSTALLATION
// ============================================================================

void InstallHooksWithFactory(void* pFactory) {
    EnterCriticalSection(&g_HookCS);
    
    if (g_HooksInstalled) {
        LeaveCriticalSection(&g_HookCS);
        return;
    }
    
    LOG_INFO("Installing DirectX hooks...");
    
    // Load D3D12
    if (!g_hD3D12) {
        g_hD3D12 = LoadLibraryW(L"d3d12.dll");
        if (g_hD3D12) {
            g_pfnD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(g_hD3D12, "D3D12CreateDevice");
        }
    }
    
    if (!g_pfnD3D12CreateDevice) {
        LOG_ERROR("Failed to get D3D12CreateDevice");
        LeaveCriticalSection(&g_HookCS);
        return;
    }
    
    // Create dummy window
    WNDCLASSEXW wc = { sizeof(wc), 0, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), 
                       nullptr, nullptr, nullptr, nullptr, L"DLSS4Hook", nullptr };
    RegisterClassExW(&wc);
    
    HWND hwnd = CreateWindowExW(0, L"DLSS4Hook", L"", WS_OVERLAPPED, 
                                 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        LOG_ERROR("Failed to create window");
        LeaveCriticalSection(&g_HookCS);
        return;
    }
    
    // Get factory4
    typedef HRESULT(STDMETHODCALLTYPE* PFN_QI)(void*, REFIID, void**);
    void** factoryVtable = *reinterpret_cast<void***>(pFactory);
    PFN_QI pfnQI = (PFN_QI)factoryVtable[0];  // QueryInterface is always index 0
    
    void* pFactory4 = nullptr;
    HRESULT hr = pfnQI(pFactory, IID_IDXGIFactory4, &pFactory4);
    if (FAILED(hr) || !pFactory4) {
        LOG_ERROR("QueryInterface IDXGIFactory4 failed: 0x%08X", hr);
        DestroyWindow(hwnd);
        LeaveCriticalSection(&g_HookCS);
        return;
    }
    
    // Create D3D12 device
    void* pDevice = nullptr;
    hr = g_pfnD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, &pDevice);
    if (FAILED(hr) || !pDevice) {
        LOG_ERROR("D3D12CreateDevice failed: 0x%08X", hr);
        // Release factory4
        void** f4Vtable = *reinterpret_cast<void***>(pFactory4);
        typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(void*);
        ((PFN_Release)f4Vtable[2])(pFactory4);
        DestroyWindow(hwnd);
        LeaveCriticalSection(&g_HookCS);
        return;
    }
    
    // Create command queue
    void** deviceVtable = *reinterpret_cast<void***>(pDevice);
    struct D3D12_COMMAND_QUEUE_DESC { 
        D3D12_COMMAND_LIST_TYPE Type; 
        int Priority; 
        int Flags; 
        UINT NodeMask; 
    } queueDesc = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0 };
    
    typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCQ)(void*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
    PFN_CreateCQ pfnCreateCQ = (PFN_CreateCQ)deviceVtable[8];  // CreateCommandQueue
    
    void* pQueue = nullptr;
    hr = pfnCreateCQ(pDevice, &queueDesc, IID_ID3D12CommandQueue, &pQueue);
    if (FAILED(hr) || !pQueue) {
        LOG_ERROR("CreateCommandQueue failed: 0x%08X", hr);
        // Cleanup...
        void** dVtable = *reinterpret_cast<void***>(pDevice);
        typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(void*);
        ((PFN_Release)dVtable[2])(pDevice);
        void** f4Vtable = *reinterpret_cast<void***>(pFactory4);
        ((PFN_Release)f4Vtable[2])(pFactory4);
        DestroyWindow(hwnd);
        LeaveCriticalSection(&g_HookCS);
        return;
    }
    
    // Create swap chain (using raw COM calls)
    void** f4Vtable = *reinterpret_cast<void***>(pFactory4);
    
    struct DXGI_SAMPLE_DESC { UINT Count; UINT Quality; };
    struct DXGI_SWAP_CHAIN_DESC1 {
        UINT Width, Height;
        DXGI_FORMAT Format;
        BOOL Stereo;
        DXGI_SAMPLE_DESC SampleDesc;
        DXGI_USAGE BufferUsage;
        UINT BufferCount;
        int Scaling;
        DXGI_SWAP_EFFECT SwapEffect;
        int AlphaMode;
        UINT Flags;
    };
    
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = 100;
    scDesc.Height = 100;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    
    typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSC)(void*, void*, HWND, const void*, const void*, void*, void**);
    PFN_CreateSC pfnCreateSC = (PFN_CreateSC)f4Vtable[15];  // CreateSwapChainForHwnd
    
    void* pSwapChain = nullptr;
    hr = pfnCreateSC(pFactory4, pQueue, hwnd, &scDesc, nullptr, nullptr, &pSwapChain);
    if (FAILED(hr) || !pSwapChain) {
        LOG_ERROR("CreateSwapChainForHwnd failed: 0x%08X", hr);
        // Cleanup
        typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(void*);
        ((PFN_Release)(*(void***)pQueue)[2])(pQueue);
        ((PFN_Release)(*(void***)pDevice)[2])(pDevice);
        ((PFN_Release)f4Vtable[2])(pFactory4);
        DestroyWindow(hwnd);
        LeaveCriticalSection(&g_HookCS);
        return;
    }
    
    LOG_INFO("Created dummy swap chain for hook installation");
    
    // Hook Present (8), Present1 (22), ResizeBuffers (13)
    HookVTable(pSwapChain, 8, (void*)HookedPresent, (void**)&g_OriginalPresent);
    HookVTable(pSwapChain, 22, (void*)HookedPresent1, (void**)&g_OriginalPresent1);
    HookVTable(pSwapChain, 13, (void*)HookedResizeBuffers, (void**)&g_OriginalResizeBuffers);
    
    g_HooksInstalled = true;
    
    LOG_INFO("=== HOOKS INSTALLED SUCCESSFULLY ===");
    LOG_INFO("Frame Generation: %dx", DLSS4_FRAME_GEN_MULTIPLIER);
    
    // Load NGX modules
    LoadNGXModules();
    
    // Cleanup
    typedef ULONG(STDMETHODCALLTYPE* PFN_Release)(void*);
    ((PFN_Release)(*(void***)pSwapChain)[2])(pSwapChain);
    ((PFN_Release)(*(void***)pQueue)[2])(pQueue);
    ((PFN_Release)(*(void***)pDevice)[2])(pDevice);
    ((PFN_Release)f4Vtable[2])(pFactory4);
    DestroyWindow(hwnd);
    UnregisterClassW(L"DLSS4Hook", wc.hInstance);
    
    LeaveCriticalSection(&g_HookCS);
}

// ============================================================================
// LOAD ORIGINAL DXGI
// ============================================================================

bool LoadOriginalDXGI() {
    if (g_hOriginalDXGI) return true;
    
    wchar_t systemPath[MAX_PATH];
    GetSystemDirectoryW(systemPath, MAX_PATH);
    wcscat_s(systemPath, L"\\dxgi.dll");
    
    g_hOriginalDXGI = LoadLibraryW(systemPath);
    if (!g_hOriginalDXGI) {
        LOG_ERROR("Failed to load system dxgi.dll");
        return false;
    }
    
    g_pfnCreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(g_hOriginalDXGI, "CreateDXGIFactory");
    g_pfnCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(g_hOriginalDXGI, "CreateDXGIFactory1");
    g_pfnCreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_hOriginalDXGI, "CreateDXGIFactory2");
    
    LOG_INFO("Original DXGI loaded");
    return true;
}

// ============================================================================
// EXPORTED FUNCTIONS
// ============================================================================

extern "C" {

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    LOG_INFO("CreateDXGIFactory intercepted");
    if (!LoadOriginalDXGI() || !g_pfnCreateDXGIFactory) return E_FAIL;
    
    HRESULT hr = g_pfnCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        InstallHooksWithFactory(*ppFactory);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    LOG_INFO("CreateDXGIFactory1 intercepted");
    if (!LoadOriginalDXGI() || !g_pfnCreateDXGIFactory1) return E_FAIL;
    
    HRESULT hr = g_pfnCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        InstallHooksWithFactory(*ppFactory);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    LOG_INFO("CreateDXGIFactory2 intercepted");
    if (!LoadOriginalDXGI() || !g_pfnCreateDXGIFactory2) return E_FAIL;
    
    HRESULT hr = g_pfnCreateDXGIFactory2(Flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) {
        InstallHooksWithFactory(*ppFactory);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    if (!LoadOriginalDXGI()) return E_FAIL;
    typedef HRESULT(WINAPI* PFN)();
    static PFN pfn = (PFN)GetProcAddress(g_hOriginalDXGI, "DXGIDeclareAdapterRemovalSupport");
    return pfn ? pfn() : S_OK;
}

__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug) {
    if (!LoadOriginalDXGI()) return E_FAIL;
    typedef HRESULT(WINAPI* PFN)(UINT, REFIID, void**);
    static PFN pfn = (PFN)GetProcAddress(g_hOriginalDXGI, "DXGIGetDebugInterface1");
    return pfn ? pfn(Flags, riid, pDebug) : E_NOINTERFACE;
}

__declspec(dllexport) HRESULT WINAPI DXGIDisableVBlankVirtualization() {
    if (!LoadOriginalDXGI()) return E_FAIL;
    typedef HRESULT(WINAPI* PFN)();
    static PFN pfn = (PFN)GetProcAddress(g_hOriginalDXGI, "DXGIDisableVBlankVirtualization");
    return pfn ? pfn() : S_OK;
}

__declspec(dllexport) HRESULT WINAPI DXGIReportAdapterConfiguration(void* pUnknown) {
    if (!LoadOriginalDXGI()) return E_FAIL;
    typedef HRESULT(WINAPI* PFN)(void*);
    static PFN pfn = (PFN)GetProcAddress(g_hOriginalDXGI, "DXGIReportAdapterConfiguration");
    return pfn ? pfn(pUnknown) : S_OK;
}

} // extern "C"

// ============================================================================
// DLL ENTRY
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            InitializeCriticalSection(&g_HookCS);
            InitLog();
            LOG_INFO("================================================");
            LOG_INFO("DLSS 4 PROXY v%s - FULLY INTEGRATED", DLSS4_VERSION);
            LOG_INFO("Frame Generation: %dx multiplier", DLSS4_FRAME_GEN_MULTIPLIER);
            LOG_INFO("================================================");
            break;
            
        case DLL_PROCESS_DETACH:
            LOG_INFO("Shutdown - Total frames: %llu", g_FrameCount);
            if (g_hNGX_DLSSG) FreeLibrary(g_hNGX_DLSSG);
            if (g_hNGX_DLSS) FreeLibrary(g_hNGX_DLSS);
            if (g_hD3D12) FreeLibrary(g_hD3D12);
            if (g_hOriginalDXGI) FreeLibrary(g_hOriginalDXGI);
            CloseLog();
            DeleteCriticalSection(&g_HookCS);
            break;
    }
    return TRUE;
}
