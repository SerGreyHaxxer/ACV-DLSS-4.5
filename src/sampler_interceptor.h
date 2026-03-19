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
#include <d3d12.h>

// ============================================================================
// Fix 8: Creation-Time Sampler LOD Bias Interception
// ============================================================================
// Old approach: ApplySamplerLodBias looped through all recorded samplers every
// frame calling CreateSampler on CPU handles. This was:
//   1. Massive CPU overhead (100+ CreateSampler calls per frame)
//   2. Fundamentally broken — overwriting CPU handles does nothing to GPU-bound
//      descriptors already copied to shader-visible heaps
//   3. Missed 80% of textures because AnvilNext uses Static Samplers
//
// New approach: Apply LOD bias at sampler creation time. The bias is stored
// atomically and applied to every D3D12_SAMPLER_DESC before it reaches
// the real CreateSampler. For static samplers, the bias will be applied
// via root signature interception (separate hook, future work).
// ============================================================================

void SamplerInterceptor_NewFrame();

// Fix 8: Set the target LOD bias. Applied to all newly created samplers.
// Call this when the user changes the LOD bias setting in the UI.
void SamplerInterceptor_SetTargetLODBias(float bias);
float SamplerInterceptor_GetTargetLODBias();

// Register a sampler — applies the current LOD bias to the desc at creation time
D3D12_SAMPLER_DESC ApplyLodBias(const D3D12_SAMPLER_DESC& desc);

void ClearSamplers();

// P1 Fix 4: Hook D3D12SerializeVersionedRootSignature to apply LOD bias
// to static samplers baked into root signatures (~85% of ACV textures).
void SamplerInterceptor_InstallRootSigHook();
bool IsRootSigHookReady();
HRESULT WINAPI Hooked_D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob);
