#include "imgui_overlay.h"
#include "config_manager.h"
#include "input_handler.h"
#include "logger.h"
#include "nvapi.h"
#include "resource_detector.h"
#include "streamline_integration.h"
#include <algorithm>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <format>
#include <limits>
#include <vector>
#include <wrl/client.h>

#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

static void ImGuiOverlay_SrvAlloc(ImGui_ImplDX12_InitInfo *info,
                                  D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu,
                                  D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu) {
  ImGuiOverlay::Get().AllocateSrv(info, out_cpu, out_gpu);
}

static void ImGuiOverlay_SrvFree(ImGui_ImplDX12_InitInfo *info,
                                 D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
  ImGuiOverlay::Get().FreeSrv(info, cpu, gpu);
}

static void WaitForFence(ID3D12Fence *fence, HANDLE eventHandle, UINT64 value) {
  if (!fence || !eventHandle)
    return;
  if (fence->GetCompletedValue() < value) {
    fence->SetEventOnCompletion(value, eventHandle);
    WaitForSingleObject(eventHandle, INFINITE);
  }
}

namespace {
uint64_t GetTimeMs() {
  static LARGE_INTEGER freq = []() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<uint64_t>((counter.QuadPart * 1000ULL) / freq.QuadPart);
}

struct NvApiMetrics {
  bool initialized = false;
  bool hasGpu = false;
  NvPhysicalGpuHandle gpu = nullptr;
  char gpuName[NVAPI_SHORT_STRING_MAX] = {};
  char dxgiName[128] = {};
  bool dxgiNameReady = false;
} g_nvapiMetrics;

bool InitNvApi() {
  if (g_nvapiMetrics.initialized)
    return g_nvapiMetrics.hasGpu;
  g_nvapiMetrics.initialized = true;
  if (NvAPI_Initialize() != NVAPI_OK)
    return false;
  NvU32 gpuCount = 0;
  NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
  if (NvAPI_EnumPhysicalGPUs(handles, &gpuCount) != NVAPI_OK || gpuCount == 0)
    return false;
  g_nvapiMetrics.gpu = handles[0];
  NvAPI_ShortString name{};
  if (NvAPI_GPU_GetFullName(g_nvapiMetrics.gpu, name) == NVAPI_OK) {
    strncpy_s(g_nvapiMetrics.gpuName, name, _TRUNCATE);
  }
  g_nvapiMetrics.hasGpu = true;
  return true;
}

void EnsureDxgiName(ID3D12Device *device) {
  if (g_nvapiMetrics.dxgiNameReady || !device)
    return;
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
    return;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgiDevice->GetAdapter(&adapter)))
    return;
  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc)))
    return;
  WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_nvapiMetrics.dxgiName,
                      sizeof(g_nvapiMetrics.dxgiName), nullptr, nullptr);
  g_nvapiMetrics.dxgiNameReady = true;
}

bool QueryGpuUtilization(uint32_t &outPercent) {
  static uint64_t s_lastInitAttempt = 0;
  uint64_t now = GetTimeMs();
  if (!g_nvapiMetrics.initialized && now - s_lastInitAttempt < 5000) {
    return false;
  }
  if (!g_nvapiMetrics.initialized) {
    s_lastInitAttempt = now;
  }
  if (!InitNvApi())
    return false;
  NV_GPU_DYNAMIC_PSTATES_INFO_EX info{};
  info.version = NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER;
  if (NvAPI_GPU_GetDynamicPstatesInfoEx(g_nvapiMetrics.gpu, &info) != NVAPI_OK)
    return false;
  if (!info.utilization[0].bIsPresent)
    return false;
  outPercent = info.utilization[0].percentage;
  return true;
}

bool QueryVramUsageMB(ID3D12Device *device, uint32_t &outUsedMB,
                      uint32_t &outBudgetMB) {
  if (!device)
    return false;
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
    return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgiDevice->GetAdapter(&adapter)))
    return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (FAILED(adapter.As(&adapter3)))
    return false;
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                            &info)))
    return false;
  outUsedMB = static_cast<uint32_t>(info.CurrentUsage / (1024 * 1024));
  outBudgetMB = static_cast<uint32_t>(info.Budget / (1024 * 1024));
  return true;
}

struct MetricsCache {
  std::atomic<uint64_t> lastUpdateMs{0};
  std::atomic<bool> gpuOk{false};
  std::atomic<uint32_t> gpuPercent{0};
  std::atomic<bool> vramOk{false};
  std::atomic<uint32_t> vramUsed{0};
  std::atomic<uint32_t> vramBudget{0};
} g_metricsCache;

void UpdateMetrics(ID3D12Device *device) {
  uint64_t now = GetTimeMs();
  uint64_t lastUpdate =
      g_metricsCache.lastUpdateMs.load(std::memory_order_relaxed);
  if (now - lastUpdate < 500)
    return;
  g_metricsCache.lastUpdateMs.store(now, std::memory_order_relaxed);
  uint32_t gpuPercent = 0;
  bool gpuOk = QueryGpuUtilization(gpuPercent);
  g_metricsCache.gpuPercent.store(gpuPercent, std::memory_order_relaxed);
  g_metricsCache.gpuOk.store(gpuOk, std::memory_order_relaxed);
  uint32_t vramUsed = 0;
  uint32_t vramBudget = 0;
  bool vramOk = QueryVramUsageMB(device, vramUsed, vramBudget);
  g_metricsCache.vramUsed.store(vramUsed, std::memory_order_relaxed);
  g_metricsCache.vramBudget.store(vramBudget, std::memory_order_relaxed);
  g_metricsCache.vramOk.store(vramOk, std::memory_order_relaxed);
}
} // namespace

ImGuiOverlay &ImGuiOverlay::Get() {
  static ImGuiOverlay instance;
  return instance;
}

void ImGuiOverlay::Initialize(IDXGISwapChain *swapChain) {
  if (m_initialized || !swapChain)
    return;
  m_shuttingDown.store(false, std::memory_order_relaxed);
  if (FAILED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3),
                                       (void **)&m_swapChain)) ||
      !m_swapChain)
    return;

  DXGI_SWAP_CHAIN_DESC desc{};
  if (FAILED(m_swapChain->GetDesc(&desc)))
    return;
  if (desc.BufferCount == 0 || desc.BufferCount > 16)
    return;
  m_backBufferCount = desc.BufferCount;
  m_hwnd = desc.OutputWindow;

  if (FAILED(
          m_swapChain->GetDevice(__uuidof(ID3D12Device), (void **)&m_device)))
    return;
  m_queue = StreamlineIntegration::Get().GetCommandQueue();
  m_fenceValue = 0;
  m_vignetteState = D3D12_RESOURCE_STATE_COPY_DEST;

  if (!EnsureDeviceResources())
    return;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ApplyStyle();
  ImGui_ImplWin32_Init(m_hwnd);
  if (m_hwnd && !m_prevWndProc) {
    m_prevWndProc = (WNDPROC)SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC,
                                               (LONG_PTR)OverlayWndProc);
  }
  ImGui_ImplDX12_InitInfo initInfo{};
  initInfo.Device = m_device;
  initInfo.CommandQueue = m_queue;
  initInfo.NumFramesInFlight = static_cast<int>(m_backBufferCount);
  initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  initInfo.SrvDescriptorHeap = m_srvHeap;
  initInfo.SrvDescriptorAllocFn = ImGuiOverlay_SrvAlloc;
  initInfo.SrvDescriptorFreeFn = ImGuiOverlay_SrvFree;
  ImGui_ImplDX12_Init(&initInfo);

  m_metricsThreadRunning.store(true, std::memory_order_relaxed);
  m_metricsThread = std::thread([this]() {
    ID3D12Device *device = m_device;
    if (device)
      device->AddRef();
    while (m_metricsThreadRunning.load(std::memory_order_relaxed)) {
      if (!device)
        break;
      UpdateMetrics(device);
      EnsureDxgiName(device);
      Sleep(100);
    }
    if (device)
      device->Release();
  });

  m_initialized = true;
  UpdateControls();
}

