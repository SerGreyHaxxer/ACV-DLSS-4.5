#pragma once
#include <chrono>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

#include "nvapi.h"
#include "sl_reflex.h"
#include <sl.h>
#include <sl_consts.h>
#include <sl_deepdvc.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>

class StreamlineIntegration {
public:
  static StreamlineIntegration &Get();

  // Non-copyable, non-movable singleton
  StreamlineIntegration(const StreamlineIntegration&) = delete;
  StreamlineIntegration& operator=(const StreamlineIntegration&) = delete;
  StreamlineIntegration(StreamlineIntegration&&) = delete;
  StreamlineIntegration& operator=(StreamlineIntegration&&) = delete;

  bool Initialize(ID3D12Device *pDevice);
  void Shutdown();
  bool IsInitialized() const { return m_initialized; }

  void NewFrame(IDXGISwapChain *pSwapChain);
  void SetCommandQueue(ID3D12CommandQueue *pQueue);
  ID3D12CommandQueue *GetCommandQueue() const { return m_pCommandQueue.Get(); }

  void TagColorBuffer(ID3D12Resource *pResource) { m_colorBuffer = pResource; }
  void TagDepthBuffer(ID3D12Resource *pResource) { m_depthBuffer = pResource; }
  void TagMotionVectors(ID3D12Resource *pResource) {
    m_motionVectors = pResource;
  }

  void SetCameraData(const float *viewMatrix, const float *projMatrix,
                     float jitterX, float jitterY);

  void EvaluateDLSS(ID3D12GraphicsCommandList *pCmdList);
  void EvaluateFrameGen(IDXGISwapChain *pSwapChain);
  void EvaluateDeepDVC(IDXGISwapChain *pSwapChain);

  // Setters for UI (with bounds validation)
  void SetDLSSModeIndex(int index);
  int GetDLSSModeIndex() const;
  void SetDLSSPreset(int preset) {
    if (preset < 0 || preset >= static_cast<int>(sl::DLSSPreset::eCount))
      return;
    m_dlssPreset = static_cast<sl::DLSSPreset>(preset);
    m_optionsDirty = true;
  }
  int GetDLSSPresetIndex() const { return static_cast<int>(m_dlssPreset); }
  void SetFrameGenMultiplier(int mult) {
    m_frameGenMultiplier = mult;
    m_optionsDirty = true;
  }
  int GetFrameGenMultiplier() const { return m_frameGenMultiplier; }
  void SetSharpness(float val) {
    m_sharpness = val;
    m_optionsDirty = true;
  }
  void SetLODBias(float val) { m_lodBias = val; }
  float GetLODBias() const { return m_lodBias; }
  void SetMVecScale(float x, float y) {
    m_mvecScaleX = x;
    m_mvecScaleY = y;
  }
  void SetReflexEnabled(bool val) { m_reflexEnabled = val; }
  void SetHUDFixEnabled(bool val) { m_hudFixEnabled = val; }
  void SetRayReconstructionEnabled(bool val) {
    m_rayReconstructionEnabled = val;
    m_optionsDirty = true;
  }
  bool IsRayReconstructionSupported() const {
    return m_rayReconstructionSupported;
  }
  bool IsRayReconstructionEnabled() const { return m_rayReconstructionEnabled; }
  void SetRRPreset(int preset);
  int GetRRPresetIndex() const { return m_rrPresetIndex; }
  void SetRRDenoiserStrength(float val) {
    m_rrDenoiserStrength = val;
    m_optionsDirty = true;
  }
  void SetDeepDVCEnabled(bool val) {
    m_deepDvcEnabled = val;
    m_optionsDirty = true;
  }
  bool IsDeepDVCSupported() const { return m_deepDvcSupported; }
  bool IsDeepDVCEnabled() const { return m_deepDvcEnabled; }
  void SetDeepDVCIntensity(float val) {
    m_deepDvcIntensity = val;
    m_optionsDirty = true;
  }
  void SetDeepDVCSaturation(float val) {
    m_deepDvcSaturation = val;
    m_optionsDirty = true;
  }
  void SetDeepDVCAdaptiveEnabled(bool val) {
    m_deepDvcAdaptiveEnabled = val;
    m_optionsDirty = true;
  }
  void SetDeepDVCAdaptiveStrength(float val) {
    m_deepDvcAdaptiveStrength = val;
    m_optionsDirty = true;
  }
  void SetDeepDVCAdaptiveMin(float val) {
    m_deepDvcAdaptiveMin = val;
    m_optionsDirty = true;
  }
  void SetDeepDVCAdaptiveMax(float val) {
    m_deepDvcAdaptiveMax = val;
    m_optionsDirty = true;
  }
  void SetDeepDVCAdaptiveSmoothing(float val) {
    m_deepDvcAdaptiveSmoothing = val;
    m_optionsDirty = true;
  }
  void SetSmartFGEnabled(bool val) {
    m_smartFgEnabled = val;
    m_optionsDirty = true;
  }
  void SetSmartFGAutoDisable(bool val) {
    m_smartFgAutoDisable = val;
    m_optionsDirty = true;
  }
  void SetSmartFGAutoDisableThreshold(float val) {
    m_smartFgAutoDisableFps = val;
    m_optionsDirty = true;
  }
  void SetSmartFGSceneChangeEnabled(bool val) {
    m_smartFgSceneChangeEnabled = val;
    m_optionsDirty = true;
  }
  void SetSmartFGSceneChangeThreshold(float val) {
    m_smartFgSceneChangeThreshold = val;
    m_optionsDirty = true;
  }
  void SetSmartFGInterpolationQuality(float val) {
    m_smartFgInterpolationQuality = val;
    m_optionsDirty = true;
  }
  bool IsSmartFGTemporarilyDisabled() const { return m_smartFgForceDisable; }
  bool IsFrameGenDisabledDueToInvalidParam() const {
    return m_disableFGDueToInvalidParam;
  }
  sl::DLSSGStatus GetFrameGenStatus() const { return m_dlssgStatus; }

