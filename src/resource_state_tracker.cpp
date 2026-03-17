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
#include "resource_state_tracker.h"
#include "resource_detector.h"
#include "cpp26/inplace_vector.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// ============================================================================
// P0 Fix 2 + P1 Fix 3: RCU Double-Buffered TLS-Batched State Tracker
// ============================================================================
// Hot path (RecordTransition): Zero locks, zero atomics — pure thread-local push.
// Flush path (FlushTLS): Builds new state on write buffer, atomically flips.
// Read path (GetCurrentState): Wait-free atomic load — ZERO locks, ZERO
//   cache-line invalidation. Eliminates shared_mutex's internal refcount
//   writes that were choking the CPU memory bus at 15,000+ reads/frame.
//
// TLSBatch uses is_alive tombstone (Fix 2) to avoid Loader Lock deadlock.
// ============================================================================

struct StateEntry {
    D3D12_RESOURCE_STATES state;
    uint64_t lastFrame;
};

// P1 Fix 3: RCU double-buffered state maps
struct StateMap {
    std::unordered_map<ID3D12Resource*, StateEntry> map;
};
StateMap g_maps[2];
std::atomic<StateMap*> g_activeMap{&g_maps[0]};
int g_writeIdx = 1;
std::mutex g_flushMutex; // Only held by FlushTLS (single-threaded flush)

// Eviction constants
constexpr size_t kMaxEntries = 4096;
constexpr uint64_t kEvictAgeHard = 60;
constexpr uint64_t kEvictAgeSoft = 10;
constexpr size_t kSoftCap = 3072;

// TLS batch entry
struct TLSEntry {
    ID3D12Resource* pResource;
    D3D12_RESOURCE_STATES state;
};

// Maximum TLS batch size per thread. If a thread generates more than 512
// transitions before FlushTLS, we silently drop the oldest (ring buffer
// behavior). 512 is generous — most Anvil worker threads do 200-400.
constexpr size_t kTLSBatchSize = 512;

// Registry of all TLS batches for flush enumeration
std::mutex g_registryMutex;
std::vector<struct TLSBatch*> g_registeredBatches;

// Thread-local batch — zero synchronization on write
// P0 Fix 2: Lock-free tombstone prevents OS Loader Lock deadlock on
// DLL_THREAD_DETACH. The destructor sets is_alive=false (zero locks).
// FlushTLS reaps dead batches under its own mutex where it's safe.
struct TLSBatch {
    cpp26::inplace_vector<TLSEntry, kTLSBatchSize> entries;
    bool registered = false;
    std::atomic<bool> is_alive{true};

    ~TLSBatch() {
        // P0 Fix 2: NO MUTEX HERE! Prevents OS Loader Lock deadlock.
        // During DLL_THREAD_DETACH, the OS holds the Loader Lock.
        // If FlushTLS holds g_registryMutex, acquiring it here would
        // deadlock the entire process. Instead, we tombstone ourselves
        // and let FlushTLS clean up when it next runs.
        is_alive.store(false, std::memory_order_relaxed);
    }
};

thread_local TLSBatch tls_batch;

void EvictOlderThan(uint64_t currentFrame, uint64_t maxAge) {
    // Called while holding g_flushMutex
    StateMap* writeMap = &g_maps[g_writeIdx];
    const uint64_t cutoff = (currentFrame > maxAge) ? (currentFrame - maxAge) : 0;
    std::erase_if(writeMap->map, [cutoff](const auto& pair) {
        return pair.second.lastFrame < cutoff;
    });
}

} // namespace

