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
#include "streamline_integration.h"

#include "config_manager.h"
#include "dlss4_config.h"
#include "imgui_overlay.h"
#include "logger.h"
#include "resource_detector.h"

#include <algorithm>
#include <cmath>

// Use inline helpers instead of redefining SDK macros
namespace {
inline bool slFailed(sl::Result r) {
  return r != sl::Result::eOk;
}
inline bool slSucceeded(sl::Result r) {
  return r == sl::Result::eOk;
}
} // namespace

StreamlineIntegration& StreamlineIntegration::Get() {
  static StreamlineIntegration instance;
  return instance;
}

StreamlineIntegration::~StreamlineIntegration() {
  Shutdown();
}

bool StreamlineIntegration::Initialize(ID3D12Device* pDevice) {
  std::lock_guard<std::mutex> lock(m_initMutex);
  if (m_initialized) return true;
  if (!pDevice) return false;
  m_pDevice = pDevice;

  ConfigManager::Get().Load();
  ModConfig& cfg = ConfigManager::Get().Data();

  m_dlssMode = static_cast<sl::DLSSMode>(cfg.dlss.mode);
  m_frameGenMultiplier = cfg.fg.multiplier;
  m_sharpness = cfg.dlss.sharpness;
  m_lodBias = cfg.dlss.lodBias;
  m_reflexEnabled = cfg.rr.enabled;
  m_rayReconstructionEnabled = cfg.rr.enabled;
  m_deepDvcEnabled = cfg.dvc.enabled;

  sl::Preferences pref{};
  pref.renderAPI = sl::RenderAPI::eD3D12;
  pref.applicationId = dlss4::kNgxAppId;
  pref.flags |= sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;

  m_featuresToLoad[0] = sl::kFeatureDLSS;
  m_featureCount = 1;
  if (m_frameGenMultiplier >= 2) m_featuresToLoad[m_featureCount++] = sl::kFeatureDLSS_G;
  if (m_reflexEnabled) {
    m_featuresToLoad[m_featureCount++] = sl::kFeatureReflex;
    m_featuresToLoad[m_featureCount++] = sl::kFeaturePCL;
  }
  if (m_rayReconstructionEnabled) m_featuresToLoad[m_featureCount++] = sl::kFeatureDLSS_RR;
  m_featuresToLoad[m_featureCount++] = sl::kFeatureDeepDVC;

  pref.featuresToLoad = m_featuresToLoad;
  pref.numFeaturesToLoad = m_featureCount;

  if (slFailed(slInit(pref, sl::kSDKVersion))) return false;
  if (slFailed(slSetD3DDevice(pDevice))) return false;

  // Phase 1.2: Create per-feature GPU resources to eliminate 3x WaitForGpu stalling
  const char* slotNames[] = {"DLSS", "FrameGen", "DeepDVC"};
  for (int i = 0; i < kFeatureSlotCount; ++i) {
    auto& gpu = m_featureGPU[i];
    HRESULT hr = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    if (FAILED(hr)) {
      LOG_ERROR("Failed to create {} command allocator (HRESULT: {:08X})", slotNames[i], static_cast<unsigned>(hr));
      continue;
    }
    hr = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&gpu.cmdList));
    if (FAILED(hr)) {
      LOG_ERROR("Failed to create {} command list (HRESULT: {:08X})", slotNames[i], static_cast<unsigned>(hr));
      continue;
    }
    gpu.cmdList->Close();
    hr = m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    if (FAILED(hr)) {
      LOG_ERROR("Failed to create {} fence (HRESULT: {:08X})", slotNames[i], static_cast<unsigned>(hr));
      continue;
    }
    gpu.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    gpu.fenceValue = 0;
    LOG_INFO("Per-feature GPU resources created for {}", slotNames[i]);
  }

  // Legacy GPU fence (kept for backward compat with EnsureCommandList)
  HRESULT fenceHR = m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_gpuFence));
  if (SUCCEEDED(fenceHR)) {
    m_gpuFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    m_gpuFenceValue = 0;
  }

  // Feature support checks
  m_dlssSupported = (slIsFeatureSupported(sl::kFeatureDLSS, sl::AdapterInfo{}) == sl::Result::eOk);
  m_dlssgSupported = (slIsFeatureSupported(sl::kFeatureDLSS_G, sl::AdapterInfo{}) == sl::Result::eOk);
  m_rayReconstructionSupported = (slIsFeatureSupported(sl::kFeatureDLSS_RR, sl::AdapterInfo{}) == sl::Result::eOk);
  m_deepDvcSupported = (slIsFeatureSupported(sl::kFeatureDeepDVC, sl::AdapterInfo{}) == sl::Result::eOk);
  m_hdrSupported = true; // HDR support is handled via output format check

  // Log support status
  LOG_INFO("Feature Support: DLSS={} DLSS-G={} RR={} DeepDVC={} HDR={}", m_dlssSupported ? "YES" : "NO",
           m_dlssgSupported ? "YES" : "NO", m_rayReconstructionSupported ? "YES" : "NO",
           m_deepDvcSupported ? "YES" : "NO", m_hdrSupported ? "YES" : "NO");

  m_initialized = true;
  LOG_INFO("Streamline Integration initialized (per-feature GPU pipeline active)");
  LOG_INFO("  Device: {:p}, CommandQueue: {:p}", static_cast<void*>(m_pDevice.Get()),
           static_cast<void*>(m_pCommandQueue.Get()));

  return true;
}

