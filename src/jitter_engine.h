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
#include <cstdint>

enum class JitterSource : uint8_t {
    None = 0,
    PatternScan,      // Tier 1: fastest, game-specific memory pattern
    CbvExtraction,    // Tier 2: extracted from projection matrix CBV
    MatrixDiff,       // Tier 3: frame-to-frame projection matrix delta
};

struct JitterResult {
    float x = 0.0f;
    float y = 0.0f;
    JitterSource source = JitterSource::None;
    bool valid = false;
};

// Call once per frame from hooks.cpp GhostCB_Close or similar
// patternX/Y: jitter from pattern scan (Tier 1). Pass NaN if unavailable.
// proj: current frame's 4x4 projection matrix (row-major). Pass nullptr if unavailable.
JitterResult JitterEngine_Update(float patternX, float patternY,
                                  const float* proj);

// Query last valid jitter
JitterResult JitterEngine_GetLast();

// Get source name for logging
const char* JitterSource_Name(JitterSource src);

// Reset state (e.g., on resolution change)
void JitterEngine_Reset();
