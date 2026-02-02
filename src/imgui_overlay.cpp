#include "imgui_overlay.h"
#include "config_manager.h"
#include "streamline_integration.h"
#include "resource_detector.h"
#include "logger.h"
#include "input_handler.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <vector>

#include "external/imgui/imgui.h"
#include "external/imgui/backends/imgui_impl_dx12.h"
#include "external/imgui/backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void ImGuiOverlay_SrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    ImGuiOverlay::Get().AllocateSrv(info, out_cpu, out_gpu);
}

static void ImGuiOverlay_SrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    ImGuiOverlay::Get().FreeSrv(info, cpu, gpu);
}

static void WaitForFence(ID3D12Fence* fence, HANDLE eventHandle, UINT64 value) {
    if (!fence || !eventHandle) return;
    if (fence->GetCompletedValue() < value) {
        fence->SetEventOnCompletion(value, eventHandle);
        WaitForSingleObject(eventHandle, INFINITE);
    }
}

ImGuiOverlay& ImGuiOverlay::Get() {
    static ImGuiOverlay instance;
    return instance;
}

void ImGuiOverlay::Initialize(IDXGISwapChain* swapChain) {
    if (m_initialized || !swapChain) return;
    swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&m_swapChain);
    if (!m_swapChain) return;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(m_swapChain->GetDesc(&desc))) return;
    m_backBufferCount = desc.BufferCount;
    m_hwnd = desc.OutputWindow;

    if (FAILED(m_swapChain->GetDevice(__uuidof(ID3D12Device), (void**)&m_device))) return;
    m_queue = StreamlineIntegration::Get().GetCommandQueue();

    if (!EnsureDeviceResources()) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ApplyStyle();
    ImGui_ImplWin32_Init(m_hwnd);
    if (m_hwnd && !m_prevWndProc) {
        m_prevWndProc = (WNDPROC)SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
    }
    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = m_device;
    initInfo.CommandQueue = m_queue;
    initInfo.NumFramesInFlight = (int)m_backBufferCount;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    initInfo.SrvDescriptorHeap = m_srvHeap;
    initInfo.SrvDescriptorAllocFn = ImGuiOverlay_SrvAlloc;
    initInfo.SrvDescriptorFreeFn = ImGuiOverlay_SrvFree;
    ImGui_ImplDX12_Init(&initInfo);

    m_initialized = true;
}

void ImGuiOverlay::Shutdown() {
    if (!m_initialized) return;
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (m_hwnd && m_prevWndProc) {
        SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)m_prevWndProc);
        m_prevWndProc = nullptr;
    }
    ReleaseDeviceResources();
    if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
    m_initialized = false;
}

LRESULT CALLBACK ImGuiOverlay::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ImGuiOverlay& overlay = ImGuiOverlay::Get();
    if (overlay.m_visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return 1;
    }
    if (overlay.m_prevWndProc) {
        return CallWindowProcW(overlay.m_prevWndProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ImGuiOverlay::EnsureDeviceResources() {
    if (!m_device) return false;

    if (!m_queue) {
        LOG_WARN("ImGuiOverlay: Command queue not available yet.");
        return false;
    }

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 128;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = m_backBufferCount;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)))) return false;

    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)))) return false;
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator, nullptr, IID_PPV_ARGS(&m_commandList)))) return false;
    m_commandList->Close();

    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) return false;
    m_fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    m_srvDescriptorCount = srvDesc.NumDescriptors;
    m_srvDescriptorNext = 0;
    RecreateRenderTargets();
    return true;
}

void ImGuiOverlay::ReleaseDeviceResources() {
    ReleaseRenderTargets();
    if (m_vignetteTexture) { m_vignetteTexture->Release(); m_vignetteTexture = nullptr; }
    if (m_commandList) { m_commandList->Release(); m_commandList = nullptr; }
    if (m_commandAllocator) { m_commandAllocator->Release(); m_commandAllocator = nullptr; }
    if (m_fence) { m_fence->Release(); m_fence = nullptr; }
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
    if (m_rtvHeap) { m_rtvHeap->Release(); m_rtvHeap = nullptr; }
    if (m_srvHeap) { m_srvHeap->Release(); m_srvHeap = nullptr; }
}