void StreamlineIntegration::Shutdown() {
  if (m_initialized) {
    WaitForGpu(); // Wait for any pending GPU work on the main queue
    slShutdown();
    m_initialized = false;
  }
  if (m_gpuFenceEvent) {
    CloseHandle(m_gpuFenceEvent);
    m_gpuFenceEvent = nullptr;
  }

  // Clean up per-feature GPU resources
  for (int i = 0; i < kFeatureSlotCount; ++i) {
    auto& gpu = m_featureGPU[i];
    if (gpu.fenceEvent) {
      CloseHandle(gpu.fenceEvent);
      gpu.fenceEvent = nullptr;
    }
    gpu.fence.Reset();
    gpu.cmdList.Reset();
    gpu.allocator.Reset();
  }
}

void StreamlineIntegration::NewFrame(IDXGISwapChain* pSwapChain) {
  if (!m_initialized) return;
  UpdateSwapChain(pSwapChain);
  slGetNewFrameToken(m_frameToken, &m_frameIndex);
  TagResources();
}

void StreamlineIntegration::SetDLSSModeIndex(int index) {
  static const sl::DLSSMode kModes[] = {sl::DLSSMode::eOff,          sl::DLSSMode::eMaxPerformance,
                                        sl::DLSSMode::eBalanced,     sl::DLSSMode::eMaxQuality,
                                        sl::DLSSMode::eUltraQuality, sl::DLSSMode::eDLAA};
  if (index >= 0 && index < 6) {
    m_dlssMode = kModes[index];
    m_optionsDirty = true;
  }
}

int StreamlineIntegration::GetDLSSModeIndex() const {
  switch (m_dlssMode) {
    case sl::DLSSMode::eOff: return 0;
    case sl::DLSSMode::eMaxPerformance: return 1;
    case sl::DLSSMode::eBalanced: return 2;
    case sl::DLSSMode::eMaxQuality: return 3;
    case sl::DLSSMode::eUltraQuality: return 4;
    case sl::DLSSMode::eDLAA: return 5;
    default: return 3;
  }
}

void StreamlineIntegration::SetRRPreset(int preset) {
  m_rrPresetIndex = preset;
  m_optionsDirty = true;
}

