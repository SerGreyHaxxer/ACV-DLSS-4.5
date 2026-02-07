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

#include <cmath>
#include <mutex>

namespace {

// Lock hierarchy level 3
std::mutex g_jitterMutex;

JitterResult lastResult{};
float emaX = 0.0f;
float emaY = 0.0f;
float prevProj8 = 0.0f;
float prevProj9 = 0.0f;
bool hasPrevProj = false;
int zeroCount = 0;
int framesSinceValid = 0;

constexpr float kEmaAlpha = 0.3f;
constexpr float kOutlierThreshold = 0.5f;
constexpr int kZeroFrameLimit = 5;

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

// Apply EMA outlier rejection and update smoothing state
JitterResult ApplySmoothing(float rawX, float rawY, JitterSource source) {
    // Outlier rejection against EMA
    if (framesSinceValid > 0) {
        float devX = std::abs(rawX - emaX);
        float devY = std::abs(rawY - emaY);
        if (devX > kOutlierThreshold || devY > kOutlierThreshold) {
            LOG_WARN("JitterEngine: outlier rejected ({:.4f},{:.4f}), using EMA ({:.4f},{:.4f})",
                     rawX, rawY, emaX, emaY);
            rawX = emaX;
            rawY = emaY;
        }
    }

    // Update EMA
    if (framesSinceValid == 0) {
        emaX = rawX;
        emaY = rawY;
    } else {
        emaX = kEmaAlpha * rawX + (1.0f - kEmaAlpha) * emaX;
        emaY = kEmaAlpha * rawY + (1.0f - kEmaAlpha) * emaY;
    }

    framesSinceValid++;
    zeroCount = 0;

    JitterResult r;
    r.x = rawX;
    r.y = rawY;
    r.source = source;
    r.valid = true;
    lastResult = r;
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
    std::lock_guard<std::mutex> lock(g_jitterMutex);

    // --- Tier 1: PatternScan ---
    if (std::isfinite(patternX) && std::isfinite(patternY) &&
        IsNonZero(patternX, patternY)) {
        if (Validate(patternX, patternY)) {
            return ApplySmoothing(patternX, patternY, JitterSource::PatternScan);
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
                return ApplySmoothing(jx, jy, JitterSource::CbvExtraction);
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
                    return ApplySmoothing(jx, jy, JitterSource::MatrixDiff);
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

    JitterResult stale = lastResult;
    stale.valid = false;
    return stale;
}

JitterResult JitterEngine_GetLast() {
    std::lock_guard<std::mutex> lock(g_jitterMutex);
    return lastResult;
}

void JitterEngine_Reset() {
    std::lock_guard<std::mutex> lock(g_jitterMutex);
    lastResult = {};
    emaX = 0.0f;
    emaY = 0.0f;
    prevProj8 = 0.0f;
    prevProj9 = 0.0f;
    hasPrevProj = false;
    zeroCount = 0;
    framesSinceValid = 0;
    LOG_INFO("JitterEngine: state reset");
}
