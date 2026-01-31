#include <d3d12.h>
#include <dxgi1_4.h>
#include "hooks.h"
#include "logger.h"
#include "dlss4_config.h"
#include "ngx_wrapper.h"
#include "streamline_integration.h"
#include <mutex>
#include <atomic>
#include <stdio.h>
#include "input_handler.h"
#include "overlay.h"

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

// ============================================================================
// VMT HOOKING UTILITY
// ============================================================================

bool HookVirtualMethod(void* pObject, int index, void* pHook, void** ppOriginal) {
    if (!pObject) return false;
    void** vtable = *reinterpret_cast<void***>(pObject);
    if (!vtable) return false;
    *ppOriginal = vtable[index];
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    vtable[index] = pHook;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
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
        }
    }
    if (!g_OriginalD3D12CreateDevice) return E_FAIL;

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
    if (SUCCEEDED(g_OriginalResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags))) {
        g_SwapChainState.width = Width;
        g_SwapChainState.height = Height;
    }
    return S_OK;
}

#include "pattern_scanner.h"

// ============================================================================
// HOOK INIT
// ============================================================================

void HookFactoryIfNeeded(void* pFactory) {
    LogStartup("HookFactoryIfNeeded Entry");
    std::lock_guard<std::mutex> lock(g_HookMutex);
    
    if (g_HooksInitialized) return;
    
    LOG_INFO("Scanning for Camera Data...");
    auto jitterOffset = PatternScanner::Scan("ACValhalla.exe", "F3 0F 10 ?? ?? ?? ?? ?? 0F 28 ?? F3 0F 11 ?? ?? ?? ?? ??");
    
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DLSS4ProxyDummy";
    RegisterClassExW(&wc);
    
    HWND dummyHwnd = CreateWindowExW(0, L"DLSS4ProxyDummy", L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    
    IDXGIFactory4* pFactory4 = nullptr;
    ((IUnknown*)pFactory)->QueryInterface(__uuidof(IDXGIFactory4), (void**)&pFactory4);
    
    IDXGIAdapter1* pAdapter = nullptr;
    pFactory4->EnumAdapters1(0, &pAdapter);
    
    ID3D12Device* pDevice = nullptr;
    D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&pDevice);
    
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* pCommandQueue = nullptr;
    pDevice->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)&pCommandQueue);
    
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = 100; scDesc.Height = 100; scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.SampleDesc.Count = 1; scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2; scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    
    IDXGISwapChain1* pSwapChain1 = nullptr;
    pFactory4->CreateSwapChainForHwnd(pCommandQueue, dummyHwnd, &scDesc, nullptr, nullptr, &pSwapChain1);
    
    // Only hook Command Queue, leave SwapChain to Wrapper
    HookVirtualMethod(pCommandQueue, 10, HookedExecuteCommandLists, (void**)&g_OriginalExecuteCommandLists);
    
    g_HooksInitialized = true;
    
    pSwapChain1->Release();
    pCommandQueue->Release();
    pDevice->Release();
    pAdapter->Release();
    pFactory4->Release();
    DestroyWindow(dummyHwnd);
    UnregisterClassW(L"DLSS4ProxyDummy", wc.hInstance);
}

bool InitializeHooks() { return g_HooksInitialized; }
void CleanupHooks() { 
    g_HooksInitialized = false; 
    StreamlineIntegration::Get().Shutdown();
}
