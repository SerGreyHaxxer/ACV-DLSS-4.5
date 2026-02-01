#include "streamline_integration.h"
#include "logger.h"
#include "dlss4_config.h"
#include "resource_detector.h"
#include "config_manager.h"
#include <string>
#include <array>
#include <dxgi1_4.h>
#include <chrono>

#define SL_FAILED(x) ((x) != sl::Result::eOk)
#define SL_SUCCEEDED(x) ((x) == sl::Result::eOk)

StreamlineIntegration& StreamlineIntegration::Get() {
    static StreamlineIntegration instance;
    return instance;
}

// MFG Debug tracking
static struct MFGDebugState {
    uint32_t baseFrames = 0;
    uint32_t lastReportedFrameIndex = 0;
    uint32_t generatedFramesDetected = 0;
    std::chrono::steady_clock::time_point lastReportTime;
    bool firstFrame = true;
    float avgFps = 0.0f;
    float peakMultiplier = 0.0f;
} g_mfgDebug;

#include <winreg.h>

static void EnforceNGXRegistry() {
    HKEY hKey;
    DWORD value = 1;
    DWORD showIndicator = 0;
    
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "EnableBetaSuperSampling", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegSetValueExA(hKey, "EnableSignatureOverride", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegSetValueExA(hKey, "ShowDlssIndicator", 0, REG_DWORD, (const BYTE*)&showIndicator, sizeof(showIndicator));
        RegCloseKey(hKey);
    }

    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\NVIDIA Corporation\\Global\\NGXCore", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "EnableBetaSuperSampling", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegSetValueExA(hKey, "EnableSignatureOverride", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegSetValueExA(hKey, "ShowDlssIndicator", 0, REG_DWORD, (const BYTE*)&showIndicator, sizeof(showIndicator));
        RegCloseKey(hKey);
    }
}

bool StreamlineIntegration::Initialize(ID3D12Device* pDevice) {
    if (m_initialized) return true;
    EnforceNGXRegistry();
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
    if (!pDevice) return false;
    sl::Preferences pref{};
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.applicationId = static_cast<uint64_t>(NGX_APP_ID);
    pref.flags |= sl::PreferenceFlags::eUseManualHooking;
    pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.flags |= sl::PreferenceFlags::eAllowOTA;
    m_featuresToLoad[0] = sl::kFeatureDLSS;
    m_featuresToLoad[1] = sl::kFeatureDLSS_G;
    m_featuresToLoad[2] = sl::kFeatureDLSS_RR;
    m_featuresToLoad[3] = sl::kFeatureReflex;
    m_featureCount = 4;
    pref.featuresToLoad = m_featuresToLoad;
    pref.numFeaturesToLoad = m_featureCount;
    sl::Result res = slInit(pref, sl::kSDKVersion);
    if (SL_FAILED(res)) return false;
    res = slSetD3DDevice(pDevice);
    if (SL_FAILED(res)) return false;
    m_pDevice = pDevice;
    slSetFeatureLoaded(sl::kFeatureDLSS, true);
    slSetFeatureLoaded(sl::kFeatureDLSS_G, true);
    slSetFeatureLoaded(sl::kFeatureReflex, true);
    m_initialized = true;
    m_viewport = sl::ViewportHandle(0);
    sl::FeatureRequirements requirements{};
    if (SL_SUCCEEDED(slGetFeatureRequirements(sl::kFeatureDLSS, requirements))) m_dlssSupported = (requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported) != 0;
    if (SL_SUCCEEDED(slGetFeatureRequirements(sl::kFeatureDLSS_G, requirements))) m_dlssgSupported = (requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported) != 0;
    m_dlssEnabled = true;
    m_useMfg = m_frameGenMultiplier > 2;
    LOG_INFO("Streamline ready. DLSS:%s DLSSG:%s", m_dlssSupported ? "OK" : "NO", m_dlssgSupported ? "OK" : "NO");
    return true;
}

void StreamlineIntegration::Shutdown() {
    if (m_initialized) { m_backBuffer.Reset(); m_pCommandList.Reset(); m_pCommandAllocator.Reset(); m_pFence.Reset(); m_pCommandQueue.Reset(); m_pSwapChain.Reset(); slShutdown(); m_initialized = false; }
}

void StreamlineIntegration::NewFrame(IDXGISwapChain* pSwapChain) {
    if (!m_initialized) return;
    UpdateSwapChain(pSwapChain);
    uint32_t prevFrameIndex = m_frameIndex;
    sl::Result res = slGetNewFrameToken(m_frameToken, &m_frameIndex);
    if (SL_FAILED(res)) return;
    if (m_frameGenMultiplier >= 2) {
        g_mfgDebug.baseFrames++;
        if (!g_mfgDebug.firstFrame && m_frameIndex > prevFrameIndex + 1) g_mfgDebug.generatedFramesDetected += (m_frameIndex - prevFrameIndex - 1);
        g_mfgDebug.firstFrame = false;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - g_mfgDebug.lastReportTime).count() >= 5) {
            float elapsed = 5.0f;
            LOG_INFO("[MFG] FPS: %.1f (Base: %.1f) Multiplier: %.2fx Status: %s", (g_mfgDebug.baseFrames + g_mfgDebug.generatedFramesDetected) / elapsed, g_mfgDebug.baseFrames / elapsed, (float)(g_mfgDebug.baseFrames + g_mfgDebug.generatedFramesDetected) / (g_mfgDebug.baseFrames ? g_mfgDebug.baseFrames : 1), (g_mfgDebug.generatedFramesDetected > 0) ? "ACTIVE ✓" : "INACTIVE ✗");
            g_mfgDebug.baseFrames = 0; g_mfgDebug.generatedFramesDetected = 0; g_mfgDebug.lastReportTime = now;
        }
    }
    TagResources();
}

