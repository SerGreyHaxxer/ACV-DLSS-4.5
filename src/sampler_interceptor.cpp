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
#include <atomic>
#include <mutex>
#include <vector>
#include <wrl/client.h>

namespace {
    struct SamplerRecord {
        D3D12_SAMPLER_DESC desc = {};
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {};
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        bool valid = false;
        uint64_t lastUsedFrame = 0;
    };

    static std::atomic<uint64_t> g_samplerFrame{0};

    // Lock hierarchy level 3 â€” same tier as Resources
    // (SwapChain=1 > Hooks=2 > Resources/Samplers=3 > Config=4 > Logging=5).
    std::mutex g_samplerMutex;
    std::vector<SamplerRecord> g_samplerRecords;
}

void SamplerInterceptor_NewFrame() {
    uint64_t frame = g_samplerFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    std::lock_guard<std::mutex> lock(g_samplerMutex);

    // Remove entries whose device has been lost
    g_samplerRecords.erase(
        std::remove_if(g_samplerRecords.begin(), g_samplerRecords.end(),
            [](const SamplerRecord& r) {
                return r.device && r.device->GetDeviceRemovedReason() != S_OK;
            }),
        g_samplerRecords.end());

    // LRU eviction: cap at 256, shrink to 192
    constexpr size_t kMaxEntries = 256;
    constexpr size_t kTargetEntries = 192;
    if (g_samplerRecords.size() > kMaxEntries) {
        std::sort(g_samplerRecords.begin(), g_samplerRecords.end(),
            [](const SamplerRecord& a, const SamplerRecord& b) {
                return a.lastUsedFrame < b.lastUsedFrame;
            });
        g_samplerRecords.erase(g_samplerRecords.begin(),
            g_samplerRecords.begin() + static_cast<ptrdiff_t>(g_samplerRecords.size() - kTargetEntries));
    }
}

void ApplySamplerLodBias(float bias) {
    uint64_t frame = g_samplerFrame.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_samplerMutex);
    if (g_samplerRecords.empty()) return;
    for (auto& record : g_samplerRecords) {
        if (!record.valid || !record.device || record.cpuHandle.ptr == 0) continue;
        // Validate device health before writing descriptors
        if (record.device->GetDeviceRemovedReason() != S_OK) continue;
        D3D12_SAMPLER_DESC nD = record.desc;
        nD.MipLODBias += bias;
        nD.MipLODBias = std::clamp(nD.MipLODBias, -3.0f, 3.0f);
        record.device->CreateSampler(&nD, record.cpuHandle);
        record.lastUsedFrame = frame;
    }
}

void RegisterSampler(const D3D12_SAMPLER_DESC& desc, D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Device* device) {
    uint64_t frame = g_samplerFrame.load(std::memory_order_relaxed);
    SamplerRecord record{};
    record.desc = desc;
    record.cpuHandle = handle;
    record.device = device;
    record.valid = true;
    record.lastUsedFrame = frame;
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

