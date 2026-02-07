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
#include "camera_scanner.h"

#include "dlss4_config.h"
#include "logger.h"
#include "streamline_integration.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <immintrin.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
enum class ScanMethod : int { None = 0, Cached, FullScan, Descriptor, Root };
inline const char* ScanMethodName(ScanMethod m) {
  switch (m) {
    case ScanMethod::Cached: return "Cached";
    case ScanMethod::FullScan: return "FullScan";
    case ScanMethod::Descriptor: return "Descriptor";
    case ScanMethod::Root: return "Root";
    default: return "None";
  }
}

struct CameraCandidate {
  float view[16];
  float proj[16];
  float jitterX = 0.0f;
  float jitterY = 0.0f;
  float score = 0.0f;
  uint64_t frame = 0;
  bool valid = false;
  ScanMethod method = ScanMethod::None;
};

// Lock hierarchy level 3 â€” same tier as Resources
// (SwapChain=1 > Hooks=2 > Resources/Camera=3 > Config=4 > Logging=5).
std::mutex g_cameraMutex;
CameraCandidate g_bestCamera;
std::atomic<bool> g_loggedCamera(false);

struct UploadCbvInfo {
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  D3D12_GPU_VIRTUAL_ADDRESS gpuBase = 0;
  uint64_t size = 0;
  uint8_t* cpuPtr = nullptr;
};

// Lock hierarchy level 3 â€” same tier as Resources
std::mutex g_cbvMutex;
std::vector<UploadCbvInfo> g_cbvInfos;
std::atomic<uint64_t> g_cameraFrame(0);
std::atomic<uint64_t> g_lastFullScanFrame(0);
std::atomic<uint64_t> g_lastCameraFoundFrame(0);

// CBV descriptor address tracking (separate from descriptor resource tracking)
// Lock hierarchy level 3 â€” same tier as Resources.  Never acquire while
// holding g_cbvMutex or g_cameraMutex at the same level.
std::mutex g_cbvAddrMutex;
struct CbvGpuAddrEntry {
  D3D12_GPU_VIRTUAL_ADDRESS addr = 0;
  uint64_t lastFrame = 0;
};
std::unordered_map<uintptr_t, CbvGpuAddrEntry> g_cbvGpuAddrs;
std::vector<D3D12_GPU_VIRTUAL_ADDRESS> g_rootCbvAddrs;
std::atomic<uint64_t> g_cbvDescriptorCount(0);
std::atomic<uint64_t> g_cbvGpuAddrCount(0);

bool IsFinite(float v) {
  return v == v && v > -FLT_MAX && v < FLT_MAX;
}
bool LooksLikeMatrix(const float* m) {
  for (int i = 0; i < 16; ++i) {
    if (!IsFinite(m[i])) return false;
  }
  return true;
}

static void TransposeMatrix(const float* in, float* out) {
  out[0] = in[0];
  out[1] = in[4];
  out[2] = in[8];
  out[3] = in[12];
  out[4] = in[1];
  out[5] = in[5];
  out[6] = in[9];
  out[7] = in[13];
  out[8] = in[2];
  out[9] = in[6];
  out[10] = in[10];
  out[11] = in[14];
  out[12] = in[3];
  out[13] = in[7];
  out[14] = in[11];
  out[15] = in[15];
}

