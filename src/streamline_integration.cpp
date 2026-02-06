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
  inline bool slFailed(sl::Result r) { return r != sl::Result::eOk; }
  inline bool slSucceeded(sl::Result r) { return r == sl::Result::eOk; }
} // namespace

StreamlineIntegration &StreamlineIntegration::Get() {
  static StreamlineIntegration instance;
  return instance;
}

StreamlineIntegration::~StreamlineIntegration() { Shutdown(); }

bool StreamlineIntegration::Initialize(ID3D12Device *pDevice) {
  if (m_initialized)
    return true;
  if (!pDevice)
    return false;
  m_pDevice = pDevice;

  ConfigManager::Get().Load();
  ModConfig &cfg = ConfigManager::Get().Data();

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
  pref.flags |= sl::PreferenceFlags::eUseManualHooking |
                sl::PreferenceFlags::eUseFrameBasedResourceTagging;

  m_featuresToLoad[0] = sl::kFeatureDLSS;
  m_featureCount = 1;
  if (m_frameGenMultiplier >= 2)
    m_featuresToLoad[m_featureCount++] = sl::kFeatureDLSS_G;
  if (m_reflexEnabled) {
    m_featuresToLoad[m_featureCount++] = sl::kFeatureReflex;
    m_featuresToLoad[m_featureCount++] = sl::kFeaturePCL;
  }
  if (m_rayReconstructionEnabled)
    m_featuresToLoad[m_featureCount++] = sl::kFeatureDLSS_RR;
  m_featuresToLoad[m_featureCount++] = sl::kFeatureDeepDVC;

  pref.featuresToLoad = m_featuresToLoad;
  pref.numFeaturesToLoad = m_featureCount;

  if (slFailed(slInit(pref, sl::kSDKVersion)))
    return false;
  if (slFailed(slSetD3DDevice(pDevice)))
    return false;

  // Create GPU synchronization fence for safe command allocator reuse
  HRESULT fenceHr = pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_gpuFence));
  if (SUCCEEDED(fenceHr)) {
    m_gpuFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_gpuFenceValue = 0;
  } else {
    LOG_WARN("Failed to create GPU sync fence (HRESULT: {:08X})", static_cast<unsigned>(fenceHr));
  }

  m_initialized = true;
  m_dlssSupported = true; // Assume supported for now or check requirements
  m_dlssgSupported = (m_frameGenMultiplier >= 2);
  m_dlssgLoaded = m_dlssgSupported;
  m_reflexLoaded = m_reflexEnabled;
  m_rrLoaded = m_rayReconstructionEnabled;
  m_deepDvcLoaded = true;
  m_deepDvcSupported = true;

  LOG_INFO("Streamline initialized (Modern)");
  return true;
}

void StreamlineIntegration::Shutdown() {
  if (m_initialized) {
    WaitForGpu();
    slShutdown();
    m_initialized = false;
  }
  if (m_gpuFenceEvent) {
    CloseHandle(m_gpuFenceEvent);
    m_gpuFenceEvent = nullptr;
  }
}

void StreamlineIntegration::NewFrame(IDXGISwapChain *pSwapChain) {
  if (!m_initialized)
    return;
  UpdateSwapChain(pSwapChain);
  slGetNewFrameToken(m_frameToken, &m_frameIndex);
  TagResources();
}

void StreamlineIntegration::SetDLSSModeIndex(int index) {
  static const sl::DLSSMode kModes[] = {
      sl::DLSSMode::eOff,          sl::DLSSMode::eMaxPerformance,
      sl::DLSSMode::eBalanced,     sl::DLSSMode::eMaxQuality,
      sl::DLSSMode::eUltraQuality, sl::DLSSMode::eDLAA};
  if (index >= 0 && index < 6) {
    m_dlssMode = kModes[index];
    m_optionsDirty = true;
  }
}

int StreamlineIntegration::GetDLSSModeIndex() const {
  switch (m_dlssMode) {
  case sl::DLSSMode::eOff:
    return 0;
  case sl::DLSSMode::eMaxPerformance:
    return 1;
  case sl::DLSSMode::eBalanced:
    return 2;
  case sl::DLSSMode::eMaxQuality:
    return 3;
  case sl::DLSSMode::eUltraQuality:
    return 4;
  case sl::DLSSMode::eDLAA:
    return 5;
  default:
    return 3;
  }
}

