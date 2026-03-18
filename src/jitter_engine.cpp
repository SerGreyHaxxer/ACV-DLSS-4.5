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
#include "jitter_engine.h"
#include "logger.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <array>
#include <algorithm>

namespace {

// Lock hierarchy level 3
std::mutex g_jitterMutex;

JitterResult lastResultLocal{};  // Used only within mutex-protected Update path
std::atomic<JitterResult> g_lastResult{};  // Lock-free query target
float prevProj8 = 0.0f;
float prevProj9 = 0.0f;
bool hasPrevProj = false;
int zeroCount = 0;
int framesSinceValid = 0;

// Previous valid jitter for outlier detection (replaces EMA)
float prevValidX = 0.0f;
float prevValidY = 0.0f;

constexpr float kOutlierThreshold = 0.5f;
constexpr int kZeroFrameLimit = 5;

// ============================================================================
// Halton/Weyl Jitter Sequence Tables
// ============================================================================
// DLSS 4.5 (Ray Reconstruction, Frame Gen) requires exact sub-pixel offsets.
// Games use deterministic sequences — Halton base-2,3 is the most common.
// EMA smoothing was DESTROYING phase alignment by averaging across sequence
// positions, causing ghosting / smearing / shimmering.
//
// Instead: if the raw jitter lies within ε of a known grid point, snap to
// the exact grid value.  If no match, pass through raw (custom sequence).
// ============================================================================

// Snap tolerance: if raw jitter is within this distance of a grid point, snap
constexpr float kSnapEpsilon = 0.02f;

// Halton base-2 (first 16 terms, centered at 0): x-axis jitter
constexpr std::array<float, 16> kHalton2 = {
    -0.5f,      0.0f,      -0.25f,     0.25f,
    -0.375f,   -0.125f,     0.125f,    0.375f,
    -0.4375f,  -0.1875f,    0.0625f,   0.3125f,
    -0.3125f,  -0.0625f,    0.1875f,   0.4375f,
};

// Halton base-3 (first 16 terms, centered at 0): y-axis jitter
constexpr std::array<float, 16> kHalton3 = {
    -0.16667f,  0.16667f,  -0.38889f, -0.05556f,
     0.27778f, -0.46296f,  -0.12963f,  0.20370f,
    -0.35185f, -0.01852f,   0.31481f, -0.42593f,
    -0.09259f,  0.24074f,  -0.31481f,  0.01852f,
};

// Try to snap a value to the nearest point in a Halton sequence.
// Returns the snapped value if within ε, or the original value unchanged.
constexpr float TrySnapToGrid(float raw, const auto& grid) {
    float bestDist = kSnapEpsilon;
    float bestVal = raw;  // Default: pass through unchanged
    for (float gridVal : grid) {
        float dist = std::abs(raw - gridVal);
        if (dist < bestDist) {
            bestDist = dist;
            bestVal = gridVal;
        }
    }
    return bestVal;
}

bool IsFiniteAndSubPixel(float x, float y) {
    return std::isfinite(x) && std::isfinite(y) &&
           std::abs(x) < 1.0f && std::abs(y) < 1.0f;
}

bool IsNonZero(float x, float y) {
    return x != 0.0f || y != 0.0f;
}

// Validate a jitter candidate: finite, sub-pixel, and at least one non-zero
bool Validate(float x, float y) {
    return IsFiniteAndSubPixel(x, y) && IsNonZero(x, y);
}

// Apply sequence snapping and outlier rejection.
// NEVER smooths — raw jitter is passed through (possibly snapped to grid).
JitterResult ApplySequenceSnapping(float rawX, float rawY, JitterSource source) {
    // Outlier rejection: reject values that deviate too far from BOTH
    // the nearest grid point AND the previous frame's jitter.
    // This catches corrupt values without smoothing valid ones.
    if (framesSinceValid > 0) {
        float devX = std::abs(rawX - prevValidX);
        float devY = std::abs(rawY - prevValidY);

        // Check if the value is close to any Halton grid point
        float snappedX = TrySnapToGrid(rawX, kHalton2);
        float snappedY = TrySnapToGrid(rawY, kHalton3);
        bool onGrid = (snappedX != rawX) || (snappedY != rawY);

        if (!onGrid && (devX > kOutlierThreshold || devY > kOutlierThreshold)) {
            LOG_WARN("JitterEngine: outlier rejected ({:.4f},{:.4f}), using previous ({:.4f},{:.4f})",
                     rawX, rawY, prevValidX, prevValidY);
            rawX = prevValidX;
            rawY = prevValidY;
        } else {
            // Snap to grid if close to a known sequence point
            rawX = snappedX;
            rawY = snappedY;
        }
    } else {
        // First valid frame: snap if possible
        rawX = TrySnapToGrid(rawX, kHalton2);
        rawY = TrySnapToGrid(rawY, kHalton3);
    }

    prevValidX = rawX;
    prevValidY = rawY;
    framesSinceValid++;
    zeroCount = 0;

    JitterResult r;
    r.x = rawX;
    r.y = rawY;
    r.source = source;
    r.valid = true;
    lastResultLocal = r;
    g_lastResult.store(r, std::memory_order_release);
    return r;
}

} // anonymous namespace

