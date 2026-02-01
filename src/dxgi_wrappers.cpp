#include "dxgi_wrappers.h"
#include "streamline_integration.h"
#include "logger.h"
#include "hooks.h"
#include "input_handler.h"
#include "overlay.h"
#include "dlss4_config.h"
#include <stdio.h>
#include <sysinfoapi.h>
#include <mutex>
#include <atomic>

extern "C" void LogStartup(const char* msg);

template <typename T>
struct ScopedInterface {
    T* ptr;
    ScopedInterface(IUnknown* p) : ptr(nullptr) { if (p) p->QueryInterface(__uuidof(T), (void**)&ptr); }
    ~ScopedInterface() { if (ptr) ptr->Release(); }
    operator T*() { return ptr; }
    T* operator->() { return ptr; }
};

WrappedIDXGIFactory::WrappedIDXGIFactory(IDXGIFactory* pReal) : m_pReal(pReal), m_refCount(1) {
    if (m_pReal) m_pReal->AddRef();
    LogStartup("WrappedIDXGIFactory Created");
}

WrappedIDXGIFactory::~WrappedIDXGIFactory() {
    if (m_pReal) m_pReal->Release();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory)) {
        *ppvObject = this; AddRef(); return S_OK;
    }
    IUnknown* test = nullptr;
    if (SUCCEEDED(m_pReal->QueryInterface(riid, (void**)&test))) {
        test->Release(); *ppvObject = this; AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WrappedIDXGIFactory::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedIDXGIFactory::Release() {
    ULONG r = InterlockedDecrement(&m_refCount);
    if ((LONG)r <= 0) {
        if ((LONG)r < 0) {
            LogStartup("WARNING: WrappedIDXGIFactory refcount underflow");
        }
        delete this;
        return 0;
    }
    return r;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumAdapters(UINT A, IDXGIAdapter** P) { 
    InstallD3D12Hooks();
    HookFactoryIfNeeded(m_pReal); return m_pReal->EnumAdapters(A, P); 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::MakeWindowAssociation(HWND H, UINT F) { return m_pReal->MakeWindowAssociation(H, F); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetWindowAssociation(HWND* H) { return m_pReal->GetWindowAssociation(H); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSoftwareAdapter(HMODULE M, IDXGIAdapter** P) { return m_pReal->CreateSoftwareAdapter(M, P); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::SetPrivateData(REFGUID N, UINT S, const void* D) { return m_pReal->SetPrivateData(N, S, D); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::SetPrivateDataInterface(REFGUID N, const IUnknown* U) { return m_pReal->SetPrivateDataInterface(N, U); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetPrivateData(REFGUID N, UINT* S, void* D) { return m_pReal->GetPrivateData(N, S, D); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetParent(REFIID R, void** P) { return m_pReal->GetParent(R, P); }

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumAdapters1(UINT A, IDXGIAdapter1** P) { 
    InstallD3D12Hooks();
    ScopedInterface<IDXGIFactory1> real(m_pReal); return real ? real->EnumAdapters1(A, P) : E_NOINTERFACE; 
}
BOOL STDMETHODCALLTYPE WrappedIDXGIFactory::IsCurrent() { 
    ScopedInterface<IDXGIFactory1> real(m_pReal); return real ? real->IsCurrent() : FALSE; 
}
BOOL STDMETHODCALLTYPE WrappedIDXGIFactory::IsWindowedStereoEnabled() { 
    ScopedInterface<IDXGIFactory2> real(m_pReal); return real ? real->IsWindowedStereoEnabled() : FALSE; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetSharedResourceAdapterLuid(HANDLE H, LUID* L) { 
    ScopedInterface<IDXGIFactory2> real(m_pReal); return real ? real->GetSharedResourceAdapterLuid(H, L) : E_NOINTERFACE; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::RegisterStereoStatusWindow(HWND H, UINT M, DWORD* C) { 
    ScopedInterface<IDXGIFactory2> real(m_pReal); return real ? real->RegisterStereoStatusWindow(H, M, C) : E_NOINTERFACE; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::RegisterStereoStatusEvent(HANDLE H, DWORD* C) { 
    ScopedInterface<IDXGIFactory2> real(m_pReal); return real ? real->RegisterStereoStatusEvent(H, C) : E_NOINTERFACE; 
}
void STDMETHODCALLTYPE WrappedIDXGIFactory::UnregisterStereoStatus(DWORD C) { 
    ScopedInterface<IDXGIFactory2> real(m_pReal); if(real) real->UnregisterStereoStatus(C); 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::RegisterOcclusionStatusWindow(HWND H, UINT M, DWORD* C) { 
    ScopedInterface<IDXGIFactory2> real(m_pReal); return real ? real->RegisterOcclusionStatusWindow(H, M, C) : E_NOINTERFACE; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::RegisterOcclusionStatusEvent(HANDLE H, DWORD* C) { 
    ScopedInterface<IDXGIFactory2> real(m_pReal); return real ? real->RegisterOcclusionStatusEvent(H, C) : E_NOINTERFACE; 
}
void STDMETHODCALLTYPE WrappedIDXGIFactory::UnregisterOcclusionStatus(DWORD C) { 
    ScopedInterface<IDXGIFactory2> real(m_pReal); if(real) real->UnregisterOcclusionStatus(C); 
}
UINT STDMETHODCALLTYPE WrappedIDXGIFactory::GetCreationFlags() { 
    ScopedInterface<IDXGIFactory3> real(m_pReal); return real ? real->GetCreationFlags() : 0; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumAdapterByLuid(LUID L, REFIID R, void** P) { 
    ScopedInterface<IDXGIFactory4> real(m_pReal); return real ? real->EnumAdapterByLuid(L, R, P) : E_NOINTERFACE; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumWarpAdapter(REFIID R, void** P) { 
    ScopedInterface<IDXGIFactory4> real(m_pReal); return real ? real->EnumWarpAdapter(R, P) : E_NOINTERFACE; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CheckFeatureSupport(DXGI_FEATURE F, void* D, UINT S) { 
    ScopedInterface<IDXGIFactory5> real(m_pReal); return real ? real->CheckFeatureSupport(F, D, S) : E_NOINTERFACE; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumAdapterByGpuPreference(UINT A, DXGI_GPU_PREFERENCE G, REFIID R, void** P) { 
    ScopedInterface<IDXGIFactory6> real(m_pReal); return real ? real->EnumAdapterByGpuPreference(A, G, R, P) : E_NOINTERFACE; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::RegisterAdaptersChangedEvent(HANDLE H, DWORD* C) { 
    ScopedInterface<IDXGIFactory7> real(m_pReal); return real ? real->RegisterAdaptersChangedEvent(H, C) : E_NOINTERFACE; 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::UnregisterAdaptersChangedEvent(DWORD C) { 
    ScopedInterface<IDXGIFactory7> real(m_pReal); return real ? real->UnregisterAdaptersChangedEvent(C) : E_NOINTERFACE; 
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    LogStartup("WrappedFactory::CreateSwapChain");
    InstallD3D12Hooks();
    HookFactoryIfNeeded(m_pReal);
    HRESULT hr = m_pReal->CreateSwapChain(pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        WrappedIDXGISwapChain* wrapper = new WrappedIDXGISwapChain(*ppSwapChain, pDevice);
        *ppSwapChain = wrapper;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LogStartup("WrappedFactory::CreateSwapChainForHwnd");
    ScopedInterface<IDXGIFactory2> real(m_pReal); if (!real) return E_NOINTERFACE;
    HRESULT hr = real->CreateSwapChainForHwnd(pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        WrappedIDXGISwapChain* wrapper = new WrappedIDXGISwapChain((IDXGISwapChain*)*ppSwapChain, pDevice);
        *ppSwapChain = (IDXGISwapChain1*)wrapper;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LogStartup("WrappedFactory::CreateSwapChainForCoreWindow");
    ScopedInterface<IDXGIFactory2> real(m_pReal); if (!real) return E_NOINTERFACE;
    HRESULT hr = real->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        WrappedIDXGISwapChain* wrapper = new WrappedIDXGISwapChain((IDXGISwapChain*)*ppSwapChain, pDevice);
        *ppSwapChain = (IDXGISwapChain1*)wrapper;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChainForComposition(IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LogStartup("WrappedFactory::CreateSwapChainForComposition");
    ScopedInterface<IDXGIFactory2> real(m_pReal); if (!real) return E_NOINTERFACE;
    HRESULT hr = real->CreateSwapChainForComposition(pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        WrappedIDXGISwapChain* wrapper = new WrappedIDXGISwapChain((IDXGISwapChain*)*ppSwapChain, pDevice);
        *ppSwapChain = (IDXGISwapChain1*)wrapper;
    }
    return hr;
}

WrappedIDXGISwapChain::WrappedIDXGISwapChain(IDXGISwapChain* pReal, IUnknown* pDevice) : m_pReal(pReal), m_refCount(1) {
    if (m_pReal) m_pReal->AddRef();
    ID3D12Device* d3d12Device = nullptr;
    if (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12Device), (void**)&d3d12Device))) {
        m_initialized = StreamlineIntegration::Get().Initialize(d3d12Device);
        d3d12Device->Release();
    }
    ID3D12CommandQueue* queue = nullptr;
    if (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&queue))) {
        StreamlineIntegration::Get().SetCommandQueue(queue);
        queue->Release();
    }
}

WrappedIDXGISwapChain::~WrappedIDXGISwapChain() { if (m_pReal) m_pReal->Release(); }

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(IDXGISwapChain)) {
        *ppvObject = this; AddRef(); return S_OK;
    }
    IUnknown* test = nullptr;
    if (SUCCEEDED(m_pReal->QueryInterface(riid, (void**)&test))) {
        test->Release(); *ppvObject = this; AddRef(); return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain::Release() {
    ULONG r = InterlockedDecrement(&m_refCount);
    if ((LONG)r <= 0) {
        if ((LONG)r < 0) {
            LogStartup("WARNING: WrappedIDXGISwapChain refcount underflow");
        }
        delete this;
        return 0;
    }
    return r;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {
    static std::atomic<bool> inputsRegistered(false);
    bool expected = false;
    if (inputsRegistered.compare_exchange_strong(expected, true)) {
        OverlayUI::Get().Initialize(GetModuleHandleA("dxgi.dll"));
        InputHandler::Get().RegisterHotkey(VK_F5, [](){ MessageBeep(MB_OK); OverlayUI::Get().ToggleVisibility(); }, "Toggle Menu");
        InputHandler::Get().RegisterHotkey(VK_F6, [](){ MessageBeep(MB_OK); OverlayUI::Get().ToggleFPS(); }, "Toggle FPS");
        InputHandler::Get().RegisterHotkey(VK_F7, [](){ MessageBeep(MB_OK); OverlayUI::Get().ToggleVignette(); }, "Toggle Vignette");
        InputHandler::Get().RegisterHotkey(VK_F8, [](){
            float jx = 0.0f, jy = 0.0f;
            StreamlineIntegration::Get().GetLastCameraJitter(jx, jy);
            bool hasCam = StreamlineIntegration::Get().HasCameraData();
            LOG_INFO("F8 Debug: Camera=%s Jitter=(%.4f, %.4f)", hasCam ? "OK" : "MISSING", jx, jy);
            OverlayUI::Get().SetCameraStatus(hasCam, jx, jy);
            MessageBeep(hasCam ? MB_OK : MB_ICONHAND);
        }, "Debug Camera Status");
        
        InputHandler::Get().RegisterHotkey(VK_F9, [](){
            MessageBeep(MB_OK);
            StreamlineIntegration::Get().PrintMFGStatus();
        }, "Debug MFG Status");
        
        InputHandler::Get().InstallHook(); // Global Hook
    }
    InputHandler::Get().ProcessInput();
    
    // FPS LOGIC
    static uint64_t lastTime = 0;
    static uint64_t frameCount = 0;
    uint64_t currentTime = GetTickCount64();
    frameCount++;
    if (currentTime - lastTime >= 1000) {
        float fps = (float)frameCount * 1000.0f / (float)(currentTime - lastTime);
        int mult = StreamlineIntegration::Get().GetFrameGenMultiplier(); if (mult < 1) mult = 1;
        float totalFps = fps * (float)mult;
        OverlayUI::Get().SetFPS(fps, totalFps);
        lastTime = currentTime; frameCount = 0;
    }

    if (!StreamlineIntegration::Get().HasCameraData()) {
        float jx = 0.0f, jy = 0.0f;
        StreamlineIntegration::Get().GetLastCameraJitter(jx, jy);
        OverlayUI::Get().SetCameraStatus(false, jx, jy);
    }
    StreamlineIntegration::Get().NewFrame(m_pReal);
    StreamlineIntegration::Get().EvaluateFrameGen(m_pReal);
    return m_pReal->Present(0, Flags | DXGI_PRESENT_ALLOW_TEARING);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::Present1(UINT Sync, UINT Flags, const DXGI_PRESENT_PARAMETERS* p) {
    static std::atomic<bool> inputsRegistered1(false);
    bool expected = false;
    if (inputsRegistered1.compare_exchange_strong(expected, true)) {
        OverlayUI::Get().Initialize(GetModuleHandleA("dxgi.dll"));
        InputHandler::Get().RegisterHotkey(VK_F5, [](){ MessageBeep(MB_OK); OverlayUI::Get().ToggleVisibility(); }, "Toggle Menu");
        InputHandler::Get().RegisterHotkey(VK_F6, [](){ MessageBeep(MB_OK); OverlayUI::Get().ToggleFPS(); }, "Toggle FPS");
        InputHandler::Get().RegisterHotkey(VK_F7, [](){ MessageBeep(MB_OK); OverlayUI::Get().ToggleVignette(); }, "Toggle Vignette");
        InputHandler::Get().RegisterHotkey(VK_F8, [](){
            float jx = 0.0f, jy = 0.0f;
            StreamlineIntegration::Get().GetLastCameraJitter(jx, jy);
            bool hasCam = StreamlineIntegration::Get().HasCameraData();
            LOG_INFO("F8 Debug: Camera=%s Jitter=(%.4f, %.4f)", hasCam ? "OK" : "MISSING", jx, jy);
            OverlayUI::Get().SetCameraStatus(hasCam, jx, jy);
            MessageBeep(hasCam ? MB_OK : MB_ICONHAND);
        }, "Debug Camera Status");
        
        InputHandler::Get().RegisterHotkey(VK_F9, [](){
            MessageBeep(MB_OK);
            StreamlineIntegration::Get().PrintMFGStatus();
        }, "Debug MFG Status");
        
        InputHandler::Get().InstallHook(); // Global Hook
    }
    InputHandler::Get().ProcessInput();

    if (!StreamlineIntegration::Get().HasCameraData()) {
        float jx = 0.0f, jy = 0.0f;
        StreamlineIntegration::Get().GetLastCameraJitter(jx, jy);
        OverlayUI::Get().SetCameraStatus(false, jx, jy);
    }
    StreamlineIntegration::Get().NewFrame(m_pReal);
    StreamlineIntegration::Get().EvaluateFrameGen(m_pReal);
    ScopedInterface<IDXGISwapChain1> real(m_pReal); return real ? real->Present1(0, Flags | DXGI_PRESENT_ALLOW_TEARING, p) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::ResizeBuffers(UINT C, UINT W, UINT H, DXGI_FORMAT F, UINT FL) {
    StreamlineIntegration::Get().ReleaseResources();
    return m_pReal->ResizeBuffers(C, W, H, F, FL);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::ResizeBuffers1(UINT C, UINT W, UINT H, DXGI_FORMAT F, UINT FL, const UINT* M, IUnknown* const* Q) {
    StreamlineIntegration::Get().ReleaseResources();
    ScopedInterface<IDXGISwapChain3> real(m_pReal); return real ? real->ResizeBuffers1(C, W, H, F, FL, M, Q) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetPrivateData(REFGUID N, UINT S, const void* D) { return m_pReal->SetPrivateData(N, S, D); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetPrivateDataInterface(REFGUID N, const IUnknown* U) { return m_pReal->SetPrivateDataInterface(N, U); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetPrivateData(REFGUID N, UINT* S, void* D) { return m_pReal->GetPrivateData(N, S, D); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetParent(REFIID R, void** P) { return m_pReal->GetParent(R, P); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetDevice(REFIID R, void** D) { return m_pReal->GetDevice(R, D); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetBuffer(UINT B, REFIID R, void** S) { return m_pReal->GetBuffer(B, R, S); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetFullscreenState(BOOL F, IDXGIOutput* T) { return m_pReal->SetFullscreenState(F, T); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetFullscreenState(BOOL* F, IDXGIOutput** T) { return m_pReal->GetFullscreenState(F, T); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* D) { return m_pReal->GetDesc(D); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::ResizeTarget(const DXGI_MODE_DESC* P) { return m_pReal->ResizeTarget(P); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetContainingOutput(IDXGIOutput** O) { return m_pReal->GetContainingOutput(O); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* S) { return m_pReal->GetFrameStatistics(S); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetLastPresentCount(UINT* C) { return m_pReal->GetLastPresentCount(C); }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* D) { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->GetDesc1(D) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* D) { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->GetFullscreenDesc(D) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetHwnd(HWND* H) { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->GetHwnd(H) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetCoreWindow(REFIID R, void** U) { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->GetCoreWindow(R, U) : E_NOINTERFACE; }
BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain::IsTemporaryMonoSupported() { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->IsTemporaryMonoSupported() : FALSE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetRestrictToOutput(IDXGIOutput** O) { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->GetRestrictToOutput(O) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetBackgroundColor(const DXGI_RGBA* C) { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->SetBackgroundColor(C) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetBackgroundColor(DXGI_RGBA* C) { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->GetBackgroundColor(C) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetRotation(DXGI_MODE_ROTATION R) { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->SetRotation(R) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetRotation(DXGI_MODE_ROTATION* R) { ScopedInterface<IDXGISwapChain1> r(m_pReal); return r ? r->GetRotation(R) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetSourceSize(UINT W, UINT H) { ScopedInterface<IDXGISwapChain2> r(m_pReal); return r ? r->SetSourceSize(W, H) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetSourceSize(UINT* W, UINT* H) { ScopedInterface<IDXGISwapChain2> r(m_pReal); return r ? r->GetSourceSize(W, H) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetMaximumFrameLatency(UINT M) { ScopedInterface<IDXGISwapChain2> r(m_pReal); return r ? r->SetMaximumFrameLatency(M) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetMaximumFrameLatency(UINT* M) { ScopedInterface<IDXGISwapChain2> r(m_pReal); return r ? r->GetMaximumFrameLatency(M) : E_NOINTERFACE; }
HANDLE STDMETHODCALLTYPE WrappedIDXGISwapChain::GetFrameLatencyWaitableObject() { ScopedInterface<IDXGISwapChain2> r(m_pReal); return r ? r->GetFrameLatencyWaitableObject() : NULL; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* M) { ScopedInterface<IDXGISwapChain2> r(m_pReal); return r ? r->SetMatrixTransform(M) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* M) { ScopedInterface<IDXGISwapChain2> r(m_pReal); return r ? r->GetMatrixTransform(M) : E_NOINTERFACE; }
UINT STDMETHODCALLTYPE WrappedIDXGISwapChain::GetCurrentBackBufferIndex() { ScopedInterface<IDXGISwapChain3> r(m_pReal); return r ? r->GetCurrentBackBufferIndex() : 0; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE C, UINT* S) { ScopedInterface<IDXGISwapChain3> r(m_pReal); return r ? r->CheckColorSpaceSupport(C, S) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE C) { ScopedInterface<IDXGISwapChain3> r(m_pReal); return r ? r->SetColorSpace1(C) : E_NOINTERFACE; }
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE T, UINT S, void* M) { ScopedInterface<IDXGISwapChain4> r(m_pReal); return r ? r->SetHDRMetaData(T, S, M) : E_NOINTERFACE; }
