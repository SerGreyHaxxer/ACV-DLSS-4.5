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
#include <cstdint>

// ============================================================================
// P0 Fix 2 + P1 Fix 3: RCU Double-Buffered TLS-Batched Resource State Tracker
// ============================================================================
// RecordTransition is called 5,000-15,000 times per frame across multiple
// Anvil worker threads. The architecture is:
//
//   RecordTransition → O(1) push to thread_local inplace_vector (zero locks)
//   FlushTLS         → Builds new state on write buffer, atomically flips
//                       the active pointer (single brief lock)
//   GetCurrentState  → Wait-free atomic load of active map pointer (ZERO
//                       locks, ZERO cache-line invalidation — eliminates the
//                       shared_mutex refcount bouncing that was choking CPUs
//                       with 15,000+ reads/frame)
//
// TLSBatch uses atomic<bool> is_alive tombstone (P0 Fix 2) to prevent
// OS Loader Lock deadlocks during DLL_THREAD_DETACH on worker thread exit.
// ============================================================================

void ResourceStateTracker_RecordTransition(ID3D12Resource* pResource,
                                           D3D12_RESOURCE_STATES stateBefore,
                                           D3D12_RESOURCE_STATES stateAfter);

bool ResourceStateTracker_GetCurrentState(ID3D12Resource* pResource,
                                          D3D12_RESOURCE_STATES& outState);

// Flush all TLS batches into the active read-optimized map via RCU swap.
// Must be called from a single point (Hooked_ExecuteCommandLists) before
// any code that reads resource states (camera scanning, DLSS evaluation).
void ResourceStateTracker_FlushTLS();

void ResourceStateTracker_EvictStale(uint64_t currentFrame, uint64_t maxAge);
void ResourceStateTracker_Clear();

// Per-thread diagnostic statistics
struct ResourceStateTrackerStats {
    uint64_t totalPushed;   // Total transitions recorded across all threads
    uint64_t totalDropped;  // Transitions lost due to full batches
    uint64_t totalFlushed;  // Transitions successfully merged into state map
    uint32_t activeThreads; // Number of live TLS batches
    size_t   mapSize;       // Current entries in the active state map
};
ResourceStateTrackerStats ResourceStateTracker_GetStats();