const char* JitterSource_Name(JitterSource src) {
    switch (src) {
        case JitterSource::None:         return "None";
        case JitterSource::PatternScan:  return "PatternScan";
        case JitterSource::CbvExtraction:return "CbvExtraction";
        case JitterSource::MatrixDiff:   return "MatrixDiff";
        default:                         return "Unknown";
    }
}

JitterResult JitterEngine_Update(float patternX, float patternY,
                                  const float* proj) {
    std::scoped_lock lock(g_jitterMutex);

    // --- Tier 1: PatternScan ---
    if (std::isfinite(patternX) && std::isfinite(patternY) &&
        IsNonZero(patternX, patternY)) {
        if (Validate(patternX, patternY)) {
            return ApplySequenceSnapping(patternX, patternY, JitterSource::PatternScan);
        }
    }

    // --- Tier 2: CbvExtraction ---
    if (proj) {
        float p0 = proj[0];  // proj[0][0]
        float p5 = proj[5];  // proj[1][1]
        float p8 = proj[8];  // proj[2][0]
        float p9 = proj[9];  // proj[2][1]

        if (p0 != 0.0f && p5 != 0.0f) {
            float jx = p8 / p0;
            float jy = p9 / p5;
            if (Validate(jx, jy)) {
                // Update prev-frame state for Tier 3
                prevProj8 = p8;
                prevProj9 = p9;
                hasPrevProj = true;
                return ApplySequenceSnapping(jx, jy, JitterSource::CbvExtraction);
            }
        }

        // --- Tier 3: MatrixDiff ---
        if (hasPrevProj) {
            float p0t = proj[0];
            float p5t = proj[5];
            if (p0t != 0.0f && p5t != 0.0f) {
                float jx = (proj[8] - prevProj8) / p0t;
                float jy = (proj[9] - prevProj9) / p5t;
                // Update prev-frame state
                prevProj8 = proj[8];
                prevProj9 = proj[9];
                if (Validate(jx, jy)) {
                    return ApplySequenceSnapping(jx, jy, JitterSource::MatrixDiff);
                }
            }
        }

        // Store for next frame even if tiers 2/3 didn't validate
        prevProj8 = proj[8];
        prevProj9 = proj[9];
        hasPrevProj = true;
    }

    // --- No valid jitter this frame ---
    zeroCount++;
    if (zeroCount == kZeroFrameLimit + 1) {
        LOG_INFO("JitterEngine: jitter (0,0) for >{} consecutive frames, likely menu/cutscene",
                 kZeroFrameLimit);
    }

    JitterResult stale = lastResultLocal;
    stale.valid = false;
    return stale;
}

JitterResult JitterEngine_GetLast() {
    return g_lastResult.load(std::memory_order_acquire);  // Zero mutex overhead!
}

void JitterEngine_Reset() {
    std::scoped_lock lock(g_jitterMutex);
    lastResultLocal = {};
    g_lastResult.store({}, std::memory_order_release);
    prevValidX = 0.0f;
    prevValidY = 0.0f;
    prevProj8 = 0.0f;
    prevProj9 = 0.0f;
    hasPrevProj = false;
    zeroCount = 0;
    framesSinceValid = 0;
    LOG_INFO("JitterEngine: state reset");
}
