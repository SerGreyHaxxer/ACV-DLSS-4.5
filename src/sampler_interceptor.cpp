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
#include "streamline_integration.h"
#include "logger.h"
#include <algorithm>
#include <mutex>
#include <vector>
#include <wrl/client.h>

namespace {
    struct SamplerRecord {
        D3D12_SAMPLER_DESC desc = {};
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        bool valid = false;
    };

    // Lock hierarchy level 3 â€” same tier as Resources
    // (SwapChain=1 > Hooks=2 > Resources/Samplers=3 > Config=4 > Logging=5).
    std::mutex g_samplerMutex;
    std::vector<SamplerRecord> g_samplerRecords;
}

void ApplySamplerLodBias(float bias) {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    if (g_samplerRecords.empty()) return;
    for (const auto& record : g_samplerRecords) {
        if (!record.valid || !record.device || record.cpuHandle.ptr == 0) continue;
        // Validate device health before writing descriptors
        if (record.device->GetDeviceRemovedReason() != S_OK) continue;
        D3D12_SAMPLER_DESC nD = record.desc;
        nD.MipLODBias += bias;
        nD.MipLODBias = std::clamp(nD.MipLODBias, -3.0f, 3.0f);
        record.device->CreateSampler(&nD, record.cpuHandle);
    }
}

void RegisterSampler(const D3D12_SAMPLER_DESC& desc, D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Device* device) {
    SamplerRecord record{};
    record.desc = desc;
    record.cpuHandle = handle;
    record.device = device;
    record.valid = true;
    {
        std::lock_guard<std::mutex> lock(g_samplerMutex);
        for (auto& existing : g_samplerRecords) {
            if (existing.cpuHandle.ptr == handle.ptr) {
                existing = record;
                return;
            }
        }
        g_samplerRecords.push_back(record);
    }
}

void ClearSamplers() {
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    g_samplerRecords.clear();
}

