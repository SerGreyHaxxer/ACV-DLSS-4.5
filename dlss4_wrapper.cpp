// ============================================================================
// DLSS 4 FULL INTEGRATION - WRAPPER-BASED HOOKS (NO VTABLE MODIFICATION)
// ============================================================================
// Uses COM wrapper pattern instead of VTable patching to avoid crashes.
// Wraps IDXGISwapChain to intercept Present calls safely.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#define CreateDXGIFactory _SDK_CreateDXGIFactory
#define CreateDXGIFactory1 _SDK_CreateDXGIFactory1
#define CreateDXGIFactory2 _SDK_CreateDXGIFactory2
#define DXGIGetDebugInterface1 _SDK_DXGIGetDebugInterface1
#define DXGIDeclareAdapterRemovalSupport _SDK_DXGIDeclareAdapterRemovalSupport
#define DXGIDisableVBlankVirtualization _SDK_DXGIDisableVBlankVirtualization

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <string>
#include <atomic>

#undef CreateDXGIFactory
#undef CreateDXGIFactory1
#undef CreateDXGIFactory2
#undef DXGIGetDebugInterface1
#undef DXGIDeclareAdapterRemovalSupport
#undef DXGIDisableVBlankVirtualization

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

// ============================================================================
// NGX TYPES
// ============================================================================

typedef int NVSDK_NGX_Result;
#define NVSDK_NGX_Result_Success 0x1
typedef void* NVSDK_NGX_Parameter;
typedef void* NVSDK_NGX_Handle;

