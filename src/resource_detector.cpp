#include "resource_detector.h"
#include "logger.h"
#include "dlss4_config.h"
#include "config_manager.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

ResourceDetector& ResourceDetector::Get() {
    static ResourceDetector instance;
    return instance;
}

void ResourceDetector::NewFrame() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frameCount++;
    const uint64_t currentFrame = m_frameCount.load();
    if (!m_motionCandidates.empty()) {
        m_motionCandidates.erase(std::remove_if(m_motionCandidates.begin(), m_motionCandidates.end(),
            [currentFrame](const ResourceCandidate& cand) {
                return (currentFrame - cand.lastFrameSeen) > RESOURCE_STALE_FRAMES;
            }), m_motionCandidates.end());
    }
    if (!m_depthCandidates.empty()) {
        m_depthCandidates.erase(std::remove_if(m_depthCandidates.begin(), m_depthCandidates.end(),
            [currentFrame](const ResourceCandidate& cand) {
                return (currentFrame - cand.lastFrameSeen) > RESOURCE_STALE_FRAMES;
            }), m_depthCandidates.end());
    }
    if (!m_colorCandidates.empty()) {
        m_colorCandidates.erase(std::remove_if(m_colorCandidates.begin(), m_colorCandidates.end(),
            [currentFrame](const ResourceCandidate& cand) {
                return (currentFrame - cand.lastFrameSeen) > RESOURCE_STALE_FRAMES;
            }), m_colorCandidates.end());
    }
    
    // Periodically clear old candidates to adapt to resolution changes
    if (m_frameCount % RESOURCE_CLEANUP_INTERVAL == 0) {
        if (!m_bestMotion || !m_bestDepth || !m_bestColor) {
            m_motionCandidates.clear();
            m_depthCandidates.clear();
            m_colorCandidates.clear();
            m_bestMotion = nullptr;
            m_bestDepth = nullptr;
            m_bestColor = nullptr;
            m_bestMotionScore = 0.0f;
            m_bestDepthScore = 0.0f;
            m_bestColorScore = 0.0f;
            m_depthFormatOverrides.clear();
            m_motionFormatOverrides.clear();
            LOG_INFO("Resource detector cache cleared (Frame %llu)", m_frameCount.load());
        }
    }
}

void ResourceDetector::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_motionCandidates.clear();
    m_depthCandidates.clear();
    m_colorCandidates.clear();
    m_bestMotion = nullptr;
    m_bestDepth = nullptr;
    m_bestColor = nullptr;
    m_bestMotionScore = 0.0f;
    m_bestDepthScore = 0.0f;
    m_bestColorScore = 0.0f;
    m_depthFormatOverrides.clear();
    m_motionFormatOverrides.clear();
    m_expectedWidth = 0;
    m_expectedHeight = 0;
    LOG_INFO("Resource detector explicitly cleared.");
}

void ResourceDetector::SetExpectedDimensions(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_expectedWidth = width;
    m_expectedHeight = height;
}

// Define a GUID for our tag: {25CDDAA4-B1C6-41E5-9C52-FE69FC2E6A3D}
static const GUID RD_GEN_TAG = { 0x25cddaa4, 0xb1c6, 0x41e5, { 0x9c, 0x52, 0xfe, 0x69, 0xfc, 0x2e, 0x6a, 0x3d } };

void ResourceDetector::RegisterResource(ID3D12Resource* pResource) {
    RegisterResource(pResource, false);
}

void ResourceDetector::RegisterDepthFromView(ID3D12Resource* pResource, DXGI_FORMAT viewFormat) {
    if (!pResource) return;
    D3D12_RESOURCE_DESC desc = pResource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    if (desc.Width < 64 || desc.Height < 64) return;
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && viewFormat == DXGI_FORMAT_UNKNOWN) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (viewFormat != DXGI_FORMAT_UNKNOWN) {
        m_depthFormatOverrides[pResource] = viewFormat;
    }
    if (m_bestDepth.Get() == pResource) return;
    m_bestDepthScore = 2.0f;
    m_bestDepth = pResource;
    bool quietScan = ConfigManager::Get().Data().quietResourceScan;
    if (!quietScan) {
        LOG_INFO("[DLSSG] Depth view bound: %dx%d Fmt:%d Ptr:%p", desc.Width, desc.Height, desc.Format, pResource);
    }
}

