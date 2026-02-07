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
#include "descriptor_tracker.h"
#include "resource_detector.h"
#include "logger.h"
#include <mutex>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <wrl/client.h>

namespace {
    struct DescriptorRecord {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        UINT descriptorSize = 0;
    };

    struct DescriptorEntry {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint64_t lastFrame = 0;
    };

    // Lock hierarchy level 3 — same tier as Resources
    // (SwapChain=1 > Hooks=2 > Resources/Descriptors=3 > Config=4 > Logging=5).
    std::mutex g_descriptorMutex;
    std::vector<DescriptorRecord> g_descriptorRecords;
    std::unordered_map<uintptr_t, DescriptorEntry> g_descriptorEntries;

    std::atomic<uint64_t> g_currentFrame{ 0 };

    // Rolling eviction thresholds
    constexpr size_t kEvictStartThreshold = 8192;
    constexpr size_t kEvictAggressiveThreshold = 12288;
    constexpr size_t kEvictFullClearThreshold = 14336;
    constexpr uint64_t kOldFrameAge = 120;
    constexpr uint64_t kAggressiveFrameAge = 30;

    void EvictStaleEntries() {
        uint64_t frame = g_currentFrame.load(std::memory_order_relaxed);

        if (g_descriptorEntries.size() > kEvictStartThreshold) {
            uint64_t cutoff = (frame > kOldFrameAge) ? (frame - kOldFrameAge) : 0;
            for (auto it = g_descriptorEntries.begin(); it != g_descriptorEntries.end(); ) {
                if (it->second.lastFrame < cutoff)
                    it = g_descriptorEntries.erase(it);
                else
                    ++it;
            }
        }

        if (g_descriptorEntries.size() > kEvictAggressiveThreshold) {
            uint64_t cutoff = (frame > kAggressiveFrameAge) ? (frame - kAggressiveFrameAge) : 0;
            for (auto it = g_descriptorEntries.begin(); it != g_descriptorEntries.end(); ) {
                if (it->second.lastFrame < cutoff)
                    it = g_descriptorEntries.erase(it);
                else
                    ++it;
            }
        }

        // Last resort: full clear
        if (g_descriptorEntries.size() > kEvictFullClearThreshold) {
            g_descriptorEntries.clear();
        }
    }

    bool IsLikelyMotionVectorFormat(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R16G16_UNORM:
            case DXGI_FORMAT_R16G16_SNORM:
            case DXGI_FORMAT_R16G16_SINT:
            case DXGI_FORMAT_R16G16_UINT:
            case DXGI_FORMAT_R32G32_FLOAT:
            case DXGI_FORMAT_R32G32_SINT:
            case DXGI_FORMAT_R32G32_UINT:
            case DXGI_FORMAT_R16G16_TYPELESS:
            case DXGI_FORMAT_R32G32_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_SNORM:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R8G8_SNORM:
                return true;
            default:
                return false;
        }
    }

    bool IsLikelyDepthFormat(DXGI_FORMAT format) {
        switch (format) {
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            case DXGI_FORMAT_D24_UNORM_S8_UINT:
            case DXGI_FORMAT_D16_UNORM:
            case DXGI_FORMAT_R32_TYPELESS:
            case DXGI_FORMAT_R32G8X24_TYPELESS:
            case DXGI_FORMAT_R24G8_TYPELESS:
            case DXGI_FORMAT_R16_TYPELESS:
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
                return true;
            default:
                return false;
        }
    }
}

void DescriptorTracker_NewFrame() {
    g_currentFrame.fetch_add(1, std::memory_order_relaxed);
}

void TrackDescriptorHeap(ID3D12DescriptorHeap* heap, UINT descriptorSize) {
    if (!heap) return;
    D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
    std::lock_guard<std::mutex> lock(g_descriptorMutex);
    for (const auto& record : g_descriptorRecords) {
        if (record.heap.Get() == heap) return;
    }
    g_descriptorRecords.push_back({ desc, heap, descriptorSize });
}

void TrackDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, DXGI_FORMAT format) {
    if (!handle.ptr || !resource) return;
    {
        std::lock_guard<std::mutex> lock(g_descriptorMutex);
        EvictStaleEntries();
        uint64_t frame = g_currentFrame.load(std::memory_order_relaxed);
        g_descriptorEntries[handle.ptr] = { resource, format, frame };
    }
    DXGI_FORMAT effectiveFmt = format;
    if (effectiveFmt == DXGI_FORMAT_UNKNOWN) {
        effectiveFmt = resource->GetDesc().Format;
    }
    if (IsLikelyMotionVectorFormat(effectiveFmt)) {
        ResourceDetector::Get().RegisterMotionVectorFromView(resource, effectiveFmt);
    }
    if (IsLikelyDepthFormat(effectiveFmt)) {
        ResourceDetector::Get().RegisterDepthFromView(resource, effectiveFmt);
    }
}

bool TryResolveDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource** outResource, DXGI_FORMAT* outFormat) {
    if (!handle.ptr) return false;
    std::lock_guard<std::mutex> lock(g_descriptorMutex);
    auto it = g_descriptorEntries.find(handle.ptr);
    if (it == g_descriptorEntries.end()) return false;
    if (outResource) *outResource = it->second.resource.Get();
    if (outFormat) *outFormat = it->second.format;
    return true;
}