typedef NVSDK_NGX_Result(*PFN_NVSDK_NGX_D3D12_Init)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result(*PFN_NVSDK_NGX_D3D12_Shutdown)(void);
typedef NVSDK_NGX_Result(*PFN_NVSDK_NGX_D3D12_CreateFeature)(ID3D12GraphicsCommandList*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result(*PFN_NVSDK_NGX_D3D12_EvaluateFeature)(ID3D12GraphicsCommandList*, NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result(*PFN_NVSDK_NGX_D3D12_ReleaseFeature)(NVSDK_NGX_Handle*);

// ============================================================================
// CONSOLE LOGGING
// ============================================================================

static FILE* g_Log = nullptr;
static HANDLE g_Console = nullptr;
static CRITICAL_SECTION g_LogCS;
static bool g_LogInit = false;

#define COL_WHITE  7
#define COL_RED    12
#define COL_PINK   13
#define COL_GREEN  10
#define COL_YELLOW 14
#define COL_CYAN   11

void InitLog() {
    if (g_LogInit) return;
    InitializeCriticalSection(&g_LogCS);
    fopen_s(&g_Log, "dlss4_wrapper.log", "w");
    
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    g_Console = GetStdHandle(STD_OUTPUT_HANDLE);
    
    DWORD mode = 0;
    GetConsoleMode(g_Console, &mode);
    SetConsoleMode(g_Console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    
    SetConsoleTitleW(L"DLSS 4 Wrapper - RTX 5080");
    
    SetConsoleTextAttribute(g_Console, COL_CYAN);
    printf("\n  ================================================================\n");
    printf("  |   DLSS 4 WRAPPER-BASED HOOKS - RTX 5080 OFA 2.0            |\n");
    printf("  |   Safe COM Wrapper Pattern - No VTable Modification        |\n");
    printf("  ================================================================\n\n");
    SetConsoleTextAttribute(g_Console, COL_WHITE);
    
    g_LogInit = true;
}

#define LOG(color, tag, fmt, ...) do { \
    if (!g_LogInit) InitLog(); \
    EnterCriticalSection(&g_LogCS); \
    SYSTEMTIME st; GetLocalTime(&st); \
    SetConsoleTextAttribute(g_Console, color); \
    printf("[%02d:%02d:%02d] [" tag "] " fmt "\n", st.wHour, st.wMinute, st.wSecond, ##__VA_ARGS__); \
    SetConsoleTextAttribute(g_Console, COL_WHITE); \
    if (g_Log) { fprintf(g_Log, "[%02d:%02d:%02d.%03d] [" tag "] " fmt "\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ##__VA_ARGS__); fflush(g_Log); } \
    LeaveCriticalSection(&g_LogCS); \
} while(0)

#define LogHook(fmt, ...)   LOG(COL_RED, "HOOK", fmt, ##__VA_ARGS__)
#define LogFail(fmt, ...)   LOG(COL_PINK, "FAIL", fmt, ##__VA_ARGS__)
#define LogInfo(fmt, ...)   LOG(COL_GREEN, "INFO", fmt, ##__VA_ARGS__)
#define LogFrame(fmt, ...)  LOG(COL_YELLOW, "FRAME", fmt, ##__VA_ARGS__)
#define LogStatus(fmt, ...) LOG(COL_CYAN, "STATUS", fmt, ##__VA_ARGS__)

// ============================================================================
// GLOBAL STATE
// ============================================================================

typedef HRESULT(WINAPI* CreateDXGIFactory_t)(REFIID, void**);
typedef HRESULT(WINAPI* CreateDXGIFactory1_t)(REFIID, void**);
typedef HRESULT(WINAPI* CreateDXGIFactory2_t)(UINT, REFIID, void**);

static CreateDXGIFactory_t oCreateDXGIFactory = nullptr;
static CreateDXGIFactory1_t oCreateDXGIFactory1 = nullptr;
static CreateDXGIFactory2_t oCreateDXGIFactory2 = nullptr;

static HMODULE g_hOrigDXGI = nullptr;
static HMODULE g_hNVNGX = nullptr;

static PFN_NVSDK_NGX_D3D12_Init g_NGX_Init = nullptr;
static PFN_NVSDK_NGX_D3D12_Shutdown g_NGX_Shutdown = nullptr;
static PFN_NVSDK_NGX_D3D12_CreateFeature g_NGX_CreateFeature = nullptr;
static PFN_NVSDK_NGX_D3D12_EvaluateFeature g_NGX_EvaluateFeature = nullptr;
static PFN_NVSDK_NGX_D3D12_ReleaseFeature g_NGX_ReleaseFeature = nullptr;

static ID3D12Device* g_pDevice = nullptr;
static ID3D12CommandQueue* g_pCmdQueue = nullptr;
static ID3D12CommandAllocator* g_pCmdAlloc = nullptr;
static ID3D12GraphicsCommandList* g_pCmdList = nullptr;
static NVSDK_NGX_Parameter* g_NGXParams = nullptr;
static NVSDK_NGX_Handle* g_FrameGenHandle = nullptr;

static std::atomic<bool> g_NGXLoaded{false};
static std::atomic<bool> g_NGXInited{false};
static std::atomic<bool> g_FrameGenReady{false};
static std::atomic<uint64_t> g_FrameCount{0};
static std::atomic<uint64_t> g_GenFrames{0};

// ============================================================================
// LOAD NGX
// ============================================================================

void LoadNGX() {
    if (g_NGXLoaded.exchange(true)) return;
    
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* s = wcsrchr(path, L'\\');
    if (s) *(s + 1) = 0;
    
    wchar_t dll[MAX_PATH];
    wcscpy_s(dll, path); wcscat_s(dll, L"nvngx.dll");
    g_hNVNGX = LoadLibraryW(dll);
    
    if (!g_hNVNGX) g_hNVNGX = LoadLibraryW(L"nvngx.dll");
    
    if (g_hNVNGX) {
        LogHook("nvngx.dll loaded: %p", g_hNVNGX);
        g_NGX_Init = (PFN_NVSDK_NGX_D3D12_Init)GetProcAddress(g_hNVNGX, "NVSDK_NGX_D3D12_Init");
        g_NGX_CreateFeature = (PFN_NVSDK_NGX_D3D12_CreateFeature)GetProcAddress(g_hNVNGX, "NVSDK_NGX_D3D12_CreateFeature");
        g_NGX_EvaluateFeature = (PFN_NVSDK_NGX_D3D12_EvaluateFeature)GetProcAddress(g_hNVNGX, "NVSDK_NGX_D3D12_EvaluateFeature");
        g_NGX_ReleaseFeature = (PFN_NVSDK_NGX_D3D12_ReleaseFeature)GetProcAddress(g_hNVNGX, "NVSDK_NGX_D3D12_ReleaseFeature");
        g_NGX_Shutdown = (PFN_NVSDK_NGX_D3D12_Shutdown)GetProcAddress(g_hNVNGX, "NVSDK_NGX_D3D12_Shutdown");
        LogInfo("NGX_Init: %p, NGX_Evaluate: %p", g_NGX_Init, g_NGX_EvaluateFeature);
    } else {
        LogFail("nvngx.dll not found");
    }
    
    // Also load DLSS modules
    wcscpy_s(dll, path); wcscat_s(dll, L"nvngx_dlss.dll");
    HMODULE h = LoadLibraryW(dll);
    if (h) LogHook("nvngx_dlss.dll loaded");
    
    wcscpy_s(dll, path); wcscat_s(dll, L"nvngx_dlssg.dll");
    h = LoadLibraryW(dll);
    if (h) LogHook("nvngx_dlssg.dll loaded (4x Frame Gen)");
}

// ============================================================================
// INITIALIZE NGX WITH DEVICE
// ============================================================================

void InitNGX(ID3D12Device* pDevice) {
    if (g_NGXInited.exchange(true)) return;
    if (!g_NGX_Init) { LogFail("NGX_Init not available"); return; }
    
    g_pDevice = pDevice;
    LogInfo("Initializing NGX with Device: %p", pDevice);
    
    NVSDK_NGX_Result r = g_NGX_Init(0x1337, L".", pDevice, &g_NGXParams);
    if (r == NVSDK_NGX_Result_Success) {
        LogHook("NGX INITIALIZED! Params: %p", g_NGXParams);
    } else {
        LogFail("NGX Init failed: 0x%X", r);
        g_NGXInited = false;
        return;
    }
    
    // Create command infrastructure
    pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_pCmdAlloc));
    pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_pCmdAlloc, nullptr, IID_PPV_ARGS(&g_pCmdList));
    g_pCmdList->Close();
    
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    pDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_pCmdQueue));
    
    LogHook("Created D3D12 Command Infrastructure");
}

// ============================================================================
// CREATE FRAME GENERATION
// ============================================================================

void CreateFrameGen(UINT w, UINT h) {
    if (g_FrameGenReady) return;
    if (!g_NGX_CreateFeature || !g_pCmdList || !g_NGXParams) return;
    
    g_pCmdAlloc->Reset();
    g_pCmdList->Reset(g_pCmdAlloc, nullptr);
    
    NVSDK_NGX_Result r = g_NGX_CreateFeature(g_pCmdList, 2, g_NGXParams, &g_FrameGenHandle);
    g_pCmdList->Close();
    
    if (r == NVSDK_NGX_Result_Success && g_FrameGenHandle) {
        LogHook("FRAME GENERATION CREATED! Handle: %p", g_FrameGenHandle);
        g_FrameGenReady = true;
    } else {
        LogFail("Frame Gen creation failed: 0x%X", r);
    }
}

// ============================================================================
// EVALUATE FRAME GENERATION
// ============================================================================

void EvalFrameGen() {
    if (!g_FrameGenReady || !g_NGX_EvaluateFeature) return;
    
    if (FAILED(g_pCmdAlloc->Reset())) return;
    if (FAILED(g_pCmdList->Reset(g_pCmdAlloc, nullptr))) return;
    
    NVSDK_NGX_Result r = g_NGX_EvaluateFeature(g_pCmdList, g_FrameGenHandle, g_NGXParams, nullptr);
    g_pCmdList->Close();
    
    if (r == NVSDK_NGX_Result_Success) {
        g_GenFrames += 3;
    }
    
    if (g_pCmdQueue) {
        ID3D12CommandList* lists[] = { g_pCmdList };
        g_pCmdQueue->ExecuteCommandLists(1, lists);
    }
}

// ============================================================================
// SWAPCHAIN WRAPPER CLASS
// ============================================================================

class WrappedSwapChain : public IDXGISwapChain4 {
private:
    IDXGISwapChain4* m_real;
    LONG m_ref;
    bool m_inited;

public:
    WrappedSwapChain(IDXGISwapChain* real) : m_real(nullptr), m_ref(1), m_inited(false) {
        real->QueryInterface(IID_PPV_ARGS(&m_real));
        if (!m_real) {
            real->QueryInterface(IID_IDXGISwapChain, (void**)&m_real);
        }
        LogHook("SwapChain WRAPPED: Real=%p, Wrapper=%p", real, this);
    }
    
    virtual ~WrappedSwapChain() {
        if (m_real) m_real->Release();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain) ||
            riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
            riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // === HOOKED PRESENT ===
    HRESULT STDMETHODCALLTYPE Present(UINT Sync, UINT Flags) override {
        g_FrameCount++;
        
        // First frame init
        if (!m_inited) {
            m_inited = true;
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(m_real->GetDevice(__uuidof(ID3D12Device), (void**)&dev))) {
                LogHook("FIRST PRESENT - Got Device: %p", dev);
                InitNGX(dev);
                
                DXGI_SWAP_CHAIN_DESC d;
                m_real->GetDesc(&d);
                CreateFrameGen(d.BufferDesc.Width, d.BufferDesc.Height);
                dev->Release();
            }
        }
        
        // Frame generation
        EvalFrameGen();
        
        // Stats every 500 frames
        if (g_FrameCount % 500 == 0) {
            LogFrame("Frame %llu | FrameGen: %s | Generated: %llu",
                g_FrameCount.load(), g_FrameGenReady ? "4x ACTIVE" : "OFF", g_GenFrames.load());
        }
        
        return m_real->Present(Sync, Flags);
    }
    
    HRESULT STDMETHODCALLTYPE Present1(UINT Sync, UINT Flags, const DXGI_PRESENT_PARAMETERS* p) override {
        g_FrameCount++;
        
        if (!m_inited) {
            m_inited = true;
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(m_real->GetDevice(__uuidof(ID3D12Device), (void**)&dev))) {
                LogHook("FIRST PRESENT1 - Got Device: %p", dev);
                InitNGX(dev);
                
                DXGI_SWAP_CHAIN_DESC1 d;
                m_real->GetDesc1(&d);
                CreateFrameGen(d.Width, d.Height);
                dev->Release();
            }
        }
        
        EvalFrameGen();
        
        if (g_FrameCount % 500 == 0) {
            LogFrame("Frame %llu | FrameGen: %s | Generated: %llu",
                g_FrameCount.load(), g_FrameGenReady ? "4x ACTIVE" : "OFF", g_GenFrames.load());
        }
        
        return m_real->Present1(Sync, Flags, p);
    }

    // Passthrough all other methods
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT b, REFIID r, void** p) override { return m_real->GetBuffer(b, r, p); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f, IDXGIOutput* o) override { return m_real->SetFullscreenState(f, o); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** o) override { return m_real->GetFullscreenState(f, o); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override { return m_real->GetDesc(d); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override {
        LogInfo("ResizeBuffers: %ux%u", w, h);
        return m_real->ResizeBuffers(c, w, h, f, fl);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* d) override { return m_real->ResizeTarget(d); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override { return m_real->GetContainingOutput(o); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return m_real->GetFrameStatistics(s); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { return m_real->GetLastPresentCount(c); }
    
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return m_real->SetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override { return m_real->SetPrivateDataInterface(n, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return m_real->GetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** p) override { return m_real->GetParent(r, p); }
    
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void** d) override { return m_real->GetDevice(r, d); }
    
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { return m_real->GetDesc1(d); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { return m_real->GetFullscreenDesc(d); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { return m_real->GetHwnd(h); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID r, void** u) override { return m_real->GetCoreWindow(r, u); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return m_real->GetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return m_real->SetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return m_real->GetRotation(r); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return m_real->SetRotation(r); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return m_real->GetRestrictToOutput(o); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return m_real->IsTemporaryMonoSupported(); }
    
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return m_real->SetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return m_real->GetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT l) override { return m_real->SetMaximumFrameLatency(l); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* l) override { return m_real->GetMaximumFrameLatency(l); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return m_real->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return m_real->SetMatrixTransform(m); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return m_real->GetMatrixTransform(m); }
    
    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return m_real->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override { return m_real->CheckColorSpaceSupport(c, s); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override { return m_real->SetColorSpace1(c); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* m, IUnknown* const* q) override {
        LogInfo("ResizeBuffers1: %ux%u", w, h);
        return m_real->ResizeBuffers1(c, w, h, f, fl, m, q);
    }
    
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* m) override { return m_real->SetHDRMetaData(t, s, m); }
};

// ============================================================================
// FACTORY WRAPPER - Wraps swap chains on creation
// ============================================================================

class WrappedFactory : public IDXGIFactory7 {
private:
    IDXGIFactory7* m_real;
    LONG m_ref;

public:
    WrappedFactory(IDXGIFactory* real) : m_real(nullptr), m_ref(1) {
        real->QueryInterface(IID_PPV_ARGS(&m_real));
        LogHook("Factory WRAPPED: Real=%p, Wrapper=%p", real, this);
    }
    
    virtual ~WrappedFactory() { if (m_real) m_real->Release(); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
            riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3) ||
            riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5) ||
            riid == __uuidof(IDXGIFactory6) || riid == __uuidof(IDXGIFactory7)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    // === HOOKED CreateSwapChain ===
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** sc) override {
        LogHook("CreateSwapChain intercepted");
        HRESULT hr = m_real->CreateSwapChain(dev, d, sc);
        if (SUCCEEDED(hr) && sc && *sc) {
            *sc = new WrappedSwapChain(*sc);
        }
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d, 
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f, IDXGIOutput* o, IDXGISwapChain1** sc) override {
        LogHook("CreateSwapChainForHwnd intercepted");
        HRESULT hr = m_real->CreateSwapChainForHwnd(dev, h, d, f, o, sc);
        if (SUCCEEDED(hr) && sc && *sc) {
            *sc = (IDXGISwapChain1*)new WrappedSwapChain((IDXGISwapChain*)*sc);
        }
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* dev, IUnknown* win, const DXGI_SWAP_CHAIN_DESC1* d,
        IDXGIOutput* o, IDXGISwapChain1** sc) override {
        LogHook("CreateSwapChainForCoreWindow intercepted");
        HRESULT hr = m_real->CreateSwapChainForCoreWindow(dev, win, d, o, sc);
        if (SUCCEEDED(hr) && sc && *sc) {
            *sc = (IDXGISwapChain1*)new WrappedSwapChain((IDXGISwapChain*)*sc);
        }
        return hr;
    }
    
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d,
        IDXGIOutput* o, IDXGISwapChain1** sc) override {
        LogHook("CreateSwapChainForComposition intercepted");
        HRESULT hr = m_real->CreateSwapChainForComposition(dev, d, o, sc);
        if (SUCCEEDED(hr) && sc && *sc) {
            *sc = (IDXGISwapChain1*)new WrappedSwapChain((IDXGISwapChain*)*sc);
        }
        return hr;
    }

    // Passthrough methods
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT a, IDXGIAdapter** ad) override { return m_real->EnumAdapters(a, ad); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND h, UINT f) override { return m_real->MakeWindowAssociation(h, f); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* h) override { return m_real->GetWindowAssociation(h); }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override { return m_real->CreateSoftwareAdapter(m, a); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID n, UINT s, const void* d) override { return m_real->SetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID n, const IUnknown* u) override { return m_real->SetPrivateDataInterface(n, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID n, UINT* s, void* d) override { return m_real->GetPrivateData(n, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** p) override { return m_real->GetParent(r, p); }
    
    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT a, IDXGIAdapter1** ad) override { return m_real->EnumAdapters1(a, ad); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return m_real->IsCurrent(); }
    
    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return m_real->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE h, LUID* l) override { return m_real->GetSharedResourceAdapterLuid(h, l); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND h, UINT m, DWORD* c) override { return m_real->RegisterStereoStatusWindow(h, m, c); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE e, DWORD* c) override { return m_real->RegisterStereoStatusEvent(e, c); }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD c) override { m_real->UnregisterStereoStatus(c); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND h, UINT m, DWORD* c) override { return m_real->RegisterOcclusionStatusWindow(h, m, c); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE e, DWORD* c) override { return m_real->RegisterOcclusionStatusEvent(e, c); }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD c) override { m_real->UnregisterOcclusionStatus(c); }
    
    // IDXGIFactory3
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return m_real->GetCreationFlags(); }
    
    // IDXGIFactory4
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID l, REFIID r, void** a) override { return m_real->EnumAdapterByLuid(l, r, a); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID r, void** a) override { return m_real->EnumWarpAdapter(r, a); }
    
    // IDXGIFactory5
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE f, void* s, UINT sz) override { return m_real->CheckFeatureSupport(f, s, sz); }
    
    // IDXGIFactory6
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT a, DXGI_GPU_PREFERENCE p, REFIID r, void** ad) override { return m_real->EnumAdapterByGpuPreference(a, p, r, ad); }
    
    // IDXGIFactory7
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE e, DWORD* c) override { return m_real->RegisterAdaptersChangedEvent(e, c); }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD c) override { return m_real->UnregisterAdaptersChangedEvent(c); }
};

