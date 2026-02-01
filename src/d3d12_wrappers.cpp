#include "d3d12_wrappers.h"
#include "streamline_integration.h"
#include "hooks.h"
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <cfloat>
#include <cmath>
#include <atomic>
#include <wrl/client.h>

namespace {
    struct CameraCandidate {
        float view[16];
        float proj[16];
        float jitterX = 0.0f;
        float jitterY = 0.0f;
        float score = 0.0f;
        uint64_t frame = 0;
        bool valid = false;
    };

    std::mutex g_cameraMutex;
    CameraCandidate g_bestCamera;
    std::atomic<bool> g_loggedCamera(false);
    struct UploadCbvInfo {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpuBase = 0;
        uint64_t size = 0;
        uint8_t* cpuPtr = nullptr;
    };

    std::mutex g_cbvMutex;
    std::vector<UploadCbvInfo> g_cbvInfos;
    std::atomic<uint64_t> g_cameraFrame(0);
    std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> g_descriptorHeaps;

    bool IsFinite(float v) { return v == v && v > -FLT_MAX && v < FLT_MAX; }
    bool LooksLikeMatrix(const float* m) {
        for (int i = 0; i < 16; ++i) {
            if (!IsFinite(m[i])) return false;
        }
        return true;
    }

    float ScoreMatrixPair(const float* view, const float* proj) {
        float score = 0.0f;
        if (!LooksLikeMatrix(view) || !LooksLikeMatrix(proj)) return 0.0f;
        
        // View Matrix [15] is always 1.0
        if (fabsf(view[15] - 1.0f) < 0.01f) score += 0.2f;
        
        // Projection Matrix Checks
        // Perspective: [15]=0, [11]=+/-1. This is what we want for 3D.
        bool isPerspective = fabsf(proj[15]) < 0.01f && fabsf(fabsf(proj[11]) - 1.0f) < 0.1f;
        
        // Orthographic: [15]=1, [11]=0. This is usually UI.
        bool isOrtho = fabsf(proj[15] - 1.0f) < 0.01f && fabsf(proj[11]) < 0.1f;

        if (isPerspective) score += 0.6f; // High score for 3D perspective
        else if (isOrtho) score += 0.0f;  // Ignore UI matrices
        
        // Sanity check translation elements
        if (fabsf(view[3]) < 1.0f && fabsf(view[7]) < 1.0f && fabsf(view[11]) < 1.0f) score += 0.1f;
        if (fabsf(view[12]) < 100000.0f && fabsf(view[13]) < 100000.0f && fabsf(view[14]) < 100000.0f) score += 0.1f;

        return score;
    }

    bool TryExtractCameraFromBuffer(const uint8_t* data, size_t size, float* outView, float* outProj, float* outScore) {
        if (!data || size < sizeof(float) * 32) return false;
        float bestScore = 0.0f;
        size_t bestOffset = 0;
        for (size_t offset = 0; offset + sizeof(float) * 32 <= size; offset += sizeof(float) * 16) {
            const float* view = reinterpret_cast<const float*>(data + offset);
            const float* proj = view + 16;
            float score = ScoreMatrixPair(view, proj);
            if (score > bestScore) {
                bestScore = score;
                bestOffset = offset;
            }
        }
        if (bestScore < 0.4f) return false;
        const float* view = reinterpret_cast<const float*>(data + bestOffset);
        const float* proj = view + 16;
        memcpy(outView, view, sizeof(float) * 16);
        memcpy(outProj, proj, sizeof(float) * 16);
        if (outScore) *outScore = bestScore;
        return true;
    }

    bool TryGetCbvData(D3D12_GPU_VIRTUAL_ADDRESS gpuAddress, const uint8_t** outData, size_t* outSize) {
        std::lock_guard<std::mutex> lock(g_cbvMutex);
        for (const auto& info : g_cbvInfos) {
            if (!info.cpuPtr || info.gpuBase == 0 || info.size == 0) continue;
            if (gpuAddress >= info.gpuBase && gpuAddress < info.gpuBase + info.size) {
                size_t offset = static_cast<size_t>(gpuAddress - info.gpuBase);
                if (offset >= info.size) return false;
                *outData = info.cpuPtr + offset;
                *outSize = static_cast<size_t>(info.size - offset);
                return true;
            }
        }
        return false;
    }

    bool TryGetCbvDataFromDescriptor(ID3D12DescriptorHeap* heap, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, const uint8_t** outData, size_t* outSize) {
        if (!heap || cpuHandle.ptr == 0) return false;
        D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
        if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return false;
        D3D12_CPU_DESCRIPTOR_HANDLE start = heap->GetCPUDescriptorHandleForHeapStart();
        if (cpuHandle.ptr < start.ptr) return false;
        UINT increment = StreamlineIntegration::Get().GetDescriptorSize();
        if (increment == 0) return false;
        uint64_t index = (cpuHandle.ptr - start.ptr) / increment;
        if (index >= desc.NumDescriptors) return false;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = heap->GetGPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
        gpuHandle.ptr = gpuStart.ptr + index * increment;
        return TryGetCbvData(gpuHandle.ptr, outData, outSize);
    }

