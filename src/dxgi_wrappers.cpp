#include "dxgi_wrappers.h"
#include "streamline_integration.h"
#include "logger.h"
#include "hooks.h"
#include "input_handler.h" // Added
#include "overlay.h"       // Added
#include "dlss4_config.h" // For DLSS4_FRAME_GEN_MULTIPLIER
#include <stdio.h>        // For sprintf_s
#include <sysinfoapi.h>   // For GetTickCount64
#include <mutex>

extern "C" void LogStartup(const char* msg);

// Helper to call forwarded methods safely
template <typename T>
struct ScopedInterface {
    T* ptr;
    ScopedInterface(IUnknown* p) : ptr(nullptr) {
        if (p) p->QueryInterface(__uuidof(T), (void**)&ptr);
    }
    ~ScopedInterface() {
        if (ptr) ptr->Release();
    }
    operator T*() { return ptr; }
    T* operator->() { return ptr; }
};

// ============================================================================
// FACTORY WRAPPER
// ============================================================================

WrappedIDXGIFactory::WrappedIDXGIFactory(IDXGIFactory* pReal) : m_pReal(pReal), m_refCount(1) {
    if (m_pReal) m_pReal->AddRef();
    LogStartup("WrappedIDXGIFactory Created");
    
    // MOVED: HookFactoryIfNeeded called in EnumAdapters/CreateSwapChain instead
}

WrappedIDXGIFactory::~WrappedIDXGIFactory() {
    if (m_pReal) m_pReal->Release();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    
    // Only return wrapper if the real object supports the requested interface
    // This prevents the game from calling methods we can't forward
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
    }
    
    // Check if real supports it
    IUnknown* test = nullptr;
    if (SUCCEEDED(m_pReal->QueryInterface(riid, (void**)&test))) {
        test->Release();
        *ppvObject = this;
        AddRef();
        return S_OK;
    }
    
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WrappedIDXGIFactory::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedIDXGIFactory::Release() {
    ULONG r = InterlockedDecrement(&m_refCount);
    if (r == 0) delete this;
    return r;
}

