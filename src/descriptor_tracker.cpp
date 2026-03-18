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
#include "deferred_gc.h"
#include "resource_detector.h"
#include "logger.h"
#include <atomic>
#include <array>
#include <mutex>
#include <ranges>
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

    // ========================================================================
    // Userspace RCU (Read-Copy-Update) for descriptor entries
    // ========================================================================
    // Readers execute ZERO atomic RMW instructions — only a single
    // atomic load (acquire) to grab the current map pointer.  This
    // eliminates the cache-line ping-pong that std::shared_mutex
    // (SRWLOCK) causes when 16+ worker threads read concurrently.
    //
    // Writers serialize via g_writerMutex, copy the map, mutate the
    // copy, swap the pointer, and defer deletion of the old map until
    // two epoch ticks have passed (guaranteeing no readers still hold
    // a reference to the old snapshot).
    //
    // Migration path: when MSVC ships C++26 <rcu>, replace the epoch
    // mechanism with std::rcu_default_domain() / std::rcu_retire().
    // ========================================================================

    using DescriptorMap = std::unordered_map<uintptr_t, DescriptorEntry>;

    // The current map pointer — readers load this with acquire semantics.
    // Writers swap it with release semantics after building a new copy.
    std::atomic<const DescriptorMap*> g_descriptorEntriesPtr{nullptr};

    // Writer-side mutex — serializes all mutators (TrackDescriptorResource,
    // EvictStaleEntries).  Only one writer at a time.
    std::mutex g_writerMutex;

    // Epoch-based deferred reclamation.  Each NewFrame() advances the epoch.
    // Old maps are retired with the epoch at which they were replaced.
    // When the current epoch is >= retiredEpoch + 2, no reader can still
    // be referencing the old map (a reader that loaded the pointer before
    // the swap will have completed its lookup within a single frame).
    struct RetiredMap {
        const DescriptorMap* map;
        uint64_t retiredEpoch;
    };
    std::vector<RetiredMap> g_retiredMaps;  // guarded by g_writerMutex
    std::atomic<uint64_t> g_currentEpoch{0};

    // Reclaim maps that are old enough.  Caller must hold g_writerMutex.
    void ReclaimRetiredMaps() {
        uint64_t epoch = g_currentEpoch.load(std::memory_order_relaxed);
        std::erase_if(g_retiredMaps, [epoch](const RetiredMap& rm) {
            if (epoch >= rm.retiredEpoch + 2) {
                delete rm.map;
                return true;
            }
            return false;
        });
    }

    // Swap the live map pointer, retiring the old one.
    // Caller must hold g_writerMutex.  newMap is heap-allocated.
    void SwapMapAndRetireOld(DescriptorMap* newMap) {
        const DescriptorMap* oldMap = g_descriptorEntriesPtr.exchange(
            newMap, std::memory_order_release);
        if (oldMap) {
            g_retiredMaps.push_back({oldMap, g_currentEpoch.load(std::memory_order_relaxed)});
        }
        ReclaimRetiredMaps();
    }

    // Descriptor heap records (low-frequency, writer-only)
    std::vector<DescriptorRecord> g_descriptorRecords;  // guarded by g_writerMutex

    std::atomic<uint64_t> g_currentFrame{ 0 };

    // Rolling eviction thresholds
    constexpr size_t kEvictStartThreshold = 8192;
    constexpr size_t kEvictAggressiveThreshold = 12288;
    constexpr size_t kEvictFullClearThreshold = 14336;
    constexpr uint64_t kOldFrameAge = 120;
    constexpr uint64_t kAggressiveFrameAge = 30;

    // Eviction operates on the writer's COW copy.  Caller must hold g_writerMutex.
    void EvictStaleEntries(DescriptorMap& map, uint64_t frame) {
        if (map.size() > kEvictStartThreshold) {
            uint64_t cutoff = (frame > kOldFrameAge) ? (frame - kOldFrameAge) : 0;
            std::erase_if(map, [cutoff](const auto& pair) {
                return pair.second.lastFrame < cutoff;
            });
        }

        if (map.size() > kEvictAggressiveThreshold) {
            uint64_t cutoff = (frame > kAggressiveFrameAge) ? (frame - kAggressiveFrameAge) : 0;
            std::erase_if(map, [cutoff](const auto& pair) {
                return pair.second.lastFrame < cutoff;
            });
        }

        // Last resort: full clear
        if (map.size() > kEvictFullClearThreshold) {
            map.clear();
        }
    }

    // ========================================================================
    // C++23 compile-time branchless format lookups
    // ========================================================================

    constexpr bool IsLikelyMotionVectorFormat(DXGI_FORMAT format) noexcept {
        constexpr auto kValidFormats = std::to_array<DXGI_FORMAT>({
            DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R16G16_UNORM,
            DXGI_FORMAT_R16G16_SNORM,
            DXGI_FORMAT_R16G16_SINT,
            DXGI_FORMAT_R16G16_UINT,
            DXGI_FORMAT_R32G32_FLOAT,
            DXGI_FORMAT_R32G32_SINT,
            DXGI_FORMAT_R32G32_UINT,
            DXGI_FORMAT_R16G16_TYPELESS,
            DXGI_FORMAT_R32G32_TYPELESS,
            DXGI_FORMAT_R16G16B16A16_SNORM,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R8G8_SNORM,
        });
        return std::ranges::contains(kValidFormats, format);
    }

    constexpr bool IsLikelyDepthFormat(DXGI_FORMAT format) noexcept {
        constexpr auto kValidFormats = std::to_array<DXGI_FORMAT>({
            DXGI_FORMAT_D32_FLOAT,
            DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
            DXGI_FORMAT_D24_UNORM_S8_UINT,
            DXGI_FORMAT_D16_UNORM,
            DXGI_FORMAT_R32_TYPELESS,
            DXGI_FORMAT_R32G8X24_TYPELESS,
            DXGI_FORMAT_R24G8_TYPELESS,
            DXGI_FORMAT_R16_TYPELESS,
            DXGI_FORMAT_R32_FLOAT,
            DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
        });
        return std::ranges::contains(kValidFormats, format);
    }
}

