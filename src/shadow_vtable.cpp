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
#include "shadow_vtable.h"

#include "logger.h"

#include <algorithm>
#include <cstring>
#include <memory>

std::mutex ShadowVTable::s_mutex;
std::unordered_map<void*, ShadowVTable::ShadowInfo> ShadowVTable::s_shadows;

void* ShadowVTable::Install(void* pObject, size_t vtableEntryCount) {
  if (!pObject || vtableEntryCount == 0) [[unlikely]] return nullptr;

  std::scoped_lock lock(s_mutex);

  // Don't double-install (C++20: .contains() replaces .count())
  if (s_shadows.contains(pObject)) {
    return s_shadows[pObject].originalVPtr;
  }

  // Read the current vptr from the object
  void** objectVPtrSlot = reinterpret_cast<void**>(pObject);
  void* originalVPtr = *objectVPtrSlot;
  void** originalVTable = reinterpret_cast<void**>(originalVPtr);

  // C++20: Exception-safe array allocation — automatically cleans up if logic
  // fails or faults out. Replaces naked `new (std::nothrow) void*[]`.
  auto shadow = std::make_unique_for_overwrite<void*[]>(vtableEntryCount);
  if (!shadow) [[unlikely]] {
    LOG_ERROR("[ShadowVT] Failed to allocate shadow vtable ({} entries)", vtableEntryCount);
    return nullptr;
  }

  // Clone all entries from the original vtable
  std::ranges::copy_n(originalVTable, vtableEntryCount, shadow.get());

  // Swap the object's vptr to point to our shadow
  void** rawShadow = shadow.get();
  *objectVPtrSlot = rawShadow;

  // Store metadata — release ownership to ShadowInfo for later removal/patching
  s_shadows[pObject] = {originalVPtr, shadow.release(), vtableEntryCount};

  LOG_INFO("[ShadowVT] Installed on {:p} ({} entries)", pObject, vtableEntryCount);
  return originalVPtr;
}

void* ShadowVTable::PatchEntry(void* pObject, size_t index, void* newFunc) {
  if (!pObject || !newFunc) [[unlikely]] return nullptr;

  std::scoped_lock lock(s_mutex);

  auto it = s_shadows.find(pObject);
  if (it == s_shadows.end()) {
    LOG_ERROR("[ShadowVT] PatchEntry on non-shadowed object {:p}", pObject);
    return nullptr;
  }

  auto& info = it->second;
  if (index >= info.entryCount) {
    LOG_ERROR("[ShadowVT] PatchEntry index {} >= entryCount {}", index, info.entryCount);
    return nullptr;
  }

  void* original = info.shadowVTable[index];
  info.shadowVTable[index] = newFunc;
  return original;
}

void ShadowVTable::Remove(void* pObject) {
  if (!pObject) [[unlikely]] return;

  std::scoped_lock lock(s_mutex);

  if (auto it = s_shadows.find(pObject); it != s_shadows.end()) {
    // Restore original vptr
    *reinterpret_cast<void**>(pObject) = it->second.originalVPtr;

    // Bind the raw pointer back to a unique_ptr to safely delete the array memory
    std::unique_ptr<void*[]> cleanup(it->second.shadowVTable);

    s_shadows.erase(it);
    LOG_INFO("[ShadowVT] Removed from {:p}", pObject);
  }
}

bool ShadowVTable::HasShadow(void* pObject) {
  if (!pObject) [[unlikely]] return false;
  std::scoped_lock lock(s_mutex);
  return s_shadows.contains(pObject);
}

void* ShadowVTable::GetOriginalEntry(void* pObject, size_t index) {
  if (!pObject) [[unlikely]] return nullptr;
  std::scoped_lock lock(s_mutex);
  auto it = s_shadows.find(pObject);
  if (it == s_shadows.end()) return nullptr;
  auto& info = it->second;
  if (index >= info.entryCount) return nullptr;
  // Read from the ORIGINAL vtable, not the shadow
  void** originalVTable = reinterpret_cast<void**>(info.originalVPtr);
  return originalVTable[index];
}
