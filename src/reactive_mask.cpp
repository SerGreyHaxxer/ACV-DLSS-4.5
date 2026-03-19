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
#include "reactive_mask.h"

#include "logger.h"

#include <d3dcompiler.h>

// ============================================================================
// Fix 9: Embedded HLSL Compute Shader for Reactive Mask Generation
// ============================================================================
// Logic: For every pixel, compute the perceptual difference between the
// final rendered frame (with HUD) and the HUD-less frame. If the difference
// exceeds a threshold, mark that pixel as "reactive" (mask=1.0), which tells
// DLSS Frame Generation to NOT interpolate that pixel using temporal history.
//
// This eliminates ghosting on compass needles, health bars, damage numbers,
// minimap icons, and all other HUD elements.
// ============================================================================

static const char* kReactiveMaskCS = R"HLSL(
Texture2D<float4> FinalColor   : register(t0);
Texture2D<float4> HudLessColor : register(t1);
RWTexture2D<float> ReactiveMask : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    // P3 Fix 4: 3x3 Dilation (Max Filter) to encompass anti-aliased UI fringes.
    // Scaleform UI elements (compass markers, text) have semi-transparent borders
    // that a strict 1-pixel smoothstep misses, causing Frame Gen to smear them.
    float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
    float maxMask = 0.0;

    [unroll]
    for (int x = -1; x <= 1; ++x) {
        [unroll]
        for (int y = -1; y <= 1; ++y) {
            int2 offsetCoord = tid.xy + int2(x, y);
            float lumWith    = dot(FinalColor[offsetCoord].rgb, lumaWeights);
            float lumWithout = dot(HudLessColor[offsetCoord].rgb, lumaWeights);

            // Safe relative diff for HDR (scRGB/PQ)
            float relDiff = abs(lumWith - lumWithout) / max(lumWith, 0.001);
            float mask = smoothstep(0.015, 0.08, relDiff);

            maxMask = max(maxMask, mask); // Keep highest reactivity in the 3x3 block
        }
    }
    ReactiveMask[tid.xy] = maxMask;
}
)HLSL";

ReactiveMask& ReactiveMask::Get() {
  static ReactiveMask instance;
  return instance;
}

bool ReactiveMask::Initialize(ID3D12Device* pDevice, UINT width, UINT height) {
  if (m_initialized && m_width == width && m_height == height) return true;
  if (!pDevice || width == 0 || height == 0) return false;

  m_pDevice = pDevice;

  if (!CreateComputePipeline(pDevice)) return false;
  if (!CreateMaskTexture(pDevice, width, height)) return false;

  m_initialized = true;
  LOG_INFO("[ReactiveMask] GPU compute pipeline initialized {}x{}", width, height);
  return true;
}

bool ReactiveMask::CreateComputePipeline(ID3D12Device* pDevice) {
  // Compile the compute shader at runtime using D3DCompile
  Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
  Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

  HRESULT hr = D3DCompile(
      kReactiveMaskCS,
      strlen(kReactiveMaskCS),
      "ReactiveMaskCS",
      nullptr,                      // No defines
      D3D_COMPILE_STANDARD_FILE_INCLUDE,
      "CSMain",                     // Entry point
      "cs_5_1",                     // Compute shader model 5.1
      D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
      0,
      &csBlob,
      &errorBlob);

  if (FAILED(hr)) {
    const char* errMsg = errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "Unknown";
    LOG_ERROR("[ReactiveMask] Failed to compile CS: {}", errMsg);
    return false;
  }

  // Create root signature:
  //   t0 = FinalColor (SRV)
  //   t1 = HudLessColor (SRV)
  //   u0 = ReactiveMask (UAV)
  D3D12_DESCRIPTOR_RANGE1 srvRange{};
  srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srvRange.NumDescriptors = 2;
  srvRange.BaseShaderRegister = 0;
  srvRange.RegisterSpace = 0;
  srvRange.OffsetInDescriptorsFromTableStart = 0;
  srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

  D3D12_DESCRIPTOR_RANGE1 uavRange{};
  uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  uavRange.NumDescriptors = 1;
  uavRange.BaseShaderRegister = 0;
  uavRange.RegisterSpace = 0;
  uavRange.OffsetInDescriptorsFromTableStart = 0;
  uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

  D3D12_ROOT_PARAMETER1 rootParams[2]{};
  // Param 0: Descriptor table with 2 SRVs
  rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
  rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
  rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  // Param 1: Descriptor table with 1 UAV
  rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
  rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
  rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc{};
  rsDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  rsDesc.Desc_1_1.NumParameters = 2;
  rsDesc.Desc_1_1.pParameters = rootParams;
  rsDesc.Desc_1_1.NumStaticSamplers = 0;
  rsDesc.Desc_1_1.pStaticSamplers = nullptr;
  rsDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  Microsoft::WRL::ComPtr<ID3DBlob> rsBlob;
  Microsoft::WRL::ComPtr<ID3DBlob> rsError;
  hr = D3D12SerializeVersionedRootSignature(&rsDesc, &rsBlob, &rsError);
  if (FAILED(hr)) {
    const char* errMsg = rsError ? static_cast<const char*>(rsError->GetBufferPointer()) : "Unknown";
    LOG_ERROR("[ReactiveMask] Failed to serialize root sig: {}", errMsg);
    return false;
  }

  hr = pDevice->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_rootSignature));
  if (FAILED(hr)) {
    LOG_ERROR("[ReactiveMask] Failed to create root sig (HRESULT: {:08X})", static_cast<unsigned>(hr));
    return false;
  }

  // Create compute PSO
  D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.pRootSignature = m_rootSignature.Get();
  psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
  psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();

  hr = pDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
  if (FAILED(hr)) {
    LOG_ERROR("[ReactiveMask] Failed to create compute PSO (HRESULT: {:08X})", static_cast<unsigned>(hr));
    return false;
  }

  // Create descriptor heap (3 descriptors per frame × kFrameBuffers frames = ring-buffered)
  // This prevents CPU writes from racing with GPU reads on the same descriptors.
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.NumDescriptors = 3 * kFrameBuffers;
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap));
  if (FAILED(hr)) {
    LOG_ERROR("[ReactiveMask] Failed to create SRV/UAV heap (HRESULT: {:08X})", static_cast<unsigned>(hr));
    return false;
  }

  m_descriptorSize = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  return true;
}

