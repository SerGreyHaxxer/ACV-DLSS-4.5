/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "resource_lifetime_tracker.h"
#include "deferred_gc.h"
#include "logger.h"

// ============================================================================
// GhostTrackerTag::Release — called by D3D12 runtime on resource destruction
// ============================================================================
ULONG STDMETHODCALLTYPE GhostTrackerTag::Release() {
  ULONG count = m_ref.fetch_sub(1, std::memory_order_release) - 1;
  if (count == 0) {
    std::atomic_thread_fence(std::memory_order_acquire);
    // Fix 2.1: Defer the destruction notification (ABA fix).
    // Instead of synchronously calling OnResourceDestroyed here
    // (which allowed the allocator to recycle the address while
    // ResourceDetector still held a stale pointer), we enqueue
    // the pointer into a lock-free ring buffer.
    DeferredGC::Get().EnqueueDestroyed(m_pRes);
    delete this;
  }
  return count;
}

// ============================================================================
// ResourceLifetimeTracker — singleton
// ============================================================================
ResourceLifetimeTracker& ResourceLifetimeTracker::Get() {
  static ResourceLifetimeTracker instance;
  return instance;
}

void ResourceLifetimeTracker::TrackResource(ID3D12Resource* pResource) {
  if (!pResource) return;
  auto* tag = new GhostTrackerTag(pResource);
  // SetPrivateDataInterface adds a reference to our tag.
  // The D3D12 runtime will call Release() when the resource is destroyed.
  pResource->SetPrivateDataInterface(GUID_GhostTracker, tag);
  tag->Release(); // Balance the initial refcount — D3D12 now owns it
}

void ResourceLifetimeTracker::UntrackResource(ID3D12Resource* pResource) {
  if (!pResource) return;
  // Setting data size to 0 with nullptr removes the private data,
  // which releases our GhostTrackerTag.
  pResource->SetPrivateData(GUID_GhostTracker, 0, nullptr);
}

bool ResourceLifetimeTracker::IsTracked(ID3D12Resource* pResource) const {
  if (!pResource) return false;
  UINT size = sizeof(IUnknown*);
  IUnknown* existing = nullptr;
  HRESULT hr = pResource->GetPrivateData(GUID_GhostTracker, &size, &existing);
  if (SUCCEEDED(hr) && existing) {
    existing->Release(); // GetPrivateData AddRef'd it
    return true;
  }
  return false;
}
