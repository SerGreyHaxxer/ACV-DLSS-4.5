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
#include "resource_detector.h"
#include "config_manager.h"
#include "dlss4_config.h"
#include "heuristic_scanner.h"
#include "logger.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

ResourceDetector &ResourceDetector::Get() {
  static ResourceDetector instance;
  return instance;
}

ResourceDetector::~ResourceDetector() {
  if (m_fenceEvent) {
    CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
  }
}

bool ResourceDetector::InitCommandList(ID3D12Device *pDevice) {
  if (m_cmdList)
    return true;

  HRESULT hr = pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&m_cmdAlloc));
  if (FAILED(hr))
    return false;

  hr = pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                  m_cmdAlloc.Get(), nullptr,
                                  IID_PPV_ARGS(&m_cmdList));
  if (FAILED(hr))
    return false;
  m_cmdList->Close(); // Start closed

  hr = pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
  if (FAILED(hr))
    return false;
  m_fenceVal = 1;

  m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (!m_fenceEvent)
    return false;
  return true;
}

void ResourceDetector::UpdateHeuristics(ID3D12CommandQueue *pQueue) {
  if (!pQueue)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);

  // Check if previous analysis is done
  if (m_fence && m_fence->GetCompletedValue() < m_fenceVal - 1 &&
      m_fenceVal > 1) {
    // Still busy
    return;
  }

  // Read back previous results if they just finished
  if (m_fence && m_fence->GetCompletedValue() >= m_fenceVal - 1 &&
      m_fenceVal > 1 && m_lastAnalyzedCandidate) {
    ScanResult result = {};
    if (HeuristicScanner::Get().GetReadbackResult(result)) {
      HeuristicData &data = m_heuristics[m_lastAnalyzedCandidate.Get()];
      data.analyzed = true;
      data.lastCheckFrame = m_frameCount;
      data.variance = (std::max)(result.varianceX, result.varianceY);
      data.validRange = result.validRange;

      LOG_INFO(
          "[Scanner] Async Result for {:p} | Var:{:.4f} Uniform:{} Data:{}",
          static_cast<void*>(m_lastAnalyzedCandidate.Get()), data.variance,
          result.isUniform ? "YES" : "NO", result.hasData ? "YES" : "NO");

      // Apply scoring (find the candidate in the list)
      auto it =
          std::find_if(m_motionCandidates.begin(), m_motionCandidates.end(),
                       [this](const ResourceCandidate &c) {
                         return c.pResource == m_lastAnalyzedCandidate;
                       });

      if (it != m_motionCandidates.end()) {
        if (result.hasData && result.validRange && !result.isUniform) {
          it->score += 1.0f;
          if (it->score > m_bestMotionScore) {
            m_bestMotionScore = it->score;
            m_bestMotion = it->pResource;
          }
        } else if (result.isUniform || !result.hasData) {
          it->score -= 0.5f;
        }
      }
    }
    m_lastAnalyzedCandidate = nullptr;
  }

  // Only start new check every N frames
  if (m_frameCount % 120 != 0)
    return;

  Microsoft::WRL::ComPtr<ID3D12Device> pDevice;
  pQueue->GetDevice(IID_PPV_ARGS(&pDevice));
  if (!pDevice)
    return;

  if (!HeuristicScanner::Get().Initialize(pDevice.Get()))
    return;

  if (!InitCommandList(pDevice.Get()))
    return;

  // Identify candidate to check
  ResourceCandidate *bestCandidate = nullptr;
  float maxScore = 0.0f;

  for (auto &cand : m_motionCandidates) {
    if (cand.score > 0.4f && cand.score < 2.0f) {
      if (m_heuristics.find(cand.pResource.Get()) == m_heuristics.end() ||
          (m_frameCount - m_heuristics[cand.pResource.Get()].lastCheckFrame >
           600)) {
        if (cand.score > maxScore) {
          maxScore = cand.score;
          bestCandidate = &cand;
        }
      }
    }
  }

  if (!bestCandidate)
    return;

  // Run Analysis â€” wait for GPU to finish previous submission first
  if (m_fence && m_fenceVal > 1) {
    if (m_fence->GetCompletedValue() < m_fenceVal - 1) {
      m_fence->SetEventOnCompletion(m_fenceVal - 1, m_fenceEvent);
      WaitForSingleObject(m_fenceEvent, 1000);
    }
  }
  m_cmdAlloc->Reset();
  m_cmdList->Reset(m_cmdAlloc.Get(), nullptr);

  ScanResult dummy = {}; // We don't use immediate result
  if (HeuristicScanner::Get().AnalyzeTexture(
          m_cmdList.Get(), bestCandidate->pResource.Get(), dummy)) {
    m_cmdList->Close();
    ID3D12CommandList *lists[] = {m_cmdList.Get()};
    pQueue->ExecuteCommandLists(1, lists);
    pQueue->Signal(m_fence.Get(), m_fenceVal);

    m_lastAnalyzedCandidate = bestCandidate->pResource;
    m_fenceVal++;
  } else {
    m_cmdList->Close();
  }
  // pDevice released automatically by ComPtr
}