bool ReactiveMask::CreateMaskTexture(ID3D12Device* pDevice, UINT width, UINT height) {
  m_width = width;
  m_height = height;

  // Create R8_UNORM UAV-capable texture for the mask output
  D3D12_RESOURCE_DESC texDesc{};
  texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texDesc.Width = width;
  texDesc.Height = height;
  texDesc.DepthOrArraySize = 1;
  texDesc.MipLevels = 1;
  texDesc.Format = DXGI_FORMAT_R8_UNORM;
  texDesc.SampleDesc.Count = 1;
  texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  D3D12_HEAP_PROPERTIES heapProps{};
  heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

  HRESULT hr = pDevice->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_maskTexture));
  if (FAILED(hr)) {
    LOG_ERROR("[ReactiveMask] Failed to create mask texture (HRESULT: {:08X})", static_cast<unsigned>(hr));
    return false;
  }

  // Create UAV for the mask texture in all frame buffer slots
  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
  uavDesc.Format = DXGI_FORMAT_R8_UNORM;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

  for (int f = 0; f < kFrameBuffers; ++f) {
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>((f * 3 + 2)) * m_descriptorSize; // Slot 2 of each frame
    pDevice->CreateUnorderedAccessView(m_maskTexture.Get(), nullptr, &uavDesc, cpuHandle);
  }

  LOG_INFO("[ReactiveMask] Created {}x{} R8_UNORM mask texture (UAV-capable, {} buffered)", width, height, kFrameBuffers);
  return true;
}

