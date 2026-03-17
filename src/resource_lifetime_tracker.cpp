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
#include "resource_lifetime_tracker.h"

#include "logger.h"
#include "resource_detector.h"

ResourceLifetimeTracker& ResourceLifetimeTracker::Get() {
  static ResourceLifetimeTracker instance;
  return instance;
}

void ResourceLifetimeTracker::TrackResource(ID3D12Resource* pResource) {
  if (!pResource) return;
  std::lock_guard<std::mutex> lock(m_mutex);
  m_tracked.insert(pResource);
}

void ResourceLifetimeTracker::UntrackResource(ID3D12Resource* pResource) {
  if (!pResource) return;
  std::lock_guard<std::mutex> lock(m_mutex);
  m_tracked.erase(pResource);
}

bool ResourceLifetimeTracker::IsTracked(ID3D12Resource* pResource) const {
  if (!pResource) return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_tracked.count(pResource) > 0;
}

void ResourceLifetimeTracker::NotifyReleased(ID3D12Resource* pResource) {
  if (!pResource) return;

  bool wasTracked = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    wasTracked = m_tracked.erase(pResource) > 0;
  }

  if (wasTracked) {
    // Notify ResourceDetector to purge this pointer from all candidate lists.
    // This is the core of Fix 1: deterministic, zero-polling, 100% safe removal
    // instead of the old SEH-based AddRef/Release probing.
    ResourceDetector::Get().OnResourceDestroyed(pResource);

    static uint64_t s_notifyCount = 0;
    if (s_notifyCount++ % 500 == 0) {
      LOG_DEBUG("[Lifetime] Notified resource destruction ({} total notifications)", s_notifyCount);
    }
  }
}

size_t ResourceLifetimeTracker::GetTrackedCount() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_tracked.size();
}
