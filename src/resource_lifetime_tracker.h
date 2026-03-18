/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once
#include <Unknwn.h>
#include <atomic>
#include <d3d12.h>

// ============================================================================
// Fix 1.4: Lock-Free Resource Lifetime Tracking via SetPrivateDataInterface
// ============================================================================
// Replaces the old ResourceLifetimeTracker (global mutex + unordered_set)
// with a zero-lock, O(1) approach using D3D12's built-in private data API.
//
// HOW IT WORKS:
//   1. TrackResource() attaches a GhostTrackerTag (IUnknown) to the resource
//      via ID3D12Object::SetPrivateDataInterface().
//   2. The D3D12 runtime holds a reference to our tag.
//   3. When the resource is destroyed, D3D12 calls Release() on the tag.
//   4. Our Release() enqueues the pointer into DeferredGC (lock-free ring).
//   5. At frame boundaries, DeferredGC drains into ResourceDetector.
//
// This eliminates:
//   - The global mutex that serialized all resource tracking (Vuln #1)
//   - The synchronous OnResourceDestroyed call causing ABA problems (Vuln #5)
//   - The overhead of maintaining a central unordered_set
// ============================================================================

// {7F8B74CD-DEAD-BEEF-0000-D3D12TRACKER}
static const GUID GUID_GhostTracker =
    {0x7f8b74cd, 0xdead, 0xbeef, {0x00, 0x00, 0xd3, 0xd1, 0x2a, 0xce, 0xba, 0x6b}};

class GhostTrackerTag : public IUnknown {
  ID3D12Resource* m_pRes;
  std::atomic<ULONG> m_ref{1};

public:
  explicit GhostTrackerTag(ID3D12Resource* res) : m_pRes(res) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
      *ppv = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override;

  ID3D12Resource* GetResource() const { return m_pRes; }
};

// ============================================================================
// ResourceLifetimeTracker — thin static API over SetPrivateDataInterface
// ============================================================================
class ResourceLifetimeTracker {
public:
  static ResourceLifetimeTracker& Get();

  // Non-copyable, non-movable singleton
  ResourceLifetimeTracker(const ResourceLifetimeTracker&) = delete;
  ResourceLifetimeTracker& operator=(const ResourceLifetimeTracker&) = delete;

  // Attach a GhostTrackerTag to the resource. Lock-free, O(1).
  void TrackResource(ID3D12Resource* pResource);

  // Remove tracking tag. Lock-free, O(1).
  void UntrackResource(ID3D12Resource* pResource);

  // Check if a resource has a tracking tag. Lock-free, O(1).
  [[nodiscard]] bool IsTracked(ID3D12Resource* pResource) const;

  // Called once to wire up the tag. No-op for now.
  void Initialize() {}
  void Shutdown() {}

private:
  ResourceLifetimeTracker() = default;
};
