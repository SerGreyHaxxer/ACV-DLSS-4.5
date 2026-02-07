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
#include "resource_state_tracker.h"
#include "resource_detector.h"

#include <mutex>
#include <unordered_map>

namespace {

struct StateEntry {
    D3D12_RESOURCE_STATES state;
    uint64_t lastFrame;
};

// Lock hierarchy level 3 (SwapChain=1 > Hooks=2 > Resources=3 > Config=4 > Logging=5).
std::mutex g_stateTrackerMutex;
std::unordered_map<ID3D12Resource*, StateEntry> g_stateMap;

constexpr size_t kMaxEntries = 4096;
constexpr uint64_t kEvictAgeHard = 60;
constexpr uint64_t kEvictAgeSoft = 10;
constexpr size_t kSoftCap = 3072;

void EvictOlderThan(uint64_t currentFrame, uint64_t maxAge) {
    uint64_t cutoff = (currentFrame > maxAge) ? (currentFrame - maxAge) : 0;
    for (auto it = g_stateMap.begin(); it != g_stateMap.end(); ) {
        if (it->second.lastFrame < cutoff)
            it = g_stateMap.erase(it);
        else
            ++it;
    }
}

} // namespace

void ResourceStateTracker_RecordTransition(ID3D12Resource* pResource,
                                           D3D12_RESOURCE_STATES /*stateBefore*/,
                                           D3D12_RESOURCE_STATES stateAfter) {
    if (!pResource)
        return;

    uint64_t frame = ResourceDetector::Get().GetFrameCount();
    std::lock_guard<std::mutex> lock(g_stateTrackerMutex);

    g_stateMap[pResource] = { stateAfter, frame };

    // Cap enforcement
    if (g_stateMap.size() > kMaxEntries) {
        EvictOlderThan(frame, kEvictAgeHard);
        if (g_stateMap.size() > kSoftCap) {
            EvictOlderThan(frame, kEvictAgeSoft);
        }
    }
}

bool ResourceStateTracker_GetCurrentState(ID3D12Resource* pResource,
                                          D3D12_RESOURCE_STATES& outState) {
    if (!pResource)
        return false;

    std::lock_guard<std::mutex> lock(g_stateTrackerMutex);
    auto it = g_stateMap.find(pResource);
    if (it == g_stateMap.end())
        return false;

    outState = it->second.state;
    return true;
}

void ResourceStateTracker_EvictStale(uint64_t currentFrame, uint64_t maxAge) {
    std::lock_guard<std::mutex> lock(g_stateTrackerMutex);
    EvictOlderThan(currentFrame, maxAge);
}

void ResourceStateTracker_Clear() {
    std::lock_guard<std::mutex> lock(g_stateTrackerMutex);
    g_stateMap.clear();
}
