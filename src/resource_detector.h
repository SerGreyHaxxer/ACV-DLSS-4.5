#pragma once
#include <d3d12.h>
#include <vector>
#include <mutex>

struct ResourceCandidate {
    ID3D12Resource* pResource;
    float score;
    D3D12_RESOURCE_DESC desc;
    uint64_t lastFrameSeen;
};

class ResourceDetector {
public:
    static ResourceDetector& Get();

    void AnalyzeCommandList(ID3D12GraphicsCommandList* pCmdList);
    void RegisterResource(ID3D12Resource* pResource);
    
    ID3D12Resource* GetBestMotionVectorCandidate();
    ID3D12Resource* GetBestDepthCandidate();
    ID3D12Resource* GetBestColorCandidate();

    void NewFrame();
    void Clear(); // Clear all candidates

private:
    ResourceDetector() = default;
    
    float ScoreMotionVector(const D3D12_RESOURCE_DESC& desc);
    float ScoreDepth(const D3D12_RESOURCE_DESC& desc);
    float ScoreColor(const D3D12_RESOURCE_DESC& desc);

    std::mutex m_mutex;
    std::vector<ResourceCandidate> m_motionCandidates;
    std::vector<ResourceCandidate> m_depthCandidates;
    std::vector<ResourceCandidate> m_colorCandidates;
    
    uint64_t m_frameCount = 0;
};