void StreamlineIntegration::SetCameraData(const float* view, const float* proj, float jitterX, float jitterY) {
  if (!m_initialized || !m_frameToken) return;

  // Phase 4.5: Validate jitter before passing to Streamline
  if (!std::isfinite(jitterX) || !std::isfinite(jitterY)) {
    jitterX = 0.0f;
    jitterY = 0.0f;
  }
  if (std::fabs(jitterX) > 1.0f || std::fabs(jitterY) > 1.0f) {
    jitterX = 0.0f;
    jitterY = 0.0f;
  }

  sl::Constants consts{};
  if (view) std::copy_n(view, 16, reinterpret_cast<float*>(&consts.cameraViewToClip));
  if (proj) std::copy_n(proj, 16, reinterpret_cast<float*>(&consts.clipToCameraView));
  consts.jitterOffset = sl::float2(jitterX, jitterY);
  consts.mvecScale = sl::float2(m_mvecScaleX, m_mvecScaleY);
  m_hasCameraData = (view != nullptr);
  m_viewport = sl::ViewportHandle(0);
  slSetConstants(consts, *m_frameToken, m_viewport);
}

void StreamlineIntegration::Evaluate(ID3D12GraphicsCommandList* pCmdList) {
  if (!m_initialized || !pCmdList || !m_frameToken) return;
  if (m_optionsDirty) UpdateOptions();
  m_viewport = sl::ViewportHandle(0);
  const sl::BaseStructure* inputs[] = {&m_viewport};
  slEvaluateFeature(sl::kFeatureDLSS, *m_frameToken, inputs, 1, pCmdList);
}

void StreamlineIntegration::EvaluateDLSSFromPresent() {
  if (!m_initialized || !m_pCommandQueue || !m_frameToken) return;

  if (m_optionsDirty) UpdateOptions();

  // Phase 1.2: Use dedicated DLSS GPU resources (no shared WaitForGpu stall)
  if (!EnsureFeatureCommandList(FeatureSlot::DLSS)) return;

  auto& gpu = m_featureGPU[static_cast<int>(FeatureSlot::DLSS)];
  WaitForFeature(FeatureSlot::DLSS);

  HRESULT hr = gpu.allocator->Reset();
  if (FAILED(hr)) {
    LOG_WARN("[DLSS] Command allocator reset failed");
    return;
  }
  hr = gpu.cmdList->Reset(gpu.allocator.Get(), nullptr);
  if (FAILED(hr)) {
    LOG_WARN("[DLSS] Command list reset failed");
    return;
  }

  m_viewport = sl::ViewportHandle(0);
  const sl::BaseStructure* inputs[] = {&m_viewport};
  slEvaluateFeature(sl::kFeatureDLSS, *m_frameToken, inputs, 1, gpu.cmdList.Get());
  gpu.cmdList->Close();
  ID3D12CommandList* lists[] = {gpu.cmdList.Get()};
  m_pCommandQueue->ExecuteCommandLists(1, lists);

  if (gpu.fence) {
    gpu.fenceValue++;
    m_pCommandQueue->Signal(gpu.fence.Get(), gpu.fenceValue);
  }
}

void StreamlineIntegration::EvaluateFrameGen(IDXGISwapChain* /*pSwapChain*/) {
  if (!m_initialized || !m_dlssgLoaded || !m_pCommandQueue || !m_frameToken) return;

  // Phase 1.2: Use dedicated FrameGen GPU resources
  if (!EnsureFeatureCommandList(FeatureSlot::FrameGen)) return;

  auto& gpu = m_featureGPU[static_cast<int>(FeatureSlot::FrameGen)];
  WaitForFeature(FeatureSlot::FrameGen);

  HRESULT hr = gpu.allocator->Reset();
  if (FAILED(hr)) {
    LOG_WARN("[DLSSG] Command allocator reset failed");
    return;
  }
  hr = gpu.cmdList->Reset(gpu.allocator.Get(), nullptr);
  if (FAILED(hr)) {
    LOG_WARN("[DLSSG] Command list reset failed");
    return;
  }

  m_viewport = sl::ViewportHandle(0);
  const sl::BaseStructure* inputs[] = {&m_viewport};
  slEvaluateFeature(sl::kFeatureDLSS_G, *m_frameToken, inputs, 1, gpu.cmdList.Get());
  gpu.cmdList->Close();
  ID3D12CommandList* lists[] = {gpu.cmdList.Get()};
  m_pCommandQueue->ExecuteCommandLists(1, lists);

  if (gpu.fence) {
    gpu.fenceValue++;
    m_pCommandQueue->Signal(gpu.fence.Get(), gpu.fenceValue);
  }
}