void ImGuiOverlay::Shutdown() {
  if (!m_initialized)
    return;
  m_shuttingDown.store(true, std::memory_order_relaxed);
  if (m_cursorUnlocked) {
    ClipCursor(&m_prevClip);
    ShowCursor(FALSE);
    m_cursorUnlocked = false;
  }
  if (m_fence) {
    WaitForFence(m_fence, m_fenceEvent, m_fenceValue);
  }
  if (m_metricsThreadRunning.exchange(false, std::memory_order_relaxed)) {
    if (m_metricsThread.joinable()) {
      m_metricsThread.join();
    }
  }
  ImGui_ImplDX12_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  if (m_hwnd && m_prevWndProc) {
    SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)m_prevWndProc);
    m_prevWndProc = nullptr;
  }
  ReleaseDeviceResources();
  if (m_device) {
    m_device->Release();
    m_device = nullptr;
  }
  if (m_swapChain) {
    m_swapChain->Release();
    m_swapChain = nullptr;
  }
  m_initialized = false;
}

LRESULT CALLBACK ImGuiOverlay::OverlayWndProc(HWND hwnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam) {
  ImGuiOverlay &overlay = ImGuiOverlay::Get();
  if (ImGui::GetCurrentContext() &&
      (overlay.m_visible || overlay.m_showSetupWizard) &&
      ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
    return 1;
  }
  if (overlay.m_prevWndProc) {
    return CallWindowProcW(overlay.m_prevWndProc, hwnd, msg, wParam, lParam);
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ImGuiOverlay::EnsureDeviceResources() {
  if (!m_device)
    return false;

  if (!m_queue) {
    LOG_WARN("ImGuiOverlay: Command queue not available yet.");
    return false;
  }

  m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
  srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvDesc.NumDescriptors = 128;
  srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(
          m_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&m_srvHeap))))
    return false;

  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
  rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvDesc.NumDescriptors = m_backBufferCount;
  rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  if (FAILED(
          m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap))))
    return false;

  m_commandAllocators.resize(m_backBufferCount);
  m_commandLists.resize(m_backBufferCount);
  m_frameFenceValues.assign(m_backBufferCount, 0);
  for (UINT i = 0; i < m_backBufferCount; ++i) {
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i]))))
      return false;
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           m_commandAllocators[i], nullptr,
                                           IID_PPV_ARGS(&m_commandLists[i]))))
      return false;
    m_commandLists[i]->Close();
  }

  if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&m_fence))))
    return false;
  m_fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!m_fenceEvent)
    return false;

  m_srvDescriptorCount = srvDesc.NumDescriptors;
  m_srvDescriptorNext = 0;
  m_srvFreeList.clear();
  RecreateRenderTargets();
  return true;
}

void ImGuiOverlay::ReleaseDeviceResources() {
  ReleaseRenderTargets();
  if (m_vignetteTexture) {
    m_vignetteTexture->Release();
    m_vignetteTexture = nullptr;
  }
  m_vignetteState = D3D12_RESOURCE_STATE_COPY_DEST;
  m_vignetteSrv = {};
  m_vignetteSrvGpu = {};
  m_pendingUploads.clear();
  for (auto *list : m_commandLists) {
    if (list)
      list->Release();
  }
  m_commandLists.clear();
  for (auto *alloc : m_commandAllocators) {
    if (alloc)
      alloc->Release();
  }
  m_commandAllocators.clear();
  m_frameFenceValues.clear();
  if (m_fence) {
    m_fence->Release();
    m_fence = nullptr;
  }
  if (m_fenceEvent) {
    CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
  }
  if (m_rtvHeap) {
    m_rtvHeap->Release();
    m_rtvHeap = nullptr;
  }
  if (m_srvHeap) {
    m_srvHeap->Release();
    m_srvHeap = nullptr;
  }
  m_srvFreeList.clear();
  m_srvDescriptorNext = 0;
}