    void UpdateBestCamera(const float* view, const float* proj, float jitterX, float jitterY) {
        float score = ScoreMatrixPair(view, proj);
        if (score < 0.4f) return;
        std::lock_guard<std::mutex> lock(g_cameraMutex);
        g_bestCamera.score = score;
        memcpy(g_bestCamera.view, view, sizeof(float) * 16);
        memcpy(g_bestCamera.proj, proj, sizeof(float) * 16);
        g_bestCamera.jitterX = jitterX;
        g_bestCamera.jitterY = jitterY;
        g_bestCamera.frame = ++g_cameraFrame;
        g_bestCamera.valid = true;
        if (!g_loggedCamera.exchange(true)) {
            LOG_INFO("Camera matrices detected (score %.2f)", score);
        }
    }

    bool FetchCamera(CameraCandidate& out) {
        std::lock_guard<std::mutex> lock(g_cameraMutex);
        if (!g_bestCamera.valid) return false;
        out = g_bestCamera;
        return true;
    }
}

void RegisterCbv(ID3D12Resource* pResource, UINT64 size, uint8_t* cpuPtr) {
    std::lock_guard<std::mutex> lock(g_cbvMutex);
    UploadCbvInfo info{};
    info.resource = pResource; // Smart pointer assignment
    info.gpuBase = pResource->GetGPUVirtualAddress();
    info.size = size;
    info.cpuPtr = cpuPtr;
    g_cbvInfos.push_back(info);
}