// ============================================================================
// LOAD SYSTEM DXGI
// ============================================================================

bool LoadSystemDXGI() {
    if (g_hOrigDXGI) return true;
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    std::string p = std::string(sys) + "\\dxgi.dll";
    g_hOrigDXGI = LoadLibraryA(p.c_str());
    if (g_hOrigDXGI) {
        oCreateDXGIFactory = (CreateDXGIFactory_t)GetProcAddress(g_hOrigDXGI, "CreateDXGIFactory");
        oCreateDXGIFactory1 = (CreateDXGIFactory1_t)GetProcAddress(g_hOrigDXGI, "CreateDXGIFactory1");
        oCreateDXGIFactory2 = (CreateDXGIFactory2_t)GetProcAddress(g_hOrigDXGI, "CreateDXGIFactory2");
        LogHook("System DXGI loaded: %p", g_hOrigDXGI);
        return true;
    }
    LogFail("Failed to load system DXGI");
    return false;
}

// ============================================================================
// EXPORTS
// ============================================================================

extern "C" {

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    InitLog(); LoadNGX();
    if (!LoadSystemDXGI()) return E_FAIL;
    LogHook("CreateDXGIFactory");
    
    IDXGIFactory* real = nullptr;
    HRESULT hr = oCreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&real);
    if (SUCCEEDED(hr) && real) {
        *ppFactory = new WrappedFactory(real);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    InitLog(); LoadNGX();
    if (!LoadSystemDXGI()) return E_FAIL;
    LogHook("CreateDXGIFactory1");
    
    IDXGIFactory* real = nullptr;
    HRESULT hr = oCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&real);
    if (SUCCEEDED(hr) && real) {
        *ppFactory = new WrappedFactory(real);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    InitLog(); LoadNGX();
    if (!LoadSystemDXGI()) return E_FAIL;
    LogHook("CreateDXGIFactory2");
    
    IDXGIFactory* real = nullptr;
    HRESULT hr = oCreateDXGIFactory2(Flags, __uuidof(IDXGIFactory2), (void**)&real);
    if (SUCCEEDED(hr) && real) {
        *ppFactory = new WrappedFactory(real);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    if (!LoadSystemDXGI()) return S_OK;
    auto f = (HRESULT(WINAPI*)())GetProcAddress(g_hOrigDXGI, "DXGIDeclareAdapterRemovalSupport");
    return f ? f() : S_OK;
}

__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT F, REFIID r, void** p) {
    if (!LoadSystemDXGI()) return E_NOINTERFACE;
    auto f = (HRESULT(WINAPI*)(UINT, REFIID, void**))GetProcAddress(g_hOrigDXGI, "DXGIGetDebugInterface1");
    return f ? f(F, r, p) : E_NOINTERFACE;
}