void ImGuiOverlay::AllocateSrv(ImGui_ImplDX12_InitInfo *info,
                               D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu,
                               D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu) {
  (void)info;
  if (!m_srvHeap) {
    out_cpu->ptr = 0;
    out_gpu->ptr = 0;
    return;
  }
  UINT index = 0;
  if (!m_srvFreeList.empty()) {
    index = m_srvFreeList.back();
    m_srvFreeList.pop_back();
  } else {
    if (m_srvDescriptorNext >= m_srvDescriptorCount) {
      out_cpu->ptr = 0;
      out_gpu->ptr = 0;
      return;
    }
    index = m_srvDescriptorNext++;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE cpu =
      m_srvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu =
      m_srvHeap->GetGPUDescriptorHandleForHeapStart();
  cpu.ptr += index * m_srvDescriptorSize;
  gpu.ptr += index * m_srvDescriptorSize;
  *out_cpu = cpu;
  *out_gpu = gpu;
  (void)cpu;
  (void)gpu;
}

void ImGuiOverlay::FreeSrv(ImGui_ImplDX12_InitInfo *info,
                           D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                           D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
  (void)info;
  (void)gpu;
  if (!m_srvHeap || cpu.ptr == 0)
    return;
  auto base = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
  if (cpu.ptr < base.ptr)
    return;
  UINT index = static_cast<UINT>((cpu.ptr - base.ptr) / m_srvDescriptorSize);
  if (index < m_srvDescriptorCount) {
    m_srvFreeList.push_back(index);
  }
}
void ImGuiOverlay::RecreateRenderTargets() {
  ReleaseRenderTargets();
  if (!m_swapChain || !m_rtvHeap)
    return;
  if (m_fence && !m_frameFenceValues.empty()) {
    for (UINT i = 0; i < m_backBufferCount; ++i) {
      if (m_frameFenceValues[i] > 0) {
        WaitForFence(m_fence, m_fenceEvent, m_frameFenceValues[i]);
      }
    }
  }
  m_rtvHandles.assign(m_backBufferCount, {});
  m_backBuffers = new ID3D12Resource *[m_backBufferCount]();
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
      m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
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
      if (m_backBuffers[i])
        m_backBuffers[i]->Release();
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

void ImGuiOverlay::SetCameraStatus(bool hasCamera, float jitterX,
                                   float jitterY) {
  m_cachedCamera = hasCamera;
  m_cachedJitterX = jitterX;
  m_cachedJitterY = jitterY;
}

void ImGuiOverlay::ToggleVisibility() {
  m_visible = !m_visible;
  ConfigManager::Get().Data().ui.visible = m_visible;
  ConfigManager::Get().MarkDirty();
  UpdateInputState();
}

void ImGuiOverlay::ToggleFPS() {
  m_showFPS = !m_showFPS;
  ConfigManager::Get().Data().ui.showFPS = m_showFPS;
  ConfigManager::Get().MarkDirty();
}

void ImGuiOverlay::ToggleVignette() {
  m_showVignette = !m_showVignette;
  ConfigManager::Get().Data().ui.showVignette = m_showVignette;
  ConfigManager::Get().MarkDirty();
  m_needRebuildTextures = true;
}

void ImGuiOverlay::UpdateInputState() {
  if (!m_hwnd)
    return;
  ImGuiIO &io = ImGui::GetIO();
  if (m_visible || m_showSetupWizard) {
    io.MouseDrawCursor = true;
    if (!m_cursorUnlocked) {
      GetClipCursor(&m_prevClip);
      ClipCursor(nullptr);
      ShowCursor(TRUE);
      m_cursorUnlocked = true;
    }
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(m_hwnd, &pt);
    io.MousePos = ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
    io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
  } else {
    io.MouseDrawCursor = false;
    if (m_cursorUnlocked) {
      ClipCursor(&m_prevClip);
      ShowCursor(FALSE);
      m_cursorUnlocked = false;
    }
  }
}

void ImGuiOverlay::ToggleDebugMode(bool enabled) { m_showDebug = enabled; }

void ImGuiOverlay::CaptureNextHotkey(int *target) {
  m_pendingHotkeyTarget = target;
}

void ImGuiOverlay::UpdateControls() {
  ModConfig &cfg = ConfigManager::Get().Data();
  m_showFPS = cfg.ui.showFPS;
  m_showVignette = cfg.ui.showVignette;
  m_showDebug = cfg.system.debugMode;
  m_visible = cfg.ui.visible;
  m_showSetupWizard = cfg.system.setupWizardForceShow || !cfg.system.setupWizardCompleted;
  if (m_initialized)
    UpdateInputState();
}

void ImGuiOverlay::ApplyStyle() {
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.ScrollbarRounding = 6.0f;
  ImVec4 *colors = style.Colors;
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
  ModConfig &cfg = ConfigManager::Get().Data();
  StreamlineIntegration &sli = StreamlineIntegration::Get();

  ImGui::SetNextWindowSize(ImVec2(480, 760), ImGuiCond_FirstUseEver);
  ImGui::Begin("DLSS 4.5 Control Panel", &m_visible,
               ImGuiWindowFlags_NoCollapse);
  if (ImGui::Button("Run Setup Wizard")) {
    m_showSetupWizard = true;
    cfg.system.setupWizardForceShow = true;
    ConfigManager::Get().MarkDirty();
  }
  ImGui::Separator();

  if (m_pendingHotkeyTarget) {
    int key = -1;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x1) {
      key = VK_ESCAPE;
    } else {
      for (int scanKey = 0x08; scanKey <= 0xFE; ++scanKey) {
        if (GetAsyncKeyState(scanKey) & 0x1) {
          key = scanKey;
          break;
        }
      }
    }
    if (key != -1) {
      if (key == VK_ESCAPE) {
        m_pendingHotkeyTarget = nullptr;
      } else {
        *m_pendingHotkeyTarget = key;
        m_pendingHotkeyTarget = nullptr;
        ModConfig &data = ConfigManager::Get().Data();
        InputHandler::Get().UpdateHotkey("Toggle Menu", data.ui.menuHotkey);
        InputHandler::Get().UpdateHotkey("Toggle FPS", data.ui.fpsHotkey);
        InputHandler::Get().UpdateHotkey("Toggle Vignette",
                                         data.ui.vignetteHotkey);
        ConfigManager::Get().MarkDirty();
      }
    }
  }

  ImVec4 statusOk(0.12f, 0.82f, 0.25f, 1.0f);
  ImVec4 statusWarn(0.95f, 0.78f, 0.18f, 1.0f);
  ImVec4 statusBad(0.92f, 0.20f, 0.20f, 1.0f);

  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Status");
  bool dlssOk = sli.IsDLSSSupported() && sli.GetDLSSModeIndex() > 0;
  bool dlssWarn = sli.IsDLSSSupported() && sli.GetDLSSModeIndex() == 0;
  ImGui::TextColored(dlssOk ? statusOk : (dlssWarn ? statusWarn : statusBad),
                     "DLSS");
  ImGui::SameLine(120.0f);
  bool fgDisabled = sli.IsFrameGenDisabledDueToInvalidParam();
  bool fgOk = sli.IsFrameGenSupported() && !fgDisabled &&
              sli.GetFrameGenMultiplier() >= 2 &&
              !sli.IsSmartFGTemporarilyDisabled() &&
              sli.GetFrameGenStatus() == sl::DLSSGStatus::eOk;
  bool fgWarn =
      sli.IsFrameGenSupported() && !fgDisabled &&
      (sli.GetFrameGenMultiplier() < 2 || sli.IsSmartFGTemporarilyDisabled() ||
       sli.GetFrameGenStatus() != sl::DLSSGStatus::eOk);
  ImGui::TextColored(fgOk ? statusOk : (fgWarn ? statusWarn : statusBad),
                     "Frame Gen");
  ImGui::SameLine(260.0f);
  bool camOk = sli.HasCameraData();
  ImGui::TextColored(camOk ? statusOk : statusWarn, "Camera");
  ImGui::SameLine(360.0f);
  bool dvcOk = sli.IsDeepDVCSupported() && sli.IsDeepDVCEnabled();
  bool dvcWarn = sli.IsDeepDVCSupported() && !sli.IsDeepDVCEnabled();
  ImGui::TextColored(dvcOk ? statusOk : (dvcWarn ? statusWarn : statusBad),
                     "DeepDVC");
  ImGui::SameLine(340.0f);
  bool hdrOk = sli.IsHDRSupported() && sli.IsHDRActive();
  bool hdrWarn =
      sli.IsHDRSupported() && !sli.IsHDRActive() && sli.IsHDREnabled();
  ImGui::TextColored(hdrOk ? statusOk : (hdrWarn ? statusWarn : statusBad),
                     "HDR");
  ImGui::Separator();

  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "General");
  const char *dlssModes[] = {"Off",         "Max Performance", "Balanced",
                             "Max Quality", "Ultra Quality",   "DLAA"};
  int dlssMode = sli.GetDLSSModeIndex();
  ImGui::BeginDisabled(!sli.IsDLSSSupported());
  if (ImGui::Combo("DLSS Quality Mode", &dlssMode, dlssModes,
                   IM_ARRAYSIZE(dlssModes))) {
    sli.SetDLSSModeIndex(dlssMode);
    cfg.dlss.mode = dlssMode;
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();

  const char *presets[] = {"Default",  "Preset A", "Preset B", "Preset C",
                           "Preset D", "Preset E", "Preset F", "Preset G"};
  int preset = sli.GetDLSSPresetIndex();
  if (ImGui::Combo("DLSS Preset", &preset, presets, IM_ARRAYSIZE(presets))) {
    sli.SetDLSSPreset(preset);
    cfg.dlss.preset = preset;
    ConfigManager::Get().MarkDirty();
  }

  if (ImGui::Button("Quality Preset")) {
    cfg.dlss.mode = 5;
    cfg.dlss.preset = 0;
    cfg.fg.multiplier = 2;
    cfg.dlss.sharpness = 0.2f;
    cfg.dlss.lodBias = -1.0f;
    cfg.rr.enabled = true;
    cfg.rr.enabled = true;
    cfg.dvc.enabled = false;
    sli.SetDLSSModeIndex(cfg.dlss.mode);
    sli.SetDLSSPreset(cfg.dlss.preset);
    sli.SetFrameGenMultiplier(cfg.fg.multiplier);
    sli.SetSharpness(cfg.dlss.sharpness);
    sli.SetLODBias(cfg.dlss.lodBias);
    ApplySamplerLodBias(cfg.dlss.lodBias);
    sli.SetReflexEnabled(cfg.rr.enabled);
    sli.SetRayReconstructionEnabled(cfg.rr.enabled);
    sli.SetDeepDVCEnabled(cfg.dvc.enabled);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::SameLine();
  if (ImGui::Button("Balanced Preset")) {
    cfg.dlss.mode = 2;
    cfg.dlss.preset = 0;
    cfg.fg.multiplier = 3;
    cfg.dlss.sharpness = 0.35f;
    cfg.dlss.lodBias = -1.0f;
    cfg.rr.enabled = true;
    cfg.rr.enabled = true;
    cfg.dvc.enabled = false;
    cfg.dvc.adaptiveEnabled = false;
    sli.SetDLSSModeIndex(cfg.dlss.mode);
    sli.SetDLSSPreset(cfg.dlss.preset);
    sli.SetFrameGenMultiplier(cfg.fg.multiplier);
    sli.SetSharpness(cfg.dlss.sharpness);
    sli.SetLODBias(cfg.dlss.lodBias);
    ApplySamplerLodBias(cfg.dlss.lodBias);
    sli.SetReflexEnabled(cfg.rr.enabled);
    sli.SetRayReconstructionEnabled(cfg.rr.enabled);
    sli.SetDeepDVCEnabled(cfg.dvc.enabled);
    sli.SetDeepDVCAdaptiveEnabled(cfg.dvc.adaptiveEnabled);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::SameLine();
  if (ImGui::Button("Performance Preset")) {
    cfg.dlss.mode = 1;
    cfg.dlss.preset = 0;
    cfg.fg.multiplier = 4;
    cfg.dlss.sharpness = 0.5f;
    cfg.dlss.lodBias = -1.2f;
    cfg.rr.enabled = true;
    cfg.rr.enabled = false;
    cfg.dvc.enabled = false;
    cfg.dvc.adaptiveEnabled = false;
    sli.SetDLSSModeIndex(cfg.dlss.mode);
    sli.SetDLSSPreset(cfg.dlss.preset);
    sli.SetFrameGenMultiplier(cfg.fg.multiplier);
    sli.SetSharpness(cfg.dlss.sharpness);
    sli.SetLODBias(cfg.dlss.lodBias);
    ApplySamplerLodBias(cfg.dlss.lodBias);
    sli.SetReflexEnabled(cfg.rr.enabled);
    sli.SetRayReconstructionEnabled(cfg.rr.enabled);
    sli.SetDeepDVCEnabled(cfg.dvc.enabled);
    sli.SetDeepDVCAdaptiveEnabled(cfg.dvc.adaptiveEnabled);
    ConfigManager::Get().MarkDirty();
  }

  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Ray Reconstruction");
  bool rrEnabled = cfg.rr.enabled;
  ImGui::BeginDisabled(!sli.IsRayReconstructionSupported());
  if (ImGui::Checkbox("Enable DLSS Ray Reconstruction", &rrEnabled)) {
    cfg.rr.enabled = rrEnabled;
    sli.SetRayReconstructionEnabled(rrEnabled);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!sli.IsRayReconstructionSupported() ||
                       !cfg.rr.enabled);
  const char *rrPresets[] = {"Default",  "Preset D", "Preset E", "Preset F",
                             "Preset G", "Preset H", "Preset I", "Preset J",
                             "Preset K", "Preset L", "Preset M", "Preset N",
                             "Preset O"};
  int rrPreset = cfg.rr.preset;
  if (ImGui::Combo("RR Preset", &rrPreset, rrPresets,
                   IM_ARRAYSIZE(rrPresets))) {
    cfg.rr.preset = rrPreset;
    sli.SetRRPreset(rrPreset);
    ConfigManager::Get().MarkDirty();
  }
  float rrStrength = cfg.rr.denoiserStrength;
  if (ImGui::SliderFloat("RR Denoiser Strength", &rrStrength, 0.0f, 1.0f,
                         "%.2f")) {
    cfg.rr.denoiserStrength = rrStrength;
    sli.SetRRDenoiserStrength(rrStrength);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f),
                     "DeepDVC (RTX Dynamic Vibrance)");
  bool deepDvcEnabled = cfg.dvc.enabled;
  ImGui::BeginDisabled(!sli.IsDeepDVCSupported());
  if (ImGui::Checkbox("Enable DeepDVC", &deepDvcEnabled)) {
    cfg.dvc.enabled = deepDvcEnabled;
    sli.SetDeepDVCEnabled(deepDvcEnabled);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!sli.IsDeepDVCSupported() || !cfg.dvc.enabled);
  float deepDvcIntensity = cfg.dvc.intensity;
  if (ImGui::SliderFloat("DeepDVC Intensity", &deepDvcIntensity, 0.0f, 1.0f,
                         "%.2f")) {
    cfg.dvc.intensity = deepDvcIntensity;
    sli.SetDeepDVCIntensity(deepDvcIntensity);
    ConfigManager::Get().MarkDirty();
  }
  float deepDvcSaturation = cfg.dvc.saturation;
  if (ImGui::SliderFloat("DeepDVC Saturation Boost", &deepDvcSaturation, 0.0f,
                         1.0f, "%.2f")) {
    cfg.dvc.saturation = deepDvcSaturation;
    sli.SetDeepDVCSaturation(deepDvcSaturation);
    ConfigManager::Get().MarkDirty();
  }
  bool deepDvcAdaptive = cfg.dvc.adaptiveEnabled;
  if (ImGui::Checkbox("Adaptive Vibrance", &deepDvcAdaptive)) {
    cfg.dvc.adaptiveEnabled = deepDvcAdaptive;
    sli.SetDeepDVCAdaptiveEnabled(deepDvcAdaptive);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::BeginDisabled(!cfg.dvc.adaptiveEnabled);
  float deepDvcAdaptiveStrength = cfg.dvc.adaptiveStrength;
  if (ImGui::SliderFloat("Adaptive Strength", &deepDvcAdaptiveStrength, 0.0f,
                         1.0f, "%.2f")) {
    cfg.dvc.adaptiveStrength = deepDvcAdaptiveStrength;
    sli.SetDeepDVCAdaptiveStrength(deepDvcAdaptiveStrength);
    ConfigManager::Get().MarkDirty();
  }
  float deepDvcAdaptiveMin = cfg.dvc.adaptiveMin;
  if (ImGui::SliderFloat("Adaptive Min", &deepDvcAdaptiveMin, 0.0f, 1.0f,
                         "%.2f")) {
    cfg.dvc.adaptiveMin = deepDvcAdaptiveMin;
    if (cfg.dvc.adaptiveMin > cfg.dvc.adaptiveMax)
      cfg.dvc.adaptiveMax = cfg.dvc.adaptiveMin;
    sli.SetDeepDVCAdaptiveMin(deepDvcAdaptiveMin);
    ConfigManager::Get().MarkDirty();
  }
  float deepDvcAdaptiveMax = cfg.dvc.adaptiveMax;
  if (ImGui::SliderFloat("Adaptive Max", &deepDvcAdaptiveMax, 0.0f, 1.0f,
                         "%.2f")) {
    cfg.dvc.adaptiveMax = deepDvcAdaptiveMax;
    if (cfg.dvc.adaptiveMin > cfg.dvc.adaptiveMax)
      cfg.dvc.adaptiveMin = cfg.dvc.adaptiveMax;
    sli.SetDeepDVCAdaptiveMax(deepDvcAdaptiveMax);
    ConfigManager::Get().MarkDirty();
  }
  float deepDvcAdaptiveSmoothing = cfg.dvc.adaptiveSmoothing;
  if (ImGui::SliderFloat("Adaptive Smoothing", &deepDvcAdaptiveSmoothing, 0.01f,
                         1.0f, "%.2f")) {
    cfg.dvc.adaptiveSmoothing = deepDvcAdaptiveSmoothing;
    sli.SetDeepDVCAdaptiveSmoothing(deepDvcAdaptiveSmoothing);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  const char *fgModes[] = {"Off",         "2x (DLSS-G)", "3x (DLSS-G)",
                           "4x (DLSS-G)", "5x (DLSS-G)", "6x (DLSS-G)",
                           "7x (DLSS-G)", "8x (DLSS-G)"};
  int fgMult = sli.GetFrameGenMultiplier();
  int fgIndex = fgMult >= 2 && fgMult <= 8 ? (fgMult - 1) : 0;
  ImGui::BeginDisabled(!sli.IsFrameGenSupported());
  if (ImGui::Combo("Frame Generation", &fgIndex, fgModes,
                   IM_ARRAYSIZE(fgModes))) {
    int mult = fgIndex > 0 ? (fgIndex + 1) : 0;
    sli.SetFrameGenMultiplier(mult);
    cfg.fg.multiplier = mult;
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f),
                     "Smart Frame Generation");
  bool smartFgEnabled = cfg.fg.smartEnabled;
  if (ImGui::Checkbox("Enable Smart FG", &smartFgEnabled)) {
    cfg.fg.smartEnabled = smartFgEnabled;
    sli.SetSmartFGEnabled(smartFgEnabled);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::BeginDisabled(!cfg.fg.smartEnabled);
  bool smartAuto = cfg.fg.autoDisable;
  if (ImGui::Checkbox("Auto-disable when FPS is high", &smartAuto)) {
    cfg.fg.autoDisable = smartAuto;
    sli.SetSmartFGAutoDisable(smartAuto);
    ConfigManager::Get().MarkDirty();
  }
  float smartThreshold = cfg.fg.autoDisableFps;
  if (ImGui::SliderFloat("Auto-disable FPS Threshold", &smartThreshold, 30.0f,
                         300.0f, "%.0f")) {
    cfg.fg.autoDisableFps = smartThreshold;
    sli.SetSmartFGAutoDisableThreshold(smartThreshold);
    ConfigManager::Get().MarkDirty();
  }
  bool smartScene = cfg.fg.sceneChangeEnabled;
  if (ImGui::Checkbox("Scene-change detection", &smartScene)) {
    cfg.fg.sceneChangeEnabled = smartScene;
    sli.SetSmartFGSceneChangeEnabled(smartScene);
    ConfigManager::Get().MarkDirty();
  }
  float smartSceneThresh = cfg.fg.sceneChangeThreshold;
  if (ImGui::SliderFloat("Scene-change sensitivity", &smartSceneThresh, 0.05f,
                         1.0f, "%.2f")) {
    cfg.fg.sceneChangeThreshold = smartSceneThresh;
    sli.SetSmartFGSceneChangeThreshold(smartSceneThresh);
    ConfigManager::Get().MarkDirty();
  }
  float smartQuality = cfg.fg.interpolationQuality;
  if (ImGui::SliderFloat("FG Interpolation Quality", &smartQuality, 0.0f, 1.0f,
                         "%.2f")) {
    cfg.fg.interpolationQuality = smartQuality;
    sli.SetSmartFGInterpolationQuality(smartQuality);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Quality");
  float sharpness = cfg.dlss.sharpness;
  if (ImGui::SliderFloat("Sharpness", &sharpness, 0.0f, 1.0f, "%.2f")) {
    cfg.dlss.sharpness = sharpness;
    sli.SetSharpness(sharpness);
    ConfigManager::Get().MarkDirty();
  }
  float lodBias = cfg.dlss.lodBias;
  if (ImGui::SliderFloat("Texture Detail (LOD Bias)", &lodBias, -2.0f, 0.0f,
                         "%.2f")) {
    cfg.dlss.lodBias = lodBias;
    sli.SetLODBias(lodBias);
    ApplySamplerLodBias(lodBias);
    ConfigManager::Get().MarkDirty();
  }
  bool mvecAuto = cfg.mvec.autoScale;
  if (ImGui::Checkbox("Auto Motion Vector Scale", &mvecAuto)) {
    cfg.mvec.autoScale = mvecAuto;
    ConfigManager::Get().MarkDirty();
  }
  ImGui::BeginDisabled(cfg.mvec.autoScale);
  float mvecScaleX = cfg.mvec.scaleX;
  if (ImGui::SliderFloat("MV Scale X", &mvecScaleX, 0.5f, 3.0f, "%.2f")) {
    cfg.mvec.scaleX = mvecScaleX;
    sli.SetMVecScale(mvecScaleX, cfg.mvec.scaleY);
    ConfigManager::Get().MarkDirty();
  }
  float mvecScaleY = cfg.mvec.scaleY;
  if (ImGui::SliderFloat("MV Scale Y", &mvecScaleY, 0.5f, 3.0f, "%.2f")) {
    cfg.mvec.scaleY = mvecScaleY;
    sli.SetMVecScale(cfg.mvec.scaleX, mvecScaleY);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "HDR");
  bool hdrEnabled = cfg.hdr.enabled;
  if (ImGui::Checkbox("Enable HDR", &hdrEnabled)) {
    cfg.hdr.enabled = hdrEnabled;
    sli.SetHDREnabled(hdrEnabled);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::BeginDisabled(!cfg.hdr.enabled);
  float peakNits = cfg.hdr.peakNits;
  if (ImGui::SliderFloat("Peak Brightness (nits)", &peakNits, 100.0f, 10000.0f,
                         "%.0f")) {
    cfg.hdr.peakNits = peakNits;
    if (cfg.hdr.paperWhiteNits > cfg.hdr.peakNits)
      cfg.hdr.paperWhiteNits = cfg.hdr.peakNits;
    sli.SetHDRPeakNits(cfg.hdr.peakNits);
    sli.SetHDRPaperWhiteNits(cfg.hdr.paperWhiteNits);
    ConfigManager::Get().MarkDirty();
  }
  float paperWhite = cfg.hdr.paperWhiteNits;
  if (ImGui::SliderFloat("Paper White (nits)", &paperWhite, 50.0f,
                         cfg.hdr.peakNits, "%.0f")) {
    cfg.hdr.paperWhiteNits = paperWhite;
    sli.SetHDRPaperWhiteNits(paperWhite);
    ConfigManager::Get().MarkDirty();
  }
  const char *exposureModes[] = {"Manual", "Auto (Game)"};
  int exposureMode = (cfg.hdr.exposure <= 0.0f) ? 1 : 0;
  if (ImGui::Combo("Exposure Mode", &exposureMode, exposureModes,
                   IM_ARRAYSIZE(exposureModes))) {
    cfg.hdr.exposure =
        (exposureMode == 1) ? 0.0f : (std::max)(cfg.hdr.exposure, 0.1f);
    sli.SetHDRExposure(cfg.hdr.exposure);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::BeginDisabled(cfg.hdr.exposure <= 0.0f);
  float exposure = cfg.hdr.exposure;
  if (ImGui::SliderFloat("Exposure", &exposure, 0.1f, 4.0f, "%.2f")) {
    cfg.hdr.exposure = exposure;
    sli.SetHDRExposure(exposure);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();
  float gamma = cfg.hdr.gamma;
  if (ImGui::SliderFloat("Gamma", &gamma, 1.6f, 2.6f, "%.2f")) {
    cfg.hdr.gamma = gamma;
    sli.SetHDRGamma(gamma);
    ConfigManager::Get().MarkDirty();
  }
  float tonemap = cfg.hdr.tonemapCurve;
  if (ImGui::SliderFloat("Tonemap Curve", &tonemap, -1.0f, 1.0f, "%.2f")) {
    cfg.hdr.tonemapCurve = tonemap;
    sli.SetHDRTonemapCurve(tonemap);
    ConfigManager::Get().MarkDirty();
  }
  float saturation = cfg.hdr.saturation;
  if (ImGui::SliderFloat("Saturation", &saturation, 0.0f, 2.0f, "%.2f")) {
    cfg.hdr.saturation = saturation;
    sli.SetHDRSaturation(saturation);
    ConfigManager::Get().MarkDirty();
  }
  ImGui::EndDisabled();

  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Overlay");
  const std::string fpsToggleLabel = std::format(
      "Show FPS Overlay ({})", InputHandler::Get().GetKeyName(cfg.ui.fpsHotkey));
  if (ImGui::Checkbox(fpsToggleLabel.c_str(), &m_showFPS)) {
    cfg.ui.showFPS = m_showFPS;
    ConfigManager::Get().MarkDirty();
  }
  const std::string vignetteToggleLabel = std::format(
      "Show Vignette ({})", InputHandler::Get().GetKeyName(cfg.ui.vignetteHotkey));
  if (ImGui::Checkbox(vignetteToggleLabel.c_str(), &m_showVignette)) {
    cfg.ui.showVignette = m_showVignette;
    ConfigManager::Get().MarkDirty();
    m_needRebuildTextures = true;
  }
  if (ImGui::SliderFloat("Vignette Intensity", &cfg.ui.vignetteIntensity, 0.0f,
                         1.0f, "%.2f")) {
    ConfigManager::Get().MarkDirty();
    m_needRebuildTextures = true;
  }
  if (ImGui::SliderFloat("Vignette Radius", &cfg.ui.vignetteRadius, 0.2f, 1.0f,
                         "%.2f")) {
    ConfigManager::Get().MarkDirty();
    m_needRebuildTextures = true;
  }
  if (ImGui::SliderFloat("Vignette Softness", &cfg.ui.vignetteSoftness, 0.05f,
                         1.0f, "%.2f")) {
    ConfigManager::Get().MarkDirty();
    m_needRebuildTextures = true;
  }
  ImGui::ColorEdit3("Vignette Color", &cfg.ui.vignetteColorR,
                    ImGuiColorEditFlags_NoInputs);
  if (ImGui::IsItemEdited()) {
    ConfigManager::Get().MarkDirty();
    m_needRebuildTextures = true;
  }

  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Hotkeys");
  ImGui::Text("Press a key to rebind (Esc to cancel).");
  ImGui::SameLine();
  if (ImGui::Button("Cancel") && m_pendingHotkeyTarget) {
    m_pendingHotkeyTarget = nullptr;
  }
  const std::string menuLabel =
      std::format("Menu: {}", InputHandler::Get().GetKeyName(cfg.ui.menuHotkey));
  if (ImGui::Button(menuLabel.c_str())) {
    CaptureNextHotkey(&cfg.ui.menuHotkey);
  }
  const std::string fpsLabelKey =
      std::format("FPS: {}", InputHandler::Get().GetKeyName(cfg.ui.fpsHotkey));
  if (ImGui::Button(fpsLabelKey.c_str())) {
    CaptureNextHotkey(&cfg.ui.fpsHotkey);
  }
  const std::string vignetteLabelKey = std::format(
      "Vignette: {}", InputHandler::Get().GetKeyName(cfg.ui.vignetteHotkey));
  if (ImGui::Button(vignetteLabelKey.c_str())) {
    CaptureNextHotkey(&cfg.ui.vignetteHotkey);
  }

  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Performance");
  float minFps = m_fpsHistory[0];
  float maxFps = m_fpsHistory[0];
  for (int i = 1; i < kFpsHistorySize; ++i) {
    minFps = (std::min)(minFps, m_fpsHistory[i]);
    maxFps = (std::max)(maxFps, m_fpsHistory[i]);
  }
  float graphMax = maxFps > 1.0f ? maxFps * 1.15f : 60.0f;
  const std::string fpsLabel =
      std::format("FPS Graph (min {:.0f} / max {:.0f})", minFps, maxFps);
  ImGui::PlotLines("##fpsgraph", m_fpsHistory, kFpsHistorySize,
                   m_fpsHistoryIndex, fpsLabel.c_str(), 0.0f, graphMax,
                   ImVec2(0.0f, 80.0f));

  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.83f, 0.69f, 0.25f, 1.0f), "Performance Metrics");
  if (g_metricsCache.gpuOk.load(std::memory_order_relaxed)) {
    ImGui::TextUnformatted(
        std::format("GPU Utilization: {}%",
                    g_metricsCache.gpuPercent.load(std::memory_order_relaxed))
            .c_str());
  } else {
    ImGui::Text("GPU Utilization: N/A");
  }
  if (g_metricsCache.vramOk.load(std::memory_order_relaxed)) {
    uint32_t used = g_metricsCache.vramUsed.load(std::memory_order_relaxed);
    uint32_t budget = g_metricsCache.vramBudget.load(std::memory_order_relaxed);
    uint32_t usedShown = used > budget ? budget : used;
    ImGui::TextUnformatted(
        std::format("VRAM: {} / {} MB", usedShown, budget).c_str());
  } else {
    ImGui::Text("VRAM: N/A");
  }
  const float fgActual = sli.GetFgActualMultiplier();
  if (fgActual > 1.01f) {
    ImGui::TextUnformatted(std::format("FG Actual: {:.2f}x", fgActual).c_str());
  } else {
    ImGui::Text("FG Actual: Off");
  }

  if (ImGui::Button("Reset to Defaults")) {
    ConfigManager::Get().ResetToDefaults();
    ConfigManager::Get().Load();
    ModConfig &reset = ConfigManager::Get().Data();
    sli.SetDLSSModeIndex(reset.dlss.mode);
    sli.SetDLSSPreset(reset.dlss.preset);
    sli.SetFrameGenMultiplier(reset.fg.multiplier);
    sli.SetSharpness(reset.dlss.sharpness);
    sli.SetLODBias(reset.dlss.lodBias);
    ApplySamplerLodBias(reset.dlss.lodBias);
    sli.SetReflexEnabled(reset.rr.enabled);
    sli.SetHUDFixEnabled(reset.system.hudFixEnabled);
    sli.SetRayReconstructionEnabled(reset.rr.enabled);
    sli.SetRRPreset(reset.rr.preset);
    sli.SetRRDenoiserStrength(reset.rr.denoiserStrength);
    sli.SetDeepDVCEnabled(reset.dvc.enabled);
    sli.SetDeepDVCIntensity(reset.dvc.intensity);
    sli.SetDeepDVCSaturation(reset.dvc.saturation);
    sli.SetDeepDVCAdaptiveEnabled(reset.dvc.adaptiveEnabled);
    sli.SetDeepDVCAdaptiveStrength(reset.dvc.adaptiveStrength);
    sli.SetDeepDVCAdaptiveMin(reset.dvc.adaptiveMin);
    sli.SetDeepDVCAdaptiveMax(reset.dvc.adaptiveMax);
    sli.SetDeepDVCAdaptiveSmoothing(reset.dvc.adaptiveSmoothing);
    sli.SetSmartFGEnabled(reset.fg.smartEnabled);
    sli.SetSmartFGAutoDisable(reset.fg.autoDisable);
    sli.SetSmartFGAutoDisableThreshold(reset.fg.autoDisableFps);
    sli.SetSmartFGSceneChangeEnabled(reset.fg.sceneChangeEnabled);
    sli.SetSmartFGSceneChangeThreshold(reset.fg.sceneChangeThreshold);
    sli.SetSmartFGInterpolationQuality(reset.fg.interpolationQuality);
    sli.SetHDREnabled(reset.hdr.enabled);
    sli.SetHDRPeakNits(reset.hdr.peakNits);
    sli.SetHDRPaperWhiteNits(reset.hdr.paperWhiteNits);
    sli.SetHDRExposure(reset.hdr.exposure);
    sli.SetHDRGamma(reset.hdr.gamma);
    sli.SetHDRTonemapCurve(reset.hdr.tonemapCurve);
    sli.SetHDRSaturation(reset.hdr.saturation);
    sli.SetMVecScale(reset.mvec.scaleX, reset.mvec.scaleY);
    m_showFPS = reset.ui.showFPS;
    m_showVignette = reset.ui.showVignette;
    m_needRebuildTextures = true;
  }

  ImGui::Separator();
  ImGui::TextUnformatted(
      std::format("Hotkeys: Menu {} | FPS {} | Vignette {}",
                  InputHandler::Get().GetKeyName(cfg.ui.menuHotkey),
                  InputHandler::Get().GetKeyName(cfg.ui.fpsHotkey),
                  InputHandler::Get().GetKeyName(cfg.ui.vignetteHotkey))
          .c_str());
  ImGui::TextUnformatted(
      std::format("Camera: {} (J {:.3f}, {:.3f}) Delta {:.3f}",
                  m_cachedCamera ? "OK" : "Missing", m_cachedJitterX,
                  m_cachedJitterY,
                  StreamlineIntegration::Get().GetLastCameraDelta())
          .c_str());

  ImGui::End();
}

void ImGuiOverlay::BuildSetupWizard() {
  if (!m_showSetupWizard)
    return;
  UpdateInputState();
  ModConfig &cfg = ConfigManager::Get().Data();
  StreamlineIntegration &sli = StreamlineIntegration::Get();

  ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
  ImGui::Begin("First-Time Setup Wizard", &m_showSetupWizard,
               ImGuiWindowFlags_NoCollapse);
  ImGui::Text("Welcome! We'll recommend settings based on your GPU.");
  ImGui::Separator();
  if (g_nvapiMetrics.gpuName[0] != '\0') {
    ImGui::TextUnformatted(
        std::format("Detected GPU: {}", g_nvapiMetrics.gpuName).c_str());
  } else if (g_nvapiMetrics.dxgiName[0] != '\0') {
    ImGui::TextUnformatted(
        std::format("Detected GPU: {}", g_nvapiMetrics.dxgiName).c_str());
  } else {
    ImGui::Text("Detected GPU: Unknown (NVAPI not available)");
  }

  const char *gpuName = g_nvapiMetrics.gpuName[0] != '\0'
                            ? g_nvapiMetrics.gpuName
                            : g_nvapiMetrics.dxgiName;
  bool isHighEnd = gpuName && (strstr(gpuName, "RTX 40") != nullptr ||
                               strstr(gpuName, "RTX 50") != nullptr ||
                               strstr(gpuName, "RTX 60") != nullptr ||
                               strstr(gpuName, "Titan") != nullptr);
  ImGui::Spacing();
  if (ImGui::Button("Apply Recommended Settings")) {
    LOG_INFO("[Wizard] Apply recommended settings clicked");
    if (isHighEnd) {
      cfg.dlss.mode = 3;
      cfg.dlss.preset = 0;
      cfg.fg.multiplier = 4;
      cfg.dlss.sharpness = 0.35f;
      cfg.dlss.lodBias = -1.0f;
      cfg.rr.enabled = true;
      cfg.rr.enabled = true;
      cfg.dvc.enabled = false;
      cfg.dvc.adaptiveEnabled = false;
    } else {
      cfg.dlss.mode = 3;
      cfg.dlss.preset = 0;
      cfg.fg.multiplier = 0;
      cfg.dlss.sharpness = 0.35f;
      cfg.dlss.lodBias = -1.0f;
      cfg.rr.enabled = true;
      cfg.rr.enabled = false;
      cfg.dvc.enabled = false;
      cfg.dvc.adaptiveEnabled = false;
    }
    ConfigManager::Get().MarkDirty();
    sli.SetDLSSModeIndex(cfg.dlss.mode);
    sli.SetDLSSPreset(cfg.dlss.preset);
    sli.SetFrameGenMultiplier(cfg.fg.multiplier);
    sli.SetSharpness(cfg.dlss.sharpness);
    sli.SetLODBias(cfg.dlss.lodBias);
    ApplySamplerLodBias(cfg.dlss.lodBias);
    sli.SetReflexEnabled(cfg.rr.enabled);
    sli.SetRayReconstructionEnabled(cfg.rr.enabled);
    sli.SetDeepDVCEnabled(cfg.dvc.enabled);
    sli.SetDeepDVCAdaptiveEnabled(cfg.dvc.adaptiveEnabled);
    LOG_INFO("[Wizard] Applied: DLSS={} Preset={} FG={}x RR={}", cfg.dlss.mode,
             cfg.dlss.preset, cfg.fg.multiplier,
             cfg.rr.enabled ? 1 : 0);
    cfg.system.setupWizardCompleted = true;
    cfg.system.setupWizardForceShow = false;
    ConfigManager::Get().MarkDirty();
    m_showSetupWizard = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("Skip for Now")) {
    cfg.system.setupWizardCompleted = true;
    cfg.system.setupWizardForceShow = false;
    ConfigManager::Get().MarkDirty();
    m_showSetupWizard = false;
  }
  ImGui::End();
}

void ImGuiOverlay::BuildFPSOverlay() {
  if (!m_showFPS)
    return;
  ImDrawList *drawList = ImGui::GetForegroundDrawList();
  int mult = StreamlineIntegration::Get().GetFrameGenMultiplier();
  if (mult < 1)
    mult = 1;
  ImVec2 pos(24.0f, 24.0f);
  drawList->AddText(nullptr, 28.0f, pos, IM_COL32(212, 175, 55, 220),
                    std::format("{:.0f} -> {:.0f} FPS",
                                m_cachedTotalFPS / static_cast<float>(mult),
                                m_cachedTotalFPS)
                        .c_str());
}

void ImGuiOverlay::BuildVignette() {
  if (!m_showVignette)
    return;
  BuildTexturesIfNeeded();
  if (!m_vignetteTexture || m_vignetteSrvGpu.ptr == 0)
    return;
  ImDrawList *drawList = ImGui::GetForegroundDrawList();
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ModConfig &cfg = ConfigManager::Get().Data();
  const float intensity = std::clamp(cfg.ui.vignetteIntensity, 0.0f, 1.0f);
  const float radius = std::clamp(cfg.ui.vignetteRadius, 0.2f, 1.0f);
  const float softness = std::clamp(cfg.ui.vignetteSoftness, 0.05f, 1.0f);
  const ImU32 color = IM_COL32(
      (int)(cfg.ui.vignetteColorR * 255.0f), (int)(cfg.ui.vignetteColorG * 255.0f),
      (int)(cfg.ui.vignetteColorB * 255.0f), (int)(intensity * 200.0f));
  const float padX = size.x * 0.5f * (std::max)(0.0f, 1.0f - radius);
  const float padY = size.y * 0.5f * (std::max)(0.0f, 1.0f - radius);
  const float softX = padX + (size.x * 0.5f - padX) * softness;
  const float softY = padY + (size.y * 0.5f - padY) * softness;
  const ImVec2 uv0(padX / size.x, padY / size.y);
  const ImVec2 uv1((size.x - padX) / size.x, (size.y - padY) / size.y);
  const ImVec2 uvSoft0(softX / size.x, softY / size.y);
  const ImVec2 uvSoft1((size.x - softX) / size.x, (size.y - softY) / size.y);
  ImTextureID texId = (ImTextureID)m_vignetteSrvGpu.ptr;
  drawList->AddImage(texId, ImVec2(0, 0), ImVec2(size.x, size.y), uv0, uv1,
                     color);
  if (softness > 0.0f) {
    ImU32 softCol = IM_COL32(
        (int)(cfg.ui.vignetteColorR * 255.0f), (int)(cfg.ui.vignetteColorG * 255.0f),
        (int)(cfg.ui.vignetteColorB * 255.0f), (int)(intensity * 140.0f));
    drawList->AddImage(texId, ImVec2(0, 0), ImVec2(size.x, size.y), uvSoft0,
                       uvSoft1, softCol);
  }
}

void ImGuiOverlay::BuildDebugWindow() {
  if (!m_showDebug)
    return;
  ImGui::Begin("Resource Debug", &m_showDebug);
  std::string debugInfo = ResourceDetector::Get().GetDebugInfo();
  if (debugInfo.empty())
    debugInfo = "No debug info available yet...";
  ImGui::TextUnformatted(debugInfo.c_str());
  ImGui::End();
}

void ImGuiOverlay::BuildTexturesIfNeeded() {
  if (!m_needRebuildTextures)
    return;
  m_needRebuildTextures = false;
  if (!m_device || !m_queue || !m_srvHeap)
    return;
  if (m_shuttingDown.load(std::memory_order_relaxed))
    return;
  if (m_fence && m_fenceValue > 0) {
    UINT64 completed = m_fence->GetCompletedValue();
    std::erase_if(m_pendingUploads, [completed](const PendingUpload &pending) {
      return completed >= pending.fenceValue;
    });
  }

  if (m_vignetteSrv.ptr == 0 || m_vignetteSrvGpu.ptr == 0) {
    AllocateSrv(nullptr, &m_vignetteSrv, &m_vignetteSrvGpu);
    if (m_vignetteSrv.ptr == 0 || m_vignetteSrvGpu.ptr == 0)
      return;
  }

  ModConfig &cfg = ConfigManager::Get().Data();
  const float radius = std::clamp(cfg.ui.vignetteRadius, 0.2f, 1.0f);
  const float softness = std::clamp(cfg.ui.vignetteSoftness, 0.05f, 1.0f);
  const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
  if (displaySize.x <= 0.0f || displaySize.y <= 0.0f) {
    m_needRebuildTextures = true;
    return;
  }
  const float displayW = (std::max)(1.0f, displaySize.x);
  const float displayH = (std::max)(1.0f, displaySize.y);
  const float aspect = displayW / displayH;
  const int texSize = 256;
  const float inner = radius;
  const float outer =
      std::clamp(radius + (1.0f - radius) * softness, radius + 0.001f, 1.0f);
  const size_t texSizeU = static_cast<size_t>(texSize);
  if (texSizeU > 4096 ||
      texSizeU > ((std::numeric_limits<size_t>::max)() / (texSizeU * 4))) {
    return;
  }
  std::vector<uint8_t> pixels(texSizeU * texSizeU * 4);
  std::vector<float> xLookup(static_cast<size_t>(texSize));
  std::vector<float> yLookup(static_cast<size_t>(texSize));
  for (int x = 0; x < texSize; ++x) {
    const float nx = ((x + 0.5f) / texSize) * 2.0f - 1.0f;
    const float dx = nx * aspect;
    xLookup[static_cast<size_t>(x)] = dx * dx;
  }
  for (int y = 0; y < texSize; ++y) {
    const float ny = ((y + 0.5f) / texSize) * 2.0f - 1.0f;
    yLookup[static_cast<size_t>(y)] = ny * ny;
  }
  for (int y = 0; y < texSize; ++y) {
    for (int x = 0; x < texSize; ++x) {
      float dist = sqrtf(xLookup[static_cast<size_t>(x)] +
                         yLookup[static_cast<size_t>(y)]);
      dist = (std::min)(dist, 1.0f);
      float t = (dist - inner) / (outer - inner);
      t = std::clamp(t, 0.0f, 1.0f);
      const float smooth = t * t * (3.0f - 2.0f * t);
      const uint8_t alpha = static_cast<uint8_t>(smooth * 255.0f);
      const size_t idx = static_cast<size_t>((y * texSize + x) * 4);
      pixels[idx + 0] = 255;
      pixels[idx + 1] = 255;
      pixels[idx + 2] = 255;
      pixels[idx + 3] = alpha;
    }
  }

  if (!m_vignetteTexture) {
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = texSize;
    texDesc.Height = texSize;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES texProps{};
    texProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(m_device->CreateCommittedResource(
            &texProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&m_vignetteTexture)))) {
      return;
    }
  }

  const UINT rowPitch =
      (texSize * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
      ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
  const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * texSize;
  D3D12_HEAP_PROPERTIES uploadProps{};
  uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC uploadDesc{};
  uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  uploadDesc.Width = uploadSize;
  uploadDesc.Height = 1;
  uploadDesc.DepthOrArraySize = 1;
  uploadDesc.MipLevels = 1;
  uploadDesc.SampleDesc.Count = 1;
  uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
  if (FAILED(m_device->CreateCommittedResource(
          &uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&uploadBuffer)))) {
    return;
  }
  uint8_t *mapped = nullptr;
  D3D12_RANGE range{0, 0};
  if (FAILED(
          uploadBuffer->Map(0, &range, reinterpret_cast<void **>(&mapped))) ||
      !mapped) {
    return;
  }
  for (int y = 0; y < texSize; ++y) {
    memcpy(mapped + y * rowPitch, pixels.data() + (y * texSize * 4),
           texSize * 4);
  }
  uploadBuffer->Unmap(0, nullptr);

  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd;
  if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
      FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&cmd)))) {
    return;
  }

  D3D12_TEXTURE_COPY_LOCATION src{};
  src.pResource = uploadBuffer.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  src.PlacedFootprint.Footprint.Width = texSize;
  src.PlacedFootprint.Footprint.Height = texSize;
  src.PlacedFootprint.Footprint.Depth = 1;
  src.PlacedFootprint.Footprint.RowPitch = rowPitch;

  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = m_vignetteTexture;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = m_vignetteTexture;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = m_vignetteState;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  if (m_vignetteState != D3D12_RESOURCE_STATE_COPY_DEST) {
    cmd->ResourceBarrier(1, &barrier);
  }
  cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  cmd->ResourceBarrier(1, &barrier);
  cmd->Close();
  ID3D12CommandList *lists[] = {cmd.Get()};
  m_queue->ExecuteCommandLists(1, lists);
  m_queue->Signal(m_fence, m_fenceValue);
  m_pendingUploads.push_back({uploadBuffer, m_fenceValue});
  m_fenceValue++;
  m_vignetteState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Texture2D.MipLevels = 1;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  m_device->CreateShaderResourceView(m_vignetteTexture, &srvDesc,
                                     m_vignetteSrv);
}

