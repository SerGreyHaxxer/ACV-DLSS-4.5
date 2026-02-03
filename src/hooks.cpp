#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <psapi.h>
#include <wchar.h>
#include "hooks.h"
#include "dlss4_config.h"
#include "iat_utils.h"
#include "logger.h"
#include "dlss4_config.h"
#include "streamline_integration.h"
#include <mutex>
#include <atomic>
#include <stdio.h>
#include "input_handler.h"
#include "imgui_overlay.h"
#include "vtable_utils.h"
#include "d3d12_wrappers.h"
#include "pattern_scanner.h"

#pragma comment(lib, "psapi.lib")

extern "C" void LogStartup(const char* msg);

// ============================================================================
// GLOBAL STATE
// ============================================================================

SwapChainHookState g_SwapChainState;
PFN_Present g_OriginalPresent = nullptr;
PFN_Present1 g_OriginalPresent1 = nullptr;
PFN_ResizeBuffers g_OriginalResizeBuffers = nullptr;

static std::mutex g_HookMutex;
static std::atomic<bool> g_HooksInitialized(false);
static std::atomic<bool> g_PatternScanDone(false);
static std::atomic<uint64_t> g_FrameCount(0);
static std::atomic<uintptr_t> g_JitterAddress(0);
static std::atomic<bool> g_JitterValid(false);
static std::atomic<bool> g_WrappedCommandListUsed(false);
static std::atomic<bool> g_VtableHooksDisabledLogged(false);
static const bool kDisableVtableHooks = true;

void NotifyWrappedCommandListUsed() {
    g_WrappedCommandListUsed.store(true);
}

bool IsWrappedCommandListUsed() {
    return g_WrappedCommandListUsed.load();
}

void SetPatternJitterAddress(uintptr_t address) {
    g_JitterAddress.store(address);
    g_JitterValid.store(address != 0);
}

