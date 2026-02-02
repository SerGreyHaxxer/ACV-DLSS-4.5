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
#include <wrl/client.h>
#include <cstring>
#include "external/nvapi/nvapi-main/nvapi.h"

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

namespace {
    struct NvApiMetrics {
        bool initialized = false;
        bool hasGpu = false;
        NvPhysicalGpuHandle gpu = nullptr;
        char gpuName[NVAPI_SHORT_STRING_MAX] = {};
        char dxgiName[128] = {};
        bool dxgiNameReady = false;
    } g_nvapiMetrics;

    bool InitNvApi() {
        if (g_nvapiMetrics.initialized) return g_nvapiMetrics.hasGpu;
        g_nvapiMetrics.initialized = true;
        if (NvAPI_Initialize() != NVAPI_OK) return false;
        NvU32 gpuCount = 0;
        NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
        if (NvAPI_EnumPhysicalGPUs(handles, &gpuCount) != NVAPI_OK || gpuCount == 0) return false;
        g_nvapiMetrics.gpu = handles[0];
        NvAPI_ShortString name{};
        if (NvAPI_GPU_GetFullName(g_nvapiMetrics.gpu, name) == NVAPI_OK) {
            strncpy_s(g_nvapiMetrics.gpuName, name, _TRUNCATE);
        }
        g_nvapiMetrics.hasGpu = true;
        return true;
    }

    void EnsureDxgiName(ID3D12Device* device) {
        if (g_nvapiMetrics.dxgiNameReady || !device) return;
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return;
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(adapter->GetDesc(&desc))) return;
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_nvapiMetrics.dxgiName, sizeof(g_nvapiMetrics.dxgiName), nullptr, nullptr);
        g_nvapiMetrics.dxgiNameReady = true;
    }

    bool QueryGpuUtilization(uint32_t& outPercent) {
        static uint64_t s_lastInitAttempt = 0;
        uint64_t now = GetTickCount64();
        if (!g_nvapiMetrics.initialized && now - s_lastInitAttempt < 5000) {
            return false;
        }
        if (!g_nvapiMetrics.initialized) {
            s_lastInitAttempt = now;
        }
        if (!InitNvApi()) return false;
        NV_GPU_DYNAMIC_PSTATES_INFO_EX info{};
        info.version = NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER;
        if (NvAPI_GPU_GetDynamicPstatesInfoEx(g_nvapiMetrics.gpu, &info) != NVAPI_OK) return false;
        if (!info.utilization[0].bIsPresent) return false;
        outPercent = info.utilization[0].percentage;
        return true;
    }

    bool QueryVramUsageMB(ID3D12Device* device, uint32_t& outUsedMB, uint32_t& outBudgetMB) {
        if (!device) return false;
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(adapter.As(&adapter3))) return false;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return false;
        outUsedMB = static_cast<uint32_t>(info.CurrentUsage / (1024 * 1024));
        outBudgetMB = static_cast<uint32_t>(info.Budget / (1024 * 1024));
        return true;
    }

    struct MetricsCache {
        uint64_t lastUpdateMs = 0;
        bool gpuOk = false;
        uint32_t gpuPercent = 0;
        bool vramOk = false;
        uint32_t vramUsed = 0;
        uint32_t vramBudget = 0;
    } g_metricsCache;

    void UpdateMetrics(ID3D12Device* device) {
        uint64_t now = GetTickCount64();
        if (now - g_metricsCache.lastUpdateMs < 500) return;
        g_metricsCache.lastUpdateMs = now;
        g_metricsCache.gpuOk = QueryGpuUtilization(g_metricsCache.gpuPercent);
        g_metricsCache.vramOk = QueryVramUsageMB(device, g_metricsCache.vramUsed, g_metricsCache.vramBudget);
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
    m_fenceValue = 0;

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
    UpdateControls();
}