void StreamlineIntegration::EvaluateDeepDVC(IDXGISwapChain* /*pSwapChain*/) {
  if (!m_initialized || !m_deepDvcLoaded || !m_pCommandQueue || !m_frameToken) return;

  // Phase 1.2: Use dedicated DeepDVC GPU resources
  if (!EnsureFeatureCommandList(FeatureSlot::DeepDVC)) return;

  auto& gpu = m_featureGPU[static_cast<int>(FeatureSlot::DeepDVC)];
  WaitForFeature(FeatureSlot::DeepDVC);

  HRESULT hr = gpu.allocator->Reset();
  if (FAILED(hr)) {
    LOG_WARN("[DeepDVC] Command allocator reset failed");
    return;
  }
  hr = gpu.cmdList->Reset(gpu.allocator.Get(), nullptr);
  if (FAILED(hr)) {
    LOG_WARN("[DeepDVC] Command list reset failed");
    return;
  }

  m_viewport = sl::ViewportHandle(0);
  const sl::BaseStructure* inputs[] = {&m_viewport};
  slEvaluateFeature(sl::kFeatureDeepDVC, *m_frameToken, inputs, 1, gpu.cmdList.Get());
  gpu.cmdList->Close();
  ID3D12CommandList* lists[] = {gpu.cmdList.Get()};
  m_pCommandQueue->ExecuteCommandLists(1, lists);

  if (gpu.fence) {
    gpu.fenceValue++;
    m_pCommandQueue->Signal(gpu.fence.Get(), gpu.fenceValue);
  }
}

