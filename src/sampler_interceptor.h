#pragma once
#include <d3d12.h>

void ApplySamplerLodBias(float bias);
void RegisterSampler(const D3D12_SAMPLER_DESC& desc, D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Device* device);
void ClearSamplers();
