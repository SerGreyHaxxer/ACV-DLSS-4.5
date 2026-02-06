#pragma once
#include <dxgi1_6.h>
#include <d3d12.h>
#include <atomic>

// Called from the D3D12 Present/submission thread — safe for GPU work
void OnPresentThread(IDXGISwapChain *pSwapChain);
void StartFrameTimer();
void StopFrameTimer();

// ============================================================================
// FACTORY WRAPPER
// ============================================================================
class WrappedIDXGIFactory : public IDXGIFactory7 {
private:
    IDXGIFactory* m_pReal;
    std::atomic<LONG> m_refCount;

public:
    WrappedIDXGIFactory(IDXGIFactory* pReal);
    virtual ~WrappedIDXGIFactory();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override;
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) override;

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override;
    BOOL STDMETHODCALLTYPE IsCurrent() override;

    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) override;
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override;
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override;
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) override;

    // IDXGIFactory3
    UINT STDMETHODCALLTYPE GetCreationFlags() override;

    // IDXGIFactory4
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) override;
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** ppvAdapter) override;

    // IDXGIFactory5
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override;

    // IDXGIFactory6
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void** ppvAdapter) override;

    // IDXGIFactory7
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD* pdwCookie) override;
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override;
};
