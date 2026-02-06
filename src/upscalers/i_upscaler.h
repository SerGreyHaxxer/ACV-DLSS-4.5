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
#include <dxgi1_4.h>
#include <string>

// Abstract interface for upscaling backends (DLSS, XeSS, FSR)
class IUpscaler {
public:
    virtual ~IUpscaler() = default;

    // Lifecycle
    virtual const char* GetName() const = 0;
    virtual bool Initialize(ID3D12Device* device) = 0;
    virtual void Shutdown() = 0;
    virtual bool IsInitialized() const = 0;

    // Setup
    virtual void SetCommandQueue(ID3D12CommandQueue* queue) = 0;
    virtual void NewFrame(IDXGISwapChain* swapChain) = 0;

    // Resource Tagging
    virtual void SetColorBuffer(ID3D12Resource* resource) = 0;
    virtual void SetDepthBuffer(ID3D12Resource* resource) = 0;
    virtual void SetMotionVectors(ID3D12Resource* resource) = 0;
    virtual void SetCameraData(const float* view, const float* proj, float jitterX, float jitterY) = 0;

    // Evaluation
    virtual void Evaluate(ID3D12GraphicsCommandList* cmdList) = 0;

    // Configuration
    virtual void SetMode(int modeIndex) = 0;
    virtual void SetPreset(int presetIndex) = 0;
    virtual void SetSharpness(float sharpness) = 0;
    virtual void SetJitterScale(float x, float y) = 0;
    
    // Feature Checks
    virtual bool IsSupported() const = 0;
};