static float Dot3(const float* a, const float* b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void GetRow3(const float* m, int row, float* out) {
  out[0] = m[row * 4 + 0];
  out[1] = m[row * 4 + 1];
  out[2] = m[row * 4 + 2];
}
static float Length3(const float* v) {
  return sqrtf(Dot3(v, v));
}

float ScoreMatrixPair(const float* view, const float* proj) {
  float score = 0.0f;
  if (!LooksLikeMatrix(view) || !LooksLikeMatrix(proj)) return 0.0f;

  // view[15] should be 1.0 for an affine view matrix
  if (fabsf(view[15] - 1.0f) > 0.1f) return 0.0f;
  if (fabsf(view[15] - 1.0f) < 0.01f) score += 0.2f;

  // Perspective projection detection
  bool isStrongPerspective = fabsf(proj[15]) < 0.01f && fabsf(fabsf(proj[11]) - 1.0f) < 0.1f;
  bool isWeakPerspective = fabsf(proj[15]) < 0.8f && fabsf(proj[11]) > 0.2f;

  if (isStrongPerspective)
    score += 0.6f;
  else if (isWeakPerspective)
    score += 0.3f;
  else
    return 0.0f; // Reject ortho/identity — not a camera projection

  // FoV validation: proj[0] and proj[5] encode focal lengths
  // Reasonable FoV range: ~30° to 120° → proj[5] ∈ [0.577, 3.73]
  if (fabsf(proj[0]) > 0.3f && fabsf(proj[0]) < 5.0f && fabsf(proj[5]) > 0.3f && fabsf(proj[5]) < 5.0f) {
    score += 0.15f;
    // Bonus for typical game FoV (60°-90°) → proj[5] ∈ [1.0, 1.73]
    if (fabsf(proj[5]) > 0.8f && fabsf(proj[5]) < 2.2f) score += 0.05f;
  }

  // Affine view matrix: last column should be [0, 0, 0, 1]
  if (fabsf(view[3]) < 1.0f && fabsf(view[7]) < 1.0f && fabsf(view[11]) < 1.0f) score += 0.1f;
  // Translation vector within reasonable game-world range
  if (fabsf(view[12]) < camera_config::kPosTolerance && fabsf(view[13]) < camera_config::kPosTolerance &&
      fabsf(view[14]) < camera_config::kPosTolerance)
    score += 0.1f;

  // Orthogonality check for rotation component
  float r0[3], r1[3], r2[3];
  GetRow3(view, 0, r0);
  GetRow3(view, 1, r1);
  GetRow3(view, 2, r2);
  float len0 = Length3(r0);
  float len1 = Length3(r1);
  float len2 = Length3(r2);
  if (len0 > 0.1f && len1 > 0.1f && len2 > 0.1f) {
    float orthoScore = 0.0f;
    float d01 = fabsf(Dot3(r0, r1) / (len0 * len1));
    float d02 = fabsf(Dot3(r0, r2) / (len0 * len2));
    float d12 = fabsf(Dot3(r1, r2) / (len1 * len2));
    if (d01 < 0.2f) orthoScore += 0.1f;
    if (d02 < 0.2f) orthoScore += 0.1f;
    if (d12 < 0.2f) orthoScore += 0.1f;
    score += orthoScore;
    // Bonus: rows should be unit-length for a proper rotation matrix
    if (fabsf(len0 - 1.0f) < 0.15f && fabsf(len1 - 1.0f) < 0.15f && fabsf(len2 - 1.0f) < 0.15f) {
      score += 0.1f;
    }
  }

  return score;
}

bool TryExtractCameraFromBuffer(const uint8_t* data, size_t size, float* outView, float* outProj, float* outScore,
                                size_t* outOffset) {
  if (!data || size < camera_config::kCbvMinSize) return false;
  float bestScore = 0.0f;
  size_t bestOffset = 0;
  auto scanWithStride = [&](size_t stride, float& bestScoreOut, size_t& bestOffsetOut) {
    const size_t scanLimit = size;
    const size_t matrixBytes = sizeof(float) * 32;
#if defined(__AVX512F__)
    const size_t laneCount = 16;
    const size_t blockSpan = stride * (laneCount - 1) + matrixBytes;
    const char* base = reinterpret_cast<const char*>(data);
    const __m512 vOne = _mm512_set1_ps(1.0f);
    const __m512 vTol = _mm512_set1_ps(0.1f);
    size_t offset = 0;
    for (; offset + blockSpan <= scanLimit; offset += stride * laneCount) {
      alignas(64) int indices[laneCount];
      for (int lane = 0; lane < static_cast<int>(laneCount); ++lane) {
        indices[lane] = static_cast<int>(offset + static_cast<size_t>(lane) * stride + 15 * sizeof(float));
      }
      __m512 view15 = _mm512_i32gather_ps(_mm512_loadu_si512(indices), base, 1);
      __m512 diff = _mm512_sub_ps(view15, vOne);
      __m512 absDiff = _mm512_max_ps(diff, _mm512_sub_ps(_mm512_setzero_ps(), diff));
      __mmask16 mask = _mm512_cmp_ps_mask(absDiff, vTol, _CMP_LE_OQ);
      if (!mask) continue;
      for (int lane = 0; lane < static_cast<int>(laneCount); ++lane) {
        if ((mask & (1u << lane)) == 0) continue;
        size_t candidateOffset = offset + static_cast<size_t>(lane) * stride;
        const float* view = reinterpret_cast<const float*>(data + candidateOffset);
        const float* proj = view + 16;
        float score = ScoreMatrixPair(view, proj);
        if (score > bestScoreOut) {
          bestScoreOut = score;
          bestOffsetOut = candidateOffset;
        }

        float tView[16], tProj[16];
        TransposeMatrix(view, tView);
        TransposeMatrix(proj, tProj);
        float tScore = ScoreMatrixPair(tView, tProj);
        if (tScore > bestScoreOut) {
          bestScoreOut = tScore;
          bestOffsetOut = candidateOffset;
        }
      }
    }
    for (; offset + matrixBytes <= scanLimit; offset += stride) {
      const float* view = reinterpret_cast<const float*>(data + offset);
      const float* proj = view + 16;
      float score = ScoreMatrixPair(view, proj);
      if (score > bestScoreOut) {
        bestScoreOut = score;
        bestOffsetOut = offset;
      }

      float tView[16], tProj[16];
      TransposeMatrix(view, tView);
      TransposeMatrix(proj, tProj);
      float tScore = ScoreMatrixPair(tView, tProj);
      if (tScore > bestScoreOut) {
        bestScoreOut = tScore;
        bestOffsetOut = offset;
      }
    }
#else
    for (size_t offset = 0; offset + matrixBytes <= scanLimit; offset += stride) {
      const float* view = reinterpret_cast<const float*>(data + offset);
      const float* proj = view + 16;
      float score = ScoreMatrixPair(view, proj);
      if (score > bestScoreOut) {
        bestScoreOut = score;
        bestOffsetOut = offset;
      }

      float tView[16], tProj[16];
      TransposeMatrix(view, tView);
      TransposeMatrix(proj, tProj);
      float tScore = ScoreMatrixPair(tView, tProj);
      if (tScore > bestScoreOut) {
        bestScoreOut = tScore;
        bestOffsetOut = offset;
      }
    }
#endif
  };

  // Multi-stride scanning: coarse to fine, carrying best score forward
  // (don't reset bestScore between passes — coarse hits are valid)
  scanWithStride(256, bestScore, bestOffset);
  if (bestScore < 0.6f) {
    scanWithStride(camera_config::kScanMedStride, bestScore, bestOffset);
  }
  if (bestScore < 0.6f) {
    scanWithStride(64, bestScore, bestOffset);
  }
  if (bestScore < 0.6f) {
    scanWithStride(camera_config::kScanFineStride, bestScore, bestOffset);
  }
  if (bestScore < 0.6f) return false;
  const float* view = reinterpret_cast<const float*>(data + bestOffset);
  const float* proj = view + 16;
  float score = ScoreMatrixPair(view, proj);
  float tView[16], tProj[16];
  TransposeMatrix(view, tView);
  TransposeMatrix(proj, tProj);
  float tScore = ScoreMatrixPair(tView, tProj);
  if (tScore > score) {
    std::copy_n(tView, 16, outView);
    std::copy_n(tProj, 16, outProj);
    if (outScore) *outScore = tScore;
  } else {
    std::copy_n(view, 16, outView);
    std::copy_n(proj, 16, outProj);
    if (outScore) *outScore = score;
  }
  if (outOffset) *outOffset = bestOffset;
  return true;
}

bool TryGetCbvData(D3D12_GPU_VIRTUAL_ADDRESS gpuAddress, const uint8_t** outData, size_t* outSize) {
  std::lock_guard<std::mutex> lock(g_cbvMutex);
  for (const auto& info : g_cbvInfos) {
    if (!info.cpuPtr || info.gpuBase == 0 || info.size == 0) continue;
    if (gpuAddress >= info.gpuBase && gpuAddress < info.gpuBase + info.size) {
      size_t offset = static_cast<size_t>(gpuAddress - info.gpuBase);
      if (offset >= info.size) return false;
      *outData = info.cpuPtr + offset;
      *outSize = static_cast<size_t>(info.size - offset);
      return true;
    }
  }
  return false;
}

void UpdateBestCamera(const float* view, const float* proj, float jitterX, float jitterY,
                      ScanMethod method = ScanMethod::None) {
  float score = ScoreMatrixPair(view, proj);
  if (score < 0.6f) return;
  std::lock_guard<std::mutex> lock(g_cameraMutex);
  // Stability bonus: if the new camera is very similar to the last, boost its score
  float stabilityBonus = 0.0f;
  if (g_bestCamera.valid) {
    float deltaSum = 0.0f;
    for (int i = 0; i < 16; ++i) {
      deltaSum += fabsf(g_bestCamera.view[i] - view[i]);
      deltaSum += fabsf(g_bestCamera.proj[i] - proj[i]);
    }
    if (deltaSum < 0.2f)
      stabilityBonus = 0.2f;
    else if (deltaSum < 1.0f)
      stabilityBonus = 0.1f;
  }
  score += stabilityBonus;
  // Only update if the new candidate is at least as good as the current best
  // (prevents flickering to a worse candidate)
  if (g_bestCamera.valid && score < g_bestCamera.score - 0.1f) return;
  g_bestCamera.score = score;
  std::copy_n(view, 16, g_bestCamera.view);
  std::copy_n(proj, 16, g_bestCamera.proj);
  g_bestCamera.jitterX = jitterX;
  g_bestCamera.jitterY = jitterY;
  g_bestCamera.frame = ++g_cameraFrame;
  g_bestCamera.valid = true;
  g_bestCamera.method = method;
  if (!g_loggedCamera.exchange(true)) {
    LOG_INFO("Camera matrices detected (score {:.2f}, method: {})", score, ScanMethodName(method));
  }
}

static D3D12_GPU_VIRTUAL_ADDRESS s_lastCameraCbv = 0;
static size_t s_lastCameraOffset = 0;
} // namespace

