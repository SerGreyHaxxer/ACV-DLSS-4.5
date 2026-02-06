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
#include <dxgi1_4.h>
#include <atomic>
#include "camera_scanner.h"
#include "descriptor_tracker.h"
#include "sampler_interceptor.h"

// ============================================================================
// D3D12 INTERCEPTION LAYER
// ============================================================================
// This project uses VTable hooks (see hooks.cpp) as the active interception
// mechanism. The previous COM wrapper classes (WrappedID3D12Device,
// WrappedID3D12CommandQueue, WrappedID3D12GraphicsCommandList) have been
// removed as dead code â€” they were fully implemented but never activated
// because the factory wrapping in proxy.cpp was disabled.
//
// All D3D12 interception is handled by:
//   - hooks.cpp: VTable hooks on Device, CommandQueue, CommandList
//   - dxgi_wrappers.cpp: DXGI factory wrapper for swap chain interception
//
// If COM wrappers are needed in the future, they should be reintroduced
// as a clean replacement for VTable hooks, not alongside them.
// ============================================================================

