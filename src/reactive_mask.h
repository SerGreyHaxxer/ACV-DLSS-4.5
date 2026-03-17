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

#include <atomic>
#include <cstdint>

// ============================================================================
// Fix 9: GPU Compute Auto-Masking for UI Ghosting Prevention
// ============================================================================
// Replaces the CPU-side heuristic draw call interception with a lightweight
// D3D12 compute shader that runs natively on the GPU timeline.
//
// The shader diffs the final rendered frame (with HUD) against the HUD-less
// color buffer to produce a pixel-accurate R8_UNORM reactive mask. Every pixel
// where the HUD is visible has mask=1.0, telling DLSS Frame Generation to
// NOT use temporal history for those pixels — eliminating compass, health bar,
// and damage number ghosting.
//
// The compute dispatch runs immediately before slEvaluateFeature, so the mask
// is always up-to-date for the current frame.
// ============================================================================

class ReactiveMask {
public:
  static ReactiveMask& Get();

  ReactiveMask(const ReactiveMask&) = delete;
  ReactiveMask& operator=(const ReactiveMask&) = delete;
  ReactiveMask(ReactiveMask&&) = delete;
  ReactiveMask& operator=(ReactiveMask&&) = delete;

  // Initialize compute pipeline (root signature, PSO, descriptor heap)
  bool Initialize(ID3D12Device* pDevice, UINT width, UINT height);

  // Fix 9: Dispatch the compute shader to generate the reactive mask.
  // finalColor:  The rendered frame WITH HUD/UI
  // hudLessColor: The rendered frame WITHOUT HUD/UI
  // Both must be in SRV-compatible state.
  void DispatchMask(ID3D12GraphicsCommandList* pCmdList,
                    ID3D12Resource* finalColor,
                    ID3D12Resource* hudLessColor);

  // Get the reactive mask texture for Streamline tagging
  ID3D12Resource* GetMaskTexture() const;

  // Resize the mask (e.g., on resolution change)
  void Resize(UINT width, UINT height);

  // Cleanup
  void Shutdown();

  bool IsInitialized() const { return m_initialized; }
  UINT GetWidth() const { return m_width; }
  UINT GetHeight() const { return m_height; }

private:
  ReactiveMask() = default;
  ~ReactiveMask() = default;

  bool CreateComputePipeline(ID3D12Device* pDevice);
  bool CreateMaskTexture(ID3D12Device* pDevice, UINT width, UINT height);

  bool m_initialized = false;
  UINT m_width = 0;
  UINT m_height = 0;

  Microsoft::WRL::ComPtr<ID3D12Device> m_pDevice;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_maskTexture;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

  // SRV/UAV descriptor heap for compute dispatch
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
  UINT m_descriptorSize = 0;

  std::atomic<uint64_t> m_dispatchCount{0};
};