// ============================================================================
// PUBLIC API
// ============================================================================

void UpdateCameraCache(const float* view, const float* proj, float jitterX, float jitterY) {
  if (!view || !proj) return;
  UpdateBestCamera(view, proj, jitterX, jitterY);
}

bool GetLastCameraStats(float& outScore, uint64_t& outFrame) {
  std::lock_guard<std::mutex> lock(g_cameraMutex);
  if (!g_bestCamera.valid) return false;
  outScore = g_bestCamera.score;
  outFrame = g_bestCamera.frame;
  return true;
}

void TrackCbvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc) {
  if (!handle.ptr || !desc || desc->BufferLocation == 0) return;
  std::lock_guard<std::mutex> lock(g_cbvAddrMutex);
  g_cbvGpuAddrs[handle.ptr] = {desc->BufferLocation, StreamlineIntegration::Get().GetFrameCount()};
  g_cbvDescriptorCount++;
}

void TrackRootCbvAddress(D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (!address) return;
  std::lock_guard<std::mutex> lock(g_cbvAddrMutex);
  auto it = std::find(g_rootCbvAddrs.begin(), g_rootCbvAddrs.end(), address);
  if (it != g_rootCbvAddrs.end()) g_rootCbvAddrs.erase(it);
  g_rootCbvAddrs.push_back(address);
  const size_t maxKeep = camera_config::kDescriptorScanMax * camera_config::kScanExtendedMultiplier;
  if (g_rootCbvAddrs.size() > maxKeep) {
    g_rootCbvAddrs.erase(g_rootCbvAddrs.begin(), g_rootCbvAddrs.begin() + (g_rootCbvAddrs.size() - maxKeep));
  }
  g_cbvGpuAddrCount++;
}