bool TryGetPatternJitter(float& jitterX, float& jitterY) {
    uintptr_t addr = g_JitterAddress.load();
    if (!addr) return false;
    
    // Safely check if memory is readable
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;

    const float* vals = reinterpret_cast<const float*>(addr);
    float jx = 0.0f;
    float jy = 0.0f;
    __try {
        jx = vals[0];
        jy = vals[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    // Validation: Must be finite number
    // _finite is MSVC specific, or std::isfinite
    if (!_finite(jx) || !_finite(jy)) return false;

    jitterX = jx;
    jitterY = jy;
    return true;
}

// ============================================================================
// VMT HOOKING UTILITY
// ============================================================================

bool HookVirtualMethod(void* pObject, int index, void* pHook, void** ppOriginal) {
    if (!pObject || !pHook || !ppOriginal || index < 0) return false;
    void** vtable = nullptr;
    void** entry = nullptr;
    if (!ResolveVTableEntry(pObject, index, &vtable, &entry)) return false;
    *ppOriginal = *entry;
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    *entry = pHook;
    VirtualProtect(entry, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

bool HookVirtualMethodOnce(void* pObject, int index, void* pHook, void** ppOriginal) {
    if (!pObject || !pHook || !ppOriginal || index < 0) return false;
    void** vtable = nullptr;
    void** entry = nullptr;
    if (!ResolveVTableEntry(pObject, index, &vtable, &entry)) return false;
    if (*entry == pHook) return true;
    if (*ppOriginal == nullptr) *ppOriginal = *entry;
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    *entry = pHook;
    VirtualProtect(entry, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

#include "resource_detector.h"
#include "d3d12_wrappers.h"

// ============================================================================
// D3D12 DEVICE HOOK
// ============================================================================

void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);
HRESULT STDMETHODCALLTYPE Hooked_CreatePlacedResource(ID3D12Device* pThis, ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource);
HRESULT STDMETHODCALLTYPE Hooked_CreateCommittedResource(ID3D12Device* pThis, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource);
void STDMETHODCALLTYPE Hooked_CreateConstantBufferView(ID3D12Device* pThis, const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
HRESULT STDMETHODCALLTYPE Hooked_Close(ID3D12GraphicsCommandList* pThis);
void STDMETHODCALLTYPE HookedResourceBarrier(ID3D12GraphicsCommandList* pThis, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers);
void STDMETHODCALLTYPE Hooked_SetGraphicsRootConstantBufferView(ID3D12GraphicsCommandList* pThis, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation);
void STDMETHODCALLTYPE Hooked_SetComputeRootConstantBufferView(ID3D12GraphicsCommandList* pThis, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation);

typedef void(STDMETHODCALLTYPE* PFN_ResourceBarrier)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePlacedResource)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCommittedResource)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef void(STDMETHODCALLTYPE* PFN_CreateConstantBufferView)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Close)(ID3D12GraphicsCommandList*);
typedef void(STDMETHODCALLTYPE* PFN_SetGraphicsRootConstantBufferView)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
typedef void(STDMETHODCALLTYPE* PFN_SetComputeRootConstantBufferView)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);

PFN_ExecuteCommandLists g_OriginalExecuteCommandLists = nullptr;
PFN_ResourceBarrier g_OriginalResourceBarrier = nullptr;
PFN_CreatePlacedResource g_OriginalCreatePlacedResource = nullptr;
PFN_CreateCommittedResource g_OriginalCreateCommittedResource = nullptr;
PFN_CreateConstantBufferView g_OriginalCreateConstantBufferView = nullptr;
PFN_Close g_OriginalClose = nullptr;
PFN_SetGraphicsRootConstantBufferView g_OriginalSetGraphicsRootCbv = nullptr;
PFN_SetComputeRootConstantBufferView g_OriginalSetComputeRootCbv = nullptr;

PFN_D3D12CreateDevice g_OriginalD3D12CreateDevice = nullptr;
static thread_local bool s_inD3D12CreateDevice = false;

void EnsureD3D12VTableHooks(ID3D12Device* pDevice) {
    if (!g_VtableHooksDisabledLogged.exchange(true)) {
        LogStartup("[HOOK] Vtable hooks disabled (wrapper mode)");
    }
    (void)pDevice;
}

HRESULT WINAPI Hooked_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void** ppDevice) {
    LogStartup("[HOOK] D3D12CreateDevice entry");
    if (s_inD3D12CreateDevice) return g_OriginalD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    s_inD3D12CreateDevice = true;
    
    if (!g_OriginalD3D12CreateDevice) { s_inD3D12CreateDevice = false; return E_FAIL; }

    HRESULT hr = g_OriginalD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    if (FAILED(hr)) {
        LogStartup("[HOOK] D3D12CreateDevice failed");
    }

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        LogStartup("[HOOK] D3D12CreateDevice success, initializing Streamline");
        ID3D12Device* pRealDevice = nullptr;
        if (SUCCEEDED(((IUnknown*)*ppDevice)->QueryInterface(__uuidof(ID3D12Device), (void**)&pRealDevice))) {
            StreamlineIntegration::Get().Initialize(pRealDevice);
            EnsureD3D12VTableHooks(pRealDevice);
            WrappedID3D12Device* wrapped = new WrappedID3D12Device(pRealDevice);
            if (riid == __uuidof(ID3D12Device) || riid == __uuidof(IUnknown)) {
                if (*ppDevice) {
                    ((IUnknown*)*ppDevice)->Release();
                }
                *ppDevice = wrapped;
            } else {
                void* outObj = nullptr;
                if (SUCCEEDED(wrapped->QueryInterface(riid, &outObj))) {
                    if (*ppDevice) {
                        ((IUnknown*)*ppDevice)->Release();
                    }
                    *ppDevice = outObj;
                }
                wrapped->Release();
            }
            pRealDevice->Release();
        } else {
            LogStartup("[HOOK] QueryInterface(ID3D12Device) failed");
        }
        LogStartup("[HOOK] D3D12CreateDevice exit");
    }
    
    s_inD3D12CreateDevice = false;
    return hr;
}