void StreamlineIntegration::TagColorBuffer(ID3D12Resource* pResource) { if (m_initialized && pResource) m_colorBuffer = pResource; }
void StreamlineIntegration::TagDepthBuffer(ID3D12Resource* pResource) { if (m_initialized && pResource) m_depthBuffer = pResource; }
void StreamlineIntegration::TagMotionVectors(ID3D12Resource* pResource) { if (m_initialized && pResource) m_motionVectors = pResource; }

void StreamlineIntegration::SetCameraData(const float* view, const float* proj, float jitterX, float jitterY) {
    if (!m_initialized || !view || !proj) return;
    sl::Constants consts{};
    for (int i = 0; i < 16; ++i) { reinterpret_cast<float*>(&consts.cameraViewToClip)[i] = view[i]; reinterpret_cast<float*>(&consts.clipToCameraView)[i] = proj[i]; }
    consts.jitterOffset = sl::float2(jitterX, jitterY);
    consts.mvecScale = sl::float2(m_mvecScaleX, m_mvecScaleY);
    consts.depthInverted = sl::Boolean::eTrue;
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.reset = sl::Boolean::eFalse;
    m_hasCameraData = true;
    if (!EnsureFrameToken()) return;
    m_viewport = sl::ViewportHandle(0);
    slSetConstants(consts, *m_frameToken, m_viewport);
}

void StreamlineIntegration::EvaluateDLSS(ID3D12GraphicsCommandList* pCmdList) {
    if (!m_initialized || !m_dlssSupported || !m_dlssEnabled || !pCmdList) return;
    if (!EnsureFrameToken()) return;
    if (m_optionsDirty) UpdateOptions();
    m_viewport = sl::ViewportHandle(0);
    const sl::BaseStructure* inputs[] = { &m_viewport };
    slEvaluateFeature(sl::kFeatureDLSS, *m_frameToken, inputs, 1, pCmdList);
}

void StreamlineIntegration::EvaluateFrameGen(IDXGISwapChain* pSwapChain) {
    if (!m_initialized || m_frameGenMultiplier < 2) return;
    UpdateSwapChain(pSwapChain);
    if (!EnsureFrameToken()) return;
    EnsureCommandList();
    if (!m_pCommandList || !m_pCommandAllocator || !m_pCommandQueue) return;
    if (m_viewportWidth == 0 || m_viewportHeight == 0) return;
    m_pCommandAllocator->Reset();
    m_pCommandList->Reset(m_pCommandAllocator.Get(), nullptr);
    if (m_optionsDirty) UpdateOptions();
    m_viewport = sl::ViewportHandle(0);
    const sl::BaseStructure* inputs[] = { &m_viewport };
    if (m_dlssgSupported) {
        sl::Result fgRes = slEvaluateFeature(sl::kFeatureDLSS_G, *m_frameToken, inputs, 1, m_pCommandList.Get());
        if (SL_FAILED(fgRes)) { static uint32_t s_failCount = 0; if (s_failCount++ % 60 == 0) LOG_ERROR("[MFG] Evaluation failed: %d", (int)fgRes); }
    }
    m_pCommandList->Close();
    ID3D12CommandList* lists[] = { m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(1, lists);
}

void StreamlineIntegration::SetDLSSModeIndex(int modeIndex) {
    static const sl::DLSSMode kUiToMode[] = { sl::DLSSMode::eOff, sl::DLSSMode::eMaxPerformance, sl::DLSSMode::eBalanced, sl::DLSSMode::eMaxQuality, sl::DLSSMode::eUltraQuality, sl::DLSSMode::eDLAA };
    if (modeIndex >= 0 && modeIndex < 6) { m_dlssMode = kUiToMode[modeIndex]; m_optionsDirty = true; }
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

void StreamlineIntegration::SetDLSSPreset(int preset) { m_dlssPreset = static_cast<sl::DLSSPreset>(preset); m_optionsDirty = true; }
void StreamlineIntegration::SetFrameGenMultiplier(int multiplier) { m_frameGenMultiplier = multiplier; m_optionsDirty = true; }
void StreamlineIntegration::SetCommandQueue(ID3D12CommandQueue* pQueue) { if (pQueue) m_pCommandQueue = pQueue; }
void StreamlineIntegration::SetSharpness(float sharpness) { m_sharpness = sharpness; m_optionsDirty = true; }
void StreamlineIntegration::SetLODBias(float bias) { m_lodBias = bias; }
void StreamlineIntegration::SetReflexEnabled(bool enabled) { m_reflexEnabled = enabled; }
void StreamlineIntegration::SetHUDFixEnabled(bool enabled) { m_hudFixEnabled = enabled; }
void StreamlineIntegration::ReleaseResources() { m_backBuffer.Reset(); m_colorBuffer = nullptr; m_depthBuffer = nullptr; m_motionVectors = nullptr; ResourceDetector::Get().Clear(); }
void StreamlineIntegration::PrintMFGStatus() { LOG_INFO("[MFG] Mult:%d Init:%d Supported:%d Viewport:%dx%d", m_frameGenMultiplier, m_initialized, m_dlssgSupported, m_viewportWidth, m_viewportHeight); }

void StreamlineIntegration::UpdateSwapChain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) return;
    if (m_pSwapChain.Get() != pSwapChain) m_pSwapChain = pSwapChain;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) { m_backBuffer.Reset(); pSwapChain->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&m_backBuffer)); }
}