void ResourceDetector::NewFrame() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_frameCount.fetch_add(1, std::memory_order_relaxed); // under unique_lock â€” relaxed is fine
  const uint64_t currentFrame = m_frameCount.load(std::memory_order_relaxed);

  auto isStale = [currentFrame, this](const ResourceCandidate &cand) {
    // Don't prune the BEST candidates if we are recently active
    if (cand.pResource == m_bestMotion || cand.pResource == m_bestDepth ||
        cand.pResource == m_bestColor) {
      return (currentFrame - cand.lastFrameSeen) > (resource_config::kStaleFrames * 2);
    }
    return (currentFrame - cand.lastFrameSeen) > resource_config::kStaleFrames;
  };

  if (!m_motionCandidates.empty()) {
    m_motionCandidates.erase(std::remove_if(m_motionCandidates.begin(),
                                            m_motionCandidates.end(), isStale),
                             m_motionCandidates.end());
  }
  if (!m_depthCandidates.empty()) {
    m_depthCandidates.erase(std::remove_if(m_depthCandidates.begin(),
                                           m_depthCandidates.end(), isStale),
                            m_depthCandidates.end());
  }
  if (!m_colorCandidates.empty()) {
    m_colorCandidates.erase(std::remove_if(m_colorCandidates.begin(),
                                           m_colorCandidates.end(), isStale),
                            m_colorCandidates.end());
  }

  // Periodically clear old candidates to adapt to resolution changes
  // But ONLY if we have new candidates to replace them with
  if (m_frameCount % resource_config::kCleanupInterval == 0) {
    // Selective cleanup: don't wipe everything if we are in the middle of a
    // game
    if (m_colorCandidates.size() > 50 || m_depthCandidates.size() > 50 ||
        m_motionCandidates.size() > 50) {
      LOG_INFO("Resource detector cache trimming (Frame {})",
               m_frameCount.load(std::memory_order_relaxed));
      // Trimming logic instead of full clear would go here
    }
  }
}

void ResourceDetector::Clear() {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  // Persist best candidates across 'Clear' calls to prevent losing buffers
  // during UI/Menu transitions. We only clear them if they haven't been updated
  // in a very long time (handled by stale logic in NewFrame).

  m_motionCandidates.clear();
  m_depthCandidates.clear();
  m_colorCandidates.clear();

  // CRITICAL: Do NOT clear m_bestMotion, m_bestDepth, m_bestColor or overrides
  // here. They will be replaced naturally if better candidates are found or
  // expire via NewFrame.

  LOG_INFO("Resource detector soft-cleared (Candidates wiped, Best & Overrides "
           "kept).");
}

void ResourceDetector::SetExpectedDimensions(uint32_t width, uint32_t height) {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_expectedWidth = width;
  m_expectedHeight = height;
}

// Define a GUID for our tag: {25CDDAA4-B1C6-41E5-9C52-FE69FC2E6A3D}
static const GUID RD_GEN_TAG = {
    0x25cddaa4,
    0xb1c6,
    0x41e5,
    {0x9c, 0x52, 0xfe, 0x69, 0xfc, 0x2e, 0x6a, 0x3d}};

void ResourceDetector::RegisterResource(ID3D12Resource *pResource) {
  RegisterResource(pResource, false);
}

void ResourceDetector::RegisterDepthFromView(ID3D12Resource *pResource,
                                             DXGI_FORMAT viewFormat) {
  if (!pResource)
    return;
  D3D12_RESOURCE_DESC desc = pResource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return;
  if (desc.Width < 64 || desc.Height < 64)
    return;
  if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
      viewFormat == DXGI_FORMAT_UNKNOWN)
    return;
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  if (viewFormat != DXGI_FORMAT_UNKNOWN) {
    m_depthFormatOverrides[pResource] = viewFormat;
  }
  if (m_bestDepth.Get() == pResource)
    return;
  m_bestDepthScore = 2.0f;
  m_bestDepth = pResource;
  bool quietScan = ConfigManager::Get().Data().system.quietResourceScan;
  if (!quietScan) {
    LOG_INFO("[DLSSG] Depth view bound: {}x{} Fmt:{} Ptr:{:p}", desc.Width,
             desc.Height, static_cast<int>(desc.Format), static_cast<void*>(pResource));
  }
}