void StreamlineIntegration::UpdateOptions() {
  if (!m_initialized) return;
  m_viewport = sl::ViewportHandle(0);

  // --- DLSS Options ---
  sl::DLSSOptions dlssOpt{};
  dlssOpt.mode = m_dlssMode;
#pragma warning(suppress : 4996) // sharpness is deprecated in DLSS 4.5 SDK but still functional
  dlssOpt.sharpness = m_sharpness;

  sl::Result dlssResult = slDLSSSetOptions(m_viewport, dlssOpt);
  if (dlssResult != sl::Result::eOk) {
    static uint64_t s_dlssOptWarn = 0;
    if (s_dlssOptWarn++ % 300 == 0) {
      LOG_WARN("[DLSS] slDLSSSetOptions failed: error {}", static_cast<int>(dlssResult));
    }
  }

  // --- DLSS-G (Frame Generation) Options ---
  if (m_dlssgLoaded) {
    sl::DLSSGOptions fgOpt{};
    fgOpt.mode = m_smartFgForceDisable ? sl::DLSSGMode::eOff : sl::DLSSGMode::eOn;
    // Use dynamic multiplier when smart FG is active, otherwise user's setting
    int effectiveMult = (m_smartFgEnabled && m_smartFgComputedMult >= 2) ? m_smartFgComputedMult : m_frameGenMultiplier;
    fgOpt.numFramesToGenerate = (std::max)(effectiveMult - 1, 0);
    fgOpt.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;

    sl::Result fgResult = slDLSSGSetOptions(m_viewport, fgOpt);
    if (fgResult != sl::Result::eOk) {
      static uint64_t s_fgOptWarn = 0;
      if (s_fgOptWarn++ % 300 == 0) {
        LOG_WARN("[DLSSG] slDLSSGSetOptions failed: error {} (mode:{} frames:{})", static_cast<int>(fgResult),
                 m_smartFgForceDisable ? "OFF" : "ON", fgOpt.numFramesToGenerate);
      }
    } else {
      static bool s_loggedSuccess = false;
      if (!s_loggedSuccess) {
        LOG_INFO("[DLSSG] Frame Generation options set: mode={} frames={}", m_smartFgForceDisable ? "OFF" : "ON",
                 fgOpt.numFramesToGenerate);
        s_loggedSuccess = true;
      }
    }
  }

  // --- Phase 1.1: DeepDVC Options (was completely missing!) ---
  if (m_deepDvcLoaded) {
    sl::DeepDVCOptions dvcOpt{};
    dvcOpt.mode = m_deepDvcEnabled ? sl::DeepDVCMode::eOn : sl::DeepDVCMode::eOff;
    dvcOpt.intensity = m_deepDvcIntensity;
    dvcOpt.saturationBoost = m_deepDvcSaturation;

    sl::Result dvcResult = slDeepDVCSetOptions(m_viewport, dvcOpt);
    if (dvcResult != sl::Result::eOk) {
      static uint64_t s_dvcOptWarn = 0;
      if (s_dvcOptWarn++ % 300 == 0) {
        LOG_WARN("[DeepDVC] slDeepDVCSetOptions failed: error {} (mode:{} intensity:{:.2f} sat:{:.2f})",
                 static_cast<int>(dvcResult), m_deepDvcEnabled ? "ON" : "OFF", m_deepDvcIntensity, m_deepDvcSaturation);
      }
    } else {
      static bool s_dvcLoggedSuccess = false;
      if (!s_dvcLoggedSuccess) {
        LOG_INFO("[DeepDVC] Options set: mode={} intensity={:.2f} saturation={:.2f}", m_deepDvcEnabled ? "ON" : "OFF",
                 m_deepDvcIntensity, m_deepDvcSaturation);
        s_dvcLoggedSuccess = true;
      }
    }
  }

  // --- Phase 1.1: Ray Reconstruction Options (was completely missing!) ---
  if (m_rrLoaded && m_rayReconstructionSupported) {
    sl::DLSSDOptions rrOpt{};
    rrOpt.mode = m_rayReconstructionEnabled ? m_dlssMode : sl::DLSSMode::eOff;
#pragma warning(suppress : 4996)
    rrOpt.sharpness = m_sharpness;
    // Set RR preset for all quality modes
    sl::DLSSDPreset rrPreset = static_cast<sl::DLSSDPreset>(m_rrPresetIndex);
    rrOpt.dlaaPreset = rrPreset;
    rrOpt.qualityPreset = rrPreset;
    rrOpt.balancedPreset = rrPreset;
    rrOpt.performancePreset = rrPreset;
    rrOpt.ultraPerformancePreset = rrPreset;
    rrOpt.ultraQualityPreset = rrPreset;

    sl::Result rrResult = slDLSSDSetOptions(m_viewport, rrOpt);
    if (rrResult != sl::Result::eOk) {
      static uint64_t s_rrOptWarn = 0;
      if (s_rrOptWarn++ % 300 == 0) {
        LOG_WARN("[DLSS-RR] slDLSSDSetOptions failed: error {} (enabled:{} preset:{})", static_cast<int>(rrResult),
                 m_rayReconstructionEnabled ? "YES" : "NO", m_rrPresetIndex);
      }
    } else {
      static bool s_rrLoggedSuccess = false;
      if (!s_rrLoggedSuccess) {
        LOG_INFO("[DLSS-RR] Ray Reconstruction options set: enabled={} preset={}",
                 m_rayReconstructionEnabled ? "YES" : "NO", m_rrPresetIndex);
        s_rrLoggedSuccess = true;
      }
    }
  }

  m_optionsDirty = false;
}

// ---------------------------------------------------------------------------
// Dynamic Smart Frame Gen â€” adjust FG multiplier based on rolling-average FPS
// ---------------------------------------------------------------------------

void StreamlineIntegration::UpdateFrameTiming(float fps) {
  m_lastBaseFps = fps;

  // Feed into the rolling ring buffer
  m_fpsRing[m_fpsRingIdx] = fps;
  m_fpsRingIdx = (m_fpsRingIdx + 1) % kFpsRingSize;
  if (m_fpsRingSamples < kFpsRingSize) ++m_fpsRingSamples;

  // If smart FG is enabled, recompute the dynamic multiplier
  if (m_smartFgEnabled) UpdateSmartFrameGen();
}