void ImGuiOverlay::Render() {
  if (!m_initialized || !m_swapChain || m_commandLists.empty() ||
      m_commandAllocators.empty()) {
    ConfigManager::Get().SaveIfDirty();
    return;
  }
  if (!m_visible && !m_showFPS && !m_showVignette && !m_showDebug &&
      !m_showSetupWizard) {
    ConfigManager::Get().SaveIfDirty();
    return;
  }

  ImGui_ImplDX12_NewFrame();
  ImGui_ImplWin32_NewFrame();

  const uint64_t metricNow = GetTimeMs();
  if (metricNow - g_metricsCache.lastUpdateMs.load(std::memory_order_relaxed) >=
      500) {
    g_metricsCache.lastUpdateMs.store(metricNow, std::memory_order_relaxed);
  }
  UpdateInputState();

  ImGui::NewFrame();

  BuildTexturesIfNeeded();

  if (m_visible)
    BuildMainMenu();
  BuildSetupWizard();
  BuildFPSOverlay();
  BuildVignette();
  BuildDebugWindow();

  ImGui::Render();
  ConfigManager::Get().SaveIfDirty();

  UINT backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
  if (!m_backBuffers || backBufferIndex >= m_backBufferCount)
    return;
  if (m_fence && m_frameFenceValues[backBufferIndex] > 0) {
    WaitForFence(m_fence, m_fenceEvent, m_frameFenceValues[backBufferIndex]);
  }
  ID3D12CommandAllocator *allocator = m_commandAllocators[backBufferIndex];
  ID3D12GraphicsCommandList *cmd = m_commandLists[backBufferIndex];
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
  ID3D12DescriptorHeap *heaps[] = {m_srvHeap};
  cmd->SetDescriptorHeaps(1, heaps);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);

  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  cmd->ResourceBarrier(1, &barrier);

  cmd->Close();
  ID3D12CommandList *lists[] = {cmd};
  m_queue->ExecuteCommandLists(1, lists);
  m_fenceValue++;
  m_queue->Signal(m_fence, m_fenceValue);
  m_frameFenceValues[backBufferIndex] = m_fenceValue;
}