void ResourceDetector::RegisterDepthFromClear(ID3D12Resource *pResource,
                                              float clearDepth) {
  if (!pResource)
    return;
  std::unique_lock<std::shared_mutex> lock(m_mutex);

  // Detection of depth inversion
  // Standard: Clear to 1.0 (Far), Near is 0.0
  // Inverted: Clear to 0.0 (Far), Near is 1.0
  if (clearDepth == 0.0f) {
    if (!m_depthInverted) {
      LOG_INFO("[Scanner] Detected Inverted Depth (ClearValue: 0.0)");
      m_depthInverted = true;
    }
  } else if (clearDepth == 1.0f) {
    if (m_depthInverted) {
      LOG_INFO("[Scanner] Detected Standard Depth (ClearValue: 1.0)");
      m_depthInverted = false;
    }
  }

  if (m_bestDepth.Get() == pResource)
    return;

  D3D12_RESOURCE_DESC desc = pResource->GetDesc();
  m_bestDepthScore = 3.0f; // Extremely high confidence
  m_bestDepth = pResource;

  if (!ConfigManager::Get().Data().system.quietResourceScan) {
    LOG_INFO("[DLSSG] Depth IDENTIFIED via Clear: {}x{} Fmt:{} Ptr:{:p}",
             desc.Width, desc.Height, static_cast<int>(desc.Format),
             static_cast<void*>(pResource));
  }
}

void ResourceDetector::RegisterColorFromClear(ID3D12Resource *pResource) {
  if (!pResource)
    return;
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  if (m_bestColor.Get() == pResource)
    return;

  D3D12_RESOURCE_DESC desc = pResource->GetDesc();

  // For AC Valhalla, avoid tagging small UI RTs as the main color buffer
  if (desc.Width < 1280)
    return;

  m_bestColorScore = 2.5f; // High confidence
  m_bestColor = pResource;

  if (!ConfigManager::Get().Data().system.quietResourceScan) {
    LOG_INFO("[DLSSG] Color IDENTIFIED via Clear: {}x{} Fmt:{} Ptr:{:p}",
             desc.Width, desc.Height, static_cast<int>(desc.Format),
             static_cast<void*>(pResource));
  }
}

void ResourceDetector::RegisterExposure(ID3D12Resource *pResource) {
  if (!pResource)
    return;
  D3D12_RESOURCE_DESC desc = pResource->GetDesc();
  // Exposure textures are very small HDR textures (1x1 to 4x4)
  if (desc.Width > 4 || desc.Height > 4)
    return;

  // Only HDR formats for exposure
  if (desc.Format != DXGI_FORMAT_R32_FLOAT &&
      desc.Format != DXGI_FORMAT_R16_FLOAT &&
      desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
      desc.Format != DXGI_FORMAT_R32G32B32A32_FLOAT &&
      desc.Format != DXGI_FORMAT_R11G11B10_FLOAT &&
      desc.Format != DXGI_FORMAT_R16G16_FLOAT &&
      desc.Format != DXGI_FORMAT_R32_TYPELESS &&
      desc.Format != DXGI_FORMAT_R16_TYPELESS)
    return;

  std::unique_lock<std::shared_mutex> lock(m_mutex);
  if (m_exposureResource.Get() == pResource)
    return;
  // Prefer 1x1 R32_FLOAT (most common exposure format)
  if (m_exposureResource) {
    D3D12_RESOURCE_DESC curDesc = m_exposureResource->GetDesc();
    if (curDesc.Width == 1 && curDesc.Height == 1 &&
        (desc.Width > 1 || desc.Height > 1))
      return;  // Keep existing 1x1 over larger
  }
  m_exposureResource = pResource;
  LOG_INFO("[Scanner] Exposure Resource Identified: {}x{} Fmt:{} Ptr:{:p}",
           desc.Width, desc.Height, static_cast<int>(desc.Format),
           static_cast<void*>(pResource));
}