void ImGuiOverlay::AllocateSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    (void)info;
    if (!m_srvHeap || m_srvDescriptorNext >= m_srvDescriptorCount) {
        out_cpu->ptr = 0;
        out_gpu->ptr = 0;
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += m_srvDescriptorNext * m_srvDescriptorSize;
    gpu.ptr += m_srvDescriptorNext * m_srvDescriptorSize;
    *out_cpu = cpu;
    *out_gpu = gpu;
    m_srvDescriptorNext++;
}

void ImGuiOverlay::FreeSrv(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    (void)info;
    (void)cpu;
    (void)gpu;
}
void ImGuiOverlay::RecreateRenderTargets() {
    ReleaseRenderTargets();
    if (!m_swapChain || !m_rtvHeap) return;
    m_backBuffers = new ID3D12Resource*[m_backBufferCount]();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < m_backBufferCount; ++i) {
        if (SUCCEEDED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])))) {
            m_rtvHandles[i] = rtvHandle;
            m_device->CreateRenderTargetView(m_backBuffers[i], nullptr, rtvHandle);
            rtvHandle.ptr += m_rtvDescriptorSize;
        }
    }
}

void ImGuiOverlay::ReleaseRenderTargets() {
    if (m_backBuffers) {
        for (UINT i = 0; i < m_backBufferCount; ++i) {
            if (m_backBuffers[i]) m_backBuffers[i]->Release();
        }
        delete[] m_backBuffers;
        m_backBuffers = nullptr;
    }
}

void ImGuiOverlay::OnResize(UINT width, UINT height) {
    m_width = width;
    m_height = height;
    if (m_initialized) {
        WaitForFence(m_fence, m_fenceEvent, m_fenceValue);
        RecreateRenderTargets();
        m_needRebuildTextures = true;
    }
}

void ImGuiOverlay::SetFPS(float gameFps, float totalFps) {
    m_cachedTotalFPS = totalFps;
    m_fpsHistory[m_fpsHistoryIndex] = gameFps;
    m_fpsHistoryIndex = (m_fpsHistoryIndex + 1) % kFpsHistorySize;
}

void ImGuiOverlay::SetCameraStatus(bool hasCamera, float jitterX, float jitterY) {
    m_cachedCamera = hasCamera;
    m_cachedJitterX = jitterX;
    m_cachedJitterY = jitterY;
}

void ImGuiOverlay::ToggleVisibility() {
    m_visible = !m_visible;
    ConfigManager::Get().Data().uiVisible = m_visible;
    ConfigManager::Get().Save();
}

void ImGuiOverlay::ToggleFPS() {
    m_showFPS = !m_showFPS;
    ConfigManager::Get().Data().showFPS = m_showFPS;
    ConfigManager::Get().Save();
}

void ImGuiOverlay::ToggleVignette() {
    m_showVignette = !m_showVignette;
    ConfigManager::Get().Data().showVignette = m_showVignette;
    ConfigManager::Get().Save();
    m_needRebuildTextures = true;
}

void ImGuiOverlay::ToggleDebugMode(bool enabled) {
    m_showDebug = enabled;
}

void ImGuiOverlay::CaptureNextHotkey(int* target) {
    m_pendingHotkeyTarget = target;
}

void ImGuiOverlay::UpdateControls() {
    ModConfig& cfg = ConfigManager::Get().Data();
    m_showFPS = cfg.showFPS;
    m_showVignette = cfg.showVignette;
    m_showDebug = cfg.debugMode;
}

void ImGuiOverlay::ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.12f, 0.92f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.06f, 0.07f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.15f, 0.18f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.24f, 0.26f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.24f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.28f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.32f, 0.34f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.83f, 0.69f, 0.25f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.83f, 0.69f, 0.25f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.92f, 0.78f, 0.30f, 1.00f);
}

