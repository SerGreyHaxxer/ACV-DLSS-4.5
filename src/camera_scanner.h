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