void ResourceDetector::RegisterMotionVectorFromView(ID3D12Resource *pResource,
                                                    DXGI_FORMAT viewFormat) {
  if (!pResource)
    return;
  D3D12_RESOURCE_DESC desc = pResource->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return;
  if (desc.Width < 64 || desc.Height < 64)
    return;
  float mvScore = ScoreMotionVector(desc);
  if (mvScore <= 0.0f) {
    // Allow view-format hint when resource is typeless or unknown.
    if (viewFormat == DXGI_FORMAT_R16G16_FLOAT ||
        viewFormat == DXGI_FORMAT_R16G16_UNORM ||
        viewFormat == DXGI_FORMAT_R16G16_SNORM ||
        viewFormat == DXGI_FORMAT_R16G16_SINT ||
        viewFormat == DXGI_FORMAT_R16G16_UINT ||
        viewFormat == DXGI_FORMAT_R16G16_TYPELESS ||
        viewFormat == DXGI_FORMAT_R32G32_FLOAT ||
        viewFormat == DXGI_FORMAT_R32G32_SINT ||
        viewFormat == DXGI_FORMAT_R32G32_UINT) {
      mvScore = 0.6f;
    } else {
      return;
    }
  }
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  if (viewFormat != DXGI_FORMAT_UNKNOWN) {
    m_motionFormatOverrides[pResource] = viewFormat;
  }
  if (m_bestMotion.Get() == pResource)
    return;
  m_bestMotionScore = 2.0f;
  m_bestMotion = pResource;
  bool quietScan = ConfigManager::Get().Data().system.quietResourceScan;
  if (!quietScan) {
    LOG_INFO("[DLSSG] MV view bound: {}x{} Fmt:{} Ptr:{:p}", desc.Width,
             desc.Height, static_cast<int>(desc.Format), static_cast<void*>(pResource));
  }
}