void ImGuiOverlay::Shutdown() {
    if (!m_initialized) return;
    if (m_cursorUnlocked) {
        ClipCursor(&m_prevClip);
        while (ShowCursor(FALSE) >= 0) {}
        m_cursorUnlocked = false;
    }
    if (m_fence) {
        WaitForFence(m_fence, m_fenceEvent, m_fenceValue);
    }
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
    if (ImGui::GetCurrentContext() && (overlay.m_visible || overlay.m_showSetupWizard) &&
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
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

    m_commandAllocators.resize(m_backBufferCount);
    m_commandLists.resize(m_backBufferCount);
    m_frameFenceValues.assign(m_backBufferCount, 0);
    for (UINT i = 0; i < m_backBufferCount; ++i) {
        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])))) return false;
        if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[i], nullptr, IID_PPV_ARGS(&m_commandLists[i])))) return false;
        m_commandLists[i]->Close();
    }

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
    for (auto* list : m_commandLists) { if (list) list->Release(); }
    m_commandLists.clear();
    for (auto* alloc : m_commandAllocators) { if (alloc) alloc->Release(); }
    m_commandAllocators.clear();
    m_frameFenceValues.clear();
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
    if (m_fence && !m_frameFenceValues.empty()) {
        for (UINT i = 0; i < m_backBufferCount; ++i) {
            if (m_frameFenceValues[i] > 0) {
                WaitForFence(m_fence, m_fenceEvent, m_frameFenceValues[i]);
            }
        }
    }
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
    UpdateInputState();
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