void GetCameraScanCounts(uint64_t& cbvCount, uint64_t& descCount, uint64_t& rootCount) {
  cbvCount = g_cbvInfos.size();
  {
    std::lock_guard<std::mutex> lock(g_cbvAddrMutex);
    descCount = g_cbvGpuAddrs.size();
    rootCount = g_rootCbvAddrs.size();
  }
}

void RegisterCbv(ID3D12Resource* pResource, UINT64 size, uint8_t* cpuPtr) {
  std::lock_guard<std::mutex> lock(g_cbvMutex);
  UploadCbvInfo info{};
  info.resource = pResource;
  info.gpuBase = pResource->GetGPUVirtualAddress();
  info.size = size;
  info.cpuPtr = cpuPtr;
  g_cbvInfos.push_back(info);
  const size_t maxCbvs = camera_config::kScanMaxCbvsPerFrame * camera_config::kScanExtendedMultiplier * 8;
  if (g_cbvInfos.size() > maxCbvs) {
    g_cbvInfos.erase(g_cbvInfos.begin(), g_cbvInfos.begin() + (g_cbvInfos.size() - maxCbvs));
  }
}

void ResetCameraScanCache() {
  std::lock_guard<std::mutex> lock(g_cbvMutex);
  g_cbvInfos.clear();
  s_lastCameraCbv = 0;
  s_lastCameraOffset = 0;
  g_lastFullScanFrame.store(0);
  g_lastCameraFoundFrame.store(0);
  g_loggedCamera.store(false);
  {
    std::lock_guard<std::mutex> dlock(g_cbvAddrMutex);
    g_cbvGpuAddrs.clear();
    g_rootCbvAddrs.clear();
  }
  g_cbvDescriptorCount.store(0);
  g_cbvGpuAddrCount.store(0);
}