  void SetHDREnabled(bool val) {
    m_hdrEnabled = val;
    m_hdrDirty = true;
  }
  void SetHDRPeakNits(float val) {
    m_hdrPeakNits = val;
    m_hdrDirty = true;
  }
  void SetHDRPaperWhiteNits(float val) {
    m_hdrPaperWhiteNits = val;
    m_hdrDirty = true;
  }
  void SetHDRExposure(float val) {
    m_hdrExposure = val;
    m_hdrDirty = true;
  }
  void SetHDRGamma(float val) {
    m_hdrGamma = val;
    m_hdrDirty = true;
  }
  void SetHDRTonemapCurve(float val) {
    m_hdrTonemapCurve = val;
    m_hdrDirty = true;
  }
  void SetHDRSaturation(float val) {
    m_hdrSaturation = val;
    m_hdrDirty = true;
  }
  bool IsHDRSupported() const { return m_hdrSupported; }
  bool IsHDREnabled() const { return m_hdrEnabled; }
  bool IsHDRActive() const { return m_hdrActive; }

  bool HasCameraData() const { return m_hasCameraData; }
  void UpdateFrameTiming(float fps) { m_lastBaseFps = fps; }
  float GetFgActualMultiplier() const { return m_fgActualMultiplier; }
  void PrintDLSSGStatus();
  bool IsDLSSSupported() const { return m_dlssSupported; }
  bool IsFrameGenSupported() const { return m_dlssgSupported; }
  void UpdateControls();
  void ToggleDebugMode(bool enabled);
  void ReflexMarker(sl::PCLMarker marker);
  void ReflexSleep();
  void ReleaseResources();
  float GetLastCameraDelta() const { return m_lastCameraDelta; }
  uint64_t GetFrameCount() const { return m_frameIndex; }
  void GetLastCameraJitter(float &jitterX, float &jitterY) const {
    jitterX = m_lastJitterX;
    jitterY = m_lastJitterY;
  }
  UINT GetDescriptorSize() const {
    if (m_pDevice)
      return m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return 32; // Fallback if device not yet initialized
  }

private:
  StreamlineIntegration() = default;
  ~StreamlineIntegration();