void StreamlineIntegration::TagResources() {
    if (!m_initialized || !m_colorBuffer || !m_backBuffer) return;
    D3D12_RESOURCE_DESC inDesc = m_colorBuffer->GetDesc();
    D3D12_RESOURCE_DESC outDesc = m_backBuffer->GetDesc();
    m_viewportWidth = (uint32_t)outDesc.Width; m_viewportHeight = outDesc.Height;
    sl::Extent fullExtent{0, 0, m_viewportWidth, m_viewportHeight};
    sl::Resource colorIn(sl::ResourceType::eTex2d, m_colorBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    colorIn.width = (uint32_t)inDesc.Width; colorIn.height = inDesc.Height; colorIn.nativeFormat = inDesc.Format;
    sl::Resource colorOut(sl::ResourceType::eTex2d, m_backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    colorOut.width = m_viewportWidth; colorOut.height = m_viewportHeight; colorOut.nativeFormat = outDesc.Format;
    sl::Resource depth(sl::ResourceType::eTex2d, m_depthBuffer ? m_depthBuffer : m_colorBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
    if (m_depthBuffer) { D3D12_RESOURCE_DESC dDesc = m_depthBuffer->GetDesc(); depth.width = (uint32_t)dDesc.Width; depth.height = dDesc.Height; depth.nativeFormat = dDesc.Format; }
    sl::Resource mvec(sl::ResourceType::eTex2d, m_motionVectors ? m_motionVectors : m_colorBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (m_motionVectors) { D3D12_RESOURCE_DESC mvDesc = m_motionVectors->GetDesc(); mvec.width = (uint32_t)mvDesc.Width; mvec.height = mvDesc.Height; mvec.nativeFormat = mvDesc.Format; }
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&colorIn, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
    };
    m_viewport = sl::ViewportHandle(0);
    slSetTagForFrame(*m_frameToken, m_viewport, tags, static_cast<uint32_t>(std::size(tags)), nullptr);
}

void StreamlineIntegration::UpdateOptions() {
    if (!m_initialized || m_viewportWidth == 0) return;
    if (m_dlssSupported && m_dlssEnabled) {
        sl::DLSSOptions dlssOptions{};
        dlssOptions.mode = m_dlssMode; dlssOptions.outputWidth = m_viewportWidth; dlssOptions.outputHeight = m_viewportHeight; dlssOptions.sharpness = m_sharpness; dlssOptions.colorBuffersHDR = sl::Boolean::eTrue;
        m_viewport = sl::ViewportHandle(0);
        slDLSSSetOptions(m_viewport, dlssOptions);
    }
    if (m_frameGenMultiplier >= 2) {
        sl::ReflexOptions reflexOptions = {}; reflexOptions.mode = sl::ReflexMode::eLowLatencyWithBoost; reflexOptions.useMarkersToOptimize = true; slReflexSetOptions(reflexOptions);
        if (m_dlssgSupported) {
            sl::DLSSGOptions fgOptions{}; fgOptions.mode = sl::DLSSGMode::eOn; fgOptions.numFramesToGenerate = (m_frameGenMultiplier > 1) ? (m_frameGenMultiplier - 1) : 1;
            m_viewport = sl::ViewportHandle(0);
            slDLSSGSetOptions(m_viewport, fgOptions);
        }
    }
    m_optionsDirty = false;
}

void StreamlineIntegration::EnsureCommandList() {
    if (!m_pDevice || !m_pCommandQueue) return;
    if (!m_pCommandAllocator) m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pCommandAllocator));
    if (!m_pCommandList) { m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_pCommandList)); m_pCommandList->Close(); }
    if (!m_pFence) { m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pFence)); m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr); }
}

bool StreamlineIntegration::EnsureFrameToken() {
    if (m_frameToken && !m_needNewFrameToken) return true;
    sl::Result res = slGetNewFrameToken(m_frameToken, &m_frameIndex);
    if (SL_FAILED(res)) return false;
    m_needNewFrameToken = false;
    return true;
}
