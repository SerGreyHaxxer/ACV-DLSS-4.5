#include "streamline_integration.h"
#include "logger.h"
#include "dlss4_config.h"
#include "resource_detector.h"
#include "d3d12_wrappers.h"
#include "config_manager.h"
#include <string>
#include <array>
#include <algorithm>
#include <sl_helpers.h>
#include <cmath>
#include "imgui_overlay.h"
#include <dxgi1_4.h>
#include <chrono>
#include <algorithm>
#include "hooks.h"

#undef SL_FAILED
#undef SL_SUCCEEDED
#define SL_FAILED(x) ((x) != sl::Result::eOk)
#define SL_SUCCEEDED(x) ((x) == sl::Result::eOk)

StreamlineIntegration& StreamlineIntegration::Get() {
    static StreamlineIntegration instance;
    return instance;
}

// Frame Generation Debug tracking (DLSS-G)
static struct DLSSGDebugState {
    uint32_t baseFrames = 0;
    uint32_t lastReportedFrameIndex = 0;
    uint32_t generatedFramesDetected = 0;
    std::chrono::steady_clock::time_point lastReportTime;
    bool firstFrame = true;
    float avgFps = 0.0f;
    float peakMultiplier = 0.0f;
} g_dlssgDebug;

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
    m_rayReconstructionEnabled = cfg.rayReconstructionEnabled;
    m_rrPresetIndex = cfg.rrPreset;
    m_rrDenoiserStrength = cfg.rrDenoiserStrength;
    m_deepDvcEnabled = cfg.deepDvcEnabled;
    m_deepDvcIntensity = cfg.deepDvcIntensity;
    m_deepDvcSaturation = cfg.deepDvcSaturation;
    m_deepDvcAdaptiveEnabled = cfg.deepDvcAdaptiveEnabled;
    m_deepDvcAdaptiveStrength = cfg.deepDvcAdaptiveStrength;
    m_deepDvcAdaptiveMin = cfg.deepDvcAdaptiveMin;
    m_deepDvcAdaptiveMax = cfg.deepDvcAdaptiveMax;
    m_deepDvcAdaptiveSmoothing = cfg.deepDvcAdaptiveSmoothing;
    m_deepDvcAdaptiveIntensity = m_deepDvcIntensity;
    m_deepDvcAdaptiveSaturation = m_deepDvcSaturation;
    m_smartFgEnabled = cfg.smartFgEnabled;
    m_smartFgAutoDisable = cfg.smartFgAutoDisable;
    m_smartFgAutoDisableFps = cfg.smartFgAutoDisableFps;
    m_smartFgSceneChangeEnabled = cfg.smartFgSceneChangeEnabled;
    m_smartFgSceneChangeThreshold = cfg.smartFgSceneChangeThreshold;
    m_smartFgInterpolationQuality = cfg.smartFgInterpolationQuality;
    SetRRPreset(m_rrPresetIndex);
    LOG_INFO("Config Loaded: DLSS=%d, FG=%dx, Preset=%d", cfg.dlssMode, cfg.frameGenMultiplier, cfg.dlssPreset);
    LOG_INFO("Initializing NVIDIA Streamline...");
    if (!pDevice) return false;
    sl::Preferences pref{};
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.applicationId = static_cast<uint64_t>(NGX_APP_ID);
    pref.flags |= sl::PreferenceFlags::eUseManualHooking;
    pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.flags |= sl::PreferenceFlags::eAllowOTA;
    m_featuresToLoad[0] = sl::kFeatureDLSS;
    m_featureCount = 1;
    if (m_disableFGDueToInvalidParam) {
        m_frameGenMultiplier = 0;
        ConfigManager::Get().Data().frameGenMultiplier = 0;
        ConfigManager::Get().Save();
    }
    if (m_disableRRDueToInvalidParam) {
        m_rayReconstructionEnabled = false;
        ConfigManager::Get().Data().rayReconstructionEnabled = false;
        ConfigManager::Get().Save();
    }
    if (m_frameGenMultiplier >= 2) {
        m_featuresToLoad[m_featureCount++] = sl::kFeatureDLSS_G;
    }
    if (m_reflexEnabled) {
        m_featuresToLoad[m_featureCount++] = sl::kFeatureReflex;
    }
    if (m_rayReconstructionEnabled) {
        m_featuresToLoad[m_featureCount++] = sl::kFeatureDLSS_RR;
    }
    m_featuresToLoad[m_featureCount++] = sl::kFeatureDeepDVC;
    pref.featuresToLoad = m_featuresToLoad;
    pref.numFeaturesToLoad = m_featureCount;
    sl::Result res = slInit(pref, sl::kSDKVersion);
    if (SL_FAILED(res)) return false;
    res = slSetD3DDevice(pDevice);
    if (SL_FAILED(res)) return false;
    m_pDevice = pDevice;
    m_cbvSrvUavDescriptorSize = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    slSetFeatureLoaded(sl::kFeatureDLSS, true);
    if (m_frameGenMultiplier >= 2) slSetFeatureLoaded(sl::kFeatureDLSS_G, true);
    if (m_reflexEnabled) slSetFeatureLoaded(sl::kFeatureReflex, true);
    if (m_rayReconstructionEnabled) slSetFeatureLoaded(sl::kFeatureDLSS_RR, true);
    slSetFeatureLoaded(sl::kFeatureDeepDVC, true);
    m_dlssgLoaded = (m_frameGenMultiplier >= 2);
    m_reflexLoaded = m_reflexEnabled;
    m_rrLoaded = m_rayReconstructionEnabled;
    m_deepDvcLoaded = true;
    m_initialized = true;
    m_viewport = sl::ViewportHandle(0);
    sl::FeatureRequirements requirements{};
    if (SL_SUCCEEDED(slGetFeatureRequirements(sl::kFeatureDLSS, requirements))) m_dlssSupported = (requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported) != 0;
    if (SL_SUCCEEDED(slGetFeatureRequirements(sl::kFeatureDLSS_G, requirements))) m_dlssgSupported = (requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported) != 0;
    if (SL_SUCCEEDED(slGetFeatureRequirements(sl::kFeatureReflex, requirements))) m_reflexSupported = (requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported) != 0;
    if (SL_SUCCEEDED(slGetFeatureRequirements(sl::kFeatureDLSS_RR, requirements))) m_rayReconstructionSupported = (requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported) != 0;
    if (SL_SUCCEEDED(slGetFeatureRequirements(sl::kFeatureDeepDVC, requirements))) m_deepDvcSupported = (requirements.flags & sl::FeatureRequirementFlags::eD3D12Supported) != 0;

    // Auto-disable features if hardware doesn't support them
    m_dlssEnabled = m_dlssEnabled && m_dlssSupported;
    if (!m_dlssgSupported) m_frameGenMultiplier = 0;
    if (!m_reflexSupported) m_reflexEnabled = false;
    if (!m_rayReconstructionSupported) m_rayReconstructionEnabled = false;
    if (!m_deepDvcSupported) {
        m_deepDvcEnabled = false;
        ConfigManager::Get().Data().deepDvcEnabled = false;
        ConfigManager::Get().Save();
        m_deepDvcAdaptiveEnabled = false;
    }
    if (!m_reflexEnabled && m_frameGenMultiplier >= 2) {
        m_reflexEnabled = true;
        m_needFeatureReload = true;
    }

    LOG_INFO("Streamline ready. DLSS:%s DLSSG:%s Reflex:%s RR:%s DeepDVC:%s", m_dlssSupported ? "OK" : "NO", m_dlssgSupported ? "OK" : "NO", m_reflexSupported ? "OK" : "NO", m_rayReconstructionSupported ? "OK" : "NO", m_deepDvcSupported ? "OK" : "NO");
    
    // Refresh UI to reflect hardware support
    ImGuiOverlay::Get().UpdateControls();
    ImGuiOverlay::Get().ToggleDebugMode(cfg.debugMode);
    
    m_forceTagging = true;
    return true;
}

