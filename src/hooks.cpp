#include <d3d12.h>
#include <dxgi1_4.h>
#include "hooks.h"
#include "iat_utils.h"
#include "logger.h"
#include "dlss4_config.h"
#include "ngx_wrapper.h"
#include "streamline_integration.h"
#include <mutex>
#include <atomic>
#include <stdio.h>
#include "input_handler.h"
#include "overlay.h"
#include "vtable_utils.h"

extern "C" void LogStartup(const char* msg);

// ============================================================================
// GLOBAL STATE
// ============================================================================

SwapChainHookState g_SwapChainState;
PFN_Present g_OriginalPresent = nullptr;
PFN_Present1 g_OriginalPresent1 = nullptr;
PFN_ResizeBuffers g_OriginalResizeBuffers = nullptr;

static std::mutex g_HookMutex;
static std::atomic<bool> g_HooksInitialized(false);
static std::atomic<uint64_t> g_FrameCount(0);
static std::atomic<uintptr_t> g_JitterAddress(0);
static std::atomic<bool> g_JitterValid(false);
static std::atomic<bool> g_WrappedCommandListUsed(false);

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

bool TryGetPatternJitter(float& jitterX, float& jitterY) {
    uintptr_t addr = g_JitterAddress.load();
    if (!addr) return false;
    __try {
        const float* vals = reinterpret_cast<const float*>(addr);
        jitterX = vals[0];
        jitterY = vals[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ============================================================================
// VMT HOOKING UTILITY
// ============================================================================

bool HookVirtualMethod(void* pObject, int index, void* pHook, void** ppOriginal) {
    if (!pObject || !pHook || !ppOriginal || index < 0) {
        LOG_ERROR("HookVirtualMethod invalid params");
        return false;
    }
    void** vtable = nullptr;
    void** entry = nullptr;
    if (!ResolveVTableEntry(pObject, index, &vtable, &entry)) {
        LOG_ERROR("HookVirtualMethod invalid vtable entry (index %d)", index);
        return false;
    }
    *ppOriginal = *entry;
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("HookVirtualMethod VirtualProtect failed");
        return false;
    }
    *entry = pHook;
    DWORD restoreProtect = 0;
    if (!VirtualProtect(entry, sizeof(void*), oldProtect, &restoreProtect)) {
        LOG_ERROR("HookVirtualMethod VirtualProtect restore failed");
    }
    return true;
}

#include "resource_detector.h"
#include "d3d12_wrappers.h"

// ============================================================================
// D3D12 DEVICE HOOK
// ============================================================================

PFN_D3D12CreateDevice g_OriginalD3D12CreateDevice = nullptr;

HRESULT WINAPI Hooked_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    if (!g_OriginalD3D12CreateDevice) {
        HMODULE hD3D12 = LoadLibraryW(L"d3d12.dll");
        if (hD3D12) {
            g_OriginalD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
        } else {
            LOG_ERROR("Failed to load d3d12.dll");
        }
    }
    if (!g_OriginalD3D12CreateDevice) {
        LOG_ERROR("D3D12CreateDevice not found");
        return E_FAIL;
    }

    ID3D12Device* pRealDevice = nullptr;
    HRESULT hr = g_OriginalD3D12CreateDevice(pAdapter, MinimumFeatureLevel, __uuidof(ID3D12Device), (void**)&pRealDevice);

    if (FAILED(hr) || !pRealDevice) return hr;

    StreamlineIntegration::Get().Initialize(pRealDevice);

    WrappedID3D12Device* pWrappedDevice = new WrappedID3D12Device(pRealDevice);
    hr = pWrappedDevice->QueryInterface(riid, ppDevice);
    pWrappedDevice->Release(); 
    pRealDevice->Release();    

    return hr;
}

// ============================================================================
// COMMAND QUEUE HOOK
// ============================================================================

PFN_ExecuteCommandLists g_OriginalExecuteCommandLists = nullptr;

void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    ResourceDetector::Get().NewFrame();
    StreamlineIntegration::Get().SetCommandQueue(pThis);

    ID3D12Resource* pColor = ResourceDetector::Get().GetBestColorCandidate();
    ID3D12Resource* pDepth = ResourceDetector::Get().GetBestDepthCandidate();
    ID3D12Resource* pMVs = ResourceDetector::Get().GetBestMotionVectorCandidate();

    if (pColor) StreamlineIntegration::Get().TagColorBuffer(pColor);
    if (pDepth) StreamlineIntegration::Get().TagDepthBuffer(pDepth);
    if (pMVs) StreamlineIntegration::Get().TagMotionVectors(pMVs);

    g_OriginalExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
}

// ============================================================================
// PRESENT HOOKS
// ============================================================================

