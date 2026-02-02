#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include "resource_detector.h"
#include "logger.h"

// Forward declarations
class WrappedID3D12Device;

// ============================================================================
// WRAPPED COMMAND LIST
// ============================================================================
// We wrap this to inspect ResourceBarriers and RenderTargets
// This allows us to identify which resources are being used as Color, Depth, and Motion Vectors

// Helper to register CBVs for camera scanning
void RegisterCbv(ID3D12Resource* pResource, UINT64 size, uint8_t* cpuPtr);
bool TryScanAllCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates, bool allowFullScan);
bool GetLastCameraStats(float& outScore, uint64_t& outFrame);
void ResetCameraScanCache();
uint64_t GetLastCameraFoundFrame();

// Descriptor helpers (used by vtable hooks)
void TrackDescriptorHeap(ID3D12DescriptorHeap* heap);
void TrackDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, DXGI_FORMAT format);
bool TryResolveDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource** outResource, DXGI_FORMAT* outFormat);

class WrappedID3D12GraphicsCommandList : public ID3D12GraphicsCommandList {
public:
    WrappedID3D12GraphicsCommandList(ID3D12GraphicsCommandList* pReal, WrappedID3D12Device* pDeviceWrapper);
    virtual ~WrappedID3D12GraphicsCommandList();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // ID3D12DeviceChild
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    // ID3D12CommandList
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override;

    // ID3D12GraphicsCommandList (Selected Critical Methods)
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) override;
    
    void STDMETHODCALLTYPE ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) override;
    
    // ... Pass-through for others ...
    // Note: In a full production proxy, ALL methods must be implemented. 
    // For this implementation, we will implement the critical ones and 
    // simply forward the vtable for the rest using a raw hook approach if needed,
    // OR implement all of them. Implementing all is safer but verbose.
    // Given the constraints, I will implement the most common ones used for resource tracking.

    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pPipelineState) override { m_pReal->ClearState(pPipelineState); }
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override { m_pReal->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override { m_pReal->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); }
    
    // For brevity in this turn, other methods call m_pReal directly.
    // In the CPP file we will implement the "PassThrough" macro.
    
    ID3D12GraphicsCommandList* GetReal() { return m_pReal; }

    // Minimal implementation required for compilation of pure virtuals
    // (We will use a macro in the CPP or fill these out to prevent abstract class errors)
    // ... [List of all 60+ methods omitted for brevity in header, will be in CPP or simplified] ...
    // To make this compile, we'll actually use the "Vtable Hook" approach for the instance 
    // rather than a full wrapper class, OR use a partial wrapper that delegates everything.
    // A full wrapper class is safer for state tracking. Let's do the VTable hook approach 
    // for the command list to avoid implementing 100 methods, 
    // BUT since we need to store state (which resources are bound), a Wrapper Class is better.
    // I will implement the full interface in the CPP using macros.

    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) override;
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartElement, UINT ElementCount, ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) override;

    // ... Standard D3D12 methods ...
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override;
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource* pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE* pTileRegionSize, ID3D12Resource* pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource* pDstResource, UINT DstSubresource, ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT* pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pPipelineState) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) override;
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) override;
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE EndEvent() override;
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset) override;

private:
    ID3D12GraphicsCommandList* m_pReal;
    WrappedID3D12Device* m_pDeviceWrapper;
    ULONG m_refCount;
};

// ============================================================================
// WRAPPED DEVICE
// ============================================================================
// We wrap this to intercept CreateCommandList and CreateCommittedResource

class WrappedID3D12Device : public ID3D12Device {
public:
    WrappedID3D12Device(ID3D12Device* pReal);
    virtual ~WrappedID3D12Device();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // ID3D12Device Critical Methods
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) override;
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) override;
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;

    // ... Pass-throughs ...
    UINT STDMETHODCALLTYPE GetNodeCount() override { return m_pReal->GetNodeCount(); }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) override { return m_pReal->CreateCommandQueue(pDesc, riid, ppCommandQueue); }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) override { return m_pReal->CreateCommandAllocator(type, riid, ppCommandAllocator); }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override { return m_pReal->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState); }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) override { return m_pReal->CreateComputePipelineState(pDesc, riid, ppPipelineState); }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) override { return m_pReal->GetDescriptorHandleIncrementSize(DescriptorHeapType); }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature) override { return m_pReal->CreateRootSignature(nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature); }
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override { m_pReal->CreateConstantBufferView(pDesc, DestDescriptor); }
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* pResource, ID3D12Resource* pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override;
    // void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) override { m_pReal->CreateSampler(pDesc, DestDescriptor); } // IMPLEMENTED IN CPP
    void STDMETHODCALLTYPE CopyDescriptors(UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts, const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts, const UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override { m_pReal->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType); }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart, D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) override { m_pReal->CopyDescriptorsSimple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType); }
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) override { return m_pReal->GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs); }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE heapType) override { return m_pReal->GetCustomHeapProperties(nodeMask, heapType); }
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override { return m_pReal->CreateHeap(pDesc, riid, ppvHeap); }
    // Placed and Reserved implemented below
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes, DWORD Access, LPCWSTR Name, HANDLE* pHandle) override { return m_pReal->CreateSharedHandle(pObject, pAttributes, Access, Name, pHandle); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE pHandle, REFIID riid, void** ppvObj) override { return m_pReal->OpenSharedHandle(pHandle, riid, ppvObj); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR Name, DWORD Access, HANDLE* pHandle) override { return m_pReal->OpenSharedHandleByName(Name, Access, pHandle); }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT NumObjects, ID3D12Pageable* const* ppObjects) override { return m_pReal->MakeResident(NumObjects, ppObjects); }
    HRESULT STDMETHODCALLTYPE Evict(UINT NumObjects, ID3D12Pageable* const* ppObjects) override { return m_pReal->Evict(NumObjects, ppObjects); }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) override { return m_pReal->CreateFence(InitialValue, Flags, riid, ppFence); }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return m_pReal->GetDeviceRemovedReason(); }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows, UINT64* pRowSizeInBytes, UINT64* pTotalBytes) override { m_pReal->GetCopyableFootprints(pResourceDesc, FirstSubresource, NumSubresources, BaseOffset, pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes); }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) override { return m_pReal->CreateQueryHeap(pDesc, riid, ppvHeap); }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL Enable) override { return m_pReal->SetStablePowerState(Enable); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override { return m_pReal->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature, REFIID riid, void** ppvCommandSignature) override { return m_pReal->CreateCommandSignature(pDesc, pRootSignature, riid, ppvCommandSignature); }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource, D3D12_PACKED_MIP_INFO* pPackedMipDesc, D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips, UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet, D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) override { m_pReal->GetResourceTiling(pTiledResource, pNumTilesForEntireResource, pPackedMipDesc, pStandardTileShapeForNonPackedMips, pNumSubresourceTilings, FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips); }
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return m_pReal->GetAdapterLuid(); }

    ID3D12Device* GetReal() { return m_pReal; }

private:
    ID3D12Device* m_pReal;
    ULONG m_refCount;
};
