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

#include <cstdint>

// Diagnostics
struct CameraDiagnostics {
  uint32_t registeredCbvCount;
  uint32_t trackedDescriptors;
  uint32_t trackedRootAddresses;
  float lastScore;
  uint64_t lastFoundFrame;
  int lastScanMethod; // 0=None, 1=Cached, 2=FullScan, 3=Descriptor, 4=Root
  bool cameraValid;
};

// ============================================================================
// CameraScanner Singleton — matches ResourceDetector architecture
// ============================================================================
class CameraScanner {
public:
  static CameraScanner& Get();

  // Non-copyable, non-movable singleton
  CameraScanner(const CameraScanner&) = delete;
  CameraScanner& operator=(const CameraScanner&) = delete;
  CameraScanner(CameraScanner&&) = delete;
  CameraScanner& operator=(CameraScanner&&) = delete;

  // CBV registration (for upload buffers mapped to GPU)
  void RegisterCbv(ID3D12Resource* pResource, UINT64 size, uint8_t* cpuPtr);
  void TrackCbvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc);
  void TrackRootCbvAddress(D3D12_GPU_VIRTUAL_ADDRESS address);

  // Camera scanning
  [[nodiscard]] bool TryScanAllCbvsForCamera(float* outView, float* outProj, float* outScore,
                                              bool logCandidates, bool allowFullScan);
  [[nodiscard]] bool TryScanDescriptorCbvsForCamera(float* outView, float* outProj, float* outScore,
                                                      bool logCandidates);
  [[nodiscard]] bool TryScanRootCbvsForCamera(float* outView, float* outProj, float* outScore,
                                                bool logCandidates);
  void UpdateCameraCache(const float* view, const float* proj, float jitterX, float jitterY);
  [[nodiscard]] bool GetLastCameraStats(float& outScore, uint64_t& outFrame);
  void ResetCameraScanCache();
  [[nodiscard]] uint64_t GetLastCameraFoundFrame();
  [[nodiscard]] uint64_t GetLastFullScanFrame();
  void GetCameraScanCounts(uint64_t& cbvCount, uint64_t& descCount, uint64_t& rootCount);

  // Diagnostics
  [[nodiscard]] CameraDiagnostics GetDiagnostics();

private:
  CameraScanner() = default;

  // Internal implementation — defined in camera_scanner.cpp
  struct Impl;
};

// ============================================================================
// FREE FUNCTION API — thin forwarding wrappers for backward compatibility
// ============================================================================
inline void RegisterCbv(ID3D12Resource* pResource, UINT64 size, uint8_t* cpuPtr) {
  CameraScanner::Get().RegisterCbv(pResource, size, cpuPtr);
}
inline void TrackCbvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc) {
  CameraScanner::Get().TrackCbvDescriptor(handle, desc);
}
inline void TrackRootCbvAddress(D3D12_GPU_VIRTUAL_ADDRESS address) {
  CameraScanner::Get().TrackRootCbvAddress(address);
}
[[nodiscard]] inline bool TryScanAllCbvsForCamera(float* outView, float* outProj, float* outScore,
                                                    bool logCandidates, bool allowFullScan) {
  return CameraScanner::Get().TryScanAllCbvsForCamera(outView, outProj, outScore, logCandidates, allowFullScan);
}
[[nodiscard]] inline bool TryScanDescriptorCbvsForCamera(float* outView, float* outProj, float* outScore,
                                                          bool logCandidates) {
  return CameraScanner::Get().TryScanDescriptorCbvsForCamera(outView, outProj, outScore, logCandidates);
}
[[nodiscard]] inline bool TryScanRootCbvsForCamera(float* outView, float* outProj, float* outScore,
                                                    bool logCandidates) {
  return CameraScanner::Get().TryScanRootCbvsForCamera(outView, outProj, outScore, logCandidates);
}
inline void UpdateCameraCache(const float* view, const float* proj, float jitterX, float jitterY) {
  CameraScanner::Get().UpdateCameraCache(view, proj, jitterX, jitterY);
}
[[nodiscard]] inline bool GetLastCameraStats(float& outScore, uint64_t& outFrame) {
  return CameraScanner::Get().GetLastCameraStats(outScore, outFrame);
}
inline void ResetCameraScanCache() {
  CameraScanner::Get().ResetCameraScanCache();
}
[[nodiscard]] inline uint64_t GetLastCameraFoundFrame() {
  return CameraScanner::Get().GetLastCameraFoundFrame();
}
[[nodiscard]] inline uint64_t GetLastFullScanFrame() {
  return CameraScanner::Get().GetLastFullScanFrame();
}
inline void GetCameraScanCounts(uint64_t& cbvCount, uint64_t& descCount, uint64_t& rootCount) {
  CameraScanner::Get().GetCameraScanCounts(cbvCount, descCount, rootCount);
}
[[nodiscard]] inline CameraDiagnostics GetCameraDiagnostics() {
  return CameraScanner::Get().GetDiagnostics();
}