void StreamlineIntegration::Shutdown() {
    if (m_initialized) {
        m_backBuffer.Reset();
        m_pCommandList.Reset();
        m_pCommandAllocator.Reset();
        m_pFence.Reset();
        m_pCommandQueue.Reset();
        m_pSwapChain.Reset();
        m_dlssgLoaded = false;
        m_reflexLoaded = false;
        m_rrLoaded = false;
        m_deepDvcLoaded = false;
        m_hasCameraData = false;
        slShutdown();
        m_initialized = false;
    }
}

void StreamlineIntegration::NewFrame(IDXGISwapChain* pSwapChain) {
    if (!m_initialized) return;
    m_needNewFrameToken = true;
    if (m_needFeatureReload) {
        LOG_INFO("Reloading Streamline features...");
        Shutdown();
        Initialize(m_pDevice.Get());
        m_needFeatureReload = false;
        m_forceTagging = true;
    }
    UpdateSwapChain(pSwapChain);
    uint32_t prevFrameIndex = m_frameIndex;
    sl::Result res = slGetNewFrameToken(m_frameToken, &m_frameIndex);
    if (SL_FAILED(res)) return;
    m_needNewFrameToken = false;
    if (m_frameGenMultiplier >= 2) {
        g_dlssgDebug.baseFrames++;
        if (!g_dlssgDebug.firstFrame && m_frameIndex > prevFrameIndex + 1) g_dlssgDebug.generatedFramesDetected += (m_frameIndex - prevFrameIndex - 1);
        g_dlssgDebug.firstFrame = false;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - g_dlssgDebug.lastReportTime).count() >= 5) {
            float elapsed = 5.0f;
            const float baseFps = g_dlssgDebug.baseFrames / elapsed;
            const float totalFps = (g_dlssgDebug.baseFrames + g_dlssgDebug.generatedFramesDetected) / elapsed;
            m_fgActualMultiplier = (g_dlssgDebug.baseFrames > 0) ? (totalFps / baseFps) : 1.0f;
            LOG_INFO("[DLSSG] FPS: %.1f (Base: %.1f) Multiplier: %.2fx Status: %s", totalFps, baseFps, m_fgActualMultiplier, (g_dlssgDebug.generatedFramesDetected > 0) ? "ACTIVE ✓" : "INACTIVE ✗");
            g_dlssgDebug.baseFrames = 0; g_dlssgDebug.generatedFramesDetected = 0; g_dlssgDebug.lastReportTime = now;
        }
    }
    UpdateSmartFGState();
    TagResources();
}

void StreamlineIntegration::TagColorBuffer(ID3D12Resource* pResource) { if (m_initialized && pResource) m_colorBuffer = pResource; }
void StreamlineIntegration::TagDepthBuffer(ID3D12Resource* pResource) { if (m_initialized && pResource) m_depthBuffer = pResource; }
void StreamlineIntegration::TagMotionVectors(ID3D12Resource* pResource) { if (m_initialized && pResource) m_motionVectors = pResource; }