void StreamlineIntegration::UpdateSmartFrameGen() {
  // Compute rolling average
  if (m_fpsRingSamples < 3) return; // need a minimum history before making decisions
  float sum = 0.0f;
  for (int i = 0; i < m_fpsRingSamples; ++i) sum += m_fpsRing[i];
  m_smartFgRollingAvg = sum / static_cast<float>(m_fpsRingSamples);

  // Map FPS to a target multiplier.  Lower base FPS â†’ higher multiplier to
  // compensate.  Higher base FPS â†’ lower multiplier (diminishing returns and
  // higher latency penalty).
  //
  //   Base FPS â‰¤ 20  â†’ 4x  (generate 3 extra frames)
  //   Base FPS â‰¤ 40  â†’ 3x
  //   Base FPS â‰¤ 70  â†’ 2x
  //   Base FPS > 70  â†’ user's configured multiplier (or 2x minimum)
  //
  // Users can still override with m_smartFgAutoDisable to turn FG off entirely
  // above a certain threshold.

  int computed = 2; // floor
  if (m_smartFgRollingAvg <= 20.0f) {
    computed = 4;
  } else if (m_smartFgRollingAvg <= 40.0f) {
    computed = 3;
  } else if (m_smartFgRollingAvg <= 70.0f) {
    computed = 2;
  } else {
    // Above 70 FPS â€” use user's configured value (at least 2)
    computed = (std::max)(m_frameGenMultiplier, 2);
  }

  // Clamp to valid range [2, 4] for DLSS-G
  computed = std::clamp(computed, 2, 4);

  // Only apply if the computed value actually changed
  if (computed != m_smartFgComputedMult) {
    int prev = m_smartFgComputedMult;
    m_smartFgComputedMult = computed;
    m_optionsDirty = true;
    LOG_INFO("[SmartFG] Rolling avg {:.1f} FPS â†’ multiplier {}x (was {}x)", m_smartFgRollingAvg, computed, prev);
  }

  // Auto-disable: if rolling average exceeds the threshold, force-disable FG
  if (m_smartFgAutoDisable && m_smartFgRollingAvg > m_smartFgAutoDisableFps) {
    if (!m_smartFgForceDisable) {
      m_smartFgForceDisable = true;
      m_optionsDirty = true;
      LOG_INFO("[SmartFG] Base FPS {:.0f} > threshold {:.0f} â€” disabling FG", m_smartFgRollingAvg,
               m_smartFgAutoDisableFps);
    }
  } else if (m_smartFgForceDisable) {
    m_smartFgForceDisable = false;
    m_optionsDirty = true;
    LOG_INFO("[SmartFG] Base FPS {:.0f} â‰¤ threshold {:.0f} â€” re-enabling FG", m_smartFgRollingAvg,
             m_smartFgAutoDisableFps);
  }

  // Track the actual effective multiplier (for GUI display)
  m_fgActualMultiplier = m_smartFgForceDisable ? 1.0f : static_cast<float>(computed);
}

void StreamlineIntegration::UpdateSwapChain(IDXGISwapChain* pSwapChain) {
  if (!pSwapChain || m_pSwapChain.Get() == pSwapChain) return;
  m_pSwapChain = pSwapChain;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
  if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
    sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&m_backBuffer));
  }
}

