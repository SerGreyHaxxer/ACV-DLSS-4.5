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
#include <wrl/client.h>
#include <vector>
#include <mutex>

// We will sample a 16x16 grid (256 pixels)
constexpr uint32_t SCAN_GRID_SIZE = 16;
constexpr uint32_t SCAN_SAMPLE_COUNT = SCAN_GRID_SIZE * SCAN_GRID_SIZE;

struct ScanResult {
    float minX, maxX;
    float minY, maxY;
    float avgX, avgY;
    float varianceX, varianceY;
    bool isUniform;      // True if variance is near zero (solid color)
    bool hasData;        // True if not completely black/zero
    bool validRange;     // True if values are within expected MV range (-2.0 to 2.0 usually for normalized)
};

class HeuristicScanner {
public:
    static HeuristicScanner& Get();

    // Non-copyable, non-movable singleton
    HeuristicScanner(const HeuristicScanner&) = delete;
    HeuristicScanner& operator=(const HeuristicScanner&) = delete;
    HeuristicScanner(HeuristicScanner&&) = delete;
    HeuristicScanner& operator=(HeuristicScanner&&) = delete;

    bool Initialize(ID3D12Device* pDevice);
    void Shutdown();

    // Returns true if analysis was successful
    bool AnalyzeTexture(ID3D12GraphicsCommandList* pCmdList, ID3D12Resource* pResource, ScanResult& outResult);

    // New: Read back the results from the buffer (Must be called AFTER fence sync)
    bool GetReadbackResult(ScanResult& outResult);

private:
    HeuristicScanner() = default;

    bool CompileShader();
    bool CreateRootSignature(ID3D12Device* pDevice);
    bool CreatePSO(ID3D12Device* pDevice);
    bool CreateBuffers(ID3D12Device* pDevice);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    
    // Shader bytecode
    std::vector<uint8_t> m_shaderBytecode;

    static constexpr uint32_t SCAN_RING_SIZE = 3;
    uint32_t m_ringIndex = 0;

    struct FrameResources {
        Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> uavBuffer;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvUavHeap;
    };
    FrameResources m_frames[SCAN_RING_SIZE];

    bool m_initialized = false;
    // Lock hierarchy level 3 â€” same tier as Resources
    // (SwapChain=1 > Hooks=2 > Resources/Scanner=3 > Config=4 > Logging=5).
    std::mutex m_mutex;
};