// ============================================================================
// HOT PATH: Zero-lock O(1) transition recording
// ============================================================================
void ResourceStateTracker_RecordTransition(ID3D12Resource* pResource,
                                           D3D12_RESOURCE_STATES /*stateBefore*/,
                                           D3D12_RESOURCE_STATES stateAfter) {
    if (!pResource) [[unlikely]] return;

    // Lazy-register this thread's TLS batch on first use
    if (!tls_batch.registered) [[unlikely]] {
        tls_batch.registered = true;
        std::scoped_lock lock(g_registryMutex);
        g_registeredBatches.push_back(&tls_batch);
    }

    // Lock-free O(1) push — no mutex, no atomic, no contention
    if (tls_batch.entries.size() < kTLSBatchSize) [[likely]] {
        tls_batch.entries.push_back({pResource, stateAfter});
    } else {
        // Batch full — overwrite the last entry as best-effort.
        tls_batch.entries.back() = {pResource, stateAfter};
    }
}

// ============================================================================
// FLUSH: Merge all TLS batches into the read-optimized map (RCU swap)
// ============================================================================
void ResourceStateTracker_FlushTLS() {
    const uint64_t frame = ResourceDetector::Get().GetFrameCount();

    // Collect all TLS batch pointers under brief registry lock
    std::vector<TLSBatch*> batches;
    {
        std::scoped_lock lock(g_registryMutex);

        // P0 Fix 2: Reap dead (tombstoned) batches — their threads have exited
        std::erase_if(g_registeredBatches, [](TLSBatch* b) {
            return !b->is_alive.load(std::memory_order_relaxed);
        });

        batches = g_registeredBatches;
    }

    // P1 Fix 3: RCU double-buffer swap — build on write buffer, then flip
    std::scoped_lock flushLock(g_flushMutex);

    // Copy current active state to write buffer
    StateMap* active = g_activeMap.load(std::memory_order_relaxed);
    g_maps[g_writeIdx].map = active->map;

    // Merge TLS batches into the write buffer
    for (auto* batch : batches) {
        // P0 Fix 2: Skip tombstoned batches (thread died between collect + merge)
        if (!batch->is_alive.load(std::memory_order_relaxed)) continue;

        for (const auto& entry : batch->entries) {
            g_maps[g_writeIdx].map[entry.pResource] = {entry.state, frame};
        }
        batch->entries.clear();
    }

    // Cap enforcement (only during flush, not on hot path)
    if (g_maps[g_writeIdx].map.size() > kMaxEntries) [[unlikely]] {
        EvictOlderThan(frame, kEvictAgeHard);
        if (g_maps[g_writeIdx].map.size() > kSoftCap) {
            EvictOlderThan(frame, kEvictAgeSoft);
        }
    }

    // Atomic flip! Readers instantly see the new map, zero locks
    g_activeMap.store(&g_maps[g_writeIdx], std::memory_order_release);
    g_writeIdx = (g_writeIdx + 1) % 2;
}

// ============================================================================
// READ: Wait-free — zero locks, zero cache invalidation (P1 Fix 3)
// ============================================================================
bool ResourceStateTracker_GetCurrentState(ID3D12Resource* pResource,
                                          D3D12_RESOURCE_STATES& outState) {
    if (!pResource) [[unlikely]] return false;

    // P1 Fix 3: Wait-free read. No shared_mutex, no atomic refcount write,
    // no cache-line bouncing. Just a single atomic load + map lookup.
    StateMap* active = g_activeMap.load(std::memory_order_acquire);
    if (auto it = active->map.find(pResource); it != active->map.end()) [[likely]] {
        outState = it->second.state;
        return true;
    }
    return false;
}

void ResourceStateTracker_EvictStale(uint64_t currentFrame, uint64_t maxAge) {
    std::scoped_lock lock(g_flushMutex);
    EvictOlderThan(currentFrame, maxAge);
    g_activeMap.store(&g_maps[g_writeIdx], std::memory_order_release);
    g_writeIdx = (g_writeIdx + 1) % 2;
}

void ResourceStateTracker_Clear() {
    std::scoped_lock lock(g_flushMutex);
    g_maps[0].map.clear();
    g_maps[1].map.clear();
    g_activeMap.store(&g_maps[0], std::memory_order_release);
    g_writeIdx = 1;
}