void StreamlineIntegration::TagResources() {
  if (!m_initialized || !m_backBuffer || !m_frameToken) return;

  // Query ResourceDetector for best candidates
  ResourceDetector& detector = ResourceDetector::Get();

  ID3D12Resource* colorRes = m_colorBuffer.Get();
  if (!colorRes) colorRes = detector.GetBestColorCandidate();
  if (!colorRes) colorRes = m_backBuffer.Get();

  ID3D12Resource* depthRes = m_depthBuffer.Get();
  if (!depthRes) depthRes = detector.GetBestDepthCandidate();

  ID3D12Resource* mvRes = m_motionVectors.Get();
  if (!mvRes) mvRes = detector.GetBestMotionVectorCandidate();

  // Validate we have the critical resources for DLSS-G
  bool hasAllResources = colorRes && depthRes && mvRes;

  static uint64_t s_lastTagLogFrame = 0;
  uint64_t currentFrame = detector.GetFrameCount();
  bool doLog = (currentFrame != s_lastTagLogFrame && currentFrame % 300 == 0);

  if (doLog) {
    LOG_INFO("[DLSSG] TagResources: Color={:p} Depth={:p} MV={:p} HUDLess={:p} Ready={}", static_cast<void*>(colorRes),
             static_cast<void*>(depthRes), static_cast<void*>(mvRes), static_cast<void*>(m_hudLessBuffer.Get()),
             hasAllResources ? "YES" : "NO");
    s_lastTagLogFrame = currentFrame;
  }

  // Build resource tags for all available resources
  std::vector<sl::ResourceTag> tags;
  tags.reserve(7); // Color + Depth + MV + Output + HUDLess + Exposure = up to 6

  m_viewport = sl::ViewportHandle(0);

  // Color input (required)
  sl::Resource colorSL(sl::ResourceType::eTex2d, colorRes);
  tags.push_back(
      sl::ResourceTag(&colorSL, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent));

  // Depth buffer (critical for DLSS-G)
  if (depthRes) {
    sl::Resource depthSL(sl::ResourceType::eTex2d, depthRes);
    tags.push_back(sl::ResourceTag(&depthSL, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent));
  }

  // Motion vectors (critical for DLSS-G)
  if (mvRes) {
    sl::Resource mvSL(sl::ResourceType::eTex2d, mvRes);
    tags.push_back(sl::ResourceTag(&mvSL, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent));
  }

  // Output color (use backbuffer)
  sl::Resource outputSL(sl::ResourceType::eTex2d, m_backBuffer.Get());
  tags.push_back(
      sl::ResourceTag(&outputSL, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent));

  // Phase 1.5: HUD-less color buffer — enables DLSS-G to exclude overlay elements
  // from interpolation, preventing ghosting artifacts on UI/HUD
  ID3D12Resource* hudLessRes = m_hudLessBuffer.Get();
  if (!hudLessRes) {
    // Query ResourceDetector for auto-detected pre-UI render target
    hudLessRes = detector.GetBestHUDLessCandidate();
  }
  if (!hudLessRes) {
    // Final fallback: use color buffer as HUD-less (pre-UI RT is pre-overlay by definition)
    hudLessRes = colorRes;
  }
  if (hudLessRes) {
    sl::Resource hudLessSL(sl::ResourceType::eTex2d, hudLessRes);
    tags.push_back(sl::ResourceTag(&hudLessSL, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent));
  }

  // Exposure buffer (improves DLSS tonemapping stability)
  ID3D12Resource* exposureRes = detector.GetExposureResource();
  if (exposureRes) {
    sl::Resource exposureSL(sl::ResourceType::eTex2d, exposureRes);
    tags.push_back(sl::ResourceTag(&exposureSL, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilPresent));
  }

  // Tag all resources in one call (frame-based tagging)
  sl::Result result =
      slSetTagForFrame(*m_frameToken, m_viewport, tags.data(), static_cast<uint32_t>(tags.size()), nullptr);

  if (result != sl::Result::eOk && doLog) {
    LOG_WARN("[DLSSG] slSetTagForFrame failed with error {}", static_cast<int>(result));
  }
}

bool StreamlineIntegration::EnsureCommandList() {
  if (!m_pCommandAllocator) {
    HRESULT hr = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pCommandAllocator));
    if (FAILED(hr)) {
      LOG_ERROR("Failed to create command allocator (HRESULT: {:08X})", static_cast<unsigned>(hr));
      return false;
    }
  }
  if (!m_pCommandList) {
    HRESULT hr = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pCommandAllocator.Get(), nullptr,
                                              IID_PPV_ARGS(&m_pCommandList));
    if (FAILED(hr)) {
      LOG_ERROR("Failed to create command list (HRESULT: {:08X})", static_cast<unsigned>(hr));
      return false;
    }
    m_pCommandList->Close();
  }
  return true;
}

