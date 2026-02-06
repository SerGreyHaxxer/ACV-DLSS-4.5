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