void ImGuiOverlay::BuildMainMenu() {
    ModConfig& cfg = ConfigManager::Get().Data();
    StreamlineIntegration& sli = StreamlineIntegration::Get();

    ImGui::SetNextWindowSize(ImVec2(460, 680), ImGuiCond_FirstUseEver);
    ImGui::Begin("DLSS 4.5 Control Panel", &m_visible, ImGuiWindowFlags_NoCollapse);

    if (m_pendingHotkeyTarget) {
        for (int key = 0x08; key <= 0xFE; ++key) {
            if (GetAsyncKeyState(key) & 0x1) {
                if (key == VK_ESCAPE) {
                    m_pendingHotkeyTarget = nullptr;
                    break;
                }
                *m_pendingHotkeyTarget = key;
                m_pendingHotkeyTarget = nullptr;
                ModConfig& data = ConfigManager::Get().Data();
                InputHandler::Get().UpdateHotkey("Toggle Menu", data.menuHotkey);
                InputHandler::Get().UpdateHotkey("Toggle FPS", data.fpsHotkey);
                InputHandler::Get().UpdateHotkey("Toggle Vignette", data.vignetteHotkey);
                ConfigManager::Get().Save();
                break;
            }
        }
    }

    ImVec4 statusOk(0.12f, 0.82f, 0.25f, 1.0f);
    ImVec4 statusWarn(0.95f, 0.78f, 0.18f, 1.0f);
    ImVec4 statusBad(0.92f, 0.20f, 0.20f, 1.0f);

    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Status");
    bool dlssOk = sli.IsDLSSSupported() && sli.GetDLSSModeIndex() > 0;
    bool dlssWarn = sli.IsDLSSSupported() && sli.GetDLSSModeIndex() == 0;
    ImGui::TextColored(dlssOk ? statusOk : (dlssWarn ? statusWarn : statusBad), "DLSS");
    ImGui::SameLine(120.0f);
    bool fgOk = sli.IsFrameGenSupported() && sli.GetFrameGenMultiplier() >= 2 && !sli.IsSmartFGTemporarilyDisabled();
    bool fgWarn = sli.IsFrameGenSupported() && (sli.GetFrameGenMultiplier() < 2 || sli.IsSmartFGTemporarilyDisabled());
    ImGui::TextColored(fgOk ? statusOk : (fgWarn ? statusWarn : statusBad), "Frame Gen");
    ImGui::SameLine(260.0f);
    bool camOk = sli.HasCameraData();
    ImGui::TextColored(camOk ? statusOk : statusWarn, "Camera");
    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "General");
    const char* dlssModes[] = {"Off", "Max Performance", "Balanced", "Max Quality", "Ultra Quality", "DLAA"};
    int dlssMode = sli.GetDLSSModeIndex();
    ImGui::BeginDisabled(!sli.IsDLSSSupported());
    if (ImGui::Combo("DLSS Quality Mode", &dlssMode, dlssModes, IM_ARRAYSIZE(dlssModes))) {
        sli.SetDLSSModeIndex(dlssMode);
        cfg.dlssMode = dlssMode;
        ConfigManager::Get().Save();
    }
    ImGui::EndDisabled();

    const char* presets[] = {"Default", "Preset A", "Preset B", "Preset C", "Preset D", "Preset E", "Preset F", "Preset G"};
    int preset = sli.GetDLSSPresetIndex();
    if (ImGui::Combo("DLSS Preset", &preset, presets, IM_ARRAYSIZE(presets))) {
        sli.SetDLSSPreset(preset);
        cfg.dlssPreset = preset;
        ConfigManager::Get().Save();
    }

    if (ImGui::Button("Quality Preset")) {
        cfg.dlssMode = 5;
        cfg.dlssPreset = 0;
        cfg.frameGenMultiplier = 2;
        cfg.sharpness = 0.2f;
        cfg.lodBias = -1.0f;
        cfg.reflexEnabled = true;
        cfg.rayReconstructionEnabled = true;
        sli.SetDLSSModeIndex(cfg.dlssMode);
        sli.SetDLSSPreset(cfg.dlssPreset);
        sli.SetFrameGenMultiplier(cfg.frameGenMultiplier);
        sli.SetSharpness(cfg.sharpness);
        sli.SetLODBias(cfg.lodBias);
        sli.SetReflexEnabled(cfg.reflexEnabled);
        sli.SetRayReconstructionEnabled(cfg.rayReconstructionEnabled);
        ConfigManager::Get().Save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Balanced Preset")) {
        cfg.dlssMode = 2;
        cfg.dlssPreset = 0;
        cfg.frameGenMultiplier = 3;
        cfg.sharpness = 0.35f;
        cfg.lodBias = -1.0f;
        cfg.reflexEnabled = true;
        cfg.rayReconstructionEnabled = true;
        sli.SetDLSSModeIndex(cfg.dlssMode);
        sli.SetDLSSPreset(cfg.dlssPreset);
        sli.SetFrameGenMultiplier(cfg.frameGenMultiplier);
        sli.SetSharpness(cfg.sharpness);
        sli.SetLODBias(cfg.lodBias);
        sli.SetReflexEnabled(cfg.reflexEnabled);
        sli.SetRayReconstructionEnabled(cfg.rayReconstructionEnabled);
        ConfigManager::Get().Save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Performance Preset")) {
        cfg.dlssMode = 1;
        cfg.dlssPreset = 0;
        cfg.frameGenMultiplier = 4;
        cfg.sharpness = 0.5f;
        cfg.lodBias = -1.2f;
        cfg.reflexEnabled = true;
        cfg.rayReconstructionEnabled = false;
        sli.SetDLSSModeIndex(cfg.dlssMode);
        sli.SetDLSSPreset(cfg.dlssPreset);
        sli.SetFrameGenMultiplier(cfg.frameGenMultiplier);
        sli.SetSharpness(cfg.sharpness);
        sli.SetLODBias(cfg.lodBias);
        sli.SetReflexEnabled(cfg.reflexEnabled);
        sli.SetRayReconstructionEnabled(cfg.rayReconstructionEnabled);
        ConfigManager::Get().Save();
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Ray Reconstruction");
    bool rrEnabled = cfg.rayReconstructionEnabled;
    ImGui::BeginDisabled(!sli.IsRayReconstructionSupported());
    if (ImGui::Checkbox("Enable DLSS Ray Reconstruction", &rrEnabled)) {
        cfg.rayReconstructionEnabled = rrEnabled;
        sli.SetRayReconstructionEnabled(rrEnabled);
        ConfigManager::Get().Save();
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!sli.IsRayReconstructionSupported() || !cfg.rayReconstructionEnabled);
    const char* rrPresets[] = {
        "Default", "Preset D", "Preset E", "Preset F", "Preset G", "Preset H",
        "Preset I", "Preset J", "Preset K", "Preset L", "Preset M", "Preset N", "Preset O"
    };
    int rrPreset = cfg.rrPreset;
    if (ImGui::Combo("RR Preset", &rrPreset, rrPresets, IM_ARRAYSIZE(rrPresets))) {
        cfg.rrPreset = rrPreset;
        sli.SetRRPreset(rrPreset);
        ConfigManager::Get().Save();
    }
    float rrStrength = cfg.rrDenoiserStrength;
    if (ImGui::SliderFloat("RR Denoiser Strength", &rrStrength, 0.0f, 1.0f, "%.2f")) {
        cfg.rrDenoiserStrength = rrStrength;
        sli.SetRRDenoiserStrength(rrStrength);
        ConfigManager::Get().Save();
    }
    ImGui::EndDisabled();

    const char* fgModes[] = {"Off", "2x (DLSS-G)", "3x (DLSS-G)", "4x (DLSS-G)"};
    int fgMult = sli.GetFrameGenMultiplier();
    int fgIndex = fgMult == 2 ? 1 : (fgMult == 3 ? 2 : (fgMult == 4 ? 3 : 0));
    ImGui::BeginDisabled(!sli.IsFrameGenSupported());
    if (ImGui::Combo("Frame Generation", &fgIndex, fgModes, IM_ARRAYSIZE(fgModes))) {
        int mult = fgIndex == 1 ? 2 : (fgIndex == 2 ? 3 : (fgIndex == 3 ? 4 : 0));
        sli.SetFrameGenMultiplier(mult);
        cfg.frameGenMultiplier = mult;
        ConfigManager::Get().Save();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Smart Frame Generation");
    bool smartFgEnabled = cfg.smartFgEnabled;
    if (ImGui::Checkbox("Enable Smart FG", &smartFgEnabled)) {
        cfg.smartFgEnabled = smartFgEnabled;
        sli.SetSmartFGEnabled(smartFgEnabled);
        ConfigManager::Get().Save();
    }
    ImGui::BeginDisabled(!cfg.smartFgEnabled);
    bool smartAuto = cfg.smartFgAutoDisable;
    if (ImGui::Checkbox("Auto-disable when FPS is high", &smartAuto)) {
        cfg.smartFgAutoDisable = smartAuto;
        sli.SetSmartFGAutoDisable(smartAuto);
        ConfigManager::Get().Save();
    }
    float smartThreshold = cfg.smartFgAutoDisableFps;
    if (ImGui::SliderFloat("Auto-disable FPS Threshold", &smartThreshold, 30.0f, 300.0f, "%.0f")) {
        cfg.smartFgAutoDisableFps = smartThreshold;
        sli.SetSmartFGAutoDisableThreshold(smartThreshold);
        ConfigManager::Get().Save();
    }
    bool smartScene = cfg.smartFgSceneChangeEnabled;
    if (ImGui::Checkbox("Scene-change detection", &smartScene)) {
        cfg.smartFgSceneChangeEnabled = smartScene;
        sli.SetSmartFGSceneChangeEnabled(smartScene);
        ConfigManager::Get().Save();
    }
    float smartSceneThresh = cfg.smartFgSceneChangeThreshold;
    if (ImGui::SliderFloat("Scene-change sensitivity", &smartSceneThresh, 0.05f, 1.0f, "%.2f")) {
        cfg.smartFgSceneChangeThreshold = smartSceneThresh;
        sli.SetSmartFGSceneChangeThreshold(smartSceneThresh);
        ConfigManager::Get().Save();
    }
    float smartQuality = cfg.smartFgInterpolationQuality;
    if (ImGui::SliderFloat("FG Interpolation Quality", &smartQuality, 0.0f, 1.0f, "%.2f")) {
        cfg.smartFgInterpolationQuality = smartQuality;
        sli.SetSmartFGInterpolationQuality(smartQuality);
        ConfigManager::Get().Save();
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Quality");
    float sharpness = cfg.sharpness;
    if (ImGui::SliderFloat("Sharpness", &sharpness, 0.0f, 1.0f, "%.2f")) {
        cfg.sharpness = sharpness;
        sli.SetSharpness(sharpness);
        ConfigManager::Get().Save();
    }
    float lodBias = cfg.lodBias;
    if (ImGui::SliderFloat("Texture Detail (LOD Bias)", &lodBias, -2.0f, 0.0f, "%.2f")) {
        cfg.lodBias = lodBias;
        sli.SetLODBias(lodBias);
        ConfigManager::Get().Save();
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Overlay");
    char fpsToggleLabel[64];
    snprintf(fpsToggleLabel, sizeof(fpsToggleLabel), "Show FPS Overlay (%s)", InputHandler::Get().GetKeyName(cfg.fpsHotkey));
    if (ImGui::Checkbox(fpsToggleLabel, &m_showFPS)) {
        cfg.showFPS = m_showFPS;
        ConfigManager::Get().Save();
    }
    char vignetteToggleLabel[64];
    snprintf(vignetteToggleLabel, sizeof(vignetteToggleLabel), "Show Vignette (%s)", InputHandler::Get().GetKeyName(cfg.vignetteHotkey));
    if (ImGui::Checkbox(vignetteToggleLabel, &m_showVignette)) {
        cfg.showVignette = m_showVignette;
        ConfigManager::Get().Save();
        m_needRebuildTextures = true;
    }
    if (ImGui::SliderFloat("Vignette Intensity", &cfg.vignetteIntensity, 0.0f, 1.0f, "%.2f")) {
        ConfigManager::Get().Save();
        m_needRebuildTextures = true;
    }
    if (ImGui::SliderFloat("Vignette Radius", &cfg.vignetteRadius, 0.2f, 1.0f, "%.2f")) {
        ConfigManager::Get().Save();
        m_needRebuildTextures = true;
    }
    if (ImGui::SliderFloat("Vignette Softness", &cfg.vignetteSoftness, 0.05f, 1.0f, "%.2f")) {
        ConfigManager::Get().Save();
        m_needRebuildTextures = true;
    }
    ImGui::ColorEdit3("Vignette Color", &cfg.vignetteColorR, ImGuiColorEditFlags_NoInputs);
    if (ImGui::IsItemEdited()) {
        ConfigManager::Get().Save();
        m_needRebuildTextures = true;
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Hotkeys");
    ImGui::Text("Press a key to rebind (Esc to cancel).");
    ImGui::SameLine();
    if (ImGui::Button("Cancel") && m_pendingHotkeyTarget) {
        m_pendingHotkeyTarget = nullptr;
    }
    char menuLabel[64];
    snprintf(menuLabel, sizeof(menuLabel), "Menu: %s", InputHandler::Get().GetKeyName(cfg.menuHotkey));
    if (ImGui::Button(menuLabel)) {
        CaptureNextHotkey(&cfg.menuHotkey);
    }
    char fpsLabelKey[64];
    snprintf(fpsLabelKey, sizeof(fpsLabelKey), "FPS: %s", InputHandler::Get().GetKeyName(cfg.fpsHotkey));
    if (ImGui::Button(fpsLabelKey)) {
        CaptureNextHotkey(&cfg.fpsHotkey);
    }
    char vignetteLabelKey[64];
    snprintf(vignetteLabelKey, sizeof(vignetteLabelKey), "Vignette: %s", InputHandler::Get().GetKeyName(cfg.vignetteHotkey));
    if (ImGui::Button(vignetteLabelKey)) {
        CaptureNextHotkey(&cfg.vignetteHotkey);
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Performance");
    float minFps = m_fpsHistory[0];
    float maxFps = m_fpsHistory[0];
    for (int i = 1; i < kFpsHistorySize; ++i) {
        minFps = std::min(minFps, m_fpsHistory[i]);
        maxFps = std::max(maxFps, m_fpsHistory[i]);
    }
    float graphMax = maxFps > 1.0f ? maxFps * 1.15f : 60.0f;
    char fpsLabel[64];
    snprintf(fpsLabel, sizeof(fpsLabel), "FPS Graph (min %.0f / max %.0f)", minFps, maxFps);
    ImGui::PlotLines("##fpsgraph", m_fpsHistory, kFpsHistorySize, m_fpsHistoryIndex, fpsLabel, 0.0f, graphMax, ImVec2(0.0f, 80.0f));

    if (ImGui::Button("Reset to Defaults")) {
        ConfigManager::Get().ResetToDefaults();
        ConfigManager::Get().Load();
        ModConfig& reset = ConfigManager::Get().Data();
        sli.SetDLSSModeIndex(reset.dlssMode);
        sli.SetDLSSPreset(reset.dlssPreset);
        sli.SetFrameGenMultiplier(reset.frameGenMultiplier);
        sli.SetSharpness(reset.sharpness);
        sli.SetLODBias(reset.lodBias);
        sli.SetReflexEnabled(reset.reflexEnabled);
        sli.SetHUDFixEnabled(reset.hudFixEnabled);
        sli.SetRayReconstructionEnabled(reset.rayReconstructionEnabled);
        sli.SetRRPreset(reset.rrPreset);
        sli.SetRRDenoiserStrength(reset.rrDenoiserStrength);
        sli.SetSmartFGEnabled(reset.smartFgEnabled);
        sli.SetSmartFGAutoDisable(reset.smartFgAutoDisable);
        sli.SetSmartFGAutoDisableThreshold(reset.smartFgAutoDisableFps);
        sli.SetSmartFGSceneChangeEnabled(reset.smartFgSceneChangeEnabled);
        sli.SetSmartFGSceneChangeThreshold(reset.smartFgSceneChangeThreshold);
        sli.SetSmartFGInterpolationQuality(reset.smartFgInterpolationQuality);
        m_showFPS = reset.showFPS;
        m_showVignette = reset.showVignette;
        m_needRebuildTextures = true;
    }

    ImGui::Separator();
    ImGui::Text("Hotkeys: Menu %s | FPS %s | Vignette %s",
        InputHandler::Get().GetKeyName(cfg.menuHotkey),
        InputHandler::Get().GetKeyName(cfg.fpsHotkey),
        InputHandler::Get().GetKeyName(cfg.vignetteHotkey));
    ImGui::Text("Camera: %s (J %.3f, %.3f)", m_cachedCamera ? "OK" : "Missing", m_cachedJitterX, m_cachedJitterY);

    ImGui::End();
}

void ImGuiOverlay::BuildFPSOverlay() {
    if (!m_showFPS) return;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    char buf[64];
    int mult = StreamlineIntegration::Get().GetFrameGenMultiplier();
    if (mult < 1) mult = 1;
    snprintf(buf, sizeof(buf), "%.0f -> %.0f FPS", m_cachedTotalFPS / (float)mult, m_cachedTotalFPS);
    ImVec2 pos(24.0f, 24.0f);
    drawList->AddText(nullptr, 28.0f, pos, IM_COL32(212, 175, 55, 220), buf);
}

void ImGuiOverlay::BuildVignette() {
    if (!m_showVignette) return;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    ModConfig& cfg = ConfigManager::Get().Data();
    float intensity = cfg.vignetteIntensity;
    ImU32 col = IM_COL32((int)(cfg.vignetteColorR * 255.0f),
                         (int)(cfg.vignetteColorG * 255.0f),
                         (int)(cfg.vignetteColorB * 255.0f),
                         (int)(intensity * 180.0f));
    drawList->AddRectFilled(ImVec2(0, 0), size, col, 0.0f);
}

void ImGuiOverlay::BuildDebugWindow() {
    if (!m_showDebug) return;
    ImGui::Begin("Resource Debug", &m_showDebug);
    std::string debugInfo = ResourceDetector::Get().GetDebugInfo();
    if (debugInfo.empty()) debugInfo = "No debug info available yet...";
    ImGui::TextUnformatted(debugInfo.c_str());
    ImGui::End();
}

void ImGuiOverlay::BuildTexturesIfNeeded() {
    if (!m_needRebuildTextures) return;
    m_needRebuildTextures = false;
}

void ImGuiOverlay::Render() {
    if (!m_initialized || !m_swapChain || !m_commandList || !m_commandAllocator) return;
    if (!m_visible && !m_showFPS && !m_showVignette && !m_showDebug) return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    BuildTexturesIfNeeded();

    if (m_visible) BuildMainMenu();
    BuildFPSOverlay();
    BuildVignette();
    BuildDebugWindow();

    ImGui::Render();

    UINT backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (!m_backBuffers || backBufferIndex >= m_backBufferCount) return;

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[backBufferIndex];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->OMSetRenderTargets(1, &m_rtvHandles[backBufferIndex], FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap };
    m_commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->Close();
    ID3D12CommandList* lists[] = { m_commandList };
    m_queue->ExecuteCommandLists(1, lists);
    m_fenceValue++;
    m_queue->Signal(m_fence, m_fenceValue);
}