DXGI_FORMAT
ResourceDetector::GetDepthFormatOverride(ID3D12Resource *pResource) {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_depthFormatOverrides.find(pResource);
  return it != m_depthFormatOverrides.end() ? it->second : DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT
ResourceDetector::GetMotionFormatOverride(ID3D12Resource *pResource) {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_motionFormatOverrides.find(pResource);
  return it != m_motionFormatOverrides.end() ? it->second : DXGI_FORMAT_UNKNOWN;
}

void ResourceDetector::RegisterResource(ID3D12Resource *pResource,
                                        bool allowDuplicate) {
  if (!pResource)
    return;

  // OPTIMIZATION: Check if we've already processed this resource in the current
  // "generation"
  uint64_t currentFrame = m_frameCount.load(std::memory_order_acquire);
  uint64_t currentGen = currentFrame / resource_config::kCleanupInterval;

  uint64_t lastSeenGen = 0;
  UINT dataSize = sizeof(uint64_t);
  if (!allowDuplicate && SUCCEEDED(pResource->GetPrivateData(
                             RD_GEN_TAG, &dataSize, &lastSeenGen))) {
    if (lastSeenGen == currentGen) {
      return; // Already processed this generation
    }
  }

  // Mark it as seen for this generation immediately
  if (FAILED(pResource->SetPrivateData(RD_GEN_TAG, sizeof(uint64_t),
                                       &currentGen))) {
    static uint32_t s_tagFailLog = 0;
    if (s_tagFailLog++ % 300 == 0) {
      LOG_WARN("[RES] SetPrivateData failed for resource {:p}",
               static_cast<void*>(pResource));
    }
    return;
  }

  D3D12_RESOURCE_DESC desc = pResource->GetDesc();

  // Check for small Exposure textures (up to 4x4)
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
      desc.Width <= 4 && desc.Height <= 4) {
    RegisterExposure(pResource);
    return;
  }

  // Ignore non-texture resources
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return;

  // Ignore small buffers (likely UI icons or constant buffers)
  if (desc.Width < 64 || desc.Height < 64)
    return;

  // Score for Motion Vectors
  float mvScore = ScoreMotionVector(desc);

  // Score for Depth
  float depthScore = ScoreDepth(desc);

  // Score for Color (Input)
  float colorScore = ScoreColor(desc);

  if (mvScore < 0.5f && depthScore < 0.5f && colorScore < 0.5f) {
    static uint32_t s_rejectLog = 0;
    if (s_rejectLog++ % 300 == 0) {
      LOG_DEBUG("[RES] Rejected resource {:p} {}x{} fmt:{} mv={:.2f} "
                "depth={:.2f} color={:.2f}",
                static_cast<void*>(pResource), desc.Width, desc.Height,
                static_cast<int>(desc.Format), mvScore, depthScore, colorScore);
    }
    return;
  }

  std::unique_lock<std::shared_mutex> lock(m_mutex);

  static uint32_t s_acceptLog = 0;
  if (s_acceptLog++ % 120 == 0) {
    LOG_INFO(
        "[RES] Candidate {:p} {}x{} fmt:{} mv={:.2f} depth={:.2f} color={:.2f}",
        static_cast<void*>(pResource), desc.Width, desc.Height,
        static_cast<int>(desc.Format), mvScore, depthScore, colorScore);
  }

  // Eviction cap: sort by score, keep top 200 to prevent unbounded growth
  auto evict = [](std::vector<ResourceCandidate> &list) {
    if (list.size() > 500) {
      std::sort(list.begin(), list.end(),
                [](const ResourceCandidate &a, const ResourceCandidate &b) {
                  return a.score > b.score;
                });
      list.resize(200);
    }
  };
  evict(m_colorCandidates);
  evict(m_motionCandidates);
  evict(m_depthCandidates);

  bool quietScan = ConfigManager::Get().Data().system.quietResourceScan;
  if (mvScore >= 0.5f) {
    bool found = false;
    ResourceCandidate *target = nullptr;
    uint64_t lastSeenFrame = currentFrame;
    for (auto &cand : m_motionCandidates) {
      if (cand.pResource.Get() == pResource) {
        lastSeenFrame = cand.lastFrameSeen;
        cand.lastFrameSeen = currentFrame;
        cand.score = mvScore;
        cand.seenCount =
            std::min<uint32_t>(cand.seenCount + 1, resource_config::kFrequencyHitCap);
        target = &cand;
        found = true;
        break;
      }
    }
    if (!found) {
      m_motionCandidates.push_back({pResource, mvScore, desc, currentFrame, 1});
      target = &m_motionCandidates.back();
      if (!quietScan) {
        LOG_DEBUG("Found MV Candidate: {}x{} Fmt:{} Score:{:.2f}", desc.Width,
                  desc.Height, static_cast<int>(desc.Format), mvScore);
      }
    }
    float adjusted = mvScore;
    if (currentFrame - lastSeenFrame <= resource_config::kRecencyFrames)
      adjusted += resource_config::kRecencyBonus;
    if (target) {
      adjusted +=
          resource_config::kFrequencyBonus *
          (std::min<uint32_t>(target->seenCount, resource_config::kFrequencyHitCap) /
           static_cast<float>(resource_config::kFrequencyHitCap));
    }
    if (adjusted >= m_bestMotionScore) {
      m_bestMotionScore = adjusted;
      m_bestMotion = pResource;
      if (!quietScan) {
        LOG_INFO("[DLSSG] New BEST MV: {}x{} Fmt:{} Score:{:.2f} Ptr:{:p}",
                 desc.Width, desc.Height, static_cast<int>(desc.Format),
                 adjusted, static_cast<void*>(pResource));
      }
    }
  }

  if (depthScore >= 0.5f) {
    bool found = false;
    ResourceCandidate *target = nullptr;
    uint64_t lastSeenFrame = currentFrame;
    for (auto &cand : m_depthCandidates) {
      if (cand.pResource.Get() == pResource) {
        lastSeenFrame = cand.lastFrameSeen;
        cand.lastFrameSeen = currentFrame;
        cand.score = depthScore;
        cand.seenCount =
            std::min<uint32_t>(cand.seenCount + 1, resource_config::kFrequencyHitCap);
        target = &cand;
        found = true;
        break;
      }
    }
    if (!found) {
      m_depthCandidates.push_back(
          {pResource, depthScore, desc, currentFrame, 1});
      target = &m_depthCandidates.back();
      if (!quietScan) {
        LOG_DEBUG("Found Depth Candidate: {}x{} Fmt:{} Score:{:.2f}",
                  desc.Width, desc.Height, static_cast<int>(desc.Format),
                  depthScore);
      }
    }
    float adjusted = depthScore;
    if (currentFrame - lastSeenFrame <= resource_config::kRecencyFrames)
      adjusted += resource_config::kRecencyBonus;
    if (target) {
      adjusted +=
          resource_config::kFrequencyBonus *
          (std::min<uint32_t>(target->seenCount, resource_config::kFrequencyHitCap) /
           static_cast<float>(resource_config::kFrequencyHitCap));
    }
    if (adjusted >= m_bestDepthScore) {
      m_bestDepthScore = adjusted;
      m_bestDepth = pResource;
      if (!quietScan) {
        LOG_INFO("[DLSSG] New BEST Depth: {}x{} Fmt:{} Score:{:.2f} Ptr:{:p}",
                 desc.Width, desc.Height, static_cast<int>(desc.Format),
                 adjusted, static_cast<void*>(pResource));
      }
    }
  }

  if (colorScore >= 0.5f) {
    bool found = false;
    ResourceCandidate *target = nullptr;
    uint64_t lastSeenFrame = currentFrame;
    for (auto &cand : m_colorCandidates) {
      if (cand.pResource.Get() == pResource) {
        lastSeenFrame = cand.lastFrameSeen;
        cand.lastFrameSeen = currentFrame;
        cand.score = colorScore;
        cand.seenCount =
            std::min<uint32_t>(cand.seenCount + 1, resource_config::kFrequencyHitCap);
        target = &cand;
        found = true;
        break;
      }
    }
    if (!found) {
      m_colorCandidates.push_back(
          {pResource, colorScore, desc, currentFrame, 1});
      target = &m_colorCandidates.back();
      if (!quietScan) {
        LOG_INFO("[DLSSG] Found Color Candidate: {}x{} Fmt:{} Score:{:.2f}",
                 desc.Width, desc.Height, static_cast<int>(desc.Format),
                 colorScore);
      }
    }
    float adjusted = colorScore;
    if (currentFrame - lastSeenFrame <= resource_config::kRecencyFrames)
      adjusted += resource_config::kRecencyBonus;
    if (target) {
      adjusted +=
          resource_config::kFrequencyBonus *
          (std::min<uint32_t>(target->seenCount, resource_config::kFrequencyHitCap) /
           static_cast<float>(resource_config::kFrequencyHitCap));
    }
    if (adjusted >= m_bestColorScore) {
      m_bestColorScore = adjusted;
      m_bestColor = pResource;
      if (!quietScan) {
        LOG_INFO("[DLSSG] New BEST Color: {}x{} Fmt:{} Score:{:.2f} Ptr:{:p}",
                 desc.Width, desc.Height, static_cast<int>(desc.Format),
                 adjusted, static_cast<void*>(pResource));
      }
    }
  }
}