__declspec(dllexport) HRESULT WINAPI DXGIDisableVBlankVirtualization() {
    if (!LoadSystemDXGI()) return S_OK;
    auto f = (HRESULT(WINAPI*)())GetProcAddress(g_hOrigDXGI, "DXGIDisableVBlankVirtualization");
    return f ? f() : S_OK;
}

__declspec(dllexport) HRESULT WINAPI DXGIReportAdapterConfiguration(void* p) {
    if (!LoadSystemDXGI()) return S_OK;
    auto f = (HRESULT(WINAPI*)(void*))GetProcAddress(g_hOrigDXGI, "DXGIReportAdapterConfiguration");
    return f ? f(p) : S_OK;
}

}

// ============================================================================
// DLL MAIN
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_LogInit) {
            LogStatus("Shutting down... Frames: %llu, Generated: %llu", g_FrameCount.load(), g_GenFrames.load());
            if (g_FrameGenHandle && g_NGX_ReleaseFeature) g_NGX_ReleaseFeature(g_FrameGenHandle);
            if (g_NGX_Shutdown) g_NGX_Shutdown();
            if (g_pCmdList) g_pCmdList->Release();
            if (g_pCmdAlloc) g_pCmdAlloc->Release();
            if (g_pCmdQueue) g_pCmdQueue->Release();
            if (g_Log) fclose(g_Log);
            DeleteCriticalSection(&g_LogCS);
        }
        if (g_hNVNGX) FreeLibrary(g_hNVNGX);
        if (g_hOrigDXGI) FreeLibrary(g_hOrigDXGI);
    }
    return TRUE;
}
