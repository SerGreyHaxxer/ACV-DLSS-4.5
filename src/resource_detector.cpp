#include "resource_detector.h"
#include "logger.h"
#include <sstream>
#include <iomanip>

ResourceDetector& ResourceDetector::Get() {
    static ResourceDetector instance;
    return instance;
}

void ResourceDetector::NewFrame() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frameCount++;
    
    // Periodically clear old candidates to adapt to resolution changes
    if (m_frameCount % 900 == 0) {
        m_motionCandidates.clear();
        m_depthCandidates.clear();
        m_colorCandidates.clear();
        m_bestMotion = nullptr;
        m_bestDepth = nullptr;
        m_bestColor = nullptr;
        m_bestMotionScore = 0.0f;
        m_bestDepthScore = 0.0f;
        m_bestColorScore = 0.0f;
        LOG_INFO("Resource detector cache cleared (Frame %llu)", m_frameCount.load());
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
    LOG_INFO("Resource detector explicitly cleared.");
}

// Define a GUID for our tag: {25CDDAA4-B1C6-41E5-9C52-FE69FC2E6A3D}
static const GUID RD_GEN_TAG = { 0x25cddaa4, 0xb1c6, 0x41e5, { 0x9c, 0x52, 0xfe, 0x69, 0xfc, 0x2e, 0x6a, 0x3d } };

void ResourceDetector::RegisterResource(ID3D12Resource* pResource) {
    if (!pResource) return;

    // OPTIMIZATION: Check if we've already processed this resource in the current "generation"
    // We clear candidates every 900 frames. We can use frameCount / 900 as a generation ID.
    uint64_t currentFrame = m_frameCount.load(std::memory_order_relaxed);
    uint64_t currentGen = currentFrame / 900;
    
    uint64_t lastSeenGen = 0;
    UINT dataSize = sizeof(uint64_t);
    if (SUCCEEDED(pResource->GetPrivateData(RD_GEN_TAG, &dataSize, &lastSeenGen))) {
        if (lastSeenGen == currentGen) {
            return; // Already processed this generation
        }
    }

    // Mark it as seen for this generation immediately
    pResource->SetPrivateData(RD_GEN_TAG, sizeof(uint64_t), &currentGen);

    D3D12_RESOURCE_DESC desc = pResource->GetDesc();
    
    // EXTREME DEBUGGING: Log everything for the first 2000 resources
    static std::atomic<uint64_t> s_globalCounter(0);
    if (s_globalCounter < 2000) {
        LOG_INFO("[EXTREME] Res: %dx%d Fmt:%d Dim:%d Flags:%d", 
            (int)desc.Width, (int)desc.Height, (int)desc.Format, (int)desc.Dimension, (int)desc.Flags);
        s_globalCounter++;
    }

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
    
    // Debug logging for all potential candidates
    static std::atomic<uint64_t> s_debugCounter(0);
    if (colorScore > 0.0f || mvScore > 0.0f || depthScore > 0.0f) {
        if (s_debugCounter < 50) { // Log first 50 candidates
            LOG_INFO("[MFG] Resource candidate: %dx%d Fmt:%d Flags:%d ColorScore:%.2f MVScore:%.2f DepthScore:%.2f", 
                desc.Width, desc.Height, desc.Format, desc.Flags, colorScore, mvScore, depthScore);
            s_debugCounter++;
        }
    }
    
    if (mvScore <= 0.5f && depthScore <= 0.5f && colorScore <= 0.5f) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (mvScore > 0.5f) {
        bool found = false;
        for (auto& cand : m_motionCandidates) {
            if (cand.pResource.Get() == pResource) {
                cand.lastFrameSeen = currentFrame;
                found = true;
                break;
            }
        }
        if (!found) {
            m_motionCandidates.push_back({pResource, mvScore, desc, currentFrame});
            LOG_DEBUG("Found MV Candidate: %dx%d Fmt:%d Score:%.2f", 
                desc.Width, desc.Height, desc.Format, mvScore);
        }
        if (mvScore >= m_bestMotionScore) {
            m_bestMotionScore = mvScore;
            m_bestMotion = pResource;
        }
    }

    if (depthScore > 0.5f) {
        bool found = false;
        for (auto& cand : m_depthCandidates) {
            if (cand.pResource.Get() == pResource) {
                cand.lastFrameSeen = currentFrame;
                found = true;
                break;
            }
        }
        if (!found) {
            m_depthCandidates.push_back({pResource, depthScore, desc, currentFrame});
        }
        if (depthScore >= m_bestDepthScore) {
            m_bestDepthScore = depthScore;
            m_bestDepth = pResource;
        }
    }

    if (colorScore > 0.5f) {
        bool found = false;
        for (auto& cand : m_colorCandidates) {
            if (cand.pResource.Get() == pResource) {
                cand.lastFrameSeen = currentFrame;
                found = true;
                break;
            }
        }
        if (!found) {
            m_colorCandidates.push_back({pResource, colorScore, desc, currentFrame});
            LOG_INFO("[MFG] Found Color Candidate: %dx%d Fmt:%d Score:%.2f", 
                desc.Width, desc.Height, desc.Format, colorScore);
        }
        if (colorScore >= m_bestColorScore) {
            m_bestColorScore = colorScore;
            m_bestColor = pResource;
            LOG_INFO("[MFG] New BEST Color: %dx%d Fmt:%d Score:%.2f Ptr:%p", 
                desc.Width, desc.Height, desc.Format, colorScore, pResource);
        }
    }
}

float ResourceDetector::ScoreMotionVector(const D3D12_RESOURCE_DESC& desc) {
    float score = 0.0f;

    // Motion vectors are usually R16G16
    if (desc.Format == DXGI_FORMAT_R16G16_FLOAT) score += 0.8f;
    else if (desc.Format == DXGI_FORMAT_R16G16_UNORM) score += 0.6f;
    else if (desc.Format == DXGI_FORMAT_R32G32_FLOAT) score += 0.4f;
    else if (desc.Format == DXGI_FORMAT_R16G16_TYPELESS) score += 0.5f; // Typeless
    else return 0.0f; // Not a likely MV format

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return 0.0f;
    
    // Flags: often allow UAV (for compute generation)
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) score += 0.2f;

    return score;
}

float ResourceDetector::ScoreDepth(const D3D12_RESOURCE_DESC& desc) {
    float score = 0.0f;
    
    if (desc.Format == DXGI_FORMAT_D32_FLOAT) score += 0.9f;
    else if (desc.Format == DXGI_FORMAT_R32_FLOAT) score += 0.7f; // Read-only depth
    else if (desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) score += 0.5f;
    else if (desc.Format == DXGI_FORMAT_R32_TYPELESS) score += 0.6f; // Typeless Depth
    else return 0.0f;

    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) score += 0.2f;

    return score;
}

float ResourceDetector::ScoreColor(const D3D12_RESOURCE_DESC& desc) {
    float score = 0.0f;
    
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
