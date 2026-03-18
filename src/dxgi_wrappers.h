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
#include <dxgi1_6.h>
#include <d3d12.h>

// Called from the D3D12 Present/submission thread — safe for GPU work
void OnPresentThread(IDXGISwapChain *pSwapChain);
void StartFrameTimer();
void StopFrameTimer();

// ============================================================================
// Fix 1.3: DXGI Factory VTable Hooking (replaces WrappedIDXGIFactory)
// ============================================================================
// Installs shadow VTable hooks on the real IDXGIFactory for the 4 swap chain
// creation methods. This preserves COM identity rules — the game always sees
// the real factory pointer, so QueryInterface between DXGI and D3D12 objects
// returns consistent IUnknown pointers.
//
// The previous WrappedIDXGIFactory COM wrapper violated COM identity rules:
// wrapping DXGI while using VTable hooks for D3D12 meant QI between the two
// returned non-matching IUnknown pointers, which can crash strict engines.
// ============================================================================
void InstallDXGIFactoryVTableHooks(IDXGIFactory* pFactory);
