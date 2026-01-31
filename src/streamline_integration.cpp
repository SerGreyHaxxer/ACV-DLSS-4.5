#include "streamline_integration.h"
#include "logger.h"
#include "dlss4_config.h"
#include "resource_detector.h"
#include "config_manager.h"
#include <string>
#include <array>
#include <dxgi1_4.h>

#define SL_FAILED(x) ((x) != sl::Result::eOk)
#define SL_SUCCEEDED(x) ((x) == sl::Result::eOk)

StreamlineIntegration& StreamlineIntegration::Get() {
    static StreamlineIntegration instance;
    return instance;
}

bool StreamlineIntegration::Initialize(ID3D12Device* pDevice) {
    if (m_initialized) return true;
    
    // Load Config
    ConfigManager::Get().Load();
    ModConfig& cfg = ConfigManager::Get().Data();
    m_dlssMode = (sl::DLSSMode)cfg.dlssMode;
    m_frameGenMultiplier = cfg.frameGenMultiplier;
    m_sharpness = cfg.sharpness;
    m_lodBias = cfg.lodBias;
    m_dlssPreset = (sl::DLSSPreset)cfg.dlssPreset;
    m_mvecScaleX = cfg.mvecScaleX;
    m_mvecScaleY = cfg.mvecScaleY;
    m_reflexEnabled = cfg.reflexEnabled;
    m_hudFixEnabled = cfg.hudFixEnabled;
    
    LOG_INFO("Initializing NVIDIA Streamline...");

    if (!pDevice) {
        LOG_ERROR("Streamline init failed: null device");
        return false;
    }

    sl::Preferences pref{};
    pref.showConsole = true;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.engine = sl::EngineType::eCustom;
    pref.applicationId = static_cast<uint64_t>(NGX_APP_ID);
    pref.flags |= sl::PreferenceFlags::eUseManualHooking;
    pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    m_featuresToLoad[0] = sl::kFeatureDLSS;
    m_featuresToLoad[1] = sl::kFeatureDLSS_G;
    m_featuresToLoad[2] = sl::kFeatureDLSS_MFG;
    m_featuresToLoad[3] = sl::kFeatureDLSS_RR;
    m_featuresToLoad[4] = sl::kFeatureReflex;
    m_featureCount = 5;
    pref.featuresToLoad = m_featuresToLoad;
    pref.numFeaturesToLoad = m_featureCount;

    sl::Result res = sl::slInit(pref, sl::kSDKVersion);
    if (SL_FAILED(res)) {
        LOG_ERROR("slInit failed: %d", (int)res);
        return false;
    }

    res = sl::slSetD3DDevice(pDevice);
    if (SL_FAILED(res)) {
        LOG_ERROR("slSetD3DDevice failed: %d", (int)res);
        return false;
    }
    
    m_pDevice = pDevice;
    m_initialized = true;

    sl::FeatureRequirements requirements{};
    if (SL_SUCCEEDED(sl::slGetFeatureRequirements(sl::kFeatureDLSS, requirements))) {
        m_dlssSupported = (requirements.flags & static_cast<uint32_t>(sl::FeatureRequirementFlags::eD3D12Supported)) != 0;
    }
    if (SL_SUCCEEDED(sl::slGetFeatureRequirements(sl::kFeatureDLSS_G, requirements))) {
        m_dlssgSupported = (requirements.flags & static_cast<uint32_t>(sl::FeatureRequirementFlags::eD3D12Supported)) != 0;
    }
    if (SL_SUCCEEDED(sl::slGetFeatureRequirements(sl::kFeatureDLSS_MFG, requirements))) {
        m_mfgSupported = (requirements.flags & static_cast<uint32_t>(sl::FeatureRequirementFlags::eD3D12Supported)) != 0;
    }
    if (SL_SUCCEEDED(sl::slGetFeatureRequirements(sl::kFeatureDLSS_RR, requirements))) {
        m_rayReconstructionSupported = (requirements.flags & static_cast<uint32_t>(sl::FeatureRequirementFlags::eD3D12Supported)) != 0;
    }

    m_dlssEnabled = DLSS4_ENABLE_SUPER_RESOLUTION != 0;
    m_rayReconstructionEnabled = DLSS4_ENABLE_RAY_RECONSTRUCTION != 0;
    if (m_frameGenMultiplier <= 0) m_frameGenMultiplier = DLSS4_FRAME_GEN_MULTIPLIER;
    if (m_dlssMode < sl::DLSSMode::eOff || m_dlssMode > sl::DLSSMode::eDLAA) {
        m_dlssMode = sl::DLSSMode::eDLAA;
    }
    if (m_dlssPreset < sl::DLSSPreset::eDefault || m_dlssPreset > sl::DLSSPreset::ePresetG) {
        m_dlssPreset = sl::DLSSPreset::eDefault;
    }
    m_useMfg = m_frameGenMultiplier > 2;

    return true;
}

