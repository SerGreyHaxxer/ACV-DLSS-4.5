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
        LOG_INFO("Resource detector cache cleared (Frame %llu)", m_frameCount);
    }
}

void ResourceDetector::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_motionCandidates.clear();
    m_depthCandidates.clear();
    m_colorCandidates.clear();
    LOG_INFO("Resource detector explicitly cleared.");
}

void ResourceDetector::RegisterResource(ID3D12Resource* pResource) {
    if (!pResource) return;

    D3D12_RESOURCE_DESC desc = pResource->GetDesc();
    
    // Ignore small buffers (likely UI icons or constant buffers)
    if (desc.Width < 64 || desc.Height < 64) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Score for Motion Vectors
    float mvScore = ScoreMotionVector(desc);
    if (mvScore > 0.5f) {
        bool found = false;
        for (auto& cand : m_motionCandidates) {
            if (cand.pResource.Get() == pResource) {
                cand.lastFrameSeen = m_frameCount;
                found = true;
                break;
            }
        }
        if (!found) {
            m_motionCandidates.push_back({pResource, mvScore, desc, m_frameCount});
            LOG_DEBUG("Found MV Candidate: %dx%d Fmt:%d Score:%.2f", 
                desc.Width, desc.Height, desc.Format, mvScore);
        }
    }

    // Score for Depth
    float depthScore = ScoreDepth(desc);
    if (depthScore > 0.5f) {
        bool found = false;
        for (auto& cand : m_depthCandidates) {
            if (cand.pResource.Get() == pResource) {
                cand.lastFrameSeen = m_frameCount;
                found = true;
                break;
            }
        }
        if (!found) {
            m_depthCandidates.push_back({pResource, depthScore, desc, m_frameCount});
        }
    }

    // Score for Color (Input)
    float colorScore = ScoreColor(desc);
    if (colorScore > 0.5f) {
        bool found = false;
        for (auto& cand : m_colorCandidates) {
            if (cand.pResource.Get() == pResource) {
                cand.lastFrameSeen = m_frameCount;
                found = true;
                break;
            }
        }
        if (!found) {
            m_colorCandidates.push_back({pResource, colorScore, desc, m_frameCount});
            LOG_DEBUG("Found Color Candidate: %dx%d Fmt:%d Score:%.2f", 
                desc.Width, desc.Height, desc.Format, colorScore);
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
    ID3D12Resource* best = nullptr;
    float bestScore = 0.0f;
    
    for (const auto& cand : m_motionCandidates) {
        if (cand.score > bestScore) {
            bestScore = cand.score;
            best = cand.pResource.Get();
        }
    }
    return best;
}

ID3D12Resource* ResourceDetector::GetBestDepthCandidate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ID3D12Resource* best = nullptr;
    float bestScore = 0.0f;
    
    for (const auto& cand : m_depthCandidates) {
        if (cand.score > bestScore) {
            bestScore = cand.score;
            best = cand.pResource.Get();
        }
    }
    return best;
}

ID3D12Resource* ResourceDetector::GetBestColorCandidate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ID3D12Resource* best = nullptr;
    float bestScore = 0.0f;
    
    for (const auto& cand : m_colorCandidates) {
        if (cand.score > bestScore) {
            bestScore = cand.score;
            best = cand.pResource.Get();
        }
    }
    return best;
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