void StreamlineIntegration::SetRRPreset(int preset) {
  m_rrPresetIndex = preset;
  m_optionsDirty = true;
}

void StreamlineIntegration::SetCameraData(const float *view, const float *proj,
                                          float jitterX, float jitterY) {
  if (!m_initialized || !m_frameToken)
    return;
  sl::Constants consts{};
  if (view)
    std::copy_n(view, 16, reinterpret_cast<float*>(&consts.cameraViewToClip));
  if (proj)
    std::copy_n(proj, 16, reinterpret_cast<float*>(&consts.clipToCameraView));
  consts.jitterOffset = sl::float2(jitterX, jitterY);
  consts.mvecScale = sl::float2(m_mvecScaleX, m_mvecScaleY);
  m_hasCameraData = (view != nullptr);
  m_viewport = sl::ViewportHandle(0);
  slSetConstants(consts, *m_frameToken, m_viewport);
}

void StreamlineIntegration::EvaluateDLSS(ID3D12GraphicsCommandList *pCmdList) {
  if (!m_initialized || !pCmdList || !m_frameToken)
    return;
  if (m_optionsDirty)
    UpdateOptions();
  m_viewport = sl::ViewportHandle(0);
  const sl::BaseStructure *inputs[] = {&m_viewport};
  slEvaluateFeature(sl::kFeatureDLSS, *m_frameToken, inputs, 1, pCmdList);
}

void StreamlineIntegration::EvaluateFrameGen(IDXGISwapChain *pSwapChain) {
  if (!m_initialized || !m_dlssgLoaded || !m_pCommandQueue || !m_frameToken)
    return;
  if (!EnsureCommandList())
    return;

  // Wait for previous GPU work to finish before resetting the allocator
  WaitForGpu();

  HRESULT hr = m_pCommandAllocator->Reset();
  if (FAILED(hr)) { LOG_WARN("[DLSSG] Command allocator reset failed"); return; }
  hr = m_pCommandList->Reset(m_pCommandAllocator.Get(), nullptr);
  if (FAILED(hr)) { LOG_WARN("[DLSSG] Command list reset failed"); return; }

  m_viewport = sl::ViewportHandle(0);
  const sl::BaseStructure *inputs[] = {&m_viewport};
  slEvaluateFeature(sl::kFeatureDLSS_G, *m_frameToken, inputs, 1,
                    m_pCommandList.Get());
  m_pCommandList->Close();
  ID3D12CommandList *lists[] = {m_pCommandList.Get()};
  m_pCommandQueue->ExecuteCommandLists(1, lists);

  // Signal fence so next call knows when GPU is done
  if (m_gpuFence) {
    m_gpuFenceValue++;
    m_pCommandQueue->Signal(m_gpuFence.Get(), m_gpuFenceValue);
  }
}

void StreamlineIntegration::EvaluateDeepDVC(IDXGISwapChain *pSwapChain) {
  if (!m_initialized || !m_deepDvcLoaded || !m_pCommandQueue || !m_frameToken)
    return;
  if (!EnsureCommandList())
    return;

  // Wait for previous GPU work to finish before resetting the allocator
  WaitForGpu();

  HRESULT hr = m_pCommandAllocator->Reset();
  if (FAILED(hr)) { LOG_WARN("[DeepDVC] Command allocator reset failed"); return; }
  hr = m_pCommandList->Reset(m_pCommandAllocator.Get(), nullptr);
  if (FAILED(hr)) { LOG_WARN("[DeepDVC] Command list reset failed"); return; }

  m_viewport = sl::ViewportHandle(0);
  const sl::BaseStructure *inputs[] = {&m_viewport};
  slEvaluateFeature(sl::kFeatureDeepDVC, *m_frameToken, inputs, 1,
                    m_pCommandList.Get());
  m_pCommandList->Close();
  ID3D12CommandList *lists[] = {m_pCommandList.Get()};
  m_pCommandQueue->ExecuteCommandLists(1, lists);

  // Signal fence so next call knows when GPU is done
  if (m_gpuFence) {
    m_gpuFenceValue++;
    m_pCommandQueue->Signal(m_gpuFence.Get(), m_gpuFenceValue);
  }
}