void ImGuiOverlay::UpdateInputState() {
    if (!m_hwnd) return;
    ImGuiIO& io = ImGui::GetIO();
    if (m_visible || m_showSetupWizard) {
        io.MouseDrawCursor = true;
        if (!m_cursorUnlocked) {
            GetClipCursor(&m_prevClip);
            ClipCursor(nullptr);
            while (ShowCursor(TRUE) < 0) {}
            m_cursorUnlocked = true;
        }
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(m_hwnd, &pt);
        io.MousePos = ImVec2((float)pt.x, (float)pt.y);
        io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    } else {
        io.MouseDrawCursor = false;
        if (m_cursorUnlocked) {
            ClipCursor(&m_prevClip);
            while (ShowCursor(FALSE) >= 0) {}
            m_cursorUnlocked = false;
        }
    }
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
    m_visible = cfg.uiVisible;
    m_showSetupWizard = cfg.setupWizardForceShow || !cfg.setupWizardCompleted;
    if (m_initialized) UpdateInputState();
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
    if (ImGui::Button("Run Setup Wizard")) {
        m_showSetupWizard = true;
        cfg.setupWizardForceShow = true;
        ConfigManager::Get().Save();
    }
    ImGui::Separator();

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
    bool fgDisabled = sli.IsFrameGenDisabledDueToInvalidParam();
    bool fgOk = sli.IsFrameGenSupported() && !fgDisabled && sli.GetFrameGenMultiplier() >= 2 && !sli.IsSmartFGTemporarilyDisabled() && sli.GetFrameGenStatus() == sl::DLSSGStatus::eOk;
    bool fgWarn = sli.IsFrameGenSupported() && !fgDisabled && (sli.GetFrameGenMultiplier() < 2 || sli.IsSmartFGTemporarilyDisabled() || sli.GetFrameGenStatus() != sl::DLSSGStatus::eOk);
    ImGui::TextColored(fgOk ? statusOk : (fgWarn ? statusWarn : statusBad), "Frame Gen");
    ImGui::SameLine(260.0f);
    bool camOk = sli.HasCameraData();
    ImGui::TextColored(camOk ? statusOk : statusWarn, "Camera");
    ImGui::SameLine(360.0f);
    bool dvcOk = sli.IsDeepDVCSupported() && sli.IsDeepDVCEnabled();
    bool dvcWarn = sli.IsDeepDVCSupported() && !sli.IsDeepDVCEnabled();
    ImGui::TextColored(dvcOk ? statusOk : (dvcWarn ? statusWarn : statusBad), "DeepDVC");
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
        cfg.deepDvcEnabled = false;
        sli.SetDLSSModeIndex(cfg.dlssMode);
        sli.SetDLSSPreset(cfg.dlssPreset);
        sli.SetFrameGenMultiplier(cfg.frameGenMultiplier);
        sli.SetSharpness(cfg.sharpness);
        sli.SetLODBias(cfg.lodBias);
        sli.SetReflexEnabled(cfg.reflexEnabled);
        sli.SetRayReconstructionEnabled(cfg.rayReconstructionEnabled);
        sli.SetDeepDVCEnabled(cfg.deepDvcEnabled);
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
        cfg.deepDvcEnabled = false;
        cfg.deepDvcAdaptiveEnabled = false;
        sli.SetDLSSModeIndex(cfg.dlssMode);
        sli.SetDLSSPreset(cfg.dlssPreset);
        sli.SetFrameGenMultiplier(cfg.frameGenMultiplier);
        sli.SetSharpness(cfg.sharpness);
        sli.SetLODBias(cfg.lodBias);
        sli.SetReflexEnabled(cfg.reflexEnabled);
        sli.SetRayReconstructionEnabled(cfg.rayReconstructionEnabled);
        sli.SetDeepDVCEnabled(cfg.deepDvcEnabled);
        sli.SetDeepDVCAdaptiveEnabled(cfg.deepDvcAdaptiveEnabled);
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
        cfg.deepDvcEnabled = false;
        cfg.deepDvcAdaptiveEnabled = false;
        sli.SetDLSSModeIndex(cfg.dlssMode);
        sli.SetDLSSPreset(cfg.dlssPreset);
        sli.SetFrameGenMultiplier(cfg.frameGenMultiplier);
        sli.SetSharpness(cfg.sharpness);
        sli.SetLODBias(cfg.lodBias);
        sli.SetReflexEnabled(cfg.reflexEnabled);
        sli.SetRayReconstructionEnabled(cfg.rayReconstructionEnabled);
        sli.SetDeepDVCEnabled(cfg.deepDvcEnabled);
        sli.SetDeepDVCAdaptiveEnabled(cfg.deepDvcAdaptiveEnabled);
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

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "DeepDVC (RTX Dynamic Vibrance)");
    bool deepDvcEnabled = cfg.deepDvcEnabled;
    ImGui::BeginDisabled(!sli.IsDeepDVCSupported());
    if (ImGui::Checkbox("Enable DeepDVC", &deepDvcEnabled)) {
        cfg.deepDvcEnabled = deepDvcEnabled;
        sli.SetDeepDVCEnabled(deepDvcEnabled);
        ConfigManager::Get().Save();
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!sli.IsDeepDVCSupported() || !cfg.deepDvcEnabled);
    float deepDvcIntensity = cfg.deepDvcIntensity;
    if (ImGui::SliderFloat("DeepDVC Intensity", &deepDvcIntensity, 0.0f, 1.0f, "%.2f")) {
        cfg.deepDvcIntensity = deepDvcIntensity;
        sli.SetDeepDVCIntensity(deepDvcIntensity);
        ConfigManager::Get().Save();
    }
    float deepDvcSaturation = cfg.deepDvcSaturation;
    if (ImGui::SliderFloat("DeepDVC Saturation Boost", &deepDvcSaturation, 0.0f, 1.0f, "%.2f")) {
        cfg.deepDvcSaturation = deepDvcSaturation;
        sli.SetDeepDVCSaturation(deepDvcSaturation);
        ConfigManager::Get().Save();
    }
    bool deepDvcAdaptive = cfg.deepDvcAdaptiveEnabled;
    if (ImGui::Checkbox("Adaptive Vibrance", &deepDvcAdaptive)) {
        cfg.deepDvcAdaptiveEnabled = deepDvcAdaptive;
        sli.SetDeepDVCAdaptiveEnabled(deepDvcAdaptive);
        ConfigManager::Get().Save();
    }
    ImGui::BeginDisabled(!cfg.deepDvcAdaptiveEnabled);
    float deepDvcAdaptiveStrength = cfg.deepDvcAdaptiveStrength;
    if (ImGui::SliderFloat("Adaptive Strength", &deepDvcAdaptiveStrength, 0.0f, 1.0f, "%.2f")) {
        cfg.deepDvcAdaptiveStrength = deepDvcAdaptiveStrength;
        sli.SetDeepDVCAdaptiveStrength(deepDvcAdaptiveStrength);
        ConfigManager::Get().Save();
    }
    float deepDvcAdaptiveMin = cfg.deepDvcAdaptiveMin;
    if (ImGui::SliderFloat("Adaptive Min", &deepDvcAdaptiveMin, 0.0f, 1.0f, "%.2f")) {
        cfg.deepDvcAdaptiveMin = deepDvcAdaptiveMin;
        if (cfg.deepDvcAdaptiveMin > cfg.deepDvcAdaptiveMax) cfg.deepDvcAdaptiveMax = cfg.deepDvcAdaptiveMin;
        sli.SetDeepDVCAdaptiveMin(deepDvcAdaptiveMin);
        ConfigManager::Get().Save();
    }
    float deepDvcAdaptiveMax = cfg.deepDvcAdaptiveMax;
    if (ImGui::SliderFloat("Adaptive Max", &deepDvcAdaptiveMax, 0.0f, 1.0f, "%.2f")) {
        cfg.deepDvcAdaptiveMax = deepDvcAdaptiveMax;
        if (cfg.deepDvcAdaptiveMin > cfg.deepDvcAdaptiveMax) cfg.deepDvcAdaptiveMin = cfg.deepDvcAdaptiveMax;
        sli.SetDeepDVCAdaptiveMax(deepDvcAdaptiveMax);
        ConfigManager::Get().Save();
    }
    float deepDvcAdaptiveSmoothing = cfg.deepDvcAdaptiveSmoothing;
    if (ImGui::SliderFloat("Adaptive Smoothing", &deepDvcAdaptiveSmoothing, 0.01f, 1.0f, "%.2f")) {
        cfg.deepDvcAdaptiveSmoothing = deepDvcAdaptiveSmoothing;
        sli.SetDeepDVCAdaptiveSmoothing(deepDvcAdaptiveSmoothing);
        ConfigManager::Get().Save();
    }
    ImGui::EndDisabled();
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
    bool mvecAuto = cfg.mvecScaleAuto;
    if (ImGui::Checkbox("Auto Motion Vector Scale", &mvecAuto)) {
        cfg.mvecScaleAuto = mvecAuto;
        ConfigManager::Get().Save();
    }
    ImGui::BeginDisabled(cfg.mvecScaleAuto);
    float mvecScaleX = cfg.mvecScaleX;
    if (ImGui::SliderFloat("MV Scale X", &mvecScaleX, 0.5f, 3.0f, "%.2f")) {
        cfg.mvecScaleX = mvecScaleX;
        sli.SetMVecScale(mvecScaleX, cfg.mvecScaleY);
        ConfigManager::Get().Save();
    }
    float mvecScaleY = cfg.mvecScaleY;
    if (ImGui::SliderFloat("MV Scale Y", &mvecScaleY, 0.5f, 3.0f, "%.2f")) {
        cfg.mvecScaleY = mvecScaleY;
        sli.SetMVecScale(cfg.mvecScaleX, mvecScaleY);
        ConfigManager::Get().Save();
    }
    ImGui::EndDisabled();

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

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Performance Metrics");
    if (g_metricsCache.gpuOk) {
        ImGui::Text("GPU Utilization: %u%%", g_metricsCache.gpuPercent);
    } else {
        ImGui::Text("GPU Utilization: N/A");
    }
    if (g_metricsCache.vramOk) {
        uint32_t usedShown = g_metricsCache.vramUsed > g_metricsCache.vramBudget ? g_metricsCache.vramBudget : g_metricsCache.vramUsed;
        ImGui::Text("VRAM: %u / %u MB", usedShown, g_metricsCache.vramBudget);
    } else {
        ImGui::Text("VRAM: N/A");
    }
    const float fgActual = sli.GetFgActualMultiplier();
    if (fgActual > 1.01f) {
        ImGui::Text("FG Actual: %.2fx", fgActual);
    } else {
        ImGui::Text("FG Actual: Off");
    }

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
        sli.SetDeepDVCEnabled(reset.deepDvcEnabled);
        sli.SetDeepDVCIntensity(reset.deepDvcIntensity);
        sli.SetDeepDVCSaturation(reset.deepDvcSaturation);
        sli.SetDeepDVCAdaptiveEnabled(reset.deepDvcAdaptiveEnabled);
        sli.SetDeepDVCAdaptiveStrength(reset.deepDvcAdaptiveStrength);
        sli.SetDeepDVCAdaptiveMin(reset.deepDvcAdaptiveMin);
        sli.SetDeepDVCAdaptiveMax(reset.deepDvcAdaptiveMax);
        sli.SetDeepDVCAdaptiveSmoothing(reset.deepDvcAdaptiveSmoothing);
        sli.SetSmartFGEnabled(reset.smartFgEnabled);
        sli.SetSmartFGAutoDisable(reset.smartFgAutoDisable);
        sli.SetSmartFGAutoDisableThreshold(reset.smartFgAutoDisableFps);
        sli.SetSmartFGSceneChangeEnabled(reset.smartFgSceneChangeEnabled);
        sli.SetSmartFGSceneChangeThreshold(reset.smartFgSceneChangeThreshold);
        sli.SetSmartFGInterpolationQuality(reset.smartFgInterpolationQuality);
        sli.SetMVecScale(reset.mvecScaleX, reset.mvecScaleY);
        m_showFPS = reset.showFPS;
        m_showVignette = reset.showVignette;
        m_needRebuildTextures = true;
    }

    ImGui::Separator();
    ImGui::Text("Hotkeys: Menu %s | FPS %s | Vignette %s",
        InputHandler::Get().GetKeyName(cfg.menuHotkey),
        InputHandler::Get().GetKeyName(cfg.fpsHotkey),
        InputHandler::Get().GetKeyName(cfg.vignetteHotkey));
    ImGui::Text("Camera: %s (J %.3f, %.3f) Delta %.3f", m_cachedCamera ? "OK" : "Missing", m_cachedJitterX, m_cachedJitterY, StreamlineIntegration::Get().GetLastCameraDelta());

    ImGui::End();
}