void ResourceDetector::RegisterMotionVectorFromView(ID3D12Resource* pResource, DXGI_FORMAT viewFormat) {
    if (!pResource) return;
    D3D12_RESOURCE_DESC desc = pResource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    if (desc.Width < 64 || desc.Height < 64) return;
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
    std::lock_guard<std::mutex> lock(m_mutex);
    if (viewFormat != DXGI_FORMAT_UNKNOWN) {
        m_motionFormatOverrides[pResource] = viewFormat;
    }
    if (m_bestMotion.Get() == pResource) return;
    m_bestMotionScore = 2.0f;
    m_bestMotion = pResource;
    bool quietScan = ConfigManager::Get().Data().quietResourceScan;
    if (!quietScan) {
        LOG_INFO("[DLSSG] MV view bound: %dx%d Fmt:%d Ptr:%p", desc.Width, desc.Height, desc.Format, pResource);
    }
}

DXGI_FORMAT ResourceDetector::GetDepthFormatOverride(ID3D12Resource* pResource) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_depthFormatOverrides.find(pResource);
    return it != m_depthFormatOverrides.end() ? it->second : DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT ResourceDetector::GetMotionFormatOverride(ID3D12Resource* pResource) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_motionFormatOverrides.find(pResource);
    return it != m_motionFormatOverrides.end() ? it->second : DXGI_FORMAT_UNKNOWN;
}