void DescriptorTracker_NewFrame() {
    uint64_t frame = g_currentFrame.fetch_add(1, std::memory_order_relaxed);

    // Fix 2.1: Drain deferred resource destruction notifications.
    // GhostTrackerTag::Release() enqueues into DeferredGC instead of
    // calling OnResourceDestroyed synchronously — this prevents ABA.
    DeferredGC::Get().DrainQueue([](ID3D12Resource* pRes) {
        ResourceDetector::Get().OnResourceDestroyed(pRes);
    });

    // Advance the RCU epoch — allows reclamation of retired map snapshots.
    g_currentEpoch.fetch_add(1, std::memory_order_release);

    // Evict on the frame boundary, NOT during hot draw calls.
    // Throttle eviction checks to every 60 frames (~once per second at 60fps).
    if (frame % 60 == 0) {
        std::unique_lock lock(g_writerMutex);
        const DescriptorMap* current = g_descriptorEntriesPtr.load(std::memory_order_acquire);
        if (current && current->size() > kEvictStartThreshold) {
            // COW: copy, mutate, swap
            auto* newMap = new DescriptorMap(*current);
            EvictStaleEntries(*newMap, frame);
            SwapMapAndRetireOld(newMap);
        }
        ReclaimRetiredMaps();
    }
}

void TrackDescriptorHeap(ID3D12DescriptorHeap* heap, UINT descriptorSize) {
    if (!heap) return;
    D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
    std::unique_lock lock(g_writerMutex);
    for (const auto& record : g_descriptorRecords) {
        if (record.heap.Get() == heap) return;
    }
    g_descriptorRecords.push_back({ desc, heap, descriptorSize });
}

void TrackDescriptorResource(D3D12_CPU_DESCRIPTOR_HANDLE handle, ID3D12Resource* resource, DXGI_FORMAT format) {
    if (!handle.ptr || !resource) return;
    {
        // Writer path: COW the map, insert the entry, swap the pointer.
        std::unique_lock lock(g_writerMutex);
        uint64_t frame = g_currentFrame.load(std::memory_order_relaxed);
        const DescriptorMap* current = g_descriptorEntriesPtr.load(std::memory_order_acquire);
        auto* newMap = current ? new DescriptorMap(*current) : new DescriptorMap();
        (*newMap)[handle.ptr] = { resource, format, frame };
        SwapMapAndRetireOld(newMap);
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
    // RCU read-side: single atomic load, ZERO lock contention.
    // No shared_lock, no atomic RMW, no cache-line invalidation.
    const DescriptorMap* current = g_descriptorEntriesPtr.load(std::memory_order_acquire);
    if (!current) return false;
    auto it = current->find(handle.ptr);
    if (it == current->end()) return false;
    if (outResource) *outResource = it->second.resource.Get();
    if (outFormat) *outFormat = it->second.format;
    return true;
}