void StreamlineIntegration::UpdateOptions() {
  if (!m_initialized)
    return;
  sl::DLSSOptions dlssOpt{};
  dlssOpt.mode = m_dlssMode;
  dlssOpt.sharpness = m_sharpness;
  m_viewport = sl::ViewportHandle(0);

  sl::Result dlssResult = slDLSSSetOptions(m_viewport, dlssOpt);
  if (dlssResult != sl::Result::eOk) {
    static uint64_t s_dlssOptWarn = 0;
    if (s_dlssOptWarn++ % 300 == 0) {
      LOG_WARN("[DLSS] slDLSSSetOptions failed: error {}",
               static_cast<int>(dlssResult));
    }
  }

  if (m_dlssgLoaded) {
    sl::DLSSGOptions fgOpt{};
    fgOpt.mode =
        m_smartFgForceDisable ? sl::DLSSGMode::eOff : sl::DLSSGMode::eOn;
    fgOpt.numFramesToGenerate = m_frameGenMultiplier - 1;

    sl::Result fgResult = slDLSSGSetOptions(m_viewport, fgOpt);
    if (fgResult != sl::Result::eOk) {
      static uint64_t s_fgOptWarn = 0;
      if (s_fgOptWarn++ % 300 == 0) {
        LOG_WARN(
            "[DLSSG] slDLSSGSetOptions failed: error {} (mode:{} frames:{})",
            static_cast<int>(fgResult), m_smartFgForceDisable ? "OFF" : "ON",
            fgOpt.numFramesToGenerate);
      }
    } else {
      static bool s_loggedSuccess = false;
      if (!s_loggedSuccess) {
        LOG_INFO("[DLSSG] Frame Generation options set: mode={} frames={}",
                 m_smartFgForceDisable ? "OFF" : "ON",
                 fgOpt.numFramesToGenerate);
        s_loggedSuccess = true;
      }
    }
  }
  m_optionsDirty = false;
}

void StreamlineIntegration::UpdateSwapChain(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain || m_pSwapChain.Get() == pSwapChain)
    return;
  m_pSwapChain = pSwapChain;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
  if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
    sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(),
                   IID_PPV_ARGS(&m_backBuffer));
  }
}

