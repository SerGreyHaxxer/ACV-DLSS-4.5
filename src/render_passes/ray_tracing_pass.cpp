/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "ray_tracing_pass.h"
#include "../logger.h"
#include "../shaders/ssrt_compute.h"
#include <vector>
#include <cstring>

RayTracingPass& RayTracingPass::Get() {
    static RayTracingPass instance;
    return instance;
}

bool RayTracingPass::Initialize(ID3D12Device* device) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return true;

    if (!CreateRootSignature(device)) {
        LOG_ERROR("Failed to create SSRT Root Signature");
        return false;
    }

    if (!CreatePipelineState(device)) {
        LOG_ERROR("Failed to create SSRT Pipeline State");
        return false;
    }

    m_initialized = true;
    LOG_INFO("SSRT Pass Initialized");
    return true;
}

void RayTracingPass::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pso.Reset();
    m_rootSignature.Reset();
    m_initialized = false;
}

bool RayTracingPass::CreateRootSignature(ID3D12Device* device) {
    // Root Parameters:
    // 0: UAV Output (RWTexture2D<float4>) : u0
    // 1: SRV Color (Texture2D<float4>) : t0
    // 2: SRV Depth (Texture2D<float>) : t1
    // 3: SRV Motion (Texture2D<float2>) : t2
    // 4: CBV Constants (b0) - Root Constants

    D3D12_DESCRIPTOR_RANGE ranges[4];
    // u0
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // t0
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // t1
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 1;
    ranges[2].RegisterSpace = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // t2
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].NumDescriptors = 1;
    ranges[3].BaseShaderRegister = 2;
    ranges[3].RegisterSpace = 0;
    ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[5];
    
    // U0 (Descriptor Table)
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // T0 (Descriptor Table)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // T1 (Descriptor Table)
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[2];
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // T2 (Descriptor Table)
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &ranges[3];
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // B0 (Root Constants)
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.ShaderRegister = 0;
    params[4].Constants.RegisterSpace = 0;
    params[4].Constants.Num32BitValues = sizeof(FrameCB) / 4;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Static Samplers
    D3D12_STATIC_SAMPLER_DESC samplers[2];
    // Point Clamp (s0)
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MipLODBias = 0;
    samplers[0].MaxAnisotropy = 0;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplers[0].MinLOD = 0.0f;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Linear Clamp (s1)
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].MipLODBias = 0;
    samplers[1].MaxAnisotropy = 0;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplers[1].MinLOD = 0.0f;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;
    samplers[1].RegisterSpace = 0;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 5;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 2;
    rootDesc.pStaticSamplers = samplers;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    
    // We don't link d3d12.lib directly for D3D12SerializeRootSignature if we can avoid it, 
    // but usually we must. Assuming we link against d3d12.lib.
    typedef HRESULT(WINAPI* PFN_D3D12SerializeRootSignature)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    static HRESULT(WINAPI* pfnSerialize)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**) = nullptr;
    
    if (!pfnSerialize) {
        HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
        if (hD3D12) pfnSerialize = (PFN_D3D12SerializeRootSignature)GetProcAddress(hD3D12, "D3D12SerializeRootSignature");
    }

    if (!pfnSerialize) return false;

    if (FAILED(pfnSerialize(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
        if (error) LOG_ERROR("Root Sig Error: {}", (char*)error->GetBufferPointer());
        return false;
    }

    if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)))) {
        return false;
    }

    return true;
}

bool RayTracingPass::CreatePipelineState(ID3D12Device* device) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.CS = { g_SSRT_CS, g_SSRT_CS_Size };
    psoDesc.NodeMask = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)))) {
        return false;
    }

    return true;
}

void RayTracingPass::Execute(ID3D12GraphicsCommandList* cmdList, 
                             ID3D12Resource* color, 
                             ID3D12Resource* depth, 
                             ID3D12Resource* mvecs,
                             const float* viewProjInv,
                             const float* viewProj,
                             const float* camPos,
                             const float2& resolution) {
    if (!m_initialized || !cmdList) return;

    cmdList->SetComputeRootSignature(m_rootSignature.Get());
    cmdList->SetPipelineState(m_pso.Get());

    // Setup Constants
    FrameCB cb;
    memcpy(cb.viewProjInv, viewProjInv, sizeof(float) * 16);
    memcpy(cb.viewProj, viewProj, sizeof(float) * 16);
    memcpy(cb.camPos, camPos, sizeof(float) * 3);
    cb.time = 0.0f; // TODO: Pass time
    cb.resolution[0] = resolution.x;
    cb.resolution[1] = resolution.y;
    cb.invResolution[0] = 1.0f / resolution.x;
    cb.invResolution[1] = 1.0f / resolution.y;

    cmdList->SetComputeRoot32BitConstants(4, sizeof(FrameCB) / 4, &cb, 0);

    // TODO: Bind Resources (Descriptors)
    // We need a way to get CPU/GPU handles for these resources.
    // If they are from the game, we need to create views for them.
    // This usually requires a descriptor heap we own.
    
    // For now, we skip the actual Draw/Dispatch because descriptor binding is complex without a heap manager.
    // The "DescriptorTracker" might help, or we allocate a small heap.
    
    // Dispatch
    // uint32_t groupX = (uint32_t)ceil(resolution.x / 8.0f);
    // uint32_t groupY = (uint32_t)ceil(resolution.y / 8.0f);
    // cmdList->Dispatch(groupX, groupY, 1);
}