// Standard Present (Hooked but we use Wrapper mostly)
HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags) {
    g_FrameCount++;
    return g_OriginalPresent(pThis, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* pThis, UINT SyncInterval, 
    UINT PresentFlags, const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    g_FrameCount++;
    return g_OriginalPresent1(pThis, SyncInterval, PresentFlags, pPresentParameters);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* pThis, UINT BufferCount, 
    UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    HRESULT hr = g_OriginalResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    if (SUCCEEDED(hr)) {
        g_SwapChainState.width = Width;
        g_SwapChainState.height = Height;
    }
    return hr;
}

#include "pattern_scanner.h"

// ============================================================================
// VTable Hooked Functions
// ============================================================================

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCommandList)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
PFN_CreateCommandList g_OriginalCreateCommandList = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* PFN_Close)(ID3D12GraphicsCommandList*);
PFN_Close g_OriginalClose = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCommittedResource)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
PFN_CreateCommittedResource g_OriginalCreateCommittedResource = nullptr;

HRESULT STDMETHODCALLTYPE Hooked_CreateCommittedResource(ID3D12Device* pThis, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    HRESULT hr = g_OriginalCreateCommittedResource(pThis, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riid, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        // OPTIMIZATION: Only scan small buffers (Typical CBVs are < 64KB) to avoid hangs
        if (pDesc && pDesc->Width <= 65536 && pHeapProperties && pHeapProperties->Type == D3D12_HEAP_TYPE_UPLOAD && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            ID3D12Resource* pRes = (ID3D12Resource*)*ppvResource;
            ResourceDetector::Get().RegisterResource(pRes);
            uint8_t* mapped = nullptr;
            D3D12_RANGE range = { 0, 0 };
            if (SUCCEEDED(pRes->Map(0, &range, reinterpret_cast<void**>(&mapped))) && mapped) {
                RegisterCbv(pRes, pDesc->Width, mapped);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_Close(ID3D12GraphicsCommandList* pThis) {
    if (IsWrappedCommandListUsed()) {
        return g_OriginalClose(pThis);
    }
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    TryGetPatternJitter(jitterX, jitterY);

    float view[16], proj[16], score = 0.0f;
    // Call the now-safe scanner
    if (TryScanAllCbvsForCamera(view, proj, &score, false)) {
        if (score > 0.4f) {
            // Extract jitter from Projection matrix if pattern scanner failed
            if (jitterX == 0.0f && jitterY == 0.0f) {
                jitterX = proj[8];
                jitterY = proj[9];
                LOG_INFO("Found Jitter via Matrix Scan: %.4f, %.4f", jitterX, jitterY);
            }
            StreamlineIntegration::Get().SetCameraData(view, proj, jitterX, jitterY);
        }
    }

    StreamlineIntegration::Get().EvaluateDLSS(pThis);
    
    return g_OriginalClose(pThis);
}

HRESULT STDMETHODCALLTYPE Hooked_CreateCommandList(ID3D12Device* pThis, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) {
    HRESULT hr = g_OriginalCreateCommandList(pThis, nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
    if (SUCCEEDED(hr) && ppCommandList && *ppCommandList) {
        // We could wrap it here, but we are using VTable hooks now.
    }
    return hr;
}

// ============================================================================
// HOOK INIT
// ============================================================================

void HookFactoryIfNeeded(void* pFactory) {
    LogStartup("HookFactoryIfNeeded Entry");
    std::lock_guard<std::mutex> lock(g_HookMutex);
    
    if (g_HooksInitialized) return;
    if (!pFactory) return;
    
    LOG_INFO("Scanning for Camera Data...");
    auto jitterOffset = PatternScanner::Scan("ACValhalla.exe", "F3 0F 10 ?? ?? ?? ?? ?? 0F 28 ?? F3 0F 11 ?? ?? ?? ?? ??");
    if (jitterOffset) {
        // Resolve Relative Address (RIP-relative)
        // Instruction: F3 0F 10 05 [RelOffset] (MOVSS xmm0, [RIP+Offset])
        uint8_t* instruction = (uint8_t*)*jitterOffset;
        int32_t relOffset = *(int32_t*)(instruction + 4);
        uintptr_t absoluteAddress = (uintptr_t)(instruction + 8 + relOffset); // RIP is Next Instruction (8 bytes)

        SetPatternJitterAddress(absoluteAddress);
        LOG_INFO("Jitter pattern found at 0x%p -> Absolute Address: 0x%p", (void*)*jitterOffset, (void*)absoluteAddress);
    } else {
        LOG_WARN("Jitter pattern not found");
    }
    
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DLSS4ProxyDummy";
    RegisterClassExW(&wc);
    
    HWND dummyHwnd = CreateWindowExW(0, L"DLSS4ProxyDummy", L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    
    IDXGIFactory4* pFactory4 = nullptr;
    if (FAILED(((IUnknown*)pFactory)->QueryInterface(__uuidof(IDXGIFactory4), (void**)&pFactory4)) || !pFactory4) {
        DestroyWindow(dummyHwnd);
        UnregisterClassW(L"DLSS4ProxyDummy", wc.hInstance);
        return;
    }
    
    IDXGIAdapter1* pAdapter = nullptr;
    if (FAILED(pFactory4->EnumAdapters1(0, &pAdapter)) || !pAdapter) {
        pFactory4->Release();
        DestroyWindow(dummyHwnd);
        UnregisterClassW(L"DLSS4ProxyDummy", wc.hInstance);
        return;
    }
    
    ID3D12Device* pDevice = nullptr;
    HRESULT hr = D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&pDevice);
    if (FAILED(hr) || !pDevice) {
        pAdapter->Release();
        pFactory4->Release();
        DestroyWindow(dummyHwnd);
        UnregisterClassW(L"DLSS4ProxyDummy", wc.hInstance);
        return;
    }
    
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* pCommandQueue = nullptr;
    hr = pDevice->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)&pCommandQueue);
    
    ID3D12GraphicsCommandList* pList = nullptr;
    ID3D12CommandAllocator* pAlloc = nullptr;
    pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&pAlloc);
    pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAlloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&pList);

    // HOOK EVERYTHING VIA VTABLE
    if (pCommandQueue) HookVirtualMethod(pCommandQueue, 10, HookedExecuteCommandLists, (void**)&g_OriginalExecuteCommandLists);
    if (pDevice) {
        HookVirtualMethod(pDevice, 9, Hooked_CreateCommandList, (void**)&g_OriginalCreateCommandList);
        HookVirtualMethod(pDevice, 27, Hooked_CreateCommittedResource, (void**)&g_OriginalCreateCommittedResource);
    }
    if (pList) HookVirtualMethod(pList, 9, Hooked_Close, (void**)&g_OriginalClose);
    
    g_HooksInitialized = true;
    
    if (pList) pList->Release();
    if (pAlloc) pAlloc->Release();
    if (pCommandQueue) pCommandQueue->Release();
    pDevice->Release();
    pAdapter->Release();
    pFactory4->Release();
    DestroyWindow(dummyHwnd);
    UnregisterClassW(L"DLSS4ProxyDummy", wc.hInstance);
}

// Hook GetProcAddress to intercept D3D12CreateDevice dynamic loading
typedef FARPROC(WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);
PFN_GetProcAddress g_OriginalGetProcAddress = nullptr;

FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    // Check if looking for D3D12CreateDevice (by name, ignore ordinals high word)
    if (HIWORD(lpProcName) != 0) {
        if (strcmp(lpProcName, "D3D12CreateDevice") == 0) {
            LogStartup("Intercepted GetProcAddress(D3D12CreateDevice)");
            LOG_INFO("Intercepted GetProcAddress(D3D12CreateDevice)");
            // Ensure we have the original pointer
            if (!g_OriginalD3D12CreateDevice) {
                // Since they asked for it, we can get it from the module they passed (or load d3d12 if needed)
                g_OriginalD3D12CreateDevice = (PFN_D3D12CreateDevice)g_OriginalGetProcAddress(hModule, lpProcName);
            }
            return (FARPROC)Hooked_D3D12CreateDevice;
        }
    }
    return g_OriginalGetProcAddress(hModule, lpProcName);
}