void ReactiveMask::DispatchMask(ID3D12GraphicsCommandList* pCmdList,
                                ID3D12Resource* finalColor,
                                ID3D12Resource* hudLessColor) {
  if (!m_initialized || !pCmdList || !finalColor || !hudLessColor) return;

  // P3 Fix 4: D3D12 Transition Barriers — ensure inputs are readable and output is writable.
  // Without these, GPU caches may serve stale data (causes corruption on AMD/Intel drivers).
  D3D12_RESOURCE_BARRIER preBars[4]{};
  int barCount = 0;

  // Transition mask texture back to UAV for writing (after previous frame left it in NPS state)
  // On first dispatch, the resource is already in UAV state — this is harmless.
  if (m_dispatchCount.load(std::memory_order_relaxed) > 0) {
    preBars[barCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preBars[barCount].Transition.pResource = m_maskTexture.Get();
    preBars[barCount].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    preBars[barCount].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    preBars[barCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barCount++;
  }

  // Transition FinalColor: RENDER_TARGET → NON_PIXEL_SHADER_RESOURCE
  preBars[barCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  preBars[barCount].Transition.pResource = finalColor;
  preBars[barCount].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  preBars[barCount].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  preBars[barCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barCount++;

  // Transition HudLessColor: RENDER_TARGET → NON_PIXEL_SHADER_RESOURCE
  if (hudLessColor != finalColor) {
    preBars[barCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preBars[barCount].Transition.pResource = hudLessColor;
    preBars[barCount].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    preBars[barCount].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    preBars[barCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barCount++;
  }

  pCmdList->ResourceBarrier(barCount, preBars);

  // Ring-buffered descriptors: offset by frame index to avoid
  // writing into heap slots the GPU is actively reading from a previous frame.
  uint64_t dispatchNum = m_dispatchCount.load(std::memory_order_relaxed);
  uint32_t frameIdx = static_cast<uint32_t>(dispatchNum % kFrameBuffers);
  UINT offset = frameIdx * 3 * m_descriptorSize;

  D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
  cpuBase.ptr += offset;

  // Slot 0: FinalColor SRV
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Format = DXGI_FORMAT_UNKNOWN;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;

  D3D12_RESOURCE_DESC colorDesc = finalColor->GetDesc();
  srvDesc.Format = colorDesc.Format;
  m_pDevice->CreateShaderResourceView(finalColor, &srvDesc, cpuBase);

  // Slot 1: HudLessColor SRV
  D3D12_CPU_DESCRIPTOR_HANDLE slot1 = cpuBase;
  slot1.ptr += m_descriptorSize;
  D3D12_RESOURCE_DESC hudDesc = hudLessColor->GetDesc();
  srvDesc.Format = hudDesc.Format;
  m_pDevice->CreateShaderResourceView(hudLessColor, &srvDesc, slot1);

  // Set compute state
  pCmdList->SetComputeRootSignature(m_rootSignature.Get());
  pCmdList->SetPipelineState(m_pso.Get());

  ID3D12DescriptorHeap* heaps[] = {m_srvUavHeap.Get()};
  pCmdList->SetDescriptorHeaps(1, heaps);

  D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
  gpuBase.ptr += offset;

  // Root param 0: SRV table (slots 0-1 of this frame)
  pCmdList->SetComputeRootDescriptorTable(0, gpuBase);

  // Root param 1: UAV table (slot 2 of this frame)
  D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = gpuBase;
  uavGpu.ptr += 2 * m_descriptorSize;
  pCmdList->SetComputeRootDescriptorTable(1, uavGpu);

  // Dispatch: 8x8 thread groups
  UINT groupsX = (m_width + 7) / 8;
  UINT groupsY = (m_height + 7) / 8;
  pCmdList->Dispatch(groupsX, groupsY, 1);

  // Post-dispatch barriers:
  // 1. UAV barrier to ensure mask write completes before Streamline reads it
  // 2. Transition mask to NON_PIXEL_SHADER_RESOURCE for SL
  // 3. Restore input resources to RENDER_TARGET
  D3D12_RESOURCE_BARRIER postBars[4]{};
  int postCount = 0;

  // UAV barrier on mask
  postBars[postCount].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  postBars[postCount].UAV.pResource = m_maskTexture.Get();
  postCount++;

  // Mask: UNORDERED_ACCESS → NON_PIXEL_SHADER_RESOURCE (for Streamline read)
  postBars[postCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  postBars[postCount].Transition.pResource = m_maskTexture.Get();
  postBars[postCount].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  postBars[postCount].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  postBars[postCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  postCount++;

  // Restore FinalColor: NON_PIXEL_SHADER_RESOURCE → RENDER_TARGET
  postBars[postCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  postBars[postCount].Transition.pResource = finalColor;
  postBars[postCount].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  postBars[postCount].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  postBars[postCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  postCount++;

  // Restore HudLessColor (if different from FinalColor)
  if (hudLessColor != finalColor) {
    postBars[postCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postBars[postCount].Transition.pResource = hudLessColor;
    postBars[postCount].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    postBars[postCount].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    postBars[postCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    postCount++;
  }

  pCmdList->ResourceBarrier(postCount, postBars);

  uint64_t count = m_dispatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count % 300 == 0) {
    LOG_DEBUG("[ReactiveMask] {} compute dispatches completed ({}x{}, dilation 3x3)", count, m_width, m_height);
  }
}

ID3D12Resource* ReactiveMask::GetMaskTexture() const {
  if (!m_initialized) return nullptr;
  return m_maskTexture.Get();
}

void ReactiveMask::Resize(UINT width, UINT height) {
  if (!m_pDevice || width == 0 || height == 0) return;
  if (width == m_width && height == m_height) return;

  LOG_INFO("[ReactiveMask] Resizing {}x{} -> {}x{}", m_width, m_height, width, height);
  m_maskTexture.Reset();
  CreateMaskTexture(m_pDevice.Get(), width, height);
}

void ReactiveMask::Shutdown() {
  m_maskTexture.Reset();
  m_srvUavHeap.Reset();
  m_pso.Reset();
  m_rootSignature.Reset();
  m_pDevice.Reset();
  m_initialized = false;
  LOG_INFO("[ReactiveMask] Shutdown complete");
}
