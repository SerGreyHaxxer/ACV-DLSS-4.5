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
// ============================================================================
// D3D12 Wrappers â€” REMOVED
// ============================================================================
// The COM wrapper classes (WrappedID3D12Device, WrappedID3D12CommandQueue,
// WrappedID3D12GraphicsCommandList) have been removed. All D3D12 interception
// is now handled exclusively by VTable hooks in hooks.cpp.
//
// This file is intentionally minimal. The ~500 lines of COM wrapper
// boilerplate were dead code â€” the factory wrapping that would activate them
// was disabled in proxy.cpp, and only VTable hooks were ever live.
//
// Resource tracking, CBV tracking, descriptor tracking, and sampler
// interception are all performed by the VTable hook callbacks.
// ============================================================================

