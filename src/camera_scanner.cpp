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
#include "resource_detector.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <expected>
#include <immintrin.h>
#include <mutex>
#include <new>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <intrin.h>

namespace {

static bool g_hasAVX2 = []{
  int cpuInfo[4];
  __cpuid(cpuInfo, 0);
  if (cpuInfo[0] >= 7) {
    __cpuidex(cpuInfo, 7, 0);
    return (cpuInfo[1] & (1 << 5)) != 0;
  }
  return false;
}();

// ============================================================================
// SEH-based memory validation — replaces VirtualQuery syscalls.
// On x64 Windows, __try/__except uses table-based unwinding: if no exception
// is thrown, the overhead is literally 0 CPU cycles.  VirtualQuery was costing
// ~1-5µs per call (Ring-3 → Ring-0 context switch + page table walk).
// ============================================================================
extern "C" bool IsMemoryReadableFast(const void* ptr, size_t size) noexcept {
  if (!ptr || size == 0) return false;
  __try {
    // Touch first and last bytes to verify the entire range is committed.
    volatile uint8_t first = *static_cast<const volatile uint8_t*>(ptr);
    volatile uint8_t last = *(static_cast<const volatile uint8_t*>(ptr) + size - 1);
    (void)first; (void)last;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

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

// Lock hierarchy level 3 — same tier as Resources
// (SwapChain=1 > Hooks=2 > Resources/Camera=3 > Config=4 > Logging=5).
std::mutex g_cameraMutex;
CameraCandidate g_bestCamera;
ScoreBreakdown g_lastBreakdown;  // Item 9: last accepted breakdown
std::atomic<bool> g_loggedCamera(false);

struct UploadCbvInfo {
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  D3D12_GPU_VIRTUAL_ADDRESS gpuBase = 0;
  uint64_t size = 0;
  uint8_t* cpuPtr = nullptr;
};

// Item 14: CBV list kept sorted by gpuBase for O(log n) lookups.
// Lock hierarchy level 3 — same tier as Resources
std::mutex g_cbvMutex;
std::vector<UploadCbvInfo> g_cbvInfos;
alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> g_lastFullScanFrame(0);
alignas(std::hardware_destructive_interference_size) std::atomic<uint64_t> g_lastCameraFoundFrame(0);

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
bool LooksLikeMatrix(std::span<const float, 16> m) {
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
static float Length3(const float* v) {
  return sqrtf(Dot3(v, v));
}

// C++23: std::expected return type for camera extraction
struct CameraExtraction {
  std::array<float, 16> view;
  std::array<float, 16> proj;
  float score;
  size_t offset;
};

// Item 9: Explainable scoring — populates ScoreBreakdown with per-signal contributions.
ScoreBreakdown ScoreMatrixPairWithBreakdown(std::span<const float, 16> viewData, std::span<const float, 16> projData) {
  ScoreBreakdown bd{};
  if (!LooksLikeMatrix(viewData) || !LooksLikeMatrix(projData)) return bd;

  const float* view = viewData.data();
  const float* proj = projData.data();

  // view[3,3] should be 1.0 for an affine view matrix
  if (std::abs(view[3 * 4 + 3] - 1.0f) > 0.1f) return bd;
  if (std::abs(view[3 * 4 + 3] - 1.0f) < 0.01f) bd.affineBonus = 0.2f;

  // Perspective projection detection
  bool isStrongPerspective = std::abs(proj[3 * 4 + 3]) < 0.01f && std::abs(std::abs(proj[2 * 4 + 3]) - 1.0f) < 0.1f;
  bool isWeakPerspective = std::abs(proj[3 * 4 + 3]) < 0.8f && std::abs(proj[2 * 4 + 3]) > 0.2f;

  if (isStrongPerspective)
    bd.perspectiveBonus = 0.6f;
  else if (isWeakPerspective)
    bd.perspectiveBonus = 0.3f;
  else
    return bd; // Reject ortho/identity

  // FoV validation: proj[0,0] and proj[1,1] encode focal lengths
  if (std::abs(proj[0 * 4 + 0]) > 0.3f && std::abs(proj[0 * 4 + 0]) < 5.0f &&
      std::abs(proj[1 * 4 + 1]) > 0.3f && std::abs(proj[1 * 4 + 1]) < 5.0f) {
    bd.fovBonus = 0.15f;
    if (std::abs(proj[1 * 4 + 1]) > 0.8f && std::abs(proj[1 * 4 + 1]) < 2.2f) bd.fovBonus += 0.05f;
  }

  // Affine view matrix: last column should be [0, 0, 0, 1]
  float transBonus = 0.0f;
  if (std::abs(view[0 * 4 + 3]) < 1.0f && std::abs(view[1 * 4 + 3]) < 1.0f && std::abs(view[2 * 4 + 3]) < 1.0f) transBonus += 0.1f;
  if (std::abs(view[3 * 4 + 0]) < camera_config::kPosTolerance &&
      std::abs(view[3 * 4 + 1]) < camera_config::kPosTolerance &&
      std::abs(view[3 * 4 + 2]) < camera_config::kPosTolerance)
    transBonus += 0.1f;
  bd.translationBonus = transBonus;

  // Orthogonality check for rotation component
  float r0[3] = { view[0 * 4 + 0], view[0 * 4 + 1], view[0 * 4 + 2] };
  float r1[3] = { view[1 * 4 + 0], view[1 * 4 + 1], view[1 * 4 + 2] };
  float r2[3] = { view[2 * 4 + 0], view[2 * 4 + 1], view[2 * 4 + 2] };
  float len0 = Length3(r0);
  float len1 = Length3(r1);
  float len2 = Length3(r2);
  if (len0 > 0.1f && len1 > 0.1f && len2 > 0.1f) {
    float ortho = 0.0f;
    if (std::abs(Dot3(r0, r1) / (len0 * len1)) < 0.2f) ortho += 0.1f;
    if (std::abs(Dot3(r0, r2) / (len0 * len2)) < 0.2f) ortho += 0.1f;
    if (std::abs(Dot3(r1, r2) / (len1 * len2)) < 0.2f) ortho += 0.1f;
    if (std::abs(len0 - 1.0f) < 0.15f && std::abs(len1 - 1.0f) < 0.15f && std::abs(len2 - 1.0f) < 0.15f) {
      ortho += 0.1f;
    }
    bd.orthogonalityBonus = ortho;
  }

  bd.total = bd.affineBonus + bd.perspectiveBonus + bd.fovBonus + bd.translationBonus + bd.orthogonalityBonus;
  return bd;
}

// Backward-compatible wrapper — returns total score only
float ScoreMatrixPair(std::span<const float, 16> viewData, std::span<const float, 16> projData) {
  return ScoreMatrixPairWithBreakdown(viewData, projData).total;
}

[[nodiscard]] std::expected<CameraExtraction, std::string_view>
TryExtractCameraFromBuffer(const uint8_t* data, size_t size) {
  if (!data || size < camera_config::kCbvMinSize)
    return std::unexpected("Buffer too small");
  float bestScore = 0.0f;
  size_t bestOffset = 0;
  auto scanWithStride = [&](size_t stride, float& bestScoreOut, size_t& bestOffsetOut) {
    const size_t scanLimit = size;
    const size_t matrixBytes = sizeof(float) * 32;
    // PERF FIX: Stream from Write-Combine (upload heap) memory into L1 cache
    // via 256-bit sequential loads before any scalar evaluation. WC memory is
    // uncached — each scalar 4-byte read triggers a distinct ~100ns PCIe bus
    // transaction. Streaming 32-byte blocks maximizes PCIe bandwidth efficiency.
    for (size_t offset = 0; offset + matrixBytes <= scanLimit; offset += stride) {
      alignas(32) float localMatrix[32];
      const uint8_t* src = data + offset;

      if (g_hasAVX2) {
        if ((reinterpret_cast<uintptr_t>(src) & 31) == 0) {
          for (int i = 0; i < 32; i += 8) {
            __m256i chunk = _mm256_stream_load_si256(
                reinterpret_cast<const __m256i*>(src + i * sizeof(float)));
            _mm256_store_ps(&localMatrix[i], _mm256_castsi256_ps(chunk));
          }
        } else {
          for (int i = 0; i < 32; i += 8) {
            __m256i chunk = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(src + i * sizeof(float)));
            _mm256_store_ps(&localMatrix[i], _mm256_castsi256_ps(chunk));
          }
        }
      } else {
        std::memcpy(localMatrix, src, sizeof(localMatrix));
      }

      std::span<const float, 16> viewSpan{localMatrix, 16};
      std::span<const float, 16> projSpan{localMatrix + 16, 16};
      float score = ScoreMatrixPair(viewSpan, projSpan);
      if (score > bestScoreOut) {
        bestScoreOut = score;
        bestOffsetOut = offset;
      }

      float tView[16], tProj[16];
      TransposeMatrix(localMatrix, tView);
      TransposeMatrix(localMatrix + 16, tProj);
      float tScore = ScoreMatrixPair(std::span<const float, 16>{tView, 16},
                                     std::span<const float, 16>{tProj, 16});
      if (tScore > bestScoreOut) {
        bestScoreOut = tScore;
        bestOffsetOut = offset;
      }
    }
  };


  // Multi-stride scanning: coarse to fine, carrying best score forward
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
  if (bestScore < 0.6f) return std::unexpected("Score below threshold");

  const float* viewPtr = reinterpret_cast<const float*>(data + bestOffset);
  const float* projPtr = viewPtr + 16;
  std::span<const float, 16> viewSpan{viewPtr, 16};
  std::span<const float, 16> projSpan{projPtr, 16};
  float score = ScoreMatrixPair(viewSpan, projSpan);
  float tView[16], tProj[16];
  TransposeMatrix(viewPtr, tView);
  TransposeMatrix(projPtr, tProj);
  float tScore = ScoreMatrixPair(std::span<const float, 16>{tView, 16},
                                  std::span<const float, 16>{tProj, 16});

  CameraExtraction result{};
  result.offset = bestOffset;
  if (tScore > score) {
    std::copy_n(tView, 16, result.view.data());
    std::copy_n(tProj, 16, result.proj.data());
    result.score = tScore;
  } else {
    std::copy_n(viewPtr, 16, result.view.data());
    std::copy_n(projPtr, 16, result.proj.data());
    result.score = score;
  }
  return result;
}

// Item 14: O(log n) lookup using sorted g_cbvInfos vector.
bool TryGetCbvData(D3D12_GPU_VIRTUAL_ADDRESS gpuAddress, const uint8_t** outData, size_t* outSize) {
  std::scoped_lock lock(g_cbvMutex);
  if (g_cbvInfos.empty()) return false;
  // Find the last element whose gpuBase <= gpuAddress
  auto it = std::upper_bound(g_cbvInfos.begin(), g_cbvInfos.end(), gpuAddress,
      [](D3D12_GPU_VIRTUAL_ADDRESS addr, const UploadCbvInfo& info) {
        return addr < info.gpuBase;
      });
  if (it == g_cbvInfos.begin()) return false;
  --it;
  if (!it->cpuPtr || it->gpuBase == 0 || it->size == 0) return false;
  if (gpuAddress >= it->gpuBase && gpuAddress < it->gpuBase + it->size) {
    size_t offset = static_cast<size_t>(gpuAddress - it->gpuBase);
    *outData = it->cpuPtr + offset;
    *outSize = static_cast<size_t>(it->size - offset);
    return true;
  }
  return false;
}

void UpdateBestCamera(const float* view, const float* proj, float jitterX, float jitterY,
                      ScanMethod method = ScanMethod::None) {
  ScoreBreakdown bd = ScoreMatrixPairWithBreakdown(std::span<const float, 16>{view, 16}, std::span<const float, 16>{proj, 16});
  float score = bd.total;
  if (score < 0.6f) return;
  std::scoped_lock lock(g_cameraMutex);

  // EMA smoothing — prevents single-frame glitch detections from overriding stable camera.
  constexpr float kEmaAlpha = 0.3f;
  static float s_smoothedScore = 0.0f;
  s_smoothedScore = kEmaAlpha * score + (1.0f - kEmaAlpha) * s_smoothedScore;

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

  float gatedScore = (g_bestCamera.valid && stabilityBonus < 0.1f)
                         ? s_smoothedScore + stabilityBonus
                         : score;

  if (g_bestCamera.valid && gatedScore < g_bestCamera.score - 0.1f) return;
  g_bestCamera.score = score;
  std::copy_n(view, 16, g_bestCamera.view);
  std::copy_n(proj, 16, g_bestCamera.proj);
  g_bestCamera.jitterX = jitterX;
  g_bestCamera.jitterY = jitterY;
  // Item 16: Use ResourceDetector frame counter as single source of truth
  g_bestCamera.frame = ResourceDetector::Get().GetFrameCount();
  g_bestCamera.valid = true;
  g_bestCamera.method = method;
  g_lastBreakdown = bd;  // Item 9: store breakdown for diagnostics
  if (!g_loggedCamera.exchange(true)) {
    LOG_INFO("Camera matrices detected (score {:.2f}, smoothed {:.2f}, method: {})",
             score, s_smoothedScore, ScanMethodName(method));
  }
}

static D3D12_GPU_VIRTUAL_ADDRESS s_lastCameraCbv = 0;
static size_t s_lastCameraOffset = 0;
} // namespace

// ============================================================================
// PUBLIC API — CameraScanner singleton methods
// ============================================================================

CameraScanner& CameraScanner::Get() {
  static CameraScanner instance;
  return instance;
}

void CameraScanner::UpdateCameraCache(const float* view, const float* proj, float jitterX, float jitterY) {
  if (!view || !proj) return;
  UpdateBestCamera(view, proj, jitterX, jitterY);
}

bool CameraScanner::GetLastCameraStats(float& outScore, uint64_t& outFrame) {
  std::scoped_lock lock(g_cameraMutex);
  if (!g_bestCamera.valid) return false;
  outScore = g_bestCamera.score;
  outFrame = g_bestCamera.frame;
  return true;
}

void CameraScanner::TrackCbvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc) {
  if (!handle.ptr || !desc || desc->BufferLocation == 0) return;
  std::scoped_lock lock(g_cbvAddrMutex);
  g_cbvGpuAddrs[handle.ptr] = {desc->BufferLocation, ResourceDetector::Get().GetFrameCount()};
  g_cbvDescriptorCount++;
}

void CameraScanner::TrackRootCbvAddress(D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (!address) return;
  std::scoped_lock lock(g_cbvAddrMutex);
  auto it = std::find(g_rootCbvAddrs.begin(), g_rootCbvAddrs.end(), address);
  if (it != g_rootCbvAddrs.end()) g_rootCbvAddrs.erase(it);
  g_rootCbvAddrs.push_back(address);
  const size_t maxKeep = camera_config::kDescriptorScanMax * camera_config::kScanExtendedMultiplier;
  if (g_rootCbvAddrs.size() > maxKeep) {
    g_rootCbvAddrs.erase(g_rootCbvAddrs.begin(), g_rootCbvAddrs.begin() + (g_rootCbvAddrs.size() - maxKeep));
  }
  g_cbvGpuAddrCount++;
}

void CameraScanner::GetCameraScanCounts(uint64_t& cbvCount, uint64_t& descCount, uint64_t& rootCount) {
  cbvCount = g_cbvInfos.size();
  {
    std::scoped_lock lock(g_cbvAddrMutex);
    descCount = g_cbvGpuAddrs.size();
    rootCount = g_rootCbvAddrs.size();
  }
}

// Item 13: Deduplicate by (resource, gpuBase) — update in place if already registered.
// Item 14: Maintain sorted order by gpuBase for O(log n) lookups.
void CameraScanner::RegisterCbv(ID3D12Resource* pResource, UINT64 size, uint8_t* cpuPtr) {
  std::scoped_lock lock(g_cbvMutex);
  D3D12_GPU_VIRTUAL_ADDRESS gpuBase = pResource->GetGPUVirtualAddress();

  // Deduplicate: find existing entry with same resource + gpuBase
  auto dup = std::find_if(g_cbvInfos.begin(), g_cbvInfos.end(),
      [&](const UploadCbvInfo& e) { return e.resource.Get() == pResource && e.gpuBase == gpuBase; });
  if (dup != g_cbvInfos.end()) {
    dup->cpuPtr = cpuPtr;
    dup->size = size;
    return; // Updated in place, sorted order unchanged
  }

  // Insert at sorted position (by gpuBase)
  UploadCbvInfo info{};
  info.resource = pResource;
  info.gpuBase = gpuBase;
  info.size = size;
  info.cpuPtr = cpuPtr;
  auto insertPos = std::lower_bound(g_cbvInfos.begin(), g_cbvInfos.end(), gpuBase,
      [](const UploadCbvInfo& e, D3D12_GPU_VIRTUAL_ADDRESS addr) { return e.gpuBase < addr; });
  g_cbvInfos.insert(insertPos, std::move(info));

  const size_t maxCbvs = camera_config::kScanMaxCbvsPerFrame * camera_config::kScanExtendedMultiplier * 8;
  if (g_cbvInfos.size() > maxCbvs) {
    g_cbvInfos.erase(g_cbvInfos.begin(), g_cbvInfos.begin() + (g_cbvInfos.size() - maxCbvs));
  }
}

void CameraScanner::ResetCameraScanCache() {
  std::scoped_lock lock(g_cbvMutex);
  g_cbvInfos.clear();
  s_lastCameraCbv = 0;
  s_lastCameraOffset = 0;
  g_lastFullScanFrame.store(0);
  g_lastCameraFoundFrame.store(0);
  g_loggedCamera.store(false);
  {
    std::scoped_lock dlock(g_cbvAddrMutex);
    g_cbvGpuAddrs.clear();
    g_rootCbvAddrs.clear();
  }
  g_cbvDescriptorCount.store(0);
  g_cbvGpuAddrCount.store(0);
}

uint64_t CameraScanner::GetLastCameraFoundFrame() {
  return g_lastCameraFoundFrame.load();
}

uint64_t CameraScanner::GetLastFullScanFrame() {
  return g_lastFullScanFrame.load();
}

bool CameraScanner::TryScanAllCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates, bool allowFullScan) {
  // Item 12: Snapshot-then-scan — lock only to snapshot metadata, then scan unlocked.
  // This prevents holding g_cbvMutex during the expensive per-buffer extraction.
  struct CbvSnapshot {
    D3D12_GPU_VIRTUAL_ADDRESS gpuBase;
    uint8_t* cpuPtr;
    uint64_t size;
  };
  std::vector<CbvSnapshot> snapshot;
  D3D12_GPU_VIRTUAL_ADDRESS cachedCbv = s_lastCameraCbv;
  size_t cachedOffset = s_lastCameraOffset;
  {
    std::scoped_lock lock(g_cbvMutex);
    // Prune entries whose CPU pointers are no longer readable
    std::erase_if(g_cbvInfos, [](const UploadCbvInfo& info) {
      if (!info.cpuPtr) return true;
      return !IsMemoryReadableFast(info.cpuPtr, static_cast<size_t>(info.size));
    });
    snapshot.reserve(g_cbvInfos.size());
    for (const auto& info : g_cbvInfos) {
      snapshot.push_back({info.gpuBase, info.cpuPtr, info.size});
    }
  }
  // From here on, no mutex is held. CPU pointers are val
  // Fast path - check known location first (operates on snapshot, no lock held)
  if (cachedCbv != 0) {
    for (const auto& snap : snapshot) {
      if (snap.gpuBase == cachedCbv) {
        if (!IsMemoryReadableFast(snap.cpuPtr, static_cast<size_t>(snap.size))) continue;
        if (cachedOffset + sizeof(float) * 32 <= snap.size) {
          const float* viewRaw = reinterpret_cast<const float*>(snap.cpuPtr + cachedOffset);
          const float* projRaw = viewRaw + 16;
          float score = ScoreMatrixPair(std::span<const float, 16>{viewRaw, 16},
                                         std::span<const float, 16>{projRaw, 16});
          float bestScore = score;
          bool useTranspose = false;
          float tView[16], tProj[16];
          TransposeMatrix(viewRaw, tView);
          TransposeMatrix(projRaw, tProj);
          float tScore = ScoreMatrixPair(std::span<const float, 16>{tView, 16},
                                           std::span<const float, 16>{tProj, 16});
          if (tScore > bestScore) {
            bestScore = tScore;
            useTranspose = true;
          }
          if (bestScore > 0.6f) {
            if (useTranspose) {
              std::copy_n(tView, 16, outView);
              std::copy_n(tProj, 16, outProj);
            } else {
              std::copy_n(viewRaw, 16, outView);
              std::copy_n(projRaw, 16, outProj);
            }
            if (outScore) *outScore = bestScore;
            // Item 16: Unified frame counter
            g_lastCameraFoundFrame.store(ResourceDetector::Get().GetFrameCount());
            return true;
          }
        }

        auto extraction = TryExtractCameraFromBuffer(snap.cpuPtr, static_cast<size_t>(snap.size));
        if (extraction.has_value()) {
          s_lastCameraOffset = extraction->offset;
          std::copy_n(extraction->view.data(), 16, outView);
          std::copy_n(extraction->proj.data(), 16, outProj);
          if (outScore) *outScore = extraction->score;
          g_lastCameraFoundFrame.store(ResourceDetector::Get().GetFrameCount());
          return true;
        }
      }
    }
  }

  float bestScore = 0.0f;
  bool found = false;
  D3D12_GPU_VIRTUAL_ADDRESS foundGpuBase = 0;

  if (snapshot.empty() && logCandidates) {
    LOG_INFO("{}", "[CAM] No CBVs registered! Check RegisterCbv hooks.");
    return false;
  }

  if (!allowFullScan) {
    return false;
  }
  // Item 16: Unified frame counter
  g_lastFullScanFrame.store(ResourceDetector::Get().GetFrameCount());

  uint32_t scanned = 0;
  const uint32_t maxScan = camera_config::kScanMaxCbvsPerFrame * camera_config::kScanExtendedMultiplier;
  for (const auto& snap : snapshot) {
    if (!snap.cpuPtr || snap.size < camera_config::kCbvMinSize) continue;
    if (scanned++ >= maxScan) break;
    if (!IsMemoryReadableFast(snap.cpuPtr, static_cast<size_t>(snap.size))) continue;

    auto extraction = TryExtractCameraFromBuffer(snap.cpuPtr, static_cast<size_t>(snap.size));
    if (extraction.has_value()) {
      if (logCandidates && extraction->score > 0.0f) {
        LOG_INFO("[CAM] Candidate GPU:0x{:x} Size:{} Score:{:.2f} View[15]:{:.2f} Proj[15]:{:.2f} Proj[11]:{:.2f}",
                 (unsigned long long)snap.gpuBase, (unsigned long long)snap.size, extraction->score, extraction->view[15], extraction->proj[15], extraction->proj[11]);
      }
      if (extraction->score > bestScore) {
        bestScore = extraction->score;
        std::copy_n(extraction->view.data(), 16, outView);
        std::copy_n(extraction->proj.data(), 16, outProj);
        foundGpuBase = snap.gpuBase;
        s_lastCameraOffset = extraction->offset;
        found = true;
      }
    }
  }

  if (found) {
    s_lastCameraCbv = foundGpuBase;
    if (outScore) *outScore = bestScore;
    g_lastCameraFoundFrame.store(ResourceDetector::Get().GetFrameCount());
    LOG_INFO("Camera matrices detected (Score: {:.2f}) at GPU: 0x{:x} Offset: +0x{:X}", bestScore, (unsigned long long)foundGpuBase,
             (unsigned long long)s_lastCameraOffset);
  } else if (logCandidates) {
    LOG_INFO("[CAM] Scan failed. Checked {} CBVs. Best Score: {:.2f}", (unsigned long long)snapshot.size(), bestScore);
  }

  return found;
}