void StreamlineIntegration::SetCameraData(const float* view, const float* proj, float jitterX, float jitterY) {
    if (!m_initialized) return;

    // Cache valid matrices
    if (view && proj) {
        memcpy(m_cachedView, view, sizeof(float) * 16);
        memcpy(m_cachedProj, proj, sizeof(float) * 16);
        m_hasCameraData = true;
    }

    // Use cached if current is null
    const float* useView = view ? view : (m_hasCameraData ? m_cachedView : nullptr);
    const float* useProj = proj ? proj : (m_hasCameraData ? m_cachedProj : nullptr);

    if (!useView || !useProj) return;

    if (m_smartFgEnabled && m_smartFgSceneChangeEnabled) {
        float maxDelta = 0.0f;
        if (m_hasPrevMatrices) {
            for (int i = 0; i < 16; ++i) {
                float delta = fabsf(useView[i] - m_prevView[i]) + fabsf(useProj[i] - m_prevProj[i]);
                if (delta > maxDelta) maxDelta = delta;
            }
        }
        m_lastCameraDelta = maxDelta;
        memcpy(m_prevView, useView, sizeof(m_prevView));
        memcpy(m_prevProj, useProj, sizeof(m_prevProj));
        m_hasPrevMatrices = true;
        if (maxDelta >= m_smartFgSceneChangeThreshold) {
            m_sceneChangeCooldownActive = true;
            m_sceneChangeCooldownUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
            m_sceneResetFrames = 2;
        }
    }

    // Clamp Jitter to reasonable range (-2.0 to 2.0 pixels usually sufficient)
    // Streamline expects pixels. If these are huge, it's likely garbage.
    if (jitterX > 5.0f) jitterX = 5.0f; else if (jitterX < -5.0f) jitterX = -5.0f;
    if (jitterY > 5.0f) jitterY = 5.0f; else if (jitterY < -5.0f) jitterY = -5.0f;

    m_lastJitterX = jitterX;
    m_lastJitterY = jitterY;

    sl::Constants consts{};
    for (int i = 0; i < 16; ++i) { reinterpret_cast<float*>(&consts.cameraViewToClip)[i] = useView[i]; reinterpret_cast<float*>(&consts.clipToCameraView)[i] = useProj[i]; }
    consts.jitterOffset = sl::float2(jitterX, jitterY);
    consts.mvecScale = sl::float2(m_mvecScaleX, m_mvecScaleY);
    consts.depthInverted = sl::Boolean::eTrue;
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.motionVectors3D = sl::Boolean::eFalse;
    if (m_sceneResetFrames > 0) {
        consts.reset = sl::Boolean::eTrue;
        m_sceneResetFrames--;
    } else {
        consts.reset = sl::Boolean::eFalse;
    }
    
    if (!EnsureFrameToken()) return;
    m_viewport = sl::ViewportHandle(0);
    slSetConstants(consts, *m_frameToken, m_viewport);
}

void StreamlineIntegration::EvaluateDLSS(ID3D12GraphicsCommandList* pCmdList) {
    if (!m_initialized || !pCmdList) return;
    if (!EnsureFrameToken()) return;
    if (m_optionsDirty) UpdateOptions();
    m_viewport = sl::ViewportHandle(0);
    const sl::BaseStructure* inputs[] = { &m_viewport };
    if (m_dlssSupported && m_dlssEnabled) {
        slEvaluateFeature(sl::kFeatureDLSS, *m_frameToken, inputs, 1, pCmdList);
    }
    if (m_rayReconstructionEnabled && m_rrLoaded && m_rayReconstructionSupported) {
        if (!m_colorBuffer || !m_depthBuffer || !m_motionVectors) {
            static uint32_t s_rrMissingLog = 0;
            if (s_rrMissingLog++ % 120 == 0) LOG_WARN("[DLSSRR] Missing inputs for RR evaluation");
        } else {
            sl::Result rrRes = slEvaluateFeature(sl::kFeatureDLSS_RR, *m_frameToken, inputs, 1, pCmdList);
            if (SL_FAILED(rrRes)) {
                static uint32_t s_rrFailCount = 0;
                if (s_rrFailCount++ % 60 == 0) LOG_ERROR("[DLSSRR] Evaluation failed: %d", (int)rrRes);
                if (rrRes == sl::Result::eErrorInvalidParameter) {
                    m_rrInvalidParamFrames++;
                if (m_rrInvalidParamFrames >= STREAMLINE_INVALID_PARAM_FALLBACK_FRAMES) {
                    m_rayReconstructionEnabled = false;
                    ConfigManager::Get().Data().rayReconstructionEnabled = false;
                    ConfigManager::Get().Save();
                    m_disableRRDueToInvalidParam = true;
                    m_needFeatureReload = true;
                    LOG_ERROR("[DLSSRR] Invalid params persist; disabling Ray Reconstruction");
                }
                }
            } else {
                m_rrInvalidParamFrames = 0;
            }
        }
    }
}