// ============================================================================
// COMMAND QUEUE HOOK
// ============================================================================

void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    // Lazy Init for Streamline if we missed the D3D12CreateDevice hook
    if (!StreamlineIntegration::Get().IsInitialized()) {
        ID3D12Device* pDevice = nullptr;
        if (SUCCEEDED(pThis->GetDevice(__uuidof(ID3D12Device), (void**)&pDevice))) {
            LOG_INFO("Lazy initializing Streamline via ExecuteCommandLists...");
            StreamlineIntegration::Get().Initialize(pDevice);
            pDevice->Release();
        }
    }

    ResourceDetector::Get().NewFrame();
    StreamlineIntegration::Get().SetCommandQueue(pThis);

    static bool s_camHookBannerLogged = false;
    if (!s_camHookBannerLogged) {
        LOG_INFO("[CAM] Camera scan active (Close hook)");
        s_camHookBannerLogged = true;
    }

    ID3D12Resource* pColor = ResourceDetector::Get().GetBestColorCandidate();
    ID3D12Resource* pDepth = ResourceDetector::Get().GetBestDepthCandidate();
    ID3D12Resource* pMVs = ResourceDetector::Get().GetBestMotionVectorCandidate();

    if (pColor) StreamlineIntegration::Get().TagColorBuffer(pColor);
    if (pDepth) StreamlineIntegration::Get().TagDepthBuffer(pDepth);
    if (pMVs) StreamlineIntegration::Get().TagMotionVectors(pMVs);

    g_OriginalExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
}

// ============================================================================
// SNIFFER HOOKS (GLOBAL VTABLE PATCHING)
// ============================================================================

void STDMETHODCALLTYPE HookedResourceBarrier(ID3D12GraphicsCommandList* pThis, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
    if (pBarriers) {
        static uint64_t s_lastScanFrame = 0;
        uint64_t currentFrame = ResourceDetector::Get().GetFrameCount();
        if (currentFrame != s_lastScanFrame) {
            s_lastScanFrame = currentFrame;
            UINT scanned = 0;
            for (UINT i = 0; i < NumBarriers && scanned < RESOURCE_BARRIER_SCAN_MAX; i++) {
                if (pBarriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                    ResourceDetector::Get().RegisterResource(pBarriers[i].Transition.pResource, true);
                    scanned++;
                }
            }
        }
    }
    g_OriginalResourceBarrier(pThis, NumBarriers, pBarriers);
}