void ResourceDetector::RegisterResource(ID3D12Resource* pResource, bool allowDuplicate) {
    if (!pResource) return;

    // OPTIMIZATION: Check if we've already processed this resource in the current "generation"
    uint64_t currentFrame = m_frameCount.load(std::memory_order_relaxed);
    uint64_t currentGen = currentFrame / RESOURCE_CLEANUP_INTERVAL;
    
    uint64_t lastSeenGen = 0;
    UINT dataSize = sizeof(uint64_t);
    if (!allowDuplicate && SUCCEEDED(pResource->GetPrivateData(RD_GEN_TAG, &dataSize, &lastSeenGen))) {
        if (lastSeenGen == currentGen) {
            return; // Already processed this generation
        }
    }

    // Mark it as seen for this generation immediately
    if (FAILED(pResource->SetPrivateData(RD_GEN_TAG, sizeof(uint64_t), &currentGen))) {
        return;
    }

    D3D12_RESOURCE_DESC desc = pResource->GetDesc();
    
    // Ignore non-texture resources
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    
    // Ignore small buffers (likely UI icons or constant buffers)
    if (desc.Width < 64 || desc.Height < 64) return;

    // Score for Motion Vectors
    float mvScore = ScoreMotionVector(desc);

    // Score for Depth
    float depthScore = ScoreDepth(desc);

    // Score for Color (Input)
    float colorScore = ScoreColor(desc);
    
    if (mvScore < 0.5f && depthScore < 0.5f && colorScore < 0.5f) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    // Safety cap to prevent unbounded growth if GetPrivateData fails
    if (m_colorCandidates.size() > 500) m_colorCandidates.clear();
    if (m_motionCandidates.size() > 500) m_motionCandidates.clear();
    if (m_depthCandidates.size() > 500) m_depthCandidates.clear();

    bool quietScan = ConfigManager::Get().Data().quietResourceScan;
    if (mvScore >= 0.5f) {
        bool found = false;
        ResourceCandidate* target = nullptr;
        uint64_t lastSeenFrame = currentFrame;
        for (auto& cand : m_motionCandidates) {
            if (cand.pResource.Get() == pResource) {
                lastSeenFrame = cand.lastFrameSeen;
                cand.lastFrameSeen = currentFrame;
                cand.score = mvScore;
                cand.seenCount = std::min<uint32_t>(cand.seenCount + 1, RESOURCE_FREQUENCY_HIT_CAP);
                target = &cand;
                found = true;
                break;
            }
        }
        if (!found) {
            m_motionCandidates.push_back({pResource, mvScore, desc, currentFrame, 1});
            target = &m_motionCandidates.back();
            if (!quietScan) {
                LOG_DEBUG("Found MV Candidate: %dx%d Fmt:%d Score:%.2f", 
                    desc.Width, desc.Height, desc.Format, mvScore);
            }
        }
        float adjusted = mvScore;
        if (currentFrame - lastSeenFrame <= RESOURCE_RECENCY_FRAMES) adjusted += RESOURCE_RECENCY_BONUS;
        if (target) {
            adjusted += RESOURCE_FREQUENCY_BONUS * (std::min<uint32_t>(target->seenCount, RESOURCE_FREQUENCY_HIT_CAP) / (float)RESOURCE_FREQUENCY_HIT_CAP);
        }
        if (adjusted >= m_bestMotionScore) {
            m_bestMotionScore = adjusted;
            m_bestMotion = pResource;
            if (!quietScan) {
                LOG_INFO("[DLSSG] New BEST MV: %dx%d Fmt:%d Score:%.2f Ptr:%p", 
                    desc.Width, desc.Height, desc.Format, adjusted, pResource);
            }
        }
    }

    if (depthScore >= 0.5f) {
        bool found = false;
        ResourceCandidate* target = nullptr;
        uint64_t lastSeenFrame = currentFrame;
        for (auto& cand : m_depthCandidates) {
            if (cand.pResource.Get() == pResource) {
                lastSeenFrame = cand.lastFrameSeen;
                cand.lastFrameSeen = currentFrame;
                cand.score = depthScore;
                cand.seenCount = std::min<uint32_t>(cand.seenCount + 1, RESOURCE_FREQUENCY_HIT_CAP);
                target = &cand;
                found = true;
                break;
            }
        }
        if (!found) {
            m_depthCandidates.push_back({pResource, depthScore, desc, currentFrame, 1});
            target = &m_depthCandidates.back();
            if (!quietScan) {
                LOG_DEBUG("Found Depth Candidate: %dx%d Fmt:%d Score:%.2f",
                    desc.Width, desc.Height, desc.Format, depthScore);
            }
        }
        float adjusted = depthScore;
        if (currentFrame - lastSeenFrame <= RESOURCE_RECENCY_FRAMES) adjusted += RESOURCE_RECENCY_BONUS;
        if (target) {
            adjusted += RESOURCE_FREQUENCY_BONUS * (std::min<uint32_t>(target->seenCount, RESOURCE_FREQUENCY_HIT_CAP) / (float)RESOURCE_FREQUENCY_HIT_CAP);
        }
        if (adjusted >= m_bestDepthScore) {
            m_bestDepthScore = adjusted;
            m_bestDepth = pResource;
            if (!quietScan) {
                LOG_INFO("[DLSSG] New BEST Depth: %dx%d Fmt:%d Score:%.2f Ptr:%p", 
                    desc.Width, desc.Height, desc.Format, adjusted, pResource);
            }
        }
    }

    if (colorScore >= 0.5f) {
        bool found = false;
        ResourceCandidate* target = nullptr;
        uint64_t lastSeenFrame = currentFrame;
        for (auto& cand : m_colorCandidates) {
            if (cand.pResource.Get() == pResource) {
                lastSeenFrame = cand.lastFrameSeen;
                cand.lastFrameSeen = currentFrame;
                cand.score = colorScore;
                cand.seenCount = std::min<uint32_t>(cand.seenCount + 1, RESOURCE_FREQUENCY_HIT_CAP);
                target = &cand;
                found = true;
                break;
            }
        }
        if (!found) {
            m_colorCandidates.push_back({pResource, colorScore, desc, currentFrame, 1});
            target = &m_colorCandidates.back();
            if (!quietScan) {
                LOG_INFO("[DLSSG] Found Color Candidate: %dx%d Fmt:%d Score:%.2f", 
                    desc.Width, desc.Height, desc.Format, colorScore);
            }
        }
        float adjusted = colorScore;
        if (currentFrame - lastSeenFrame <= RESOURCE_RECENCY_FRAMES) adjusted += RESOURCE_RECENCY_BONUS;
        if (target) {
            adjusted += RESOURCE_FREQUENCY_BONUS * (std::min<uint32_t>(target->seenCount, RESOURCE_FREQUENCY_HIT_CAP) / (float)RESOURCE_FREQUENCY_HIT_CAP);
        }
        if (adjusted >= m_bestColorScore) {
            m_bestColorScore = adjusted;
            m_bestColor = pResource;
            if (!quietScan) {
                LOG_INFO("[DLSSG] New BEST Color: %dx%d Fmt:%d Score:%.2f Ptr:%p", 
                    desc.Width, desc.Height, desc.Format, adjusted, pResource);
            }
        }
    }
}

