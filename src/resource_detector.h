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
#pragma once
#include <d3d12.h>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <wrl/client.h>

struct ResourceCandidate {
    Microsoft::WRL::ComPtr<ID3D12Resource> pResource;
    float score;
    D3D12_RESOURCE_DESC desc;
    uint64_t lastFrameSeen;
    uint32_t seenCount;
};

#include <atomic> // Added

class ResourceDetector {
public:
    static ResourceDetector& Get();

    // Non-copyable, non-movable singleton
    ResourceDetector(const ResourceDetector&) = delete;
    ResourceDetector& operator=(const ResourceDetector&) = delete;
    ResourceDetector(ResourceDetector&&) = delete;
    ResourceDetector& operator=(ResourceDetector&&) = delete;

    void RegisterResource(ID3D12Resource* pResource);
    void RegisterResource(ID3D12Resource* pResource, bool allowDuplicate);
    void RegisterDepthFromView(ID3D12Resource* pResource, DXGI_FORMAT viewFormat);
    void RegisterMotionVectorFromView(ID3D12Resource* pResource, DXGI_FORMAT viewFormat);
    
    // High-confidence signals from Clear calls
    void RegisterDepthFromClear(ID3D12Resource* pResource, float clearDepth);
    void RegisterColorFromClear(ID3D12Resource* pResource);
    void RegisterExposure(ID3D12Resource* pResource);

    bool IsDepthInverted() const { return m_depthInverted; }
    DXGI_FORMAT GetDepthFormatOverride(ID3D12Resource* pResource);
    ID3D12Resource* GetExposureResource() { return m_exposureResource.Get(); }
    DXGI_FORMAT GetMotionFormatOverride(ID3D12Resource* pResource);
    
    ID3D12Resource* GetBestMotionVectorCandidate();
    ID3D12Resource* GetBestDepthCandidate();
    ID3D12Resource* GetBestColorCandidate();
    uint64_t GetFrameCount();

    std::string GetDebugInfo();
    void LogDebugInfo(); // Dump to log file

    void NewFrame();
    void Clear(); // Clear all candidates
    void SetExpectedDimensions(uint32_t width, uint32_t height);

    // New: Trigger dynamic analysis using a compute shader
    void UpdateHeuristics(ID3D12CommandQueue* pQueue);

private:
    ResourceDetector() = default;
    ~ResourceDetector();
    
    float ScoreMotionVector(const D3D12_RESOURCE_DESC& desc);
    float ScoreDepth(const D3D12_RESOURCE_DESC& desc);
    float ScoreColor(const D3D12_RESOURCE_DESC& desc);

    // Lock hierarchy level 3 (SwapChain=1 > Hooks=2 > Resources=3 > Config=4 > Logging=5).
    // Use shared_lock for read-only access (Get*, GetDebugInfo, Score*),
    // unique_lock for mutations (Register*, NewFrame, Clear, SetExpected*).
    mutable std::shared_mutex m_mutex;
    std::vector<ResourceCandidate> m_motionCandidates;
    std::vector<ResourceCandidate> m_depthCandidates;
    std::vector<ResourceCandidate> m_colorCandidates;
    
    std::atomic<uint64_t> m_frameCount{0};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bestMotion;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bestDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bestColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_exposureResource;
    float m_bestMotionScore = 0.0f;
    float m_bestDepthScore = 0.0f;
    float m_bestColorScore = 0.0f;
    uint32_t m_expectedWidth = 0;
    uint32_t m_expectedHeight = 0;
    bool m_depthInverted = true; // Default to true for Valhalla
    std::unordered_map<ID3D12Resource*, DXGI_FORMAT> m_depthFormatOverrides;
    std::unordered_map<ID3D12Resource*, DXGI_FORMAT> m_motionFormatOverrides;

    // Heuristic State
    struct HeuristicData {
        bool analyzed = false;
        float variance = 0.0f;
        bool validRange = false;
        uint64_t lastCheckFrame = 0;
    };
    std::unordered_map<ID3D12Resource*, HeuristicData> m_heuristics;
    
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lastAnalyzedCandidate;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceVal = 0;
    HANDLE m_fenceEvent = nullptr;

    bool InitCommandList(ID3D12Device* pDevice);
};