uint64_t GetLastCameraFoundFrame() {
  return g_lastCameraFoundFrame.load();
}

uint64_t GetLastFullScanFrame() {
  return g_lastFullScanFrame.load();
}

bool TryScanAllCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates, bool allowFullScan) {
  std::lock_guard<std::mutex> lock(g_cbvMutex);

  auto it = std::remove_if(g_cbvInfos.begin(), g_cbvInfos.end(), [](const UploadCbvInfo& info) {
    if (!info.cpuPtr) return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(info.cpuPtr, &mbi, sizeof(mbi)) == 0) return true;
    if (mbi.State != MEM_COMMIT) return true;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return true;
    return false;
  });
  if (it != g_cbvInfos.end()) g_cbvInfos.erase(it, g_cbvInfos.end());

  // Fast path - check known location first
  if (s_lastCameraCbv != 0) {
    for (const auto& info : g_cbvInfos) {
      if (info.gpuBase == s_lastCameraCbv) {
        if (s_lastCameraOffset + sizeof(float) * 32 <= info.size) {
          const float* view = reinterpret_cast<const float*>(info.cpuPtr + s_lastCameraOffset);
          const float* proj = view + 16;
          float score = ScoreMatrixPair(view, proj);
          float bestScore = score;
          bool useTranspose = false;
          float tView[16], tProj[16];
          TransposeMatrix(view, tView);
          TransposeMatrix(proj, tProj);
          float tScore = ScoreMatrixPair(tView, tProj);
          if (tScore > bestScore) {
            bestScore = tScore;
            useTranspose = true;
          }
          if (bestScore > 0.6f) {
            if (useTranspose) {
              std::copy_n(tView, 16, outView);
              std::copy_n(tProj, 16, outProj);
            } else {
              std::copy_n(view, 16, outView);
              std::copy_n(proj, 16, outProj);
            }
            if (outScore) *outScore = bestScore;
            g_lastCameraFoundFrame.store(StreamlineIntegration::Get().GetFrameCount());
            return true;
          }
        }

        float tempView[16], tempProj[16], score = 0.0f;
        size_t newOffset = 0;
        if (TryExtractCameraFromBuffer(info.cpuPtr, static_cast<size_t>(info.size), tempView, tempProj, &score,
                                       &newOffset)) {
          s_lastCameraOffset = newOffset;
          std::copy_n(tempView, 16, outView);
          std::copy_n(tempProj, 16, outProj);
          if (outScore) *outScore = score;
          g_lastCameraFoundFrame.store(StreamlineIntegration::Get().GetFrameCount());
          return true;
        }
      }
    }
  }

  float bestScore = 0.0f;
  bool found = false;
  D3D12_GPU_VIRTUAL_ADDRESS foundGpuBase = 0;

  if (g_cbvInfos.empty() && logCandidates) {
    LOG_INFO("[CAM] No CBVs registered! Check RegisterCbv hooks.");
    return false;
  }

  if (!allowFullScan) {
    return false;
  }
  g_lastFullScanFrame.store(StreamlineIntegration::Get().GetFrameCount());

  uint32_t scanned = 0;
  const uint32_t maxScan = camera_config::kScanMaxCbvsPerFrame * camera_config::kScanExtendedMultiplier;
  for (const auto& info : g_cbvInfos) {
    if (!info.cpuPtr || info.size < camera_config::kCbvMinSize) continue;
    if (scanned++ >= maxScan) break;

    float tempView[16], tempProj[16], score = 0.0f;
    size_t foundOffset = 0;
    if (TryExtractCameraFromBuffer(info.cpuPtr, static_cast<size_t>(info.size), tempView, tempProj, &score,
                                   &foundOffset)) {
      if (logCandidates && score > 0.0f) {
        LOG_INFO("[CAM] Candidate GPU:0x{:x} Size:{} Score:{:.2f} View[15]:{:.2f} Proj[15]:{:.2f} Proj[11]:{:.2f}",
                 info.gpuBase, info.size, score, tempView[15], tempProj[15], tempProj[11]);
      }
      if (score > bestScore) {
        bestScore = score;
        std::copy_n(tempView, 16, outView);
        std::copy_n(tempProj, 16, outProj);
        foundGpuBase = info.gpuBase;
        s_lastCameraOffset = foundOffset;
        found = true;
      }
    }
  }

  if (found) {
    s_lastCameraCbv = foundGpuBase;
    if (outScore) *outScore = bestScore;
    g_lastCameraFoundFrame.store(StreamlineIntegration::Get().GetFrameCount());
    LOG_INFO("Camera matrices detected (Score: {:.2f}) at GPU: 0x{:x} Offset: +0x{:X}", bestScore, foundGpuBase,
             s_lastCameraOffset);
  } else if (logCandidates) {
    LOG_INFO("[CAM] Scan failed. Checked {} CBVs. Best Score: {:.2f}", (uint64_t)g_cbvInfos.size(), bestScore);
  }

  return found;
}