  bool m_initialized = false;
  Microsoft::WRL::ComPtr<ID3D12Device> m_pDevice;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_pCommandQueue;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_pCommandAllocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_pCommandList;
  Microsoft::WRL::ComPtr<IDXGISwapChain> m_pSwapChain;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_backBuffer;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_colorBuffer;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_motionVectors;

  // GPU synchronization fence for command allocator reuse
  Microsoft::WRL::ComPtr<ID3D12Fence> m_gpuFence;
  HANDLE m_gpuFenceEvent = nullptr;
  UINT64 m_gpuFenceValue = 0;

  sl::FrameToken *m_frameToken = nullptr;
  uint32_t m_frameIndex = 0;
  sl::ViewportHandle m_viewport = sl::ViewportHandle(0);
  bool m_optionsDirty = true;
  bool m_hdrDirty = true;

  sl::DLSSMode m_dlssMode = sl::DLSSMode::eDLAA;
  sl::DLSSPreset m_dlssPreset = sl::DLSSPreset::eDefault;
  int m_frameGenMultiplier = 0;
  float m_sharpness = 0.5f;
  float m_lodBias = 0.0f;
  float m_mvecScaleX = 1.0f;
  float m_mvecScaleY = 1.0f;
  bool m_reflexEnabled = true;
  bool m_hudFixEnabled = false;
  bool m_rayReconstructionEnabled = false;
  bool m_rayReconstructionSupported = false;
  int m_rrPresetIndex = 0;
  float m_rrDenoiserStrength = 0.5f;
  bool m_deepDvcEnabled = false;
  bool m_deepDvcSupported = false;
  float m_deepDvcIntensity = 0.5f;
  float m_deepDvcSaturation = 0.5f;
  bool m_deepDvcAdaptiveEnabled = false;
  float m_deepDvcAdaptiveStrength = 0.5f;
  float m_deepDvcAdaptiveMin = 0.0f;
  float m_deepDvcAdaptiveMax = 1.0f;
  float m_deepDvcAdaptiveSmoothing = 0.1f;
  bool m_smartFgEnabled = false;
  bool m_smartFgAutoDisable = false;
  float m_smartFgAutoDisableFps = 60.0f;
  bool m_smartFgSceneChangeEnabled = false;
  float m_smartFgSceneChangeThreshold = 0.5f;
  float m_smartFgInterpolationQuality = 0.5f;
  bool m_smartFgForceDisable = false;
  bool m_disableFGDueToInvalidParam = false;
  sl::DLSSGStatus m_dlssgStatus = sl::DLSSGStatus::eOk;
  bool m_hdrEnabled = false;
  bool m_hdrSupported = false;
  bool m_hdrActive = false;
  float m_hdrPeakNits = 1000.0f;
  float m_hdrPaperWhiteNits = 200.0f;
  float m_hdrExposure = 1.0f;
  float m_hdrGamma = 2.2f;
  float m_hdrTonemapCurve = 0.0f;
  float m_hdrSaturation = 1.0f;

  bool m_dlssSupported = false;
  bool m_dlssgSupported = false;
  bool m_dlssgLoaded = false;
  bool m_reflexLoaded = false;
  bool m_rrLoaded = false;
  bool m_deepDvcLoaded = false;
  bool m_hasCameraData = false;
  float m_lastBaseFps = 0.0f;
  float m_fgActualMultiplier = 1.0f;
  float m_lastCameraDelta = 0.0f;
  float m_lastJitterX = 0.0f;
  float m_lastJitterY = 0.0f;

  sl::Feature m_featuresToLoad[6] = {};
  uint32_t m_featureCount = 0;

  void UpdateSwapChain(IDXGISwapChain *pSwapChain);
  void TagResources();
  bool EnsureCommandList();
  void UpdateOptions();
  void WaitForGpu();
};
