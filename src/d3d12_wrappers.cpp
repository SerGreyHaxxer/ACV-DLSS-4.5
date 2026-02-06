// ============================================================================
// D3D12 Wrappers — REMOVED
// ============================================================================
// The COM wrapper classes (WrappedID3D12Device, WrappedID3D12CommandQueue,
// WrappedID3D12GraphicsCommandList) have been removed. All D3D12 interception
// is now handled exclusively by VTable hooks in hooks.cpp.
//
// This file is intentionally minimal. The ~500 lines of COM wrapper
// boilerplate were dead code — the factory wrapping that would activate them
// was disabled in proxy.cpp, and only VTable hooks were ever live.
//
// Resource tracking, CBV tracking, descriptor tracking, and sampler
// interception are all performed by the VTable hook callbacks.
// ============================================================================