bool TryScanDescriptorCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates) {
  std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, uint64_t>> addrs;
  {
    std::lock_guard<std::mutex> lock(g_cbvAddrMutex);
    addrs.reserve(g_cbvGpuAddrs.size());
    for (const auto& entry : g_cbvGpuAddrs) {
      addrs.push_back({entry.second.addr, entry.second.lastFrame});
    }
  }
  if (addrs.empty()) {
    if (logCandidates) {
      LOG_INFO("[CAM] No CBV descriptors captured (CBV descriptors: {}, GPU addr hits: {}).",
               (unsigned long long)g_cbvDescriptorCount.load(), (unsigned long long)g_cbvGpuAddrCount.load());
    }
    return false;
  }
  float bestScore = 0.0f;
  bool found = false;
  uint64_t currentFrame = StreamlineIntegration::Get().GetFrameCount();
  uint64_t lastFound = g_lastCameraFoundFrame.load();
  bool stale = lastFound == 0 || (currentFrame > lastFound + camera_config::kScanStaleFrames);
  uint32_t maxScan = stale ? (camera_config::kDescriptorScanMax * camera_config::kScanExtendedMultiplier)
                           : camera_config::kDescriptorScanMax;
  std::sort(addrs.begin(), addrs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::unordered_set<D3D12_GPU_VIRTUAL_ADDRESS> seen;
  uint32_t scanned = 0;
  for (const auto& entry : addrs) {
    if (scanned >= maxScan) break;
    if (!seen.insert(entry.first).second) continue;
    scanned++;
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!TryGetCbvData(entry.first, &data, &size)) continue;
    float tempView[16], tempProj[16], score = 0.0f;
    size_t offset = 0;
    if (!TryExtractCameraFromBuffer(data, size, tempView, tempProj, &score, &offset)) continue;
    if (score > bestScore) {
      bestScore = score;
      std::copy_n(tempView, 16, outView);
      std::copy_n(tempProj, 16, outProj);
      found = true;
    }
  }
  if (logCandidates) {
    LOG_INFO("[CAM] Descriptor scan: candidates={} scanned={} bestScore={:.2f}", (unsigned long long)addrs.size(),
             scanned, bestScore);
  }
  if (found && outScore) *outScore = bestScore;
  return found;
}

