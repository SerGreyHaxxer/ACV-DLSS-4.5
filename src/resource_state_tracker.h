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

// Lightweight resource state tracker fed by ResourceBarrier ghost hook.
// Lock hierarchy level 3 (SwapChain=1 > Hooks=2 > Resources=3 > Config=4 > Logging=5).
void ResourceStateTracker_RecordTransition(ID3D12Resource* pResource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter);
bool ResourceStateTracker_GetCurrentState(ID3D12Resource* pResource, D3D12_RESOURCE_STATES& outState);
void ResourceStateTracker_EvictStale(uint64_t currentFrame, uint64_t maxAge);
void ResourceStateTracker_Clear();