float ResourceDetector::ScoreMotionVector(const D3D12_RESOURCE_DESC &desc) {
  float score = 0.0f;

  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return 0.0f;
  if (desc.Width < 64 || desc.Height < 64)
    return 0.0f;

  // Motion vectors are usually R16G16 or R32G32
  // AnvilNext engine (AC Valhalla) may use R16G16B16A16_SNORM or packed formats
  switch (desc.Format) {
  // Primary MV formats (highest confidence)
  case DXGI_FORMAT_R16G16_FLOAT:       score += 0.8f; break;
  case DXGI_FORMAT_R16G16_SNORM:       score += 0.7f; break;
  case DXGI_FORMAT_R32G32_FLOAT:       score += 0.7f; break;
  case DXGI_FORMAT_R16G16_UNORM:       score += 0.6f; break;
  case DXGI_FORMAT_R16G16_TYPELESS:    score += 0.6f; break;
  case DXGI_FORMAT_R32G32_TYPELESS:    score += 0.55f; break;
  // Secondary MV formats
  case DXGI_FORMAT_R16G16B16A16_SNORM: score += 0.65f; break;  // AnvilNext packed MVs
  case DXGI_FORMAT_R16G16B16A16_FLOAT: score += 0.5f; break;   // Some engines pack MV+extras
  case DXGI_FORMAT_R16G16_SINT:        score += 0.5f; break;
  case DXGI_FORMAT_R16G16_UINT:        score += 0.5f; break;
  case DXGI_FORMAT_R32G32_SINT:        score += 0.4f; break;
  case DXGI_FORMAT_R32G32_UINT:        score += 0.4f; break;
  case DXGI_FORMAT_R11G11B10_FLOAT:    score += 0.4f; break;   // Rare but seen
  // Additional formats for broader engine support
  case DXGI_FORMAT_R8G8_SNORM:         score += 0.45f; break;  // Low-precision MVs
  case DXGI_FORMAT_R8G8_UNORM:         score += 0.35f; break;
  case DXGI_FORMAT_R32G32B32A32_FLOAT: score += 0.4f; break;   // Full-precision MV+depth
  case DXGI_FORMAT_R16G16B16A16_UINT:  score += 0.35f; break;  // Packed integer MVs
  case DXGI_FORMAT_R16G16B16A16_SINT:  score += 0.35f; break;
  case DXGI_FORMAT_R16G16B16A16_UNORM: score += 0.4f; break;
  case DXGI_FORMAT_R16G16B16A16_TYPELESS: score += 0.45f; break;
  case DXGI_FORMAT_R32G32B32A32_TYPELESS: score += 0.35f; break;
  default:
    return 0.0f; // Not a likely MV format
  }

  // Flags: MVs are typically generated via compute (UAV) in modern engines
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    score += 0.3f;
  else
    score -= 0.1f;

  // Also allow RT since some engines render MVs via pixel shader
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    score += 0.15f;

  if (desc.SampleDesc.Count > 1)
    score -= resource_config::kMsaaPenalty;
  if (desc.MipLevels > 1)
    score -= resource_config::kMipPenalty;
  if (m_expectedWidth > 0 && m_expectedHeight > 0) {
    float ratioW =
        static_cast<float>(desc.Width) / static_cast<float>(m_expectedWidth);
    float ratioH =
        static_cast<float>(desc.Height) / static_cast<float>(m_expectedHeight);
    if (ratioW >= resource_config::kExpectedMinRatio &&
        ratioW <= resource_config::kExpectedMaxRatio &&
        ratioH >= resource_config::kExpectedMinRatio &&
        ratioH <= resource_config::kExpectedMaxRatio) {
      score += resource_config::kExpectedMatchBonus;
    } else {
      score -= resource_config::kExpectedMatchBonus;
    }
  }

  return score;
}

float ResourceDetector::ScoreDepth(const D3D12_RESOURCE_DESC &desc) {
  float score = 0.0f;

  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return 0.0f;
  if (desc.Width < 64 || desc.Height < 64)
    return 0.0f;

  // Standard depth formats
  switch (desc.Format) {
  case DXGI_FORMAT_D32_FLOAT:              score += 0.9f; break;
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:   score += 0.85f; break;
  case DXGI_FORMAT_R32_FLOAT:              score += 0.7f; break;  // Read-only depth copy
  case DXGI_FORMAT_D24_UNORM_S8_UINT:      score += 0.6f; break;
  case DXGI_FORMAT_D16_UNORM:              score += 0.5f; break;
  // Typeless variants (common in modern engines)
  case DXGI_FORMAT_R32_TYPELESS:           score += 0.75f; break;
  case DXGI_FORMAT_R32G8X24_TYPELESS:      score += 0.7f; break;  // D32+S8
  case DXGI_FORMAT_R24G8_TYPELESS:         score += 0.6f; break;
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:  score += 0.55f; break;
  case DXGI_FORMAT_R16_TYPELESS:           score += 0.5f; break;  // D16
  case DXGI_FORMAT_R16_UNORM:              score += 0.45f; break;  // D16 read-only
  // SRV-compatible depth formats (engines that copy depth to SRV)
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: score += 0.65f; break;
  case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:  score += 0.5f; break;
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:     score += 0.4f; break;
  default:
    return 0.0f;
  }

  // Depth-stencil flag is a strong indicator
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    score += 0.3f;
  // Deny-SRV flag often accompanies depth-only resources
  if (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
    score += 0.1f;
  if (desc.SampleDesc.Count > 1)
    score -= resource_config::kMsaaPenalty;
  if (desc.MipLevels > 1)
    score -= resource_config::kMipPenalty;
  if (m_expectedWidth > 0 && m_expectedHeight > 0) {
    float ratioW =
        static_cast<float>(desc.Width) / static_cast<float>(m_expectedWidth);
    float ratioH =
        static_cast<float>(desc.Height) / static_cast<float>(m_expectedHeight);
    if (ratioW >= resource_config::kExpectedMinRatio &&
        ratioW <= resource_config::kExpectedMaxRatio &&
        ratioH >= resource_config::kExpectedMinRatio &&
        ratioH <= resource_config::kExpectedMaxRatio) {
      score += resource_config::kExpectedMatchBonus;
    } else {
      score -= resource_config::kExpectedMatchBonus;
    }
  }

  return score;
}

float ResourceDetector::ScoreColor(const D3D12_RESOURCE_DESC &desc) {
  float score = 0.0f;

  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    return 0.0f;
  if (desc.Width < 64 || desc.Height < 64)
    return 0.0f;

  switch (desc.Format) {
  // HDR Float formats (highest priority for modern rendering)
  case DXGI_FORMAT_R16G16B16A16_FLOAT:     score += 0.7f; break;
  case DXGI_FORMAT_R11G11B10_FLOAT:        score += 0.65f; break;  // Common HDR RT
  case DXGI_FORMAT_R10G10B10A2_UNORM:      score += 0.6f; break;   // HDR10
  case DXGI_FORMAT_R32G32B32A32_FLOAT:     score += 0.55f; break;  // Full-precision HDR
  // Standard Backbuffer Formats
  case DXGI_FORMAT_R8G8B8A8_UNORM:         score += 0.5f; break;
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:    score += 0.5f; break;
  case DXGI_FORMAT_B8G8R8A8_UNORM:         score += 0.5f; break;
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:    score += 0.5f; break;
  // Additional formats for broader engine support
  case DXGI_FORMAT_R16G16B16A16_UNORM:     score += 0.55f; break;
  case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM: score += 0.5f; break;
  // Typeless variants (engines create typeless then cast to SRV/RTV)
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:      score += 0.4f; break;
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:      score += 0.4f; break;
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:  score += 0.45f; break;
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:   score += 0.4f; break;
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:  score += 0.35f; break;
  default:
    return 0.0f;
  }

  // Must be Render Target or match typical RT resolution/format
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
    score += 0.3f;
  } else if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
    // Some engines use UAV for post-processing output
    score += 0.15f;
  } else {
    // If it's a known RT format and large, give it a chance
    if (desc.Width > 1280)
      score += 0.1f;
  }
  if (desc.SampleDesc.Count > 1)
    score -= resource_config::kMsaaPenalty;
  if (desc.MipLevels > 1)
    score -= resource_config::kMipPenalty;
  if (m_expectedWidth > 0 && m_expectedHeight > 0) {
    float ratioW =
        static_cast<float>(desc.Width) / static_cast<float>(m_expectedWidth);
    float ratioH =
        static_cast<float>(desc.Height) / static_cast<float>(m_expectedHeight);
    if (ratioW >= resource_config::kExpectedMinRatio &&
        ratioW <= resource_config::kExpectedMaxRatio &&
        ratioH >= resource_config::kExpectedMinRatio &&
        ratioH <= resource_config::kExpectedMaxRatio) {
      score += resource_config::kExpectedMatchBonus;
    } else {
      score -= resource_config::kExpectedMatchBonus;
    }
  }

  return score;
}

ID3D12Resource *ResourceDetector::GetBestMotionVectorCandidate() {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_bestMotion.Get();
}

ID3D12Resource *ResourceDetector::GetBestDepthCandidate() {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  return m_bestDepth.Get();
}

ID3D12Resource *ResourceDetector::GetBestColorCandidate() {
  std::shared_lock<std::shared_mutex> lock(m_mutex);

  // Heuristic: If we have a Motion Vector, prefer a Color buffer with matching
  // resolution. This solves issues where the game upscales (Color=4K) but MVs
  // are native (e.g. 1080p).
  if (m_bestMotion) {
    D3D12_RESOURCE_DESC mvDesc = m_bestMotion->GetDesc();
    ID3D12Resource *bestMatch = nullptr;
    float bestMatchScore = 0.0f;

    for (const auto &cand : m_colorCandidates) {
      if (cand.desc.Width == mvDesc.Width &&
          cand.desc.Height == mvDesc.Height) {
        if (cand.score > bestMatchScore && cand.score > 0.6f) {
          bestMatchScore = cand.score;
          bestMatch = cand.pResource.Get();
        }
      }
    }

    if (bestMatch)
      return bestMatch;
  }

  if (m_expectedWidth > 0 && m_expectedHeight > 0) {
    ID3D12Resource *bestMatch = nullptr;
    float bestMatchScore = 0.0f;
    for (const auto &cand : m_colorCandidates) {
      float ratioW = static_cast<float>(cand.desc.Width) /
                     static_cast<float>(m_expectedWidth);
      float ratioH = static_cast<float>(cand.desc.Height) /
                     static_cast<float>(m_expectedHeight);
      if (ratioW >= resource_config::kExpectedMinRatio &&
          ratioW <= resource_config::kExpectedMaxRatio &&
          ratioH >= resource_config::kExpectedMinRatio &&
          ratioH <= resource_config::kExpectedMaxRatio) {
        if (cand.score > bestMatchScore) {
          bestMatchScore = cand.score;
          bestMatch = cand.pResource.Get();
        }
      }
    }
    if (bestMatch)
      return bestMatch;
  }

  return m_bestColor.Get();
}

uint64_t ResourceDetector::GetFrameCount() {
  // Lock-free read â€” acquire ensures we see at least the most recent increment
  return m_frameCount.load(std::memory_order_acquire);
}

std::string ResourceDetector::GetDebugInfo() {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::stringstream ss;
  ss << "=== RESOURCE DETECTOR DEBUG ===\r\n";
  ss << "Frame: " << m_frameCount << "\r\n\r\n";

  auto printList = [&](const char *name,
                       const std::vector<ResourceCandidate> &list) {
    ss << "--- " << name << " (" << list.size() << ") ---\r\n";
    for (const auto &c : list) {
      ss << "Ptr: " << static_cast<void*>(c.pResource.Get()) << " | " << c.desc.Width << "x"
         << c.desc.Height << " | Fmt: " << c.desc.Format
         << " | Score: " << std::fixed << std::setprecision(2) << c.score
         << " | Hits: " << c.seenCount << " | Last: " << c.lastFrameSeen
         << "\r\n";
    }
    ss << "\r\n";
  };

  printList("Color Candidates", m_colorCandidates);
  printList("Depth Candidates", m_depthCandidates);
  printList("Motion Vec Candidates", m_motionCandidates);

  return ss.str();
}

void ResourceDetector::LogDebugInfo() {
  std::string info = GetDebugInfo();
  // Breakdown lines to avoid massive log entries
  std::stringstream ss(info);
  std::string line;
  while (std::getline(ss, line)) {
    if (!line.empty())
      LOG_INFO("[MEM] {}", line);
  }
}

