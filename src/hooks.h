/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "error_types.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

struct SwapChainHookState {
  Microsoft::WRL::ComPtr<IDXGISwapChain> pSwapChain;
};

// Function pointer types for original functions
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCommandQueue)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID,
                                                           void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCommittedResource)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                                D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID,
                                                                void**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDescriptorHeap)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID,
                                                             void**);
typedef void(STDMETHODCALLTYPE* PFN_CreateShaderResourceView)(ID3D12Device*, ID3D12Resource*,
                                                              const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                                              D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE* PFN_CreateUnorderedAccessView)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*,
                                                               const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE* PFN_CreateRenderTargetView)(ID3D12Device*, ID3D12Resource*,
                                                            const D3D12_RENDER_TARGET_VIEW_DESC*,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE* PFN_CreateDepthStencilView)(ID3D12Device*, ID3D12Resource*,
                                                            const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE);

// CreateConstantBufferView function pointer (Phase 2.5: CBV descriptor tracking)
typedef void(STDMETHODCALLTYPE* PFN_CreateConstantBufferView)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*,
                                                              D3D12_CPU_DESCRIPTOR_HANDLE);

extern SwapChainHookState g_SwapChainState;
extern PFN_Present g_OriginalPresent;
extern PFN_Present1 g_OriginalPresent1;
extern PFN_ResizeBuffers g_OriginalResizeBuffers;

// Function pointer types for new hooks
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePlacedResource)(ID3D12Device*, ID3D12Heap*, UINT64,
                                                             const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                             const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef void(STDMETHODCALLTYPE* PFN_ClearDepthStencilView)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                           D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
typedef void(STDMETHODCALLTYPE* PFN_ClearRenderTargetView)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                           const FLOAT[4], UINT, const D3D12_RECT*);

// Phase 3: CreateSampler hook for LOD bias interception
typedef void(STDMETHODCALLTYPE* PFN_CreateSampler)(ID3D12Device*, const D3D12_SAMPLER_DESC*,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE);

// Camera detection: ID3D12Resource::Map hook for upload buffer tracking
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResourceMap)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);

// Phase 3: CreateCommandList hook for auto-vtable capture
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCommandList)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE,
                                                          ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID,
                                                          void**);

// Extern declarations for captured original function pointers
extern PFN_ExecuteCommandLists g_OriginalExecuteCommandLists;
extern PFN_CreateCommandQueue g_OriginalCreateCommandQueue;
extern PFN_CreateCommittedResource g_OriginalCreateCommittedResource;
extern PFN_D3D12CreateDevice g_OriginalD3D12CreateDevice;
extern PFN_CreatePlacedResource g_OriginalCreatePlacedResource;
extern PFN_CreateShaderResourceView g_OriginalCreateSRV;
extern PFN_CreateUnorderedAccessView g_OriginalCreateUAV;
extern PFN_CreateRenderTargetView g_OriginalCreateRTV;
extern PFN_CreateDepthStencilView g_OriginalCreateDSV;
extern PFN_ClearDepthStencilView g_OriginalClearDSV;
extern PFN_ClearRenderTargetView g_OriginalClearRTV;
extern PFN_CreateConstantBufferView g_OriginalCreateCBV;
extern PFN_CreateSampler g_OriginalCreateSampler;
extern PFN_CreateDescriptorHeap g_OriginalCreateDescriptorHeap;
extern PFN_CreateCommandList g_OriginalCreateCommandList;

void WrapCreatedD3D12Device(REFIID riid, void** ppDevice, bool takeOwnership = true);
void InstallD3D12Hooks();
bool InitializeHooks();
void CleanupHooks();
void EnsureD3D12VTableHooks(ID3D12Device* device);
void NotifyWrappedCommandListUsed();
bool IsWrappedCommandListUsed();
void SetPatternJitterAddress(uintptr_t address);
bool TryGetPatternJitter(float& jitterX, float& jitterY);