float ResourceDetector::ScoreMotionVector(const D3D12_RESOURCE_DESC& desc) {
    float score = 0.0f;

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return 0.0f;
    if (desc.Width < 64 || desc.Height < 64) return 0.0f;

    // Motion vectors are usually R16G16
    if (desc.Format == DXGI_FORMAT_R16G16_FLOAT) score += 0.8f;
    else if (desc.Format == DXGI_FORMAT_R16G16_UNORM) score += 0.6f;
    else if (desc.Format == DXGI_FORMAT_R16G16_SNORM) score += 0.6f;
    else if (desc.Format == DXGI_FORMAT_R16G16_SINT) score += 0.5f;
    else if (desc.Format == DXGI_FORMAT_R16G16_UINT) score += 0.5f;
    else if (desc.Format == DXGI_FORMAT_R32G32_FLOAT) score += 0.4f;
    else if (desc.Format == DXGI_FORMAT_R32G32_SINT) score += 0.3f;
    else if (desc.Format == DXGI_FORMAT_R32G32_UINT) score += 0.3f;
    else if (desc.Format == DXGI_FORMAT_R16G16_TYPELESS) score += 0.5f; // Typeless
    else if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) score += 0.3f;
    else return 0.0f; // Not a likely MV format
    // Flags: often allow UAV (for compute generation)
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) score += 0.2f;
    if (desc.SampleDesc.Count > 1) score -= RESOURCE_MSAA_PENALTY;
    if (desc.MipLevels > 1) score -= RESOURCE_MIP_PENALTY;
    if (m_expectedWidth > 0 && m_expectedHeight > 0) {
        float ratioW = static_cast<float>(desc.Width) / static_cast<float>(m_expectedWidth);
        float ratioH = static_cast<float>(desc.Height) / static_cast<float>(m_expectedHeight);
        if (ratioW >= RESOURCE_EXPECTED_MIN_RATIO && ratioW <= RESOURCE_EXPECTED_MAX_RATIO &&
            ratioH >= RESOURCE_EXPECTED_MIN_RATIO && ratioH <= RESOURCE_EXPECTED_MAX_RATIO) {
            score += RESOURCE_EXPECTED_MATCH_BONUS;
        } else {
            score -= RESOURCE_EXPECTED_MATCH_BONUS;
        }
    }

    return score;
}

float ResourceDetector::ScoreDepth(const D3D12_RESOURCE_DESC& desc) {
    float score = 0.0f;
    
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return 0.0f;
    if (desc.Width < 64 || desc.Height < 64) return 0.0f;

    if (desc.Format == DXGI_FORMAT_D32_FLOAT) score += 0.9f;
    else if (desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) score += 0.8f;
    else if (desc.Format == DXGI_FORMAT_R32_FLOAT) score += 0.7f; // Read-only depth
    else if (desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) score += 0.5f;
    else if (desc.Format == DXGI_FORMAT_R32_TYPELESS) score += 0.6f; // Typeless Depth
    else if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS) score += 0.5f;
    else if (desc.Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS) score += 0.5f;
    else if (desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS) score += 0.5f;
    else return 0.0f;

    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) score += 0.2f;
    if (desc.SampleDesc.Count > 1) score -= RESOURCE_MSAA_PENALTY;
    if (desc.MipLevels > 1) score -= RESOURCE_MIP_PENALTY;
    if (m_expectedWidth > 0 && m_expectedHeight > 0) {
        float ratioW = static_cast<float>(desc.Width) / static_cast<float>(m_expectedWidth);
        float ratioH = static_cast<float>(desc.Height) / static_cast<float>(m_expectedHeight);
        if (ratioW >= RESOURCE_EXPECTED_MIN_RATIO && ratioW <= RESOURCE_EXPECTED_MAX_RATIO &&
            ratioH >= RESOURCE_EXPECTED_MIN_RATIO && ratioH <= RESOURCE_EXPECTED_MAX_RATIO) {
            score += RESOURCE_EXPECTED_MATCH_BONUS;
        } else {
            score -= RESOURCE_EXPECTED_MATCH_BONUS;
        }
    }

    return score;
}

