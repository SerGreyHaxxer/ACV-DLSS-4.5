/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>

struct ID3D12Resource;

// ============================================================================
// Fix 2.1: Lock-Free SPSC Ring Buffer for Deferred Resource Destruction
// ============================================================================
// Solves the ABA problem (Vuln #5): when a resource is released, we
// DON'T immediately process the destruction notification.  Instead, the
// pointer is enqueued here and drained at frame boundaries.  This gives
// the D3D12 memory allocator time to recycle the address safely before
// ResourceDetector acts on it.
//
// Single-Producer: GhostTrackerTag::Release() (D3D12 runtime thread)
// Single-Consumer: DescriptorTracker_NewFrame() (render thread)
// ============================================================================
class DeferredGC {
public:
  static DeferredGC& Get() {
    static DeferredGC instance;
    return instance;
  }

  // Non-copyable, non-movable singleton
  DeferredGC(const DeferredGC&) = delete;
  DeferredGC& operator=(const DeferredGC&) = delete;

  // Enqueue a destroyed resource pointer for deferred processing.
  // Lock-free, safe to call from any thread (producer side).
  void EnqueueDestroyed(ID3D12Resource* pResource) {
    size_t head = m_head.load(std::memory_order_relaxed);
    size_t next = (head + 1) % kRingSize;
    // If ring is full, drop the notification (best effort).
    // This should never happen in practice with a 4096-entry ring.
    if (next == m_tail.load(std::memory_order_acquire)) return;
    m_ring[head] = pResource;
    m_head.store(next, std::memory_order_release);
  }

  // Drain all pending destruction notifications. Call once per frame.
  // Invokes the callback for each destroyed resource pointer.
  template <typename Callback>
  void DrainQueue(Callback&& cb) {
    size_t tail = m_tail.load(std::memory_order_relaxed);
    size_t head = m_head.load(std::memory_order_acquire);
    while (tail != head) {
      cb(m_ring[tail]);
      tail = (tail + 1) % kRingSize;
    }
    m_tail.store(tail, std::memory_order_release);
  }

private:
  DeferredGC() = default;

  static constexpr size_t kRingSize = 4096;
  std::array<ID3D12Resource*, kRingSize> m_ring{};
  std::atomic<size_t> m_head{0};
  std::atomic<size_t> m_tail{0};
};