bool TryScanRootCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates) {
  std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addrs;
  {
    std::lock_guard<std::mutex> lock(g_cbvAddrMutex);
    addrs = g_rootCbvAddrs;
  }
  if (addrs.empty()) {
    if (logCandidates) {
      LOG_INFO("[CAM] No root CBV addresses captured yet.");
    }
    return false;
  }
  float bestScore = 0.0f;
  bool found = false;
  uint64_t currentFrame = StreamlineIntegration::Get().GetFrameCount();
  uint64_t lastFound = g_lastCameraFoundFrame.load();
  bool stale = lastFound == 0 || (currentFrame > lastFound + camera_config::kScanStaleFrames);
  uint32_t maxScan = stale ? (camera_config::kDescriptorScanMax * camera_config::kScanExtendedMultiplier)
                           : camera_config::kDescriptorScanMax;
  uint32_t scanned = 0;
  for (size_t i = addrs.size(); i-- > 0;) {
    if (scanned++ >= maxScan) break;
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (!TryGetCbvData(addrs[i], &data, &size)) continue;
    float tempView[16], tempProj[16], score = 0.0f;
    size_t offset = 0;
    if (!TryExtractCameraFromBuffer(data, size, tempView, tempProj, &score, &offset)) continue;
    if (score > bestScore) {
      bestScore = score;
      std::copy_n(tempView, 16, outView);
      std::copy_n(tempProj, 16, outProj);
      found = true;
    }
  }
  if (logCandidates) {
    LOG_INFO("[CAM] Root CBV scan: candidates={} scanned={} bestScore={:.2f}", (unsigned long long)addrs.size(),
             scanned, bestScore);
  }
  if (found && outScore) *outScore = bestScore;
  return found;
}

CameraDiagnostics GetCameraDiagnostics() {
  CameraDiagnostics diag{};
  {
    std::lock_guard<std::mutex> lock(g_cbvMutex);
    diag.registeredCbvCount = static_cast<uint32_t>(g_cbvInfos.size());
  }
  {
    std::lock_guard<std::mutex> lock(g_cbvAddrMutex);
    diag.trackedDescriptors = static_cast<uint32_t>(g_cbvGpuAddrs.size());
    diag.trackedRootAddresses = static_cast<uint32_t>(g_rootCbvAddrs.size());
  }
  {
    std::lock_guard<std::mutex> lock(g_cameraMutex);
    diag.lastScore = g_bestCamera.score;
    diag.lastFoundFrame = g_bestCamera.frame;
    diag.lastScanMethod = static_cast<int>(g_bestCamera.method);
    diag.cameraValid = g_bestCamera.valid;
  }
  return diag;
}