void StreamlineIntegration::EvaluateDeepDVC(IDXGISwapChain* pSwapChain) {
    if (!m_initialized || !pSwapChain) return;
    if (!m_deepDvcSupported || !m_deepDvcLoaded || !m_deepDvcEnabled) return;
    UpdateSwapChain(pSwapChain);
    if (!m_backBuffer) return;
    if (!EnsureFrameToken()) return;
    UpdateDeepDVCAdaptive();
    EnsureCommandList();
    if (!m_pCommandList || !m_pCommandAllocator || !m_pCommandQueue) return;
    if (m_optionsDirty) UpdateOptions();
    m_pCommandAllocator->Reset();
    m_pCommandList->Reset(m_pCommandAllocator.Get(), nullptr);
    m_viewport = sl::ViewportHandle(0);
    const sl::BaseStructure* inputs[] = { &m_viewport };
    slEvaluateFeature(sl::kFeatureDeepDVC, *m_frameToken, inputs, 1, m_pCommandList.Get());
    m_pCommandList->Close();
    ID3D12CommandList* lists[] = { m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(1, lists);
}

void StreamlineIntegration::EvaluateFrameGen(IDXGISwapChain* pSwapChain) {
    if (!m_initialized || m_frameGenMultiplier < 2) return;
    if (!m_dlssgLoaded || !m_dlssgSupported) return;
    UpdateSwapChain(pSwapChain);
    if (!EnsureFrameToken()) return;
    EnsureCommandList();
    if (!m_pCommandList || !m_pCommandAllocator || !m_pCommandQueue) return;
    if (m_viewportWidth == 0 || m_viewportHeight == 0) return;
    if (!m_colorBuffer || !m_motionVectors || !m_depthBuffer) {
        LOG_WARN("[DLSSG] Missing buffers: color=%p depth=%p mv=%p", m_colorBuffer.Get(), m_depthBuffer.Get(), m_motionVectors.Get());
        return;
    }
    D3D12_RESOURCE_DESC colorDesc = m_colorBuffer->GetDesc();
    D3D12_RESOURCE_DESC depthDesc = m_depthBuffer->GetDesc();
    D3D12_RESOURCE_DESC mvDesc = m_motionVectors->GetDesc();
    if (colorDesc.Width == 0 || colorDesc.Height == 0 || depthDesc.Width == 0 || depthDesc.Height == 0 || mvDesc.Width == 0 || mvDesc.Height == 0) {
        LOG_WARN("[DLSSG] Invalid buffer sizes: color=%llux%llu depth=%llux%llu mv=%llux%llu",
            colorDesc.Width, colorDesc.Height, depthDesc.Width, depthDesc.Height, mvDesc.Width, mvDesc.Height);
        return;
    }
    if ((m_dlssgMinWidthOrHeight > 0) &&
        (m_viewportWidth < m_dlssgMinWidthOrHeight || m_viewportHeight < m_dlssgMinWidthOrHeight)) {
        LOG_WARN("[DLSSG] Resolution too low for frame generation (%ux%u < %u)", m_viewportWidth, m_viewportHeight, m_dlssgMinWidthOrHeight);
        return;
    }
    ModConfig& cfg = ConfigManager::Get().Data();
    if (cfg.mvecScaleAuto) {
        m_mvecScaleX = (float)m_viewportWidth / (float)mvDesc.Width;
        m_mvecScaleY = (float)m_viewportHeight / (float)mvDesc.Height;
    } else {
        m_mvecScaleX = cfg.mvecScaleX;
        m_mvecScaleY = cfg.mvecScaleY;
    }
    m_pCommandAllocator->Reset();
    m_pCommandList->Reset(m_pCommandAllocator.Get(), nullptr);
    if (m_optionsDirty) UpdateOptions();
    m_viewport = sl::ViewportHandle(0);
    const sl::BaseStructure* inputs[] = { &m_viewport };
    if (m_dlssgSupported) {
        sl::Result fgRes = slEvaluateFeature(sl::kFeatureDLSS_G, *m_frameToken, inputs, 1, m_pCommandList.Get());
        if (SL_FAILED(fgRes)) {
            static uint32_t s_failCount = 0;
            if (s_failCount++ % 60 == 0) LOG_ERROR("[DLSSG] Evaluation failed: %d", (int)fgRes);
            if ((int)fgRes == 31) {
                m_dlssgInvalidParamFrames++;
                if (m_dlssgInvalidParamFrames >= STREAMLINE_INVALID_PARAM_FALLBACK_FRAMES) {
                    m_frameGenMultiplier = 0;
                    ConfigManager::Get().Data().frameGenMultiplier = 0;
                    ConfigManager::Get().Save();
                    m_disableFGDueToInvalidParam = true;
                    m_needFeatureReload = true;
                    LOG_ERROR("[DLSSG] Invalid params persist; disabling Frame Generation");
                }
            }
        } else {
            m_dlssgInvalidParamFrames = 0;
        }
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
void StreamlineIntegration::SetFrameGenMultiplier(int multiplier) { m_frameGenMultiplier = multiplier; m_disableFGDueToInvalidParam = false; m_optionsDirty = true; if ((multiplier >= 2) != m_dlssgLoaded) m_needFeatureReload = true; if (multiplier >= 2) { m_reflexEnabled = true; } }
void StreamlineIntegration::SetSharpness(float sharpness) { m_sharpness = sharpness; m_optionsDirty = true; }
void StreamlineIntegration::SetLODBias(float bias) { m_lodBias = bias; }
void StreamlineIntegration::SetMVecScale(float x, float y) { m_mvecScaleX = x; m_mvecScaleY = y; }
void StreamlineIntegration::SetReflexEnabled(bool enabled) { m_reflexEnabled = enabled; if (enabled && !m_reflexLoaded) m_needFeatureReload = true; if (!enabled && m_reflexLoaded) m_needFeatureReload = true; }
void StreamlineIntegration::SetHUDFixEnabled(bool enabled) { m_hudFixEnabled = enabled; m_forceTagging = true; }
void StreamlineIntegration::SetRayReconstructionEnabled(bool enabled) { m_rayReconstructionEnabled = enabled; m_disableRRDueToInvalidParam = false; if ((enabled && !m_rrLoaded) || (!enabled && m_rrLoaded)) m_needFeatureReload = true; m_optionsDirty = true; }
void StreamlineIntegration::SetRRPreset(int preset) {
    static const sl::DLSSDPreset kPresetMap[] = {
        sl::DLSSDPreset::eDefault,
        sl::DLSSDPreset::ePresetD,
        sl::DLSSDPreset::ePresetE,
        sl::DLSSDPreset::ePresetF,
        sl::DLSSDPreset::ePresetG,
        sl::DLSSDPreset::ePresetH,
        sl::DLSSDPreset::ePresetI,
        sl::DLSSDPreset::ePresetJ,
        sl::DLSSDPreset::ePresetK,
        sl::DLSSDPreset::ePresetL,
        sl::DLSSDPreset::ePresetM,
        sl::DLSSDPreset::ePresetN,
        sl::DLSSDPreset::ePresetO,
    };
    const int presetCount = static_cast<int>(sizeof(kPresetMap) / sizeof(kPresetMap[0]));
    int clamped = preset;
    if (clamped < 0 || clamped >= presetCount) clamped = 0;
    m_rrPresetIndex = clamped;
    m_rrPreset = kPresetMap[clamped];
    m_optionsDirty = true;
}
void StreamlineIntegration::SetRRDenoiserStrength(float strength) { m_rrDenoiserStrength = strength; m_optionsDirty = true; }
void StreamlineIntegration::UpdateFrameTiming(float baseFps) { m_lastBaseFps = baseFps; }
void StreamlineIntegration::SetDeepDVCEnabled(bool enabled) { m_deepDvcEnabled = enabled; m_optionsDirty = true; }
void StreamlineIntegration::SetDeepDVCIntensity(float intensity) { m_deepDvcIntensity = intensity; m_optionsDirty = true; }
void StreamlineIntegration::SetDeepDVCSaturation(float saturation) { m_deepDvcSaturation = saturation; m_optionsDirty = true; }
void StreamlineIntegration::SetDeepDVCAdaptiveEnabled(bool enabled) { m_deepDvcAdaptiveEnabled = enabled; m_optionsDirty = true; }
void StreamlineIntegration::SetDeepDVCAdaptiveStrength(float strength) { m_deepDvcAdaptiveStrength = strength; m_optionsDirty = true; }
void StreamlineIntegration::SetDeepDVCAdaptiveMin(float minValue) { m_deepDvcAdaptiveMin = minValue; m_optionsDirty = true; }
void StreamlineIntegration::SetDeepDVCAdaptiveMax(float maxValue) { m_deepDvcAdaptiveMax = maxValue; m_optionsDirty = true; }
void StreamlineIntegration::SetDeepDVCAdaptiveSmoothing(float smoothing) { m_deepDvcAdaptiveSmoothing = smoothing; m_optionsDirty = true; }
void StreamlineIntegration::SetSmartFGEnabled(bool enabled) { m_smartFgEnabled = enabled; m_optionsDirty = true; }
void StreamlineIntegration::SetSmartFGAutoDisable(bool enabled) { m_smartFgAutoDisable = enabled; m_optionsDirty = true; }
void StreamlineIntegration::SetSmartFGAutoDisableThreshold(float fps) { m_smartFgAutoDisableFps = fps; m_optionsDirty = true; }
void StreamlineIntegration::SetSmartFGSceneChangeEnabled(bool enabled) { m_smartFgSceneChangeEnabled = enabled; }
void StreamlineIntegration::SetSmartFGSceneChangeThreshold(float threshold) { m_smartFgSceneChangeThreshold = threshold; }
void StreamlineIntegration::SetSmartFGInterpolationQuality(float quality) { m_smartFgInterpolationQuality = quality; }

void StreamlineIntegration::UpdateSmartFGState() {
    if (!m_smartFgEnabled) {
        if (m_smartFgForceDisable) {
            m_smartFgForceDisable = false;
            m_optionsDirty = true;
        }
        return;
    }

    bool forceDisable = false;
    if (m_smartFgAutoDisable && m_lastBaseFps > 0.0f && m_lastBaseFps >= m_smartFgAutoDisableFps) {
        forceDisable = true;
    }
    if (m_smartFgSceneChangeEnabled && m_sceneChangeCooldownActive) {
        if (std::chrono::steady_clock::now() < m_sceneChangeCooldownUntil) {
            forceDisable = true;
        } else {
            m_sceneChangeCooldownActive = false;
        }
    }

    if (forceDisable != m_smartFgForceDisable) {
        m_smartFgForceDisable = forceDisable;
        m_optionsDirty = true;
    }
}
void StreamlineIntegration::ReleaseResources() {
    m_backBuffer.Reset();
    m_colorBuffer.Reset();
    m_depthBuffer.Reset();
    m_motionVectors.Reset();
    m_lastTaggedColor.Reset();
    m_lastTaggedDepth.Reset();
    m_lastTaggedMvec.Reset();
    m_lastTaggedBackBuffer.Reset();
    m_descInitialized = false;
    m_forceTagging = true;
    ResourceDetector::Get().Clear();
    ResetCameraScanCache();
    m_rrInvalidParamFrames = 0;
    m_dlssgInvalidParamFrames = 0;
    // Keep invalid-param disables across resource resets to avoid re-enabling broken features.
}
void StreamlineIntegration::NotifySwapChainResize(UINT width, UINT height) {
    m_forceTagging = true;
    ResourceDetector::Get().SetExpectedDimensions(width, height);
}
void StreamlineIntegration::PrintDLSSGStatus() {
    LOG_INFO("[DLSSG] Mult:%d Init:%d Supported:%d MaxFrames:%u MinDim:%u Status:0x%x Viewport:%ux%u Buffers:%u",
        m_frameGenMultiplier, m_initialized, m_dlssgSupported, m_dlssgMaxFramesToGenerate, m_dlssgMinWidthOrHeight,
        static_cast<uint32_t>(m_dlssgStatus), m_viewportWidth, m_viewportHeight, m_swapChainBufferCount);
}

void StreamlineIntegration::UpdateSwapChain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) return;
    if (m_pSwapChain.Get() != pSwapChain) m_pSwapChain = pSwapChain;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        m_backBuffer.Reset();
        pSwapChain->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&m_backBuffer));
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
            ResourceDetector::Get().SetExpectedDimensions(desc.BufferDesc.Width, desc.BufferDesc.Height);
            m_swapChainBufferCount = desc.BufferCount;
            m_backBufferFormat = desc.BufferDesc.Format;
        }
    }
}

