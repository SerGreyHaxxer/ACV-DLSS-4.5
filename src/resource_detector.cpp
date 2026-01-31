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
    if (m_frameCount % 300 == 0) {
        m_motionCandidates.clear();
        m_depthCandidates.clear();
        m_colorCandidates.clear();
        m_bestMotion = nullptr;
        m_bestDepth = nullptr;
        m_bestColor = nullptr;
        m_bestMotionScore = 0.0f;
        m_bestDepthScore = 0.0f;
        m_bestColorScore = 0.0f;
        LOG_INFO("Resource detector cache cleared (Frame %llu)", m_frameCount);
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

void ResourceDetector::RegisterResource(ID3D12Resource* pResource) {
    if (!pResource) return;

    D3D12_RESOURCE_DESC desc = pResource->GetDesc();
    
    // Ignore small buffers (likely UI icons or constant buffers)
    if (desc.Width < 64 || desc.Height < 64) return;

    uint64_t frameCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        frameCount = m_frameCount;
    }

    // Score for Motion Vectors
    float mvScore = ScoreMotionVector(desc);

    // Score for Depth
    float depthScore = ScoreDepth(desc);

    // Score for Color (Input)
    float colorScore = ScoreColor(desc);
    if (mvScore <= 0.5f && depthScore <= 0.5f && colorScore <= 0.5f) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (mvScore > 0.5f) {
        bool found = false;
        for (auto& cand : m_motionCandidates) {
            if (cand.pResource.Get() == pResource) {
                cand.lastFrameSeen = frameCount;
                found = true;
                break;
            }
        }
        if (!found) {
            m_motionCandidates.push_back({pResource, mvScore, desc, frameCount});
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
                cand.lastFrameSeen = frameCount;
                found = true;
                break;
            }
        }
        if (!found) {
            m_depthCandidates.push_back({pResource, depthScore, desc, frameCount});
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
                cand.lastFrameSeen = frameCount;
                found = true;
                break;
            }
        }
        if (!found) {
            m_colorCandidates.push_back({pResource, colorScore, desc, frameCount});
            LOG_DEBUG("Found Color Candidate: %dx%d Fmt:%d Score:%.2f", 
                desc.Width, desc.Height, desc.Format, colorScore);
        }
        if (colorScore >= m_bestColorScore) {
            m_bestColorScore = colorScore;
            m_bestColor = pResource;
        }
    }
}

float ResourceDetector::ScoreMotionVector(const D3D12_RESOURCE_DESC& desc) {
    float score = 0.0f;

    // Motion vectors are usually R16G16
    if (desc.Format == DXGI_FORMAT_R16G16_FLOAT) score += 0.8f;
    else if (desc.Format == DXGI_FORMAT_R16G16_UNORM) score += 0.6f;
    else if (desc.Format == DXGI_FORMAT_R32G32_FLOAT) score += 0.4f;
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
    else return 0.0f;

    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) score += 0.2f;

    return score;
}

float ResourceDetector::ScoreColor(const D3D12_RESOURCE_DESC& desc) {
    float score = 0.0f;
    
    // Standard Backbuffer Formats
    if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) score += 0.5f;
    else if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) score += 0.5f;
    else if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) score += 0.6f; // HDR
    else if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) score += 0.7f; // HDR Float
    else return 0.0f;

    // Must be Render Target
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) score += 0.3f;
    
    return score;
}

ID3D12Resource* ResourceDetector::GetBestMotionVectorCandidate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bestMotion;
}

ID3D12Resource* ResourceDetector::GetBestDepthCandidate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bestDepth;
}

ID3D12Resource* ResourceDetector::GetBestColorCandidate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bestColor;
}

void ResourceDetector::AnalyzeCommandList(ID3D12GraphicsCommandList* pCmdList) {
    // Placeholder
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
