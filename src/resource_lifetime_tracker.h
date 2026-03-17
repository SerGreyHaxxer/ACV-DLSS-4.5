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

#include <mutex>
#include <unordered_set>

// ============================================================================
// Fix 1: Deterministic Resource Lifetime Tracking
// ============================================================================
// Replaces the catastrophic SEH-based IsResourceAlive() with a deterministic
// notification system. When a tracked resource's Release() drops the refcount
// to 0, the tracker notifies ResourceDetector to safely erase the pointer.
//
// This eliminates silent memory corruption caused by the old approach, where
// freed heap addresses were recycled for unrelated objects and AddRef() would
// blindly increment arbitrary memory.
// ============================================================================

class ResourceLifetimeTracker {
public:
  static ResourceLifetimeTracker& Get();

  // Non-copyable, non-movable singleton
  ResourceLifetimeTracker(const ResourceLifetimeTracker&) = delete;
  ResourceLifetimeTracker& operator=(const ResourceLifetimeTracker&) = delete;
  ResourceLifetimeTracker(ResourceLifetimeTracker&&) = delete;
  ResourceLifetimeTracker& operator=(ResourceLifetimeTracker&&) = delete;

  // Start tracking a resource pointer. Thread-safe.
  void TrackResource(ID3D12Resource* pResource);

  // Stop tracking a resource (e.g., if we no longer care). Thread-safe.
  void UntrackResource(ID3D12Resource* pResource);

  // Check if a resource is currently tracked (safe replacement for IsResourceAlive).
  bool IsTracked(ID3D12Resource* pResource) const;

  // Called by the Release hook when a resource's refcount drops to 0.
  // Removes from tracker and notifies ResourceDetector.
  void NotifyReleased(ID3D12Resource* pResource);

  // Get count of tracked resources (for diagnostics)
  size_t GetTrackedCount() const;

private:
  ResourceLifetimeTracker() = default;
  ~ResourceLifetimeTracker() = default;

  // Lock hierarchy level 3 — same tier as Resources
  mutable std::mutex m_mutex;
  std::unordered_set<ID3D12Resource*> m_tracked;
};