void StreamlineIntegration::Shutdown() {
    if (m_initialized) {
        if (m_frameToken) m_frameToken = nullptr;
        
        m_backBuffer.Reset();
        m_pCommandList.Reset();
        m_pCommandAllocator.Reset();
        m_pFence.Reset();
        m_pCommandQueue.Reset();
        m_pSwapChain.Reset();
        
        if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
        
        sl::slShutdown();
        m_initialized = false;
    }
}

void StreamlineIntegration::NewFrame(IDXGISwapChain* pSwapChain) {
    if (!m_initialized) return;
    m_needNewFrameToken = true;
    UpdateSwapChain(pSwapChain);
    
    // Pull candidates
    ID3D12Resource* color = ResourceDetector::Get().GetBestColorCandidate();
    ID3D12Resource* depth = ResourceDetector::Get().GetBestDepthCandidate();
    ID3D12Resource* mvec = ResourceDetector::Get().GetBestMotionVectorCandidate();
    if (color) TagColorBuffer(color);
    if (depth) TagDepthBuffer(depth);
    if (mvec) TagMotionVectors(mvec);

    // Defer UpdateOptions/TagResources until Evaluate to ensure a frame token is available
}

void StreamlineIntegration::TagColorBuffer(ID3D12Resource* pResource) { if (m_initialized && pResource) m_colorBuffer = pResource; }
void StreamlineIntegration::TagDepthBuffer(ID3D12Resource* pResource) { if (m_initialized && pResource) m_depthBuffer = pResource; }
void StreamlineIntegration::TagMotionVectors(ID3D12Resource* pResource) { if (m_initialized && pResource) m_motionVectors = pResource; }

void StreamlineIntegration::SetCameraData(const float* view, const float* proj, float jitterX, float jitterY) {
    if (!m_initialized || !view || !proj) return;
    sl::Constants consts{};
    for (int i = 0; i < 16; ++i) {
        reinterpret_cast<float*>(&consts.cameraViewToClip)[i] = view[i];
        reinterpret_cast<float*>(&consts.cameraClipToView)[i] = proj[i];
        reinterpret_cast<float*>(&consts.cameraViewToWorld)[i] = view[i];
        reinterpret_cast<float*>(&consts.cameraWorldToView)[i] = proj[i];
    }
    consts.jitterOffset = sl::float2(jitterX, jitterY);
    consts.mvecScale = sl::float2(m_mvecScaleX, m_mvecScaleY); // Use configured scale
    consts.depthInverted = sl::Boolean::eFalse;
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.reset = sl::Boolean::eFalse;

    if (!EnsureFrameToken()) return;
    sl::slSetConstants(consts, *m_frameToken, m_viewport);
}

void StreamlineIntegration::EvaluateDLSS(ID3D12GraphicsCommandList* pCmdList) {
    if (!m_initialized || !m_dlssSupported || !m_dlssEnabled || !pCmdList) return;
    if (!EnsureFrameToken()) return;
    if (m_optionsDirty) UpdateOptions();
    TagResources();
    const sl::BaseStructure* inputs[] = { &m_viewport };
    sl::slEvaluateFeature(sl::kFeatureDLSS, *m_frameToken, inputs, 1, pCmdList);
    if (m_rayReconstructionEnabled && m_rayReconstructionSupported) {
        sl::slEvaluateFeature(sl::kFeatureDLSS_RR, *m_frameToken, inputs, 1, pCmdList);
    }
}