void InstallGetProcAddressHook() {
    LogStartup("Installing GetProcAddress Hook...");
    // We hook GetProcAddress in ALL module IATs to be sure we catch it
    HookAllModulesIAT("kernel32.dll", "GetProcAddress", (void*)Hooked_GetProcAddress, (void**)&g_OriginalGetProcAddress);
    
    if (g_OriginalGetProcAddress) {
        LOG_INFO("Successfully hooked GetProcAddress (System-wide)");
        LogStartup("GetProcAddress Hook Success");
    } else {
        LOG_WARN("Failed to hook GetProcAddress in any module");
        LogStartup("GetProcAddress Hook Failed");
    }
}

void InstallD3D12Hooks() {
    LogStartup("InstallD3D12Hooks Entry");
    
    // 1. Hook D3D12CreateDevice in ALL currently loaded modules
    HookAllModulesIAT("d3d12.dll", "D3D12CreateDevice", (void*)Hooked_D3D12CreateDevice, (void**)&g_OriginalD3D12CreateDevice);
    
    if (g_OriginalD3D12CreateDevice) {
        LOG_INFO("Successfully hooked D3D12CreateDevice (System-wide)");
        LogStartup("D3D12 Hook Success");
    }

    // 2. Install GetProcAddress Hook to catch future dynamic loads
    InstallGetProcAddressHook();
}

bool InitializeHooks() { return g_HooksInitialized; }
void CleanupHooks() { 
    g_HooksInitialized = false; 
    StreamlineIntegration::Get().Shutdown();
}