void StreamlineIntegration::TagResources() {
    if (!m_initialized || !m_colorBuffer || !m_backBuffer) return;
    D3D12_RESOURCE_DESC inDesc = m_colorBuffer->GetDesc();
    D3D12_RESOURCE_DESC outDesc = m_backBuffer->GetDesc();
    if (!m_forceTagging && m_lastTaggedColor.Get() == m_colorBuffer.Get() &&
        m_lastTaggedDepth.Get() == m_depthBuffer.Get() &&
        m_lastTaggedMvec.Get() == m_motionVectors.Get() &&
        m_lastTaggedBackBuffer.Get() == m_backBuffer.Get() &&
        m_descInitialized &&
        memcmp(&m_lastColorDesc, &inDesc, sizeof(D3D12_RESOURCE_DESC)) == 0 &&
        memcmp(&m_lastBackBufferDesc, &outDesc, sizeof(D3D12_RESOURCE_DESC)) == 0 &&
        m_viewportWidth == (uint32_t)outDesc.Width && m_viewportHeight == outDesc.Height) {
        return;
    }
    m_viewportWidth = (uint32_t)outDesc.Width; m_viewportHeight = outDesc.Height;
    sl::Extent fullExtent{0, 0, m_viewportWidth, m_viewportHeight};
    sl::Resource colorIn(sl::ResourceType::eTex2d, m_colorBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    colorIn.width = (uint32_t)inDesc.Width; colorIn.height = inDesc.Height; colorIn.nativeFormat = inDesc.Format;
    sl::Resource colorOut(sl::ResourceType::eTex2d, m_backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    colorOut.width = m_viewportWidth; colorOut.height = m_viewportHeight; colorOut.nativeFormat = outDesc.Format;
    sl::Resource depth(sl::ResourceType::eTex2d, m_depthBuffer ? m_depthBuffer.Get() : m_colorBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_READ);
    if (m_depthBuffer) {
        D3D12_RESOURCE_DESC dDesc = m_depthBuffer->GetDesc();
        depth.width = (uint32_t)dDesc.Width; depth.height = dDesc.Height; depth.nativeFormat = dDesc.Format;
        DXGI_FORMAT overrideFmt = ResourceDetector::Get().GetDepthFormatOverride(m_depthBuffer.Get());
        if (overrideFmt != DXGI_FORMAT_UNKNOWN) depth.nativeFormat = overrideFmt;
    }
    sl::Resource mvec(sl::ResourceType::eTex2d, m_motionVectors ? m_motionVectors.Get() : m_colorBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (m_motionVectors) {
        D3D12_RESOURCE_DESC mvDesc = m_motionVectors->GetDesc();
        mvec.width = (uint32_t)mvDesc.Width; mvec.height = (uint32_t)mvDesc.Height; mvec.nativeFormat = mvDesc.Format;
        DXGI_FORMAT overrideFmt = ResourceDetector::Get().GetMotionFormatOverride(m_motionVectors.Get());
        if (overrideFmt != DXGI_FORMAT_UNKNOWN) mvec.nativeFormat = overrideFmt;
    }
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&colorIn, m_hudFixEnabled ? sl::kBufferTypeHUDLessColor : sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&colorOut, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
        sl::ResourceTag(&colorOut, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent),
    };
    m_viewport = sl::ViewportHandle(0);
    slSetTagForFrame(*m_frameToken, m_viewport, tags, static_cast<uint32_t>(std::size(tags)), nullptr);
    m_lastTaggedColor = m_colorBuffer;
    m_lastTaggedDepth = m_depthBuffer;
    m_lastTaggedMvec = m_motionVectors;
    m_lastTaggedBackBuffer = m_backBuffer;
    m_lastColorDesc = inDesc;
    if (m_depthBuffer) m_lastDepthDesc = m_depthBuffer->GetDesc();
    if (m_motionVectors) m_lastMvecDesc = m_motionVectors->GetDesc();
    m_lastBackBufferDesc = outDesc;
    m_descInitialized = true;
    m_forceTagging = false;
}

void StreamlineIntegration::SetCommandQueue(ID3D12CommandQueue* pQueue) {
    if (pQueue) {
        m_pCommandQueue = pQueue;
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(pQueue->GetDevice(__uuidof(ID3D12Device), (void**)&device))) {
            EnsureD3D12VTableHooks(device);
            device->Release();
        }
    }
}

void StreamlineIntegration::UpdateOptions() {
    if (!m_initialized || m_viewportWidth == 0) return;
    if (!m_hasCameraData) {
        uint64_t currentFrame = m_frameIndex;
        if (m_frameGenMultiplier >= 2 && !m_disableFGDueToInvalidParam) {
            if (currentFrame > CAMERA_GRACE_FRAMES) {
                m_disableFGDueToInvalidParam = true;
                m_frameGenMultiplier = 0;
                ConfigManager::Get().Data().frameGenMultiplier = 0;
                ConfigManager::Get().Save();
                LOG_WARN("[DLSSG] No camera data; disabling Frame Generation");
                m_needFeatureReload = true;
            }
        }
        if (m_deepDvcEnabled) {
            if (currentFrame > CAMERA_GRACE_FRAMES) {
                m_deepDvcEnabled = false;
                ConfigManager::Get().Data().deepDvcEnabled = false;
                ConfigManager::Get().Save();
                LOG_WARN("[DeepDVC] No camera data; disabling DeepDVC");
            }
        }
    }
    if (m_dlssSupported && m_dlssEnabled) {
        sl::DLSSOptions dlssOptions{};
        dlssOptions.mode = m_dlssMode; dlssOptions.outputWidth = m_viewportWidth; dlssOptions.outputHeight = m_viewportHeight; dlssOptions.sharpness = m_sharpness; dlssOptions.colorBuffersHDR = sl::Boolean::eTrue;
        dlssOptions.useAutoExposure = sl::Boolean::eTrue;
        m_viewport = sl::ViewportHandle(0);
        slDLSSSetOptions(m_viewport, dlssOptions);
    }
    UpdateDeepDVCAdaptive();
    UpdateDeepDVCOptions();
    if (m_rayReconstructionEnabled && m_rrLoaded && m_rayReconstructionSupported) {
        if (!m_colorBuffer || !m_depthBuffer || !m_motionVectors) {
            return;
        }
        sl::DLSSDOptions rrOptions{};
        rrOptions.mode = m_dlssMode;
        rrOptions.outputWidth = m_viewportWidth;
        rrOptions.outputHeight = m_viewportHeight;
        rrOptions.sharpness = m_rrDenoiserStrength;
        rrOptions.colorBuffersHDR = sl::Boolean::eTrue;
        sl::DLSSDPreset resolved = sl::resolveDLSSDPreset(m_rrPreset);
        rrOptions.dlaaPreset = resolved;
        rrOptions.qualityPreset = resolved;
        rrOptions.balancedPreset = resolved;
        rrOptions.performancePreset = resolved;
        rrOptions.ultraPerformancePreset = resolved;
        rrOptions.ultraQualityPreset = resolved;
        m_viewport = sl::ViewportHandle(0);
        sl::Result rrOptRes = slDLSSDSetOptions(m_viewport, rrOptions);
        if (SL_FAILED(rrOptRes)) {
            LOG_WARN("[DLSSRR] SetOptions failed: %d", (int)rrOptRes);
            if (rrOptRes == sl::Result::eErrorInvalidParameter && m_rayReconstructionEnabled) {
                m_rayReconstructionEnabled = false;
                ConfigManager::Get().Data().rayReconstructionEnabled = false;
                ConfigManager::Get().Save();
                m_disableRRDueToInvalidParam = true;
                m_needFeatureReload = true;
                LOG_ERROR("[DLSSRR] Invalid params; disabling Ray Reconstruction");
            }
        }
    }
    if (m_frameGenMultiplier >= 2) {
        sl::ReflexOptions reflexOptions = {}; reflexOptions.mode = sl::ReflexMode::eLowLatencyWithBoost; reflexOptions.useMarkersToOptimize = false; slReflexSetOptions(reflexOptions);
        if (m_dlssgSupported) {
            if (!m_motionVectors || !m_depthBuffer) {
                static uint32_t s_missingFgBuffersLog = 0;
                if (s_missingFgBuffersLog++ % 120 == 0) {
                    LOG_WARN("[DLSSG] Waiting for motion vectors/depth before SetOptions");
                }
                m_optionsDirty = false;
                return;
            }
            sl::DLSSGOptions fgOptions{};
            const int requestedMult = m_frameGenMultiplier;
            int desiredMult = requestedMult;
            if (m_smartFgEnabled && m_smartFgAutoDisable && m_lastBaseFps > 0.0f && m_lastBaseFps >= m_smartFgAutoDisableFps) {
                desiredMult = 0;
            }
            m_smartFgForceDisable = (desiredMult < 2);
            fgOptions.mode = m_smartFgForceDisable ? sl::DLSSGMode::eOff : sl::DLSSGMode::eOn;
            fgOptions.numFramesToGenerate = (desiredMult > 1) ? (desiredMult - 1) : 1;
            fgOptions.numBackBuffers = m_swapChainBufferCount;
            fgOptions.colorWidth = m_viewportWidth;
            fgOptions.colorHeight = m_viewportHeight;
            fgOptions.colorBufferFormat = static_cast<uint32_t>(m_backBufferFormat);
            fgOptions.mvecDepthWidth = m_viewportWidth;
            fgOptions.mvecDepthHeight = m_viewportHeight;
            D3D12_RESOURCE_DESC mvDesc = m_motionVectors->GetDesc();
            D3D12_RESOURCE_DESC depthDesc = m_depthBuffer->GetDesc();
            DXGI_FORMAT mvFormat = mvDesc.Format;
            DXGI_FORMAT depthFormat = depthDesc.Format;
            DXGI_FORMAT mvOverride = ResourceDetector::Get().GetMotionFormatOverride(m_motionVectors.Get());
            DXGI_FORMAT depthOverride = ResourceDetector::Get().GetDepthFormatOverride(m_depthBuffer.Get());
            if (mvOverride != DXGI_FORMAT_UNKNOWN) mvFormat = mvOverride;
            if (depthOverride != DXGI_FORMAT_UNKNOWN) depthFormat = depthOverride;
            fgOptions.mvecBufferFormat = static_cast<uint32_t>(mvFormat);
            fgOptions.depthBufferFormat = static_cast<uint32_t>(depthFormat);
            if (m_smartFgEnabled) {
                fgOptions.flags |= sl::DLSSGFlags::eEnableFullscreenMenuDetection;
                if (m_smartFgInterpolationQuality <= 0.0f) {
                    fgOptions.flags |= sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
                }
            }
            m_viewport = sl::ViewportHandle(0);
            sl::Result optRes = slDLSSGSetOptions(m_viewport, fgOptions);
            if (SL_FAILED(optRes) && optRes == sl::Result::eErrorInvalidParameter && m_frameGenMultiplier > 0) {
                m_frameGenMultiplier = 0;
                ConfigManager::Get().Data().frameGenMultiplier = 0;
                ConfigManager::Get().Save();
                m_disableFGDueToInvalidParam = true;
                m_needFeatureReload = true;
                LOG_ERROR("[DLSSG] Invalid params; disabling Frame Generation");
            } else if (SL_SUCCEEDED(optRes)) {
                sl::DLSSGState state{};
                sl::Result stateRes = slDLSSGGetState(m_viewport, state, &fgOptions);
                if (SL_SUCCEEDED(stateRes)) {
                    m_dlssgMaxFramesToGenerate = state.numFramesToGenerateMax ? state.numFramesToGenerateMax : 1;
                    m_dlssgMinWidthOrHeight = state.minWidthOrHeight;
                    m_dlssgStatus = state.status;
                    if (fgOptions.numFramesToGenerate > m_dlssgMaxFramesToGenerate) {
                        m_frameGenMultiplier = static_cast<int>(m_dlssgMaxFramesToGenerate + 1);
                        m_optionsDirty = true;
                        LOG_WARN("[DLSSG] GPU caps to %ux (requested %dx)", m_frameGenMultiplier, fgOptions.numFramesToGenerate + 1);
                    }
                }
            } else {
                LOG_WARN("[DLSSG] SetOptions failed: %d", static_cast<int>(optRes));
            }
        }
    }
    m_optionsDirty = false;
}

void StreamlineIntegration::UpdateDeepDVCOptions() {
    if (!m_deepDvcSupported || !m_deepDvcLoaded) return;
    sl::DeepDVCOptions dvcOptions{};
    dvcOptions.mode = m_deepDvcEnabled ? sl::DeepDVCMode::eOn : sl::DeepDVCMode::eOff;
    dvcOptions.intensity = m_deepDvcAdaptiveEnabled ? m_deepDvcAdaptiveIntensity : m_deepDvcIntensity;
    dvcOptions.saturationBoost = m_deepDvcAdaptiveEnabled ? m_deepDvcAdaptiveSaturation : m_deepDvcSaturation;
    m_viewport = sl::ViewportHandle(0);
    sl::Result dvcRes = slDeepDVCSetOptions(m_viewport, dvcOptions);
    if (SL_FAILED(dvcRes)) {
        LOG_WARN("[DeepDVC] SetOptions failed: %d", static_cast<int>(dvcRes));
    }
}

float StreamlineIntegration::EstimateSceneLuma(ID3D12Resource* resource) {
    if (!resource || !m_pDevice || !m_pCommandQueue) return 0.5f;
    D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0) return 0.5f;
    DXGI_FORMAT format = desc.Format;
    if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        format != DXGI_FORMAT_R16G16B16A16_FLOAT && format != DXGI_FORMAT_R10G10B10A2_UNORM) {
        return 0.5f;
    }

    UINT width = static_cast<UINT>(std::max<uint64_t>(1, desc.Width));
    UINT height = static_cast<UINT>(std::max<uint64_t>(1, desc.Height));
    UINT sampleW = std::min<UINT>(width, DEEPDVC_LUMA_SAMPLE_SIZE);
    UINT sampleH = std::min<UINT>(height, DEEPDVC_LUMA_SAMPLE_SIZE);
    UINT srcX = (width - sampleW) / 2;
    UINT srcY = (height - sampleH) / 2;

    D3D12_RESOURCE_DESC readDesc = desc;
    readDesc.Width = sampleW;
    readDesc.Height = sampleH;
    readDesc.MipLevels = 1;
    readDesc.DepthOrArraySize = 1;
    readDesc.SampleDesc.Count = 1;
    readDesc.SampleDesc.Quality = 0;
    readDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    readDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 totalBytes = 0;
    m_pDevice->GetCopyableFootprints(&readDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

    bool needNewReadback = !m_lumaReadback || m_lumaReadbackFormat != format || m_lumaRowPitch != footprint.Footprint.RowPitch;
    if (needNewReadback) {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = totalBytes;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        if (SUCCEEDED(m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
            m_lumaReadback = readback;
            m_lumaReadbackFormat = format;
            m_lumaRowPitch = footprint.Footprint.RowPitch;
        }
    }
    if (!m_lumaReadback) return 0.5f;

    EnsureCommandList();
    if (!m_pCommandList || !m_pCommandAllocator) return 0.5f;
    m_pCommandAllocator->Reset();
    m_pCommandList->Reset(m_pCommandAllocator.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = resource;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = m_lumaReadback.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = footprint;
    D3D12_BOX srcBox{};
    srcBox.left = srcX;
    srcBox.top = srcY;
    srcBox.right = srcX + sampleW;
    srcBox.bottom = srcY + sampleH;
    srcBox.front = 0;
    srcBox.back = 1;
    m_pCommandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
    m_pCommandList->Close();
    ID3D12CommandList* lists[] = { m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(1, lists);
    m_fenceValue++;
    if (!m_pFence) {
        m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pFence));
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }
    m_pCommandQueue->Signal(m_pFence.Get(), m_fenceValue);
    if (m_pFence->GetCompletedValue() < m_fenceValue) {
        m_pFence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        if (WaitForSingleObject(m_fenceEvent, 2) != WAIT_OBJECT_0) {
            return 0.5f;
        }
    }

    float luma = 0.5f;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range{ 0, static_cast<SIZE_T>(totalBytes) };
    if (SUCCEEDED(m_lumaReadback->Map(0, &range, reinterpret_cast<void**>(&mapped))) && mapped) {
        uint64_t sum = 0;
        uint64_t count = 0;
        UINT maxRows = std::min<UINT>(sampleH, 32);
        UINT maxCols = std::min<UINT>(sampleW, 32);
        for (UINT y = 0; y < maxRows; ++y) {
            const uint8_t* row = mapped + y * footprint.Footprint.RowPitch;
            for (UINT x = 0; x < maxCols; ++x) {
                const uint8_t* px = row + x * 4;
                uint8_t r = px[0];
                uint8_t g = px[1];
                uint8_t b = px[2];
                if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                    r = px[2];
                    g = px[1];
                    b = px[0];
                }
                sum += (uint64_t)r * 30 + (uint64_t)g * 59 + (uint64_t)b * 11;
                count++;
            }
        }
        m_lumaReadback->Unmap(0, nullptr);
        if (count > 0) {
            float avg = static_cast<float>(sum) / static_cast<float>(count * 100);
            luma = std::clamp(avg / 255.0f, 0.0f, 1.0f);
        }
    }
    return luma;
}