void StreamlineIntegration::EvaluateFrameGen(IDXGISwapChain* pSwapChain) {
    if (!m_initialized || m_frameGenMultiplier < 2) return;
    UpdateSwapChain(pSwapChain);
    if (!EnsureFrameToken()) return;
    EnsureCommandList();
    if (!m_pCommandList || !m_pCommandAllocator || !m_pCommandQueue) return;

    m_pCommandAllocator->Reset();
    m_pCommandList->Reset(m_pCommandAllocator.Get(), nullptr);
    if (m_optionsDirty) UpdateOptions();
    TagResources();
    EvaluateDLSS(m_pCommandList.Get());

    const sl::BaseStructure* inputs[] = { &m_viewport };
    if (m_useMfg && m_mfgSupported) {
        sl::slEvaluateFeature(sl::kFeatureDLSS_MFG, *m_frameToken, inputs, 1, m_pCommandList.Get());
    } else if (m_dlssgSupported) {
        sl::slEvaluateFeature(sl::kFeatureDLSS_G, *m_frameToken, inputs, 1, m_pCommandList.Get());
    }
    m_pCommandList->Close();
    ID3D12CommandList* lists[] = { m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(1, lists);
}

// Getters/Setters
void StreamlineIntegration::SetDLSSMode(int mode) { 
    if(m_initialized) { 
        m_dlssMode = static_cast<sl::DLSSMode>(mode); 
        m_optionsDirty = true;
        ConfigManager::Get().Data().dlssMode = mode;
        ConfigManager::Get().Save();
    } 
}
void StreamlineIntegration::SetDLSSModeIndex(int modeIndex) {
    static const sl::DLSSMode kUiToMode[] = {
        sl::DLSSMode::eOff,
        sl::DLSSMode::eMaxPerformance,
        sl::DLSSMode::eBalanced,
        sl::DLSSMode::eMaxQuality,
        sl::DLSSMode::eUltraQuality,
        sl::DLSSMode::eDLAA
    };
    if (modeIndex < 0 || modeIndex >= (int)(sizeof(kUiToMode) / sizeof(kUiToMode[0]))) {
        return;
    }
    m_dlssMode = kUiToMode[modeIndex];
    m_optionsDirty = true;
    ConfigManager::Get().Data().dlssMode = static_cast<int>(m_dlssMode);
    ConfigManager::Get().Save();
}
int StreamlineIntegration::GetDLSSModeIndex() const {
    switch (m_dlssMode) {
    case sl::DLSSMode::eOff: return 0;
    case sl::DLSSMode::eMaxPerformance: return 1;
    case sl::DLSSMode::eBalanced: return 2;
    case sl::DLSSMode::eMaxQuality: return 3;
    case sl::DLSSMode::eUltraQuality: return 4;
    case sl::DLSSMode::eDLAA: return 5;
    case sl::DLSSMode::eUltraPerformance: return 1;
    default: return 3;
    }
}
void StreamlineIntegration::SetDLSSPreset(int preset) {
    if(m_initialized) {
        m_dlssPreset = static_cast<sl::DLSSPreset>(preset);
        m_optionsDirty = true;
        ConfigManager::Get().Data().dlssPreset = preset;
        ConfigManager::Get().Save();
        LOG_INFO("DLSS Preset set to: %d", preset);
    }
}
void StreamlineIntegration::SetFrameGenMultiplier(int multiplier) {
    if (!m_initialized) return;
    m_frameGenMultiplier = multiplier;
    m_optionsDirty = true;
    ConfigManager::Get().Data().frameGenMultiplier = multiplier;
    ConfigManager::Get().Save();
    LOG_INFO("Frame Gen Multiplier set to: %dx", multiplier);
}
void StreamlineIntegration::SetCommandQueue(ID3D12CommandQueue* pQueue) { 
    if(!m_initialized || !pQueue || m_pCommandQueue.Get() == pQueue) return;
    
    // ComPtr assignment handles Release/AddRef
    m_pCommandQueue = pQueue;
    m_optionsDirty = true;
}
void StreamlineIntegration::SetSharpness(float sharpness) { 
    m_sharpness = sharpness; 
    m_optionsDirty = true; 
    ConfigManager::Get().Data().sharpness = sharpness;
    ConfigManager::Get().Save();
}
void StreamlineIntegration::SetLODBias(float bias) { 
    m_lodBias = bias; 
    ConfigManager::Get().Data().lodBias = bias;
    ConfigManager::Get().Save();
}
void StreamlineIntegration::SetMVecScale(float x, float y) {
    m_mvecScaleX = x;
    m_mvecScaleY = y;
    ConfigManager::Get().Data().mvecScaleX = x;
    ConfigManager::Get().Data().mvecScaleY = y;
    ConfigManager::Get().Save();
}
void StreamlineIntegration::SetReflexEnabled(bool enabled) {
    m_reflexEnabled = enabled;
    ConfigManager::Get().Data().reflexEnabled = enabled;
    ConfigManager::Get().Save();
}
void StreamlineIntegration::SetHUDFixEnabled(bool enabled) {
    m_hudFixEnabled = enabled;
    ConfigManager::Get().Data().hudFixEnabled = enabled;
    ConfigManager::Get().Save();
}

void StreamlineIntegration::CycleDLSSMode() {
    int current = GetDLSSModeIndex();
    current = (current + 1) % 6; // 0-5
    SetDLSSModeIndex(current);
}
void StreamlineIntegration::CycleDLSSPreset() {
    int current = (int)m_dlssPreset;
    current = (current + 1) % 7; // 0-6
    if (current == 0) current = 1; // Skip Default when cycling manually, maybe? No, let's include it.
    SetDLSSPreset(current);
}
void StreamlineIntegration::CycleFrameGen() {
    // Cycle 0 -> 2 -> 3 -> 4 -> 0
    if (m_frameGenMultiplier == 0) m_frameGenMultiplier = 2;
    else if (m_frameGenMultiplier == 4) m_frameGenMultiplier = 0;
    else m_frameGenMultiplier++;
    
    m_optionsDirty = true;
    LOG_INFO("Cycled Frame Gen: %dx", m_frameGenMultiplier);
}
void StreamlineIntegration::CycleLODBias() { m_lodBias -= 1.0f; if(m_lodBias < -2.0f) m_lodBias = 0.0f; }

void StreamlineIntegration::ReleaseResources() {
    m_backBuffer.Reset();
    m_colorBuffer = nullptr; m_depthBuffer = nullptr; m_motionVectors = nullptr;
    ResourceDetector::Get().Clear();
}

void StreamlineIntegration::UpdateSwapChain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) return;
    if (m_pSwapChain.Get() != pSwapChain) {
        m_pSwapChain = pSwapChain; // ComPtr handles ref counting
    }
    Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        m_backBuffer.Reset();
        pSwapChain->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&m_backBuffer));
    }
}

