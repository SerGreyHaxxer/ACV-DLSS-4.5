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


// CBV registration (for upload buffers mapped to GPU)
void RegisterCbv(ID3D12Resource* pResource, UINT64 size, uint8_t* cpuPtr);
void TrackCbvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc);
void TrackRootCbvAddress(D3D12_GPU_VIRTUAL_ADDRESS address);

// Camera scanning
bool TryScanAllCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates, bool allowFullScan);
bool TryScanDescriptorCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates);
bool TryScanRootCbvsForCamera(float* outView, float* outProj, float* outScore, bool logCandidates);
void UpdateCameraCache(const float* view, const float* proj, float jitterX, float jitterY);
bool GetLastCameraStats(float& outScore, uint64_t& outFrame);
void ResetCameraScanCache();
uint64_t GetLastCameraFoundFrame();
uint64_t GetLastFullScanFrame();
void GetCameraScanCounts(uint64_t& cbvCount, uint64_t& descCount, uint64_t& rootCount);

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
CameraDiagnostics GetCameraDiagnostics();