float ResourceDetector::ScoreColor(const D3D12_RESOURCE_DESC& desc) {
    float score = 0.0f;
    
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return 0.0f;
    if (desc.Width < 64 || desc.Height < 64) return 0.0f;

    // Standard Backbuffer Formats
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) score += 0.5f;
    else if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) score += 0.5f; // Added BGRA (Format 87/91)
    else if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) score += 0.6f; // HDR
    else if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) score += 0.7f; // HDR Float
    else if (desc.Format == DXGI_FORMAT_R11G11B10_FLOAT) score += 0.6f; // Common RT format
    else return 0.0f;

    // Must be Render Target or match typical RT resolution/format
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
        score += 0.3f;
    } else {
        // If it's a known RT format and large, give it a chance
        if (desc.Width > 1280) score += 0.1f;
    }
    if (desc.SampleDesc.Count > 1) score -= RESOURCE_MSAA_PENALTY;
    if (desc.MipLevels > 1) score -= RESOURCE_MIP_PENALTY;
    if (m_expectedWidth > 0 && m_expectedHeight > 0) {
        float ratioW = static_cast<float>(desc.Width) / static_cast<float>(m_expectedWidth);
        float ratioH = static_cast<float>(desc.Height) / static_cast<float>(m_expectedHeight);
        if (ratioW >= RESOURCE_EXPECTED_MIN_RATIO && ratioW <= RESOURCE_EXPECTED_MAX_RATIO &&
            ratioH >= RESOURCE_EXPECTED_MIN_RATIO && ratioH <= RESOURCE_EXPECTED_MAX_RATIO) {
            score += RESOURCE_EXPECTED_MATCH_BONUS;
        } else {
            score -= RESOURCE_EXPECTED_MATCH_BONUS;
        }
    }

    return score;
}

ID3D12Resource* ResourceDetector::GetBestMotionVectorCandidate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bestMotion.Get();
}

ID3D12Resource* ResourceDetector::GetBestDepthCandidate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bestDepth.Get();
}

ID3D12Resource* ResourceDetector::GetBestColorCandidate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Heuristic: If we have a Motion Vector, prefer a Color buffer with matching resolution.
    // This solves issues where the game upscales (Color=4K) but MVs are native (e.g. 1080p).
    if (m_bestMotion) {
        D3D12_RESOURCE_DESC mvDesc = m_bestMotion->GetDesc();
        ID3D12Resource* bestMatch = nullptr;
        float bestMatchScore = 0.0f;

        for (const auto& cand : m_colorCandidates) {
            if (cand.desc.Width == mvDesc.Width && cand.desc.Height == mvDesc.Height) {
                if (cand.score > bestMatchScore && cand.score > 0.6f) {
                    bestMatchScore = cand.score;
                    bestMatch = cand.pResource.Get();
                }
            }
        }
        
        if (bestMatch) return bestMatch;
    }

    if (m_expectedWidth > 0 && m_expectedHeight > 0) {
        ID3D12Resource* bestMatch = nullptr;
        float bestMatchScore = 0.0f;
        for (const auto& cand : m_colorCandidates) {
            float ratioW = static_cast<float>(cand.desc.Width) / static_cast<float>(m_expectedWidth);
            float ratioH = static_cast<float>(cand.desc.Height) / static_cast<float>(m_expectedHeight);
            if (ratioW >= RESOURCE_EXPECTED_MIN_RATIO && ratioW <= RESOURCE_EXPECTED_MAX_RATIO &&
                ratioH >= RESOURCE_EXPECTED_MIN_RATIO && ratioH <= RESOURCE_EXPECTED_MAX_RATIO) {
                if (cand.score > bestMatchScore) {
                    bestMatchScore = cand.score;
                    bestMatch = cand.pResource.Get();
                }
            }
        }
        if (bestMatch) return bestMatch;
    }

    return m_bestColor.Get();
}

uint64_t ResourceDetector::GetFrameCount() {
    // Atomic load
    return m_frameCount.load();
}

std::string ResourceDetector::GetDebugInfo() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::stringstream ss;
    ss << "=== RESOURCE DETECTOR DEBUG ===\r\n";
    ss << "Frame: " << m_frameCount << "\r\n\r\n";

    auto printList = [&](const char* name, const std::vector<ResourceCandidate>& list) {
        ss << "--- " << name << " (" << list.size() << ") ---\r\n";
        for (const auto& c : list) {
            ss << "Ptr: " << (void*)c.pResource.Get() 
               << " | " << c.desc.Width << "x" << c.desc.Height 
               << " | Fmt: " << c.desc.Format 
               << " | Score: " << std::fixed << std::setprecision(2) << c.score
               << " | Hits: " << c.seenCount
               << " | Last: " << c.lastFrameSeen << "\r\n";
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
        if (!line.empty()) LOG_INFO("[MEM] %s", line.c_str());
    }
}
