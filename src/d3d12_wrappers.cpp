#include "d3d12_wrappers.h"
#include "streamline_integration.h"
#include <algorithm>

// ============================================================================
// WRAPPED COMMAND LIST IMPLEMENTATION
// ============================================================================

WrappedID3D12GraphicsCommandList::WrappedID3D12GraphicsCommandList(ID3D12GraphicsCommandList* pReal, WrappedID3D12Device* pDeviceWrapper) 
    : m_pReal(pReal), m_pDeviceWrapper(pDeviceWrapper), m_refCount(1) {
    if(m_pReal) m_pReal->AddRef();
    if(m_pDeviceWrapper) m_pDeviceWrapper->AddRef();
}

WrappedID3D12GraphicsCommandList::~WrappedID3D12GraphicsCommandList() {
    if(m_pReal) m_pReal->Release();
    if(m_pDeviceWrapper) m_pDeviceWrapper->Release();
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || 
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) || 
        riid == __uuidof(ID3D12GraphicsCommandList)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Release() {
    ULONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0) delete this;
    return ref;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return m_pReal->SetPrivateDataInterface(guid, pData);
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetName(LPCWSTR Name) {
    return m_pReal->SetName(Name);
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::GetDevice(REFIID riid, void** ppvDevice) {
    return m_pDeviceWrapper->QueryInterface(riid, ppvDevice);
}

D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::GetType() {
    return m_pReal->GetType();
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Close() {
    ResourceDetector::Get().AnalyzeCommandList(this);
    StreamlineIntegration::Get().EvaluateDLSS(this);
    return m_pReal->Close();
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Reset(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) {
    return m_pReal->Reset(pAllocator, pInitialState);
}

void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    // === CRITICAL: INTERCEPT RESOURCES HERE ===
    for (UINT i = 0; i < NumBarriers; i++) {
        const auto& b = pBarriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            // Register this resource as a potential candidate
            // Games often transition Depth/Color/Motion buffers around now
            ResourceDetector::Get().RegisterResource(b.Transition.pResource);
            
            // Check for common Motion Vector states
            // MVs often go from RENDER_TARGET/UAV -> SHADER_RESOURCE
            if (b.Transition.StateAfter == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE || 
                b.Transition.StateAfter == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
                // Potential input for next pass
            }
        }
    }
    
    m_pReal->ResourceBarrier(NumBarriers, pBarriers);
}

void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::OMSetRenderTargets(
    UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, 
    BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    
    // In a full implementation, we would map these descriptors back to resources
    // using a descriptor heap wrapper or by hooking CreateRenderTargetView.
    // Since we don't have that yet, we rely on ResourceBarrier tracking.
    
    m_pReal->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, 
        RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
}

void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) { m_pReal->ExecuteBundle(pCommandList); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::DiscardResource(ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) { m_pReal->DiscardResource(pResource, pRegion); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ResolveQueryData(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartElement, UINT ElementCount, ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) { m_pReal->ResolveQueryData(pQueryHeap, Type, StartElement, ElementCount, pDestinationBuffer, AlignedDestinationBufferOffset); }

// Implement remaining stubs
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Dispatch(UINT X, UINT Y, UINT Z) { m_pReal->Dispatch(X, Y, Z); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::CopyBufferRegion(ID3D12Resource* D, UINT64 DO, ID3D12Resource* S, UINT64 SO, UINT64 B) { m_pReal->CopyBufferRegion(D, DO, S, SO, B); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* D, UINT DX, UINT DY, UINT DZ, const D3D12_TEXTURE_COPY_LOCATION* S, const D3D12_BOX* SB) { m_pReal->CopyTextureRegion(D, DX, DY, DZ, S, SB); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::CopyResource(ID3D12Resource* D, ID3D12Resource* S) { m_pReal->CopyResource(D, S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::CopyTiles(ID3D12Resource* R, const D3D12_TILED_RESOURCE_COORDINATE* C, const D3D12_TILE_REGION_SIZE* S, ID3D12Resource* B, UINT64 O, D3D12_TILE_COPY_FLAGS F) { m_pReal->CopyTiles(R, C, S, B, O, F); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ResolveSubresource(ID3D12Resource* D, UINT DI, ID3D12Resource* S, UINT SI, DXGI_FORMAT F) { m_pReal->ResolveSubresource(D, DI, S, SI, F); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY T) { m_pReal->IASetPrimitiveTopology(T); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::RSSetViewports(UINT N, const D3D12_VIEWPORT* V) { m_pReal->RSSetViewports(N, V); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::RSSetScissorRects(UINT N, const D3D12_RECT* R) { m_pReal->RSSetScissorRects(N, R); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::OMSetBlendFactor(const FLOAT F[4]) { m_pReal->OMSetBlendFactor(F); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::OMSetStencilRef(UINT R) { m_pReal->OMSetStencilRef(R); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPipelineState(ID3D12PipelineState* P) { m_pReal->SetPipelineState(P); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetDescriptorHeaps(UINT N, ID3D12DescriptorHeap* const* H) { m_pReal->SetDescriptorHeaps(N, H); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootSignature(ID3D12RootSignature* S) { m_pReal->SetComputeRootSignature(S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootSignature(ID3D12RootSignature* S) { m_pReal->SetGraphicsRootSignature(S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootDescriptorTable(UINT I, D3D12_GPU_DESCRIPTOR_HANDLE H) { m_pReal->SetComputeRootDescriptorTable(I, H); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable(UINT I, D3D12_GPU_DESCRIPTOR_HANDLE H) { m_pReal->SetGraphicsRootDescriptorTable(I, H); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRoot32BitConstant(UINT I, UINT D, UINT O) { m_pReal->SetComputeRoot32BitConstant(I, D, O); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant(UINT I, UINT D, UINT O) { m_pReal->SetGraphicsRoot32BitConstant(I, D, O); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRoot32BitConstants(UINT I, UINT N, const void* D, UINT O) { m_pReal->SetComputeRoot32BitConstants(I, N, D, O); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants(UINT I, UINT N, const void* D, UINT O) { m_pReal->SetGraphicsRoot32BitConstants(I, N, D, O); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootConstantBufferView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetComputeRootConstantBufferView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetGraphicsRootConstantBufferView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootShaderResourceView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetComputeRootShaderResourceView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetGraphicsRootShaderResourceView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetComputeRootUnorderedAccessView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView(UINT I, D3D12_GPU_VIRTUAL_ADDRESS A) { m_pReal->SetGraphicsRootUnorderedAccessView(I, A); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* V) { m_pReal->IASetIndexBuffer(V); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::IASetVertexBuffers(UINT S, UINT N, const D3D12_VERTEX_BUFFER_VIEW* V) { m_pReal->IASetVertexBuffers(S, N, V); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SOSetTargets(UINT S, UINT N, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* V) { m_pReal->SOSetTargets(S, N, V); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::BeginQuery(ID3D12QueryHeap* H, D3D12_QUERY_TYPE T, UINT I) { m_pReal->BeginQuery(H, T, I); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::EndQuery(ID3D12QueryHeap* H, D3D12_QUERY_TYPE T, UINT I) { m_pReal->EndQuery(H, T, I); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPredication(ID3D12Resource* B, UINT64 O, D3D12_PREDICATION_OP Op) { m_pReal->SetPredication(B, O, Op); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetMarker(UINT M, const void* D, UINT S) { m_pReal->SetMarker(M, D, S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::BeginEvent(UINT M, const void* D, UINT S) { m_pReal->BeginEvent(M, D, S); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::EndEvent() { m_pReal->EndEvent(); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ExecuteIndirect(ID3D12CommandSignature* S, UINT M, ID3D12Resource* A, UINT64 AO, ID3D12Resource* C, UINT64 CO) { m_pReal->ExecuteIndirect(S, M, A, AO, C, CO); }

// ============================================================================
// WRAPPED DEVICE IMPLEMENTATION
// ============================================================================

WrappedID3D12Device::WrappedID3D12Device(ID3D12Device* pReal) : m_pReal(pReal), m_refCount(1) {
    if(m_pReal) m_pReal->AddRef();
}

WrappedID3D12Device::~WrappedID3D12Device() {
    if(m_pReal) m_pReal->Release();
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || 
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Device)) {
        *ppvObject = this;
        AddRef();
        return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE WrappedID3D12Device::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE WrappedID3D12Device::Release() {
    ULONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0) delete this;
    return ref;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return m_pReal->SetPrivateDataInterface(guid, pData);
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::SetName(LPCWSTR Name) {
    return m_pReal->SetName(Name);
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateCommandList(
    UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator, 
    ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) {
    
    // Create the REAL command list
    ID3D12GraphicsCommandList* pRealList = nullptr;
    HRESULT hr = m_pReal->CreateCommandList(nodeMask, type, pCommandAllocator, pInitialState, 
        __uuidof(ID3D12GraphicsCommandList), (void**)&pRealList);
    
    if (FAILED(hr)) return hr;
    
    // Wrap it
    // Only wrap Graphics command lists (type 0)
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        WrappedID3D12GraphicsCommandList* pWrapper = new WrappedID3D12GraphicsCommandList(pRealList, this);
        hr = pWrapper->QueryInterface(riid, ppCommandList);
        pWrapper->Release(); // QueryInterface added ref
        pRealList->Release(); // Wrapper holds ref
        LOG_INFO("Wrapped CommandList created: %p (Real: %p)", *ppCommandList, pRealList);
    } else {
        // Return raw for bundles/others if not needed
        hr = pRealList->QueryInterface(riid, ppCommandList);
        pRealList->Release();
    }
    
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, 
    const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, 
    const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    
    HRESULT hr = m_pReal->CreateCommittedResource(pHeapProperties, HeapFlags, pDesc, 
        InitialResourceState, pOptimizedClearValue, riid, ppvResource);
    
        if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
            ID3D12Resource* pRes = (ID3D12Resource*)*ppvResource;
            ResourceDetector::Get().RegisterResource(pRes);
        }
    
        return hr;
    }
    
    void STDMETHODCALLTYPE WrappedID3D12Device::CreateSampler(const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
        if (pDesc) {
            D3D12_SAMPLER_DESC newDesc = *pDesc;
            // Force Negative LOD Bias if set
            float bias = StreamlineIntegration::Get().GetLODBias();
            if (bias != 0.0f) {
                newDesc.MipLODBias += bias;
                // Clamp to avoid extreme blur or noise
                if (newDesc.MipLODBias < -3.0f) newDesc.MipLODBias = -3.0f;
            }
            m_pReal->CreateSampler(&newDesc, DestDescriptor);
        } else {
            m_pReal->CreateSampler(pDesc, DestDescriptor);
        }
    }