// Passthroughs
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumAdapters(UINT A, IDXGIAdapter** P) { 
    HookFactoryIfNeeded(m_pReal); // Initialize hooks here lazily
    return m_pReal->EnumAdapters(A, P); 
}
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::MakeWindowAssociation(HWND H, UINT F) { return m_pReal->MakeWindowAssociation(H, F); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetWindowAssociation(HWND* H) { return m_pReal->GetWindowAssociation(H); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSoftwareAdapter(HMODULE M, IDXGIAdapter** P) { return m_pReal->CreateSoftwareAdapter(M, P); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::SetPrivateData(REFGUID N, UINT S, const void* D) { return m_pReal->SetPrivateData(N, S, D); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::SetPrivateDataInterface(REFGUID N, const IUnknown* U) { return m_pReal->SetPrivateDataInterface(N, U); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetPrivateData(REFGUID N, UINT* S, void* D) { return m_pReal->GetPrivateData(N, S, D); }
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::GetParent(REFIID R, void** P) { return m_pReal->GetParent(R, P); }

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::EnumAdapters1(UINT A, IDXGIAdapter1** P) { 
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

// INTERCEPT CREATION
HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    LogStartup("WrappedFactory::CreateSwapChain");
    HookFactoryIfNeeded(m_pReal); // Ensure hooks are ready
    HRESULT hr = m_pReal->CreateSwapChain(pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        *ppSwapChain = new WrappedIDXGISwapChain(*ppSwapChain, pDevice);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LogStartup("WrappedFactory::CreateSwapChainForHwnd");
    ScopedInterface<IDXGIFactory2> real(m_pReal);
    if (!real) return E_NOINTERFACE;
    
    HRESULT hr = real->CreateSwapChainForHwnd(pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        *ppSwapChain = (IDXGISwapChain1*)new WrappedIDXGISwapChain((IDXGISwapChain*)*ppSwapChain, pDevice);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LogStartup("WrappedFactory::CreateSwapChainForCoreWindow");
    ScopedInterface<IDXGIFactory2> real(m_pReal);
    if (!real) return E_NOINTERFACE;
    
    HRESULT hr = real->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        *ppSwapChain = (IDXGISwapChain1*)new WrappedIDXGISwapChain((IDXGISwapChain*)*ppSwapChain, pDevice);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGIFactory::CreateSwapChainForComposition(IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    LogStartup("WrappedFactory::CreateSwapChainForComposition");
    ScopedInterface<IDXGIFactory2> real(m_pReal);
    if (!real) return E_NOINTERFACE;
    
    HRESULT hr = real->CreateSwapChainForComposition(pDevice, pDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        *ppSwapChain = (IDXGISwapChain1*)new WrappedIDXGISwapChain((IDXGISwapChain*)*ppSwapChain, pDevice);
    }
    return hr;
}

// ============================================================================
// SWAPCHAIN WRAPPER
// ============================================================================

WrappedIDXGISwapChain::WrappedIDXGISwapChain(IDXGISwapChain* pReal, IUnknown* pDevice) : m_pReal(pReal), m_refCount(1) {
    if (m_pReal) m_pReal->AddRef();
    LogStartup("WrappedIDXGISwapChain Created");
    
    // Initialize Streamline here
    ID3D12Device* d3d12Device = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12Device), (void**)&d3d12Device))) {
        LogStartup("Initializing Streamline in SwapChain Constructor");
        StreamlineIntegration::Get().Initialize(d3d12Device);
        d3d12Device->Release();
        m_initialized = true;
    }

    // Capture command queue if the device argument is actually a queue
    ID3D12CommandQueue* queue = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&queue))) {
        StreamlineIntegration::Get().SetCommandQueue(queue);
        queue->Release();
    }
}

WrappedIDXGISwapChain::~WrappedIDXGISwapChain() {
    if (m_pReal) m_pReal->Release();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    
    // Check if real object supports the interface before returning wrapper
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
    }
    
    // Check derived
    IUnknown* test = nullptr;
    if (SUCCEEDED(m_pReal->QueryInterface(riid, (void**)&test))) {
        test->Release();
        *ppvObject = this; // We implement all up to SwapChain4
        AddRef();
        return S_OK;
    }
    
    return m_pReal->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain::Release() {
    ULONG r = InterlockedDecrement(&m_refCount);
    if (r == 0) delete this;
    return r;
}

// INTERCEPT PRESENT
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::Present(UINT SyncInterval, UINT Flags) {
    // --- INPUT & UI HANDLING ---
    static bool inputsRegistered = false;
    if (!inputsRegistered) {
        OverlayUI::Get().Initialize(GetModuleHandleA("dxgi.dll"));
        
        InputHandler::Get().RegisterHotkey(VK_F1, [](){ 
            LOG_INFO("F1 Pressed - Toggling Overlay");
            MessageBeep(MB_OK); 
            OverlayUI::Get().ToggleVisibility(); 
        }, "Toggle Menu");
        
        InputHandler::Get().RegisterHotkey(VK_F2, [](){ 
            MessageBeep(MB_OK);
            OverlayUI::Get().ToggleFPS(); 
        }, "Toggle FPS");
        
        InputHandler::Get().RegisterHotkey(VK_F3, [](){ StreamlineIntegration::Get().SetSharpness(0.0f); }, "Sharpness 0.0");
        InputHandler::Get().RegisterHotkey(VK_F4, [](){ StreamlineIntegration::Get().SetSharpness(0.5f); }, "Sharpness 0.5");
        InputHandler::Get().RegisterHotkey(VK_F5, [](){ StreamlineIntegration::Get().CycleLODBias(); }, "Cycle LOD Bias");
        
        inputsRegistered = true;
    }
    InputHandler::Get().ProcessInput();
    // OverlayUI::Get().Update(); // REMOVED: UI is threaded
    // ---------------------------

    // --- FPS COUNTER START ---
    static uint64_t lastTime = 0;
    static uint64_t frameCount = 0;
    
    uint64_t currentTime = GetTickCount64();
    frameCount++;
    
    if (currentTime - lastTime >= 1000) {
        float fps = (float)frameCount * 1000.0f / (float)(currentTime - lastTime);
        
        int mult = StreamlineIntegration::Get().GetFrameGenMultiplier();
        if (mult < 1) mult = 1; // 0 means 1x
        float totalFps = fps * (float)mult;
        
        // Log to Startup Log for immediate debug
        char msg[128];
        sprintf_s(msg, "Game FPS: %.2f | DLSS 4.5 Output FPS: %.2f", fps, totalFps);
        LogStartup(msg);
        
        // Log to CSV for the user
        FILE* fp;
        if (fopen_s(&fp, "dlss_stats.csv", "a") == 0) {
            fprintf(fp, "%llu,%.2f,%.2f\n", currentTime, fps, totalFps);
            fclose(fp);
        }
        
        // Update the Control Panel Live
        OverlayUI::Get().SetFPS(fps, totalFps);
        
        lastTime = currentTime;
        frameCount = 0;
    }
    // --- FPS COUNTER END ---

    StreamlineIntegration::Get().NewFrame(m_pReal);
    StreamlineIntegration::Get().EvaluateFrameGen(m_pReal);
    
    // FORCE V-SYNC OFF: Frame Gen manages its own pacing. 
    // Double buffering with V-Sync ON causes massive input lag and stutter with FG.
    return m_pReal->Present(0, Flags | DXGI_PRESENT_ALLOW_TEARING);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::Present1(UINT Sync, UINT Flags, const DXGI_PRESENT_PARAMETERS* p) {
    StreamlineIntegration::Get().NewFrame(m_pReal);
    StreamlineIntegration::Get().EvaluateFrameGen(m_pReal);
    ScopedInterface<IDXGISwapChain1> real(m_pReal); return real ? real->Present1(Sync, Flags, p) : E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::ResizeBuffers(UINT C, UINT W, UINT H, DXGI_FORMAT F, UINT FL) {
    LogStartup("ResizeBuffers");
    StreamlineIntegration::Get().ReleaseResources();
    return m_pReal->ResizeBuffers(C, W, H, F, FL);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain::ResizeBuffers1(UINT C, UINT W, UINT H, DXGI_FORMAT F, UINT FL, const UINT* M, IUnknown* const* Q) {
    LogStartup("ResizeBuffers1");
    StreamlineIntegration::Get().ReleaseResources();
    ScopedInterface<IDXGISwapChain3> real(m_pReal); return real ? real->ResizeBuffers1(C, W, H, F, FL, M, Q) : E_NOINTERFACE;
}

// Passthroughs using ScopedInterface for derived methods
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

// Derived
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
