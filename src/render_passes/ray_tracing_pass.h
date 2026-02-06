/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <mutex>
#include <vector>

class RayTracingPass {
public:
    static RayTracingPass& Get();

    bool Initialize(ID3D12Device* device);
    void Shutdown();
    
    // Execute the pass
    void Execute(ID3D12GraphicsCommandList* cmdList, 
                 ID3D12Resource* color, 
                 ID3D12Resource* depth, 
                 ID3D12Resource* mvecs,
                 const float* viewProjInv,
                 const float* viewProj,
                 const float* camPos,
                 const float2& resolution);

private:
    RayTracingPass() = default;
    
    bool CreateRootSignature(ID3D12Device* device);
    bool CreatePipelineState(ID3D12Device* device);
    
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    
    // Resource heap for SRVs/UAVs if needed, or we use dynamic descriptors
    // For simplicity, we assume we can set root descriptors or use a transient heap
    
    bool m_initialized = false;
    std::mutex m_mutex;
    
    struct alignas(256) FrameCB {
        float viewProjInv[16];
        float viewProj[16];
        float camPos[3];
        float time;
        float resolution[2];
        float invResolution[2];
    };
};

struct float2 {
    float x, y;
};