bool TryScanAllCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates) {
    std::lock_guard<std::mutex> lock(g_cbvMutex);
    auto it = std::remove_if(g_cbvInfos.begin(), g_cbvInfos.end(), [](const UploadCbvInfo& info) {
        if (!info.cpuPtr) return true;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(info.cpuPtr, &mbi, sizeof(mbi)) == 0) return true;
        if (mbi.State != MEM_COMMIT) return true;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return true;
        return false;
    });
    if (it != g_cbvInfos.end()) g_cbvInfos.erase(it, g_cbvInfos.end());
    float bestScore = 0.0f;
    bool found = false;
    for (const auto& info : g_cbvInfos) {
        if (!info.cpuPtr || info.size < sizeof(float) * 32) continue;
        float tempView[16], tempProj[16], score = 0.0f;
        if (TryExtractCameraFromBuffer(info.cpuPtr, static_cast<size_t>(info.size), tempView, tempProj, &score)) {
            if (score > bestScore) {
                bestScore = score;
                memcpy(outView, tempView, sizeof(float) * 16);
                memcpy(outProj, tempProj, sizeof(float) * 16);
                found = true;
            }
        }
    }
    if (found && outScore) *outScore = bestScore;
    return found;
}

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
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList)) {
        *ppvObject = this; AddRef(); return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Release() { ULONG ref = InterlockedDecrement(&m_refCount); if (ref == 0) delete this; return ref; }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) { return m_pReal->GetPrivateData(guid, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) { return m_pReal->SetPrivateData(guid, DataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) { return m_pReal->SetPrivateDataInterface(guid, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::SetName(LPCWSTR Name) { return m_pReal->SetName(Name); }
HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::GetDevice(REFIID riid, void** ppvDevice) { return m_pDeviceWrapper->QueryInterface(riid, ppvDevice); }
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::GetType() { return m_pReal->GetType(); }

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Close() {
    NotifyWrappedCommandListUsed();
    float jitterX = 0.0f, jitterY = 0.0f;
    TryGetPatternJitter(jitterX, jitterY);
    CameraCandidate cam{};
    if (!FetchCamera(cam)) {
        float view[16], proj[16], score = 0.0f;
        if (TryScanAllCbvsForCamera(view, proj, &score, false)) {
            UpdateBestCamera(view, proj, jitterX, jitterY);
        }
    }
    if (FetchCamera(cam)) {
        StreamlineIntegration::Get().SetCameraData(cam.view, cam.proj, cam.jitterX, cam.jitterY);
    } else {
        StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitterX, jitterY);
    }
    StreamlineIntegration::Get().EvaluateDLSS(this);
    return m_pReal->Close();
}

HRESULT STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::Reset(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) { return m_pReal->Reset(pAllocator, pInitialState); }

void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    for (UINT i = 0; i < NumBarriers; i++) {
        if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            ResourceDetector::Get().RegisterResource(pBarriers[i].Transition.pResource);
        }
    }
    m_pReal->ResourceBarrier(NumBarriers, pBarriers);
}

void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::OMSetRenderTargets(UINT N, const D3D12_CPU_DESCRIPTOR_HANDLE* R, BOOL S, const D3D12_CPU_DESCRIPTOR_HANDLE* D) { m_pReal->OMSetRenderTargets(N, R, S, D); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) { m_pReal->ExecuteBundle(pCommandList); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) { m_pReal->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::DiscardResource(ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) { m_pReal->DiscardResource(pResource, pRegion); }
void STDMETHODCALLTYPE WrappedID3D12GraphicsCommandList::ResolveQueryData(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartElement, UINT ElementCount, ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) { m_pReal->ResolveQueryData(pQueryHeap, Type, StartElement, ElementCount, pDestinationBuffer, AlignedDestinationBufferOffset); }
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

WrappedID3D12Device::WrappedID3D12Device(ID3D12Device* pReal) : m_pReal(pReal), m_refCount(1) { if(m_pReal) m_pReal->AddRef(); }
WrappedID3D12Device::~WrappedID3D12Device() { if(m_pReal) m_pReal->Release(); }
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Device)) {
        *ppvObject = this; AddRef(); return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObject);
}
ULONG STDMETHODCALLTYPE WrappedID3D12Device::AddRef() { return InterlockedIncrement(&m_refCount); }
ULONG STDMETHODCALLTYPE WrappedID3D12Device::Release() { ULONG ref = InterlockedDecrement(&m_refCount); if (ref == 0) delete this; return ref; }
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) { return m_pReal->GetPrivateData(guid, pDataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) { return m_pReal->SetPrivateData(guid, DataSize, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) { return m_pReal->SetPrivateDataInterface(guid, pData); }
HRESULT STDMETHODCALLTYPE WrappedID3D12Device::SetName(LPCWSTR Name) { return m_pReal->SetName(Name); }

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) {
    ID3D12GraphicsCommandList* pRealList = nullptr;
    HRESULT hr = m_pReal->CreateCommandList(nodeMask, type, pCommandAllocator, pInitialState, __uuidof(ID3D12GraphicsCommandList), (void**)&pRealList);
    if (FAILED(hr)) return hr;
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE) {
        WrappedID3D12GraphicsCommandList* pWrapper = new WrappedID3D12GraphicsCommandList(pRealList, this);
        hr = pWrapper->QueryInterface(riid, ppCommandList);
        pWrapper->Release(); pRealList->Release();
    } else {
        hr = pRealList->QueryInterface(riid, ppCommandList);
        pRealList->Release();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* pH, D3D12_HEAP_FLAGS HF, const D3D12_RESOURCE_DESC* pD, D3D12_RESOURCE_STATES IS, const D3D12_CLEAR_VALUE* pO, REFIID riid, void** ppv) {
    HRESULT hr = m_pReal->CreateCommittedResource(pH, HF, pD, IS, pO, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        ID3D12Resource* pRes = (ID3D12Resource*)*ppv;
        ResourceDetector::Get().RegisterResource(pRes);
        if (pD && pH && pH->Type == D3D12_HEAP_TYPE_UPLOAD && pD->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            uint8_t* mapped = nullptr; D3D12_RANGE range = { 0, 0 };
            if (SUCCEEDED(pRes->Map(0, &range, reinterpret_cast<void**>(&mapped))) && mapped) {
                RegisterCbv(pRes, pD->Width, mapped);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreatePlacedResource(ID3D12Heap* pH, UINT64 HO, const D3D12_RESOURCE_DESC* pD, D3D12_RESOURCE_STATES IS, const D3D12_CLEAR_VALUE* pO, REFIID riid, void** ppv) {
    HRESULT hr = m_pReal->CreatePlacedResource(pH, HO, pD, IS, pO, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && pD && pD->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        DXGI_FORMAT f = pD->Format;
        if (f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16_UNORM || f == DXGI_FORMAT_R16G16_TYPELESS || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R10G10B10A2_UNORM) {
            ResourceDetector::Get().RegisterResource((ID3D12Resource*)*ppv);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedID3D12Device::CreateReservedResource(const D3D12_RESOURCE_DESC* pD, D3D12_RESOURCE_STATES IS, const D3D12_CLEAR_VALUE* pO, REFIID riid, void** ppv) {
    HRESULT hr = m_pReal->CreateReservedResource(pD, IS, pO, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && pD && pD->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        DXGI_FORMAT f = pD->Format;
        if (f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16_UNORM || f == DXGI_FORMAT_R16G16_TYPELESS || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R32_TYPELESS) {
            ResourceDetector::Get().RegisterResource((ID3D12Resource*)*ppv);
        }
    }
    return hr;
}

void STDMETHODCALLTYPE WrappedID3D12Device::CreateSampler(const D3D12_SAMPLER_DESC* pD, D3D12_CPU_DESCRIPTOR_HANDLE Dest) {
    if (pD) {
        D3D12_SAMPLER_DESC nD = *pD;
        float bias = StreamlineIntegration::Get().GetLODBias();
        if (bias != 0.0f) { nD.MipLODBias += bias; if (nD.MipLODBias < -3.0f) nD.MipLODBias = -3.0f; }
        m_pReal->CreateSampler(&nD, Dest);
    } else m_pReal->CreateSampler(pD, Dest);
}