void StreamlineIntegration::TagResources() {
    if (!m_initialized) return;
    if (!m_colorBuffer || !m_backBuffer) return; // Need at least these two for DLSS

    // Detect Input/Output Resolution
    D3D12_RESOURCE_DESC inDesc = m_colorBuffer->GetDesc();
    D3D12_RESOURCE_DESC outDesc = m_backBuffer->GetDesc();

    // Store for UpdateOptions
    m_viewportWidth = (uint32_t)outDesc.Width;
    m_viewportHeight = outDesc.Height;
    uint32_t inputWidth = (uint32_t)inDesc.Width;
    uint32_t inputHeight = inDesc.Height;

    sl::Extent fullExtent{};
    fullExtent.width = m_viewportWidth;
    fullExtent.height = m_viewportHeight;

    sl::Resource colorIn(sl::ResourceType::eTex2d, m_colorBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    colorIn.width = inputWidth;
    colorIn.height = inputHeight;
    colorIn.nativeFormat = inDesc.Format;

    sl::Resource colorOut(sl::ResourceType::eTex2d, m_backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    colorOut.width = m_viewportWidth;
    colorOut.height = m_viewportHeight;
    colorOut.nativeFormat = outDesc.Format;

    // Use backbuffer size for MVec/Depth if they match output, otherwise assume input
    sl::Resource depth(sl::ResourceType::eTex2d, m_depthBuffer ? m_depthBuffer : m_colorBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
    if (m_depthBuffer) {
        D3D12_RESOURCE_DESC dDesc = m_depthBuffer->GetDesc();
        depth.width = (uint32_t)dDesc.Width;
        depth.height = dDesc.Height;
        depth.nativeFormat = dDesc.Format;
    } else {
        depth.width = inputWidth; depth.height = inputHeight; depth.nativeFormat = DXGI_FORMAT_D32_FLOAT;
    }

    sl::Resource mvec(sl::ResourceType::eTex2d, m_motionVectors ? m_motionVectors : m_colorBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (m_motionVectors) {
        D3D12_RESOURCE_DESC mvDesc = m_motionVectors->GetDesc();
        mvec.width = (uint32_t)mvDesc.Width;
        mvec.height = mvDesc.Height;
        mvec.nativeFormat = mvDesc.Format;
    } else {
        mvec.width = inputWidth; mvec.height = inputHeight; mvec.nativeFormat = DXGI_FORMAT_R16G16_FLOAT;
    }

    sl::ResourceTag tags[] = {
        sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
    };

    sl::slSetTagForFrame(*m_frameToken, m_viewport, tags, static_cast<uint32_t>(std::size(tags)), nullptr);
}

void StreamlineIntegration::UpdateOptions() {
    if (!m_initialized) return;

    // Get input size from tagged buffer if possible, otherwise default to viewport
    uint32_t renderWidth = m_viewportWidth;
    uint32_t renderHeight = m_viewportHeight;
    
    if (m_colorBuffer) {
        D3D12_RESOURCE_DESC desc = m_colorBuffer->GetDesc();
        renderWidth = (uint32_t)desc.Width;
        renderHeight = desc.Height;
    }

    if (m_dlssSupported && m_dlssEnabled) {
        sl::DLSSOptions dlssOptions{};
        dlssOptions.mode = m_dlssMode;
        dlssOptions.outputWidth = m_viewportWidth;
        dlssOptions.outputHeight = m_viewportHeight;
        dlssOptions.sharpness = m_sharpness;
        dlssOptions.colorBuffersHDR = true;
        dlssOptions.inputWidth = renderWidth;
        dlssOptions.inputHeight = renderHeight;
        dlssOptions.preset = m_dlssPreset; // Set the Preset
        
        sl::slSetFeatureOptions(sl::kFeatureDLSS, &dlssOptions);
    }

    if (m_frameGenMultiplier >= 2 && m_reflexEnabled) {
        sl::ReflexOptions reflexOptions = {};
        reflexOptions.mode = sl::ReflexMode::eLowLatencyWithBoost;
        reflexOptions.useMarkersToOptimize = true;
        sl::slSetFeatureOptions(sl::kFeatureReflex, &reflexOptions);

        if (m_frameGenMultiplier > 2) { // Use MFG for 3x and 4x
            sl::DLSSMFGOptions mfgOptions{};
            if (m_frameGenMultiplier == 3) mfgOptions.mode = sl::DLSSMFGMode::e3x;
            else mfgOptions.mode = sl::DLSSMFGMode::e4x;
            
            mfgOptions.numBackBuffers = 2;
            mfgOptions.enableOFA = true;
            sl::slSetFeatureOptions(sl::kFeatureDLSS_MFG, &mfgOptions);
        } else { // Use DLSS-G for 2x
            sl::DLSSGOptions fgOptions{};
            fgOptions.mode = sl::DLSSGMode::eOn;
            fgOptions.numFramesToGenerate = 1; // 2x total
            sl::slSetFeatureOptions(sl::kFeatureDLSS_G, &fgOptions);
        }
    }
}

void StreamlineIntegration::EnsureCommandList() {
    if (!m_pDevice || !m_pCommandQueue) return;
    if (!m_pCommandAllocator) {
        m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pCommandAllocator));
    }
    if (!m_pCommandList) {
        m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_pCommandList));
        m_pCommandList->Close();
    }
    if (!m_pFence) {
        m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pFence));
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }
}

void StreamlineIntegration::WaitForGpu() {
    if (!m_pFence || !m_pCommandQueue || !m_fenceEvent) return;
    const uint64_t fenceValue = ++m_fenceValue;
    if (SUCCEEDED(m_pCommandQueue->Signal(m_pFence.Get(), fenceValue))) {
        if (m_pFence->GetCompletedValue() < fenceValue) {
            m_pFence->SetEventOnCompletion(fenceValue, m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, 1000);
        }
    }
}

bool StreamlineIntegration::EnsureFrameToken() {
    if (m_frameToken && !m_needNewFrameToken) {
        return true;
    }
    sl::Result res = sl::slGetNewFrameToken(m_frameToken, &m_frameIndex);
    if (SL_FAILED(res)) {
        LOG_ERROR("slGetNewFrameToken failed: %d", (int)res);
        return false;
    }
    m_needNewFrameToken = false;
    return true;
}