// Phase 1.2: Per-feature command list initialization
bool StreamlineIntegration::EnsureFeatureCommandList(FeatureSlot slot) {
  int idx = static_cast<int>(slot);
  auto& gpu = m_featureGPU[idx];
  if (gpu.allocator && gpu.cmdList && gpu.fence) return true;

  if (!m_pDevice) return false;

  if (!gpu.allocator) {
    HRESULT hr = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
    if (FAILED(hr)) return false;
  }
  if (!gpu.cmdList) {
    HRESULT hr = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&gpu.cmdList));
    if (FAILED(hr)) return false;
    gpu.cmdList->Close();
  }
  if (!gpu.fence) {
    HRESULT hr = m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    if (FAILED(hr)) return false;
    gpu.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    gpu.fenceValue = 0;
  }
  return true;
}

// Phase 1.2: Wait for specific feature's GPU work to complete
void StreamlineIntegration::WaitForFeature(FeatureSlot slot) {
  auto& gpu = m_featureGPU[static_cast<int>(slot)];
  if (!gpu.fence || !gpu.fenceEvent || !m_pCommandQueue) return;
  if (gpu.fence->GetCompletedValue() < gpu.fenceValue) {
    gpu.fence->SetEventOnCompletion(gpu.fenceValue, gpu.fenceEvent);
    WaitForSingleObject(gpu.fenceEvent, 5000);
  }
}

void StreamlineIntegration::WaitForGpu() {
  if (!m_gpuFence || !m_gpuFenceEvent || !m_pCommandQueue) return;
  if (m_gpuFence->GetCompletedValue() < m_gpuFenceValue) {
    m_gpuFence->SetEventOnCompletion(m_gpuFenceValue, m_gpuFenceEvent);
    WaitForSingleObject(m_gpuFenceEvent, 5000);
  }
}

void StreamlineIntegration::SetCommandQueue(ID3D12CommandQueue* pQueue) {
  m_pCommandQueue = pQueue;
}

void StreamlineIntegration::ReflexMarker(sl::PCLMarker marker) {
  if (m_initialized && m_reflexLoaded && m_frameToken) slPCLSetMarker(marker, *m_frameToken);
}

void StreamlineIntegration::UpdateControls() {
  ImGuiOverlay::Get().UpdateControls();
}

void StreamlineIntegration::ToggleDebugMode(bool /*enabled*/) {
  // No-op or log
}

void StreamlineIntegration::PrintDLSSGStatus() {
  LOG_INFO("[DLSSG] Frame Gen: {}x, Mode: {}, Status: {}", m_frameGenMultiplier, static_cast<int>(m_dlssMode),
           static_cast<int>(m_dlssgStatus));
}

void StreamlineIntegration::ReflexSleep() {
  if (m_reflexLoaded && m_frameToken) slReflexSleep(*m_frameToken);
}

void StreamlineIntegration::ReleaseResources() {
  m_colorBuffer.Reset();
  m_depthBuffer.Reset();
  m_motionVectors.Reset();
  m_hudLessBuffer.Reset();
  m_backBuffer.Reset();
}

// Phase 1.4: Cache camera data for fallback when scanner misses
void StreamlineIntegration::CacheCameraData(const float* view, const float* proj) {
  if (view) {
    std::copy_n(view, 16, m_cachedViewMatrix);
    m_hasCachedCamera = true;
  }
  if (proj) {
    std::copy_n(proj, 16, m_cachedProjMatrix);
    m_hasCachedCamera = true;
  }
  m_lastCameraUpdateFrame = m_frameIndex;
}

bool StreamlineIntegration::GetCachedCameraData(float* view, float* proj) const {
  if (!m_hasCachedCamera) return false;
  // Only use cached data if it's recent (within 120 frames ~ 2 seconds)
  if (m_frameIndex - m_lastCameraUpdateFrame > 120) return false;
  if (view) std::copy_n(m_cachedViewMatrix, 16, view);
  if (proj) std::copy_n(m_cachedProjMatrix, 16, proj);
  return true;
}