void StreamlineIntegration::TagResources() {
  if (!m_initialized || !m_backBuffer || !m_frameToken)
    return;

  // Query ResourceDetector for best candidates
  ResourceDetector &detector = ResourceDetector::Get();

  ID3D12Resource *colorRes = m_colorBuffer.Get();
  if (!colorRes)
    colorRes = detector.GetBestColorCandidate();
  if (!colorRes)
    colorRes = m_backBuffer.Get();

  ID3D12Resource *depthRes = m_depthBuffer.Get();
  if (!depthRes)
    depthRes = detector.GetBestDepthCandidate();

  ID3D12Resource *mvRes = m_motionVectors.Get();
  if (!mvRes)
    mvRes = detector.GetBestMotionVectorCandidate();

  // Validate we have the critical resources for DLSS-G
  bool hasAllResources = colorRes && depthRes && mvRes;

  static uint64_t s_lastTagLogFrame = 0;
  uint64_t currentFrame = detector.GetFrameCount();
  bool doLog = (currentFrame != s_lastTagLogFrame && currentFrame % 300 == 0);

  if (doLog) {
    LOG_INFO("[DLSSG] TagResources: Color={:p} Depth={:p} MV={:p} Ready={}",
             static_cast<void*>(colorRes), static_cast<void*>(depthRes), static_cast<void*>(mvRes),
             hasAllResources ? "YES" : "NO");
    s_lastTagLogFrame = currentFrame;
  }

  // Build resource tags for all available resources
  std::vector<sl::ResourceTag> tags;
  tags.reserve(4);

  m_viewport = sl::ViewportHandle(0);

  // Color input (required)
  sl::Resource colorSL(sl::ResourceType::eTex2d, colorRes);
  tags.push_back(sl::ResourceTag(&colorSL, sl::kBufferTypeScalingInputColor,
                                 sl::ResourceLifecycle::eValidUntilPresent));

  // Depth buffer (critical for DLSS-G)
  if (depthRes) {
    sl::Resource depthSL(sl::ResourceType::eTex2d, depthRes);
    tags.push_back(sl::ResourceTag(&depthSL, sl::kBufferTypeDepth,
                                   sl::ResourceLifecycle::eValidUntilPresent));
  }

  // Motion vectors (critical for DLSS-G)
  if (mvRes) {
    sl::Resource mvSL(sl::ResourceType::eTex2d, mvRes);
    tags.push_back(sl::ResourceTag(&mvSL, sl::kBufferTypeMotionVectors,
                                   sl::ResourceLifecycle::eValidUntilPresent));
  }

  // Output/HUDless (use backbuffer)
  sl::Resource outputSL(sl::ResourceType::eTex2d, m_backBuffer.Get());
  tags.push_back(sl::ResourceTag(&outputSL, sl::kBufferTypeScalingOutputColor,
                                 sl::ResourceLifecycle::eValidUntilPresent));

  // Tag all resources in one call
  sl::Result result = slSetTag(m_viewport, tags.data(),
                               static_cast<uint32_t>(tags.size()), nullptr);

  if (result != sl::Result::eOk && doLog) {
    LOG_WARN("[DLSSG] slSetTag failed with error {}", static_cast<int>(result));
  }
}

bool StreamlineIntegration::EnsureCommandList() {
  if (!m_pCommandAllocator) {
    HRESULT hr = m_pDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pCommandAllocator));
    if (FAILED(hr)) {
      LOG_ERROR("Failed to create command allocator (HRESULT: {:08X})", static_cast<unsigned>(hr));
      return false;
    }
  }
  if (!m_pCommandList) {
    HRESULT hr = m_pDevice->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pCommandAllocator.Get(), nullptr,
        IID_PPV_ARGS(&m_pCommandList));
    if (FAILED(hr)) {
      LOG_ERROR("Failed to create command list (HRESULT: {:08X})", static_cast<unsigned>(hr));
      return false;
    }
    m_pCommandList->Close();
  }
  return true;
}

void StreamlineIntegration::WaitForGpu() {
  if (!m_gpuFence || !m_gpuFenceEvent || !m_pCommandQueue)
    return;
  if (m_gpuFence->GetCompletedValue() < m_gpuFenceValue) {
    m_gpuFence->SetEventOnCompletion(m_gpuFenceValue, m_gpuFenceEvent);
    WaitForSingleObject(m_gpuFenceEvent, 5000); // 5s timeout to avoid infinite hang
  }
}

void StreamlineIntegration::SetCommandQueue(ID3D12CommandQueue *pQueue) {
  m_pCommandQueue = pQueue;
}

void StreamlineIntegration::ReflexMarker(sl::PCLMarker marker) {
  if (m_initialized && m_reflexLoaded && m_frameToken)
    slPCLSetMarker(marker, *m_frameToken);
}

void StreamlineIntegration::UpdateControls() {
  ImGuiOverlay::Get().UpdateControls();
}

void StreamlineIntegration::ToggleDebugMode(bool enabled) {
  // No-op or log
}

void StreamlineIntegration::PrintDLSSGStatus() {
  LOG_INFO("[DLSSG] Frame Gen: {}x, Mode: {}, Status: {}", m_frameGenMultiplier,
           static_cast<int>(m_dlssMode), static_cast<int>(m_dlssgStatus));
}

void StreamlineIntegration::ReflexSleep() {
  if (m_reflexLoaded && m_frameToken)
    slReflexSleep(*m_frameToken);
}
void StreamlineIntegration::ReleaseResources() {
  m_colorBuffer.Reset();
  m_depthBuffer.Reset();
  m_motionVectors.Reset();
  m_backBuffer.Reset();
}