void StreamlineIntegration::UpdateDeepDVCAdaptive() {
    if (!m_deepDvcAdaptiveEnabled) {
        m_deepDvcAdaptiveIntensity = m_deepDvcIntensity;
        m_deepDvcAdaptiveSaturation = m_deepDvcSaturation;
        return;
    }
    uint64_t now = GetTickCount64();
    if (now - m_deepDvcLastSampleMs < DEEPDVC_LUMA_SAMPLE_INTERVAL_MS) return;
    m_deepDvcLastSampleMs = now;
    ID3D12Resource* lumaSource = m_colorBuffer.Get() ? m_colorBuffer.Get() : m_backBuffer.Get();
    float luma = EstimateSceneLuma(lumaSource);
    float target = 1.0f - luma;
    target = m_deepDvcAdaptiveMin + (m_deepDvcAdaptiveMax - m_deepDvcAdaptiveMin) * target;
    float strength = std::clamp(m_deepDvcAdaptiveStrength, 0.0f, 1.0f);
    float desiredIntensity = std::clamp(m_deepDvcIntensity * (0.5f + target * strength), 0.0f, 1.0f);
    float desiredSaturation = std::clamp(m_deepDvcSaturation * (0.5f + target * strength), 0.0f, 1.0f);
    float smooth = std::clamp(m_deepDvcAdaptiveSmoothing, 0.01f, 1.0f);
    m_deepDvcAdaptiveIntensity += (desiredIntensity - m_deepDvcAdaptiveIntensity) * smooth;
    m_deepDvcAdaptiveSaturation += (desiredSaturation - m_deepDvcAdaptiveSaturation) * smooth;
    m_optionsDirty = true;
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
