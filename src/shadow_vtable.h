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
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// ============================================================================
// Fix 3: Shadow VTable Hooking
// ============================================================================
// Replaces the Hardware Breakpoint rotation scheduler for D3D12 interface hooks.
// Instead of cycling 14+ hooks through 2 debug registers (missing ~85% of calls),
// we clone the vtable into our own memory and swap the object's vptr.
//
// Denuvo protects the .text segment, not heap-allocated COM objects. Shadow
// vtables are invisible to integrity checks since we never modify any code.
//
// Present + ExecuteCommandLists remain on pinned HWBP Dr0/Dr1 since they
// intercept exported DXGI functions where code integrity may apply.
// ============================================================================

class ShadowVTable {
public:
  // Install a shadow vtable on the given COM object.
  // vtableEntryCount: number of entries in the vtable to clone.
  // Returns the original vptr so it can be restored later.
  // Thread-safe: uses internal lock for the shadow allocation table.
  static void* Install(void* pObject, size_t vtableEntryCount);

  // Replace a single entry in an already-installed shadow vtable.
  // index: vtable slot index, newFunc: the hook function pointer.
  // Returns the original function pointer at that slot.
  static void* PatchEntry(void* pObject, size_t index, void* newFunc);

  // Restore the original vtable pointer and free the shadow allocation.
  static void Remove(void* pObject);

  // Check if an object already has a shadow vtable installed.
  static bool HasShadow(void* pObject);

  // Get the original function pointer at a given vtable slot.
  // Returns nullptr if the object has no shadow vtable or index is out of bounds.
  static void* GetOriginalEntry(void* pObject, size_t index);

private:
  struct ShadowInfo {
    void* originalVPtr;      // The original vtable pointer
    void** shadowVTable;     // Our cloned vtable (heap-allocated)
    size_t entryCount;       // Number of entries
  };

  static std::mutex s_mutex;
  static std::unordered_map<void*, ShadowInfo> s_shadows;
};
