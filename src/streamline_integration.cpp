#include "streamline_integration.h"
#include "logger.h"
#include "dlss4_config.h"
#include "resource_detector.h"
#include "config_manager.h" // Added
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
    m_frameGenMultiplier = DLSS4_FRAME_GEN_MULTIPLIER; // Init from config
    m_rayReconstructionEnabled = DLSS4_ENABLE_RAY_RECONSTRUCTION != 0;
    m_useMfg = DLSS4_FRAME_GEN_MULTIPLIER > 2;

    switch (DLSS4_SR_QUALITY_MODE) {
    case 0: m_dlssMode = sl::DLSSMode::eMaxPerformance; break;
    case 1: m_dlssMode = sl::DLSSMode::eBalanced; break;
    case 2: m_dlssMode = sl::DLSSMode::eMaxQuality; break;
    case 3: m_dlssMode = sl::DLSSMode::eUltraQuality; break;
    case 4: m_dlssMode = sl::DLSSMode::eDLAA; break;
    default: m_dlssMode = sl::DLSSMode::eDLAA; break; // Default to Max
    }

    return true;
}

void StreamlineIntegration::Shutdown() {
    if (m_initialized) {
        if (m_frameToken) m_frameToken = nullptr;
        if (m_pSwapChain) { m_pSwapChain->Release(); m_pSwapChain = nullptr; }
        if (m_backBuffer) { m_backBuffer->Release(); m_backBuffer = nullptr; }
        if (m_pCommandList) { m_pCommandList->Release(); m_pCommandList = nullptr; }
        if (m_pCommandAllocator) { m_pCommandAllocator->Release(); m_pCommandAllocator = nullptr; }
        if (m_pFence) { m_pFence->Release(); m_pFence = nullptr; }
        if (m_pCommandQueue) { m_pCommandQueue->Release(); m_pCommandQueue = nullptr; }
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

    UpdateOptions(); // Update options with new resolution data from tags
    TagResources();
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
    consts.mvecScale = sl::float2(1.0f, 1.0f);
    consts.depthInverted = sl::Boolean::eFalse;
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.reset = sl::Boolean::eFalse;

    if (!EnsureFrameToken()) return;
    sl::slSetConstants(consts, *m_frameToken, m_viewport);
}

void StreamlineIntegration::EvaluateDLSS(ID3D12GraphicsCommandList* pCmdList) {
    if (!m_initialized || !m_dlssSupported || !m_dlssEnabled || !pCmdList) return;
    const sl::BaseStructure* inputs[] = { &m_viewport };
    sl::slEvaluateFeature(sl::kFeatureDLSS, *m_frameToken, inputs, 1, pCmdList);
    if (m_rayReconstructionEnabled && m_rayReconstructionSupported) {
        sl::slEvaluateFeature(sl::kFeatureDLSS_RR, *m_frameToken, inputs, 1, pCmdList);
    }
}

void StreamlineIntegration::EvaluateFrameGen(IDXGISwapChain* pSwapChain) {
    if (!m_initialized || m_frameGenMultiplier < 2) return;
    UpdateSwapChain(pSwapChain);
    // UpdateOptions called in NewFrame now to catch res changes
    if (!EnsureFrameToken()) return;
    EnsureCommandList();
    if (!m_pCommandList || !m_pCommandAllocator || !m_pCommandQueue) return;

    m_pCommandAllocator->Reset();
    m_pCommandList->Reset(m_pCommandAllocator, nullptr);
    EvaluateDLSS(m_pCommandList);

    const sl::BaseStructure* inputs[] = { &m_viewport };
    if (m_useMfg && m_mfgSupported) {
        sl::slEvaluateFeature(sl::kFeatureDLSS_MFG, *m_frameToken, inputs, 1, m_pCommandList);
    } else if (m_dlssgSupported) {
        sl::slEvaluateFeature(sl::kFeatureDLSS_G, *m_frameToken, inputs, 1, m_pCommandList);
    }
    m_pCommandList->Close();
    ID3D12CommandList* lists[] = { m_pCommandList };
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
void StreamlineIntegration::SetFrameGenMultiplier(int multiplier) {
    if (!m_initialized) return;
    m_frameGenMultiplier = multiplier;
    m_optionsDirty = true;
    ConfigManager::Get().Data().frameGenMultiplier = multiplier;
    ConfigManager::Get().Save();
    LOG_INFO("Frame Gen Multiplier set to: %dx", multiplier);
}
void StreamlineIntegration::SetCommandQueue(ID3D12CommandQueue* pQueue) { 
    if(!m_initialized || !pQueue || m_pCommandQueue == pQueue) return;
    if(m_pCommandQueue) m_pCommandQueue->Release();
    m_pCommandQueue = pQueue; m_pCommandQueue->AddRef(); m_optionsDirty = true;
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
} // Checked in Sampler hook

void StreamlineIntegration::CycleDLSSMode() {
    int current = (int)m_dlssMode;
    current = (current + 1) % 6; // 0-5
    SetDLSSMode(current);
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
    if (m_backBuffer) { m_backBuffer->Release(); m_backBuffer = nullptr; }
    m_colorBuffer = nullptr; m_depthBuffer = nullptr; m_motionVectors = nullptr;
    ResourceDetector::Get().Clear();
}

void StreamlineIntegration::UpdateSwapChain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) return;
    if (m_pSwapChain != pSwapChain) {
        if (m_pSwapChain) m_pSwapChain->Release();
        m_pSwapChain = pSwapChain; m_pSwapChain->AddRef();
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3))) {
        if (m_backBuffer) m_backBuffer->Release();
        pSwapChain->GetBuffer(sc3->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), (void**)&m_backBuffer);
        sc3->Release();
    }
    // Note: We don't set m_viewportWidth from BackBuffer anymore, we detect it from TagResources
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

    sl::Resource colorOut(sl::ResourceType::eTex2d, m_backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
    colorOut.width = m_viewportWidth;
    colorOut.height = m_viewportHeight;
    colorOut.nativeFormat = outDesc.Format;

    // Use backbuffer size for MVec/Depth if they match output, otherwise assume input
    // Usually Depth matches Input
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
    if (!m_initialized) return; // Note: Dirty check removed to allow dynamic res updates per frame

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
        dlssOptions.inputWidth = renderWidth;   // True DLSS Input
        dlssOptions.inputHeight = renderHeight; // True DLSS Input
        
        sl::slSetFeatureOptions(sl::kFeatureDLSS, &dlssOptions);
    }

    if (m_frameGenMultiplier >= 2) {
        sl::ReflexOptions reflexOptions = {};
        reflexOptions.mode = sl::ReflexMode::eLowLatency;
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
        m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pCommandAllocator, nullptr, IID_PPV_ARGS(&m_pCommandList));
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
    if (SUCCEEDED(m_pCommandQueue->Signal(m_pFence, fenceValue))) {
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