void ImGuiOverlay::BuildSetupWizard() {
    if (!m_showSetupWizard) return;
    UpdateInputState();
    ModConfig& cfg = ConfigManager::Get().Data();
    StreamlineIntegration& sli = StreamlineIntegration::Get();

    ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    ImGui::Begin("First-Time Setup Wizard", &m_showSetupWizard, ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Welcome! We'll recommend settings based on your GPU.");
    ImGui::Separator();
    if (g_nvapiMetrics.gpuName[0] != '\0') {
        ImGui::Text("Detected GPU: %s", g_nvapiMetrics.gpuName);
    } else if (g_nvapiMetrics.dxgiName[0] != '\0') {
        ImGui::Text("Detected GPU: %s", g_nvapiMetrics.dxgiName);
    } else {
        ImGui::Text("Detected GPU: Unknown (NVAPI not available)");
    }

    const char* gpuName = g_nvapiMetrics.gpuName[0] != '\0' ? g_nvapiMetrics.gpuName : g_nvapiMetrics.dxgiName;
    bool is40Series = gpuName && strstr(gpuName, "RTX 40") != nullptr;
    ImGui::Spacing();
    if (ImGui::Button("Apply Recommended Settings")) {
        LOG_INFO("[Wizard] Apply recommended settings clicked");
        if (is40Series) {
            cfg.dlssMode = 3;
            cfg.dlssPreset = 0;
            cfg.frameGenMultiplier = 4;
            cfg.sharpness = 0.35f;
            cfg.lodBias = -1.0f;
            cfg.reflexEnabled = true;
            cfg.rayReconstructionEnabled = true;
            cfg.deepDvcEnabled = false;
            cfg.deepDvcAdaptiveEnabled = false;
        } else {
            cfg.dlssMode = 3;
            cfg.dlssPreset = 0;
            cfg.frameGenMultiplier = 0;
            cfg.sharpness = 0.35f;
            cfg.lodBias = -1.0f;
            cfg.reflexEnabled = true;
            cfg.rayReconstructionEnabled = false;
            cfg.deepDvcEnabled = false;
            cfg.deepDvcAdaptiveEnabled = false;
        }
        ConfigManager::Get().Save();
        sli.SetDLSSModeIndex(cfg.dlssMode);
        sli.SetDLSSPreset(cfg.dlssPreset);
        sli.SetFrameGenMultiplier(cfg.frameGenMultiplier);
        sli.SetSharpness(cfg.sharpness);
        sli.SetLODBias(cfg.lodBias);
        sli.SetReflexEnabled(cfg.reflexEnabled);
        sli.SetRayReconstructionEnabled(cfg.rayReconstructionEnabled);
        sli.SetDeepDVCEnabled(cfg.deepDvcEnabled);
        sli.SetDeepDVCAdaptiveEnabled(cfg.deepDvcAdaptiveEnabled);
        LOG_INFO("[Wizard] Applied: DLSS=%d Preset=%d FG=%dx RR=%d", cfg.dlssMode, cfg.dlssPreset, cfg.frameGenMultiplier, cfg.rayReconstructionEnabled ? 1 : 0);
        cfg.setupWizardCompleted = true;
        cfg.setupWizardForceShow = false;
        ConfigManager::Get().Save();
        m_showSetupWizard = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip for Now")) {
        cfg.setupWizardCompleted = true;
        cfg.setupWizardForceShow = false;
        ConfigManager::Get().Save();
        m_showSetupWizard = false;
    }
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
    if (!m_initialized || !m_swapChain || m_commandLists.empty() || m_commandAllocators.empty()) return;
    if (!m_visible && !m_showFPS && !m_showVignette && !m_showDebug && !m_showSetupWizard) return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();

    UpdateMetrics(m_device);
    EnsureDxgiName(m_device);
    UpdateInputState();

    ImGui::NewFrame();

    BuildTexturesIfNeeded();

    if (m_visible) BuildMainMenu();
    BuildSetupWizard();
    BuildFPSOverlay();
    BuildVignette();
    BuildDebugWindow();

    ImGui::Render();

    UINT backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (!m_backBuffers || backBufferIndex >= m_backBufferCount) return;
    if (m_fence && m_frameFenceValues[backBufferIndex] > 0) {
        WaitForFence(m_fence, m_fenceEvent, m_frameFenceValues[backBufferIndex]);
    }
    ID3D12CommandAllocator* allocator = m_commandAllocators[backBufferIndex];
    ID3D12GraphicsCommandList* cmd = m_commandLists[backBufferIndex];
    allocator->Reset();
    cmd->Reset(allocator, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_backBuffers[backBufferIndex];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd->ResourceBarrier(1, &barrier);

    cmd->OMSetRenderTargets(1, &m_rtvHandles[backBufferIndex], FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap };
    cmd->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(1, &barrier);

    cmd->Close();
    ID3D12CommandList* lists[] = { cmd };
    m_queue->ExecuteCommandLists(1, lists);
    m_fenceValue++;
    m_queue->Signal(m_fence, m_fenceValue);
    m_frameFenceValues[backBufferIndex] = m_fenceValue;
}
