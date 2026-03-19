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
#include "sampler_interceptor.h"
#include "logger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>

namespace {

// Fix 8: Atomic LOD bias — set once by UI, read on every CreateSampler call.
// No per-frame loops, no mutex contention.
static std::atomic<float> g_targetLODBias{0.0f};

// Frame counter for diagnostics only
static std::atomic<uint64_t> g_samplerFrame{0};
static std::atomic<uint64_t> g_samplersCreated{0};

} // namespace

void SamplerInterceptor_NewFrame() {
    g_samplerFrame.fetch_add(1, std::memory_order_relaxed);
    // Fix 8: No per-frame work needed — bias is applied at creation time.
    // The old code looped through all samplers calling CreateSampler every frame.
}

void SamplerInterceptor_SetTargetLODBias(float bias) {
    bias = std::clamp(bias, -3.0f, 3.0f);
    float old = g_targetLODBias.exchange(bias, std::memory_order_relaxed);
    if (std::fabs(old - bias) > 0.001f) {
        LOG_INFO("[Sampler] LOD bias updated: {:.3f} -> {:.3f}", old, bias);
    }
}

float SamplerInterceptor_GetTargetLODBias() {
    return g_targetLODBias.load(std::memory_order_relaxed);
}

D3D12_SAMPLER_DESC ApplyLodBias(const D3D12_SAMPLER_DESC& desc) {
    // Fix 8: Apply LOD bias at creation time — the ONLY point where it matters.
    // Once the game copies this CPU descriptor to a shader-visible GPU heap,
    // overwriting the original CPU handle does nothing.
    float bias = g_targetLODBias.load(std::memory_order_relaxed);
    D3D12_SAMPLER_DESC modifiedDesc = desc;

    if (std::fabs(bias) > 0.001f) {
        modifiedDesc.MipLODBias += bias;
        modifiedDesc.MipLODBias = std::clamp(modifiedDesc.MipLODBias, -16.0f, 15.99f);
    }

    uint64_t count = g_samplersCreated.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count % 200 == 0) {
        LOG_DEBUG("[Sampler] {} samplers created with LOD bias {:.3f}", count, bias);
    }

    return modifiedDesc;
}

void ClearSamplers() {
    // Fix 8: No sampler records to clear — we don't track them anymore.
    // The bias is applied at creation time and forgotten.
    g_samplersCreated.store(0, std::memory_order_relaxed);
}

// ============================================================================
// P1 Fix 4: Static Sampler Root Signature Interception
// ============================================================================
// AnvilNext 2.0 bakes ~85% of environmental samplers into Root Signatures
// as D3D12_STATIC_SAMPLER_DESC. CreateSampler only catches dynamic ones.
// This hook intercepts D3D12SerializeVersionedRootSignature to apply LOD
// bias to static sampler entries before the engine serializes them.
// ============================================================================

using PFN_D3D12SerializeVersionedRootSignature =
    HRESULT (WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
static PFN_D3D12SerializeVersionedRootSignature g_OrigSerializeRootSig = nullptr;

bool IsRootSigHookReady() {
    return g_OrigSerializeRootSig != nullptr;
}

static bool IsFilterAnisotropicOrLinear(D3D12_FILTER filter) {
    // Match anisotropic and all linear variants
    switch (filter) {
        case D3D12_FILTER_ANISOTROPIC:
        case D3D12_FILTER_MIN_MAG_MIP_LINEAR:
        case D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT:
        case D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
        case D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT:
        case D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR:
            return true;
        default:
            return false;
    }
}

HRESULT WINAPI Hooked_D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob) {

    if (!g_OrigSerializeRootSig) return E_FAIL;

    float bias = SamplerInterceptor_GetTargetLODBias();

    // If no LOD bias is active, pass through directly (zero overhead)
    if (std::fabs(bias) <= 0.001f || !pRootSignature) {
        return g_OrigSerializeRootSig(pRootSignature, ppBlob, ppErrorBlob);
    }

    // Copy the descriptor to modify static samplers
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC modifiedDesc = *pRootSignature;

    // Determine which version we have and get static sampler array
    std::vector<D3D12_STATIC_SAMPLER_DESC> modifiedSamplers_1_0;

    if (modifiedDesc.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
        UINT numSamplers = modifiedDesc.Desc_1_0.NumStaticSamplers;
        if (numSamplers > 0 && modifiedDesc.Desc_1_0.pStaticSamplers) {
            modifiedSamplers_1_0.assign(
                modifiedDesc.Desc_1_0.pStaticSamplers,
                modifiedDesc.Desc_1_0.pStaticSamplers + numSamplers);
            for (auto& sampler : modifiedSamplers_1_0) {
                if (IsFilterAnisotropicOrLinear(sampler.Filter)) {
                    sampler.MipLODBias += bias;
                    sampler.MipLODBias = std::clamp(sampler.MipLODBias, -16.0f, 15.99f);
                }
            }
            modifiedDesc.Desc_1_0.pStaticSamplers = modifiedSamplers_1_0.data();
        }
    } else if (modifiedDesc.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        UINT numSamplers = modifiedDesc.Desc_1_1.NumStaticSamplers;
        if (numSamplers > 0 && modifiedDesc.Desc_1_1.pStaticSamplers) {
            modifiedSamplers_1_0.assign(
                modifiedDesc.Desc_1_1.pStaticSamplers,
                modifiedDesc.Desc_1_1.pStaticSamplers + numSamplers);
            for (auto& sampler : modifiedSamplers_1_0) {
                if (IsFilterAnisotropicOrLinear(sampler.Filter)) {
                    sampler.MipLODBias += bias;
                    sampler.MipLODBias = std::clamp(sampler.MipLODBias, -16.0f, 15.99f);
                }
            }
            modifiedDesc.Desc_1_1.pStaticSamplers = modifiedSamplers_1_0.data();

            static std::atomic<uint32_t> s_logCount{0};
            if (s_logCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                LOG_INFO("[Sampler] Patched {} static samplers in root sig (LOD bias {:.3f})",
                         numSamplers, bias);
            }
        }
    }

    return g_OrigSerializeRootSig(&modifiedDesc, ppBlob, ppErrorBlob);
}

void SamplerInterceptor_InstallRootSigHook() {
    // Get the real D3D12SerializeVersionedRootSignature from d3d12.dll
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hD3D12) {
        LOG_WARN("[Sampler] d3d12.dll not loaded, cannot hook SerializeVersionedRootSignature");
        return;
    }

    auto realFunc = reinterpret_cast<PFN_D3D12SerializeVersionedRootSignature>(
        GetProcAddress(hD3D12, "D3D12SerializeVersionedRootSignature"));
    if (!realFunc) {
        LOG_WARN("[Sampler] D3D12SerializeVersionedRootSignature not found in d3d12.dll");
        return;
    }

    g_OrigSerializeRootSig = realFunc;
    LOG_INFO("[Sampler] Root signature hook prepared — awaiting GetProcAddress interception");
}