HRESULT STDMETHODCALLTYPE Hooked_CreatePlacedResource(ID3D12Device* pThis, ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    HRESULT hr = g_OriginalCreatePlacedResource(pThis, pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        ID3D12Resource* pRes = (ID3D12Resource*)*ppvResource;
        ResourceDetector::Get().RegisterResource(pRes);
        
        // Check for CBV
        if (pDesc && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && pHeap) {
            D3D12_HEAP_DESC hDesc = pHeap->GetDesc();
            if (hDesc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
                uint8_t* mapped = nullptr; D3D12_RANGE range = { 0, 0 };
                if (SUCCEEDED(pRes->Map(0, &range, reinterpret_cast<void**>(&mapped))) && mapped) {
                    RegisterCbv(pRes, pDesc->Width, mapped);
                }
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_CreateCommittedResource(ID3D12Device* pThis, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    HRESULT hr = g_OriginalCreateCommittedResource(pThis, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riid, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        ID3D12Resource* pRes = (ID3D12Resource*)*ppvResource;
        ResourceDetector::Get().RegisterResource(pRes);

        // Check for CBV
        if (pDesc && pHeapProperties && pHeapProperties->Type == D3D12_HEAP_TYPE_UPLOAD && pDesc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            uint8_t* mapped = nullptr; D3D12_RANGE range = { 0, 0 };
            if (SUCCEEDED(pRes->Map(0, &range, reinterpret_cast<void**>(&mapped))) && mapped) {
                RegisterCbv(pRes, pDesc->Width, mapped);
            }
        }
    }
    return hr;
}

void STDMETHODCALLTYPE Hooked_CreateConstantBufferView(ID3D12Device* pThis, const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!g_OriginalCreateConstantBufferView) return;
    g_OriginalCreateConstantBufferView(pThis, pDesc, DestDescriptor);
    if (pDesc && pDesc->BufferLocation) {
        TrackCbvDescriptor(DestDescriptor, pDesc);
    }
}


HRESULT STDMETHODCALLTYPE Hooked_Close(ID3D12GraphicsCommandList* pThis) {
    if (!IsWrappedCommandListUsed()) {
        if (!StreamlineIntegration::Get().IsInitialized()) {
            return g_OriginalClose(pThis);
        }
        static uint64_t s_hbLast = 0;
        static uint64_t s_hbCloseCount = 0;
        float jitterX = 0.0f, jitterY = 0.0f;
        if (!TryGetPatternJitter(jitterX, jitterY)) {
            jitterX = 0.0f;
            jitterY = 0.0f;
        }

        s_hbCloseCount++;
        uint64_t hbNow = GetTickCount64();
        if (hbNow - s_hbLast >= 2000) {
            LOG_DEBUG("[HB] Hooked_Close tick (calls=%llu)", (unsigned long long)s_hbCloseCount);
            s_hbLast = hbNow;
        }
        
        static uint64_t s_lastScanFrame = 0;
        uint64_t currentFrame = ResourceDetector::Get().GetFrameCount();
        
        // Scan only once per frame (Present) to avoid CPU kill
        if (currentFrame > s_lastScanFrame) {
            float view[16], proj[16], score = 0.0f;
            static int s_camLog = 0;
            bool doLog = (++s_camLog % CAMERA_SCAN_LOG_INTERVAL == 0);
            
            uint64_t lastFound = GetLastCameraFoundFrame();
            bool stale = lastFound == 0 || (currentFrame > lastFound + CAMERA_SCAN_STALE_FRAMES);
            bool forceFull = stale || (currentFrame % CAMERA_SCAN_FORCE_FULL_FRAMES == 0);
            bool allowFull = forceFull || (currentFrame > s_lastScanFrame + CAMERA_SCAN_MIN_INTERVAL_FRAMES);
        if (doLog) {
            float lastScore = 0.0f;
            uint64_t lastFrame = 0;
            uint64_t lastFound = GetLastCameraFoundFrame();
            const bool hasStats = GetLastCameraStats(lastScore, lastFrame);
            uint64_t cbvCount = 0, descCount = 0, rootCount = 0;
            GetCameraScanCounts(cbvCount, descCount, rootCount);
            LOG_INFO("[CAM] Scan start (frame %llu): LastFound=%llu LastScore=%.2f LastFrame=%llu CBVs=%llu DescCBVs=%llu RootCBVs=%llu",
                currentFrame, lastFound, hasStats ? lastScore : 0.0f, hasStats ? lastFrame : 0,
                (unsigned long long)cbvCount, (unsigned long long)descCount, (unsigned long long)rootCount);
        }
            bool found = TryScanAllCbvsForCamera(view, proj, &score, doLog, allowFull);
            if (!found) {
                found = TryScanDescriptorCbvsForCamera(view, proj, &score, doLog);
            }
            if (!found) {
                found = TryScanRootCbvsForCamera(view, proj, &score, doLog);
            }
        if (found) {
            UpdateCameraCache(view, proj, jitterX, jitterY);
            StreamlineIntegration::Get().SetCameraData(view, proj, jitterX, jitterY);
            if (doLog) {
                LOG_INFO("[CAM] Camera scan found candidate (score %.2f)", score);
            }
        } else {
            StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitterX, jitterY);
            if (doLog) {
                LOG_WARN("[CAM] Camera scan failed (frame %llu, score %.2f)", currentFrame, score);
            }
        }
            s_lastScanFrame = currentFrame;
        } else {
            StreamlineIntegration::Get().SetCameraData(nullptr, nullptr, jitterX, jitterY);
        }
        
        StreamlineIntegration::Get().EvaluateDLSS(pThis);
    }
    return g_OriginalClose(pThis);
}

void STDMETHODCALLTYPE Hooked_SetGraphicsRootConstantBufferView(ID3D12GraphicsCommandList* pThis, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    TrackRootCbvAddress(BufferLocation);
    if (g_OriginalSetGraphicsRootCbv) g_OriginalSetGraphicsRootCbv(pThis, RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Hooked_SetComputeRootConstantBufferView(ID3D12GraphicsCommandList* pThis, UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) {
    TrackRootCbvAddress(BufferLocation);
    if (g_OriginalSetComputeRootCbv) g_OriginalSetComputeRootCbv(pThis, RootParameterIndex, BufferLocation);
}

// ============================================================================
// HOOK INIT
// ============================================================================

void HookFactoryIfNeeded(void* pFactory) {
    if (!pFactory) return;
    LogStartup("[HOOK] HookFactoryIfNeeded entry");
    
    // Pattern Scan for Jitter
    bool expected = false;
    if (g_PatternScanDone.compare_exchange_strong(expected, true)) {
        LogStartup("[HOOK] Pattern scan start");
        auto jitterOffset = PatternScanner::Scan("ACValhalla.exe", "F3 0F 10 ?? ?? ?? ?? ?? 0F 28 ?? F3 0F 11 ?? ?? ?? ?? ??");
        if (jitterOffset) {
            uint8_t* ins = (uint8_t*)*jitterOffset;
            SetPatternJitterAddress((uintptr_t)(ins + 8 + *(int32_t*)(ins + 4)));
            LogStartup("[HOOK] Pattern scan found jitter");
        } else {
            LogStartup("[HOOK] Pattern scan no match");
        }
    } else {
        LogStartup("[HOOK] Pattern scan already done");
    }
    LogStartup("[HOOK] HookFactoryIfNeeded exit");
}

static std::atomic<bool> g_descriptorHooksInitialized(false);
void InitDescriptorHooks() {
    if (g_descriptorHooksInitialized.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_HookMutex);
    if (g_descriptorHooksInitialized.load(std::memory_order_relaxed)) return;
    g_descriptorHooksInitialized.store(true, std::memory_order_release);
}

// ... Rest of GetProcAddress / LoadLibrary hooks (UNCHANGED) ...
typedef FARPROC(WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);
PFN_GetProcAddress g_OriginalGetProcAddress = nullptr;
FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    if (HIWORD(lpProcName) != 0 && strcmp(lpProcName, "D3D12CreateDevice") == 0) {
        if (!g_OriginalD3D12CreateDevice) g_OriginalD3D12CreateDevice = (PFN_D3D12CreateDevice)g_OriginalGetProcAddress(hModule, lpProcName);
        return (FARPROC)Hooked_D3D12CreateDevice;
    }
    return g_OriginalGetProcAddress(hModule, lpProcName);
}
void InstallGetProcAddressHook() { HookAllModulesIAT("kernel32.dll", "GetProcAddress", (void*)Hooked_GetProcAddress, (void**)&g_OriginalGetProcAddress); }
// ... (Typedefs) ...
typedef HMODULE(WINAPI* PFN_LoadLibraryW)(LPCWSTR);
PFN_LoadLibraryW g_OriginalLoadLibraryW = nullptr;

typedef HMODULE(WINAPI* PFN_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
PFN_LoadLibraryExW g_OriginalLoadLibraryExW = nullptr;

static thread_local bool s_inLoadLibraryHook = false;

void CheckAndHookD3D12(HMODULE hModule, LPCWSTR name) {
    if (!hModule || !name) return;
    const wchar_t* lastSlash = wcsrchr(name, L'\\');
    if (lastSlash) name = lastSlash + 1;
    if (_wcsicmp(name, L"d3d12.dll") == 0 && g_OriginalD3D12CreateDevice == nullptr) {
        LOG_INFO("d3d12.dll loaded via LoadLibrary, resolving D3D12CreateDevice...");
        g_OriginalD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hModule, "D3D12CreateDevice");
    }
}

HMODULE WINAPI Hooked_LoadLibraryW(LPCWSTR lpLibFileName) {
    if (s_inLoadLibraryHook) return g_OriginalLoadLibraryW(lpLibFileName);
    s_inLoadLibraryHook = true;
    HMODULE result = g_OriginalLoadLibraryW(lpLibFileName);
    if (result) CheckAndHookD3D12(result, lpLibFileName);
    s_inLoadLibraryHook = false;
    return result;
}

HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (s_inLoadLibraryHook) return g_OriginalLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    s_inLoadLibraryHook = true;
    HMODULE result = g_OriginalLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (result) CheckAndHookD3D12(result, lpLibFileName);
    s_inLoadLibraryHook = false;
    return result;
}

void InstallLoadLibraryHook() {
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32) {
        if (!g_OriginalLoadLibraryW) g_OriginalLoadLibraryW = (PFN_LoadLibraryW)GetProcAddress(hKernel32, "LoadLibraryW");
        if (!g_OriginalLoadLibraryExW) g_OriginalLoadLibraryExW = (PFN_LoadLibraryExW)GetProcAddress(hKernel32, "LoadLibraryExW");
    }
    
    if (g_OriginalLoadLibraryW) HookAllModulesIAT("kernel32.dll", "LoadLibraryW", (void*)Hooked_LoadLibraryW, nullptr);
    if (g_OriginalLoadLibraryExW) HookAllModulesIAT("kernel32.dll", "LoadLibraryExW", (void*)Hooked_LoadLibraryExW, nullptr);
}

static std::atomic<bool> s_hooksInstalled(false);
void InstallD3D12Hooks() {
    if (s_hooksInstalled.exchange(true)) return;
    
    LOG_INFO("Installing D3D12 IAT Hooks...");
    InstallLoadLibraryHook();
    
    HMODULE hD3D12Already = GetModuleHandleW(L"d3d12.dll");
    if (hD3D12Already && g_OriginalD3D12CreateDevice == nullptr) {
        LOG_INFO("d3d12.dll already in memory, resolving D3D12CreateDevice...");
        g_OriginalD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12Already, "D3D12CreateDevice");
    }
    
    HMODULE hMods[1024];
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded;
    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        int count = cbNeeded / sizeof(HMODULE);
        LOG_INFO("Scanning %d modules for IAT hooks...", count);
        for (int i = 0; i < count; i++) {
            char modName[MAX_PATH];
            if (GetModuleBaseNameA(hProcess, hMods[i], modName, sizeof(modName))) {
                if (_stricmp(modName, "dxgi.dll") == 0) continue; // Don't hook ourselves (or real dxgi if we are proxying it?)
                // Actually we are dxgi.dll proxy, so we might appear as dxgi.dll.
                // We should avoid hooking ourself to avoid recursion if we call D3D12CreateDevice.
                
                // HookIAT returns true if it patched something
                if (HookIAT(hMods[i], "d3d12.dll", "D3D12CreateDevice", (void*)Hooked_D3D12CreateDevice, (void**)&g_OriginalD3D12CreateDevice)) {
                    LOG_INFO("Hooked D3D12CreateDevice in module: %s", modName);
                }
            }
        }
    } else {
        LOG_ERROR("EnumProcessModules failed!");
    }
    InstallGetProcAddressHook();
}
bool InitializeHooks() { return g_HooksInitialized; }
void CleanupHooks() {
    g_HooksInitialized = false;
    InputHandler::Get().UninstallHook();
    InputHandler::Get().ClearHotkeys();
    ImGuiOverlay::Get().Shutdown();
    StreamlineIntegration::Get().Shutdown();
}