bool CameraScanner::TryScanDescriptorCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates) {
  std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, uint64_t>> addrs;
  {
    std::scoped_lock lock(g_cbvAddrMutex);
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
  // Item 16: Unified frame counter
  uint64_t currentFrame = ResourceDetector::Get().GetFrameCount();
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
    auto extraction = TryExtractCameraFromBuffer(data, size);
    if (!extraction.has_value()) continue;
    if (extraction->score > bestScore) {
      bestScore = extraction->score;
      std::copy_n(extraction->view.data(), 16, outView);
      std::copy_n(extraction->proj.data(), 16, outProj);
      found = true;
    }
  }
  if (logCandidates) {
    LOG_INFO("[CAM] Descriptor scan: candidates={} scanned={} bestScore={:.2f}", (unsigned long long)addrs.size(),
             (unsigned long long)scanned, bestScore);
  }
  if (found && outScore) { *outScore = bestScore; }
  return found;
}

bool CameraScanner::TryScanRootCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates) {
  std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addrs;
  {
    std::scoped_lock lock(g_cbvAddrMutex);
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
  // Item 16: Unified frame counter
  uint64_t currentFrame = ResourceDetector::Get().GetFrameCount();
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
    auto extraction = TryExtractCameraFromBuffer(data, size);
    if (!extraction.has_value()) continue;
    if (extraction->score > bestScore) {
      bestScore = extraction->score;
      std::copy_n(extraction->view.data(), 16, outView);
      std::copy_n(extraction->proj.data(), 16, outProj);
      found = true;
    }
  }
  if (logCandidates) {
    LOG_INFO("[CAM] Root CBV scan: candidates={} scanned={} bestScore={:.2f}", (unsigned long long)addrs.size(),
             (unsigned long long)scanned, bestScore);
  }
  if (found && outScore) { *outScore = bestScore; }
  return found;
}

CameraDiagnostics CameraScanner::GetDiagnostics() {
  CameraDiagnostics diag{};
  {
    std::scoped_lock lock(g_cbvMutex);
    diag.registeredCbvCount = static_cast<uint32_t>(g_cbvInfos.size());
  }
  {
    std::scoped_lock lock(g_cbvAddrMutex);
    diag.trackedDescriptors = static_cast<uint32_t>(g_cbvGpuAddrs.size());
    diag.trackedRootAddresses = static_cast<uint32_t>(g_rootCbvAddrs.size());
  }
  {
    std::scoped_lock lock(g_cameraMutex);
    diag.lastScore = g_bestCamera.score;
    diag.lastFoundFrame = g_bestCamera.frame;
    diag.lastScanMethod = std::to_underlying(g_bestCamera.method);
    diag.cameraValid = g_bestCamera.valid;
    diag.lastBreakdown = g_lastBreakdown;  // Item 9: score breakdown
  }
  return diag;
}
