# Proposed Fixes for Crash on Startup (0xC0000005)

## Issue Summary
Access violation (write) at 0x00007FF80F14AE00 in "Unknown" module during startup.
Most likely cause: IAT hooking attempting to write to invalid/protected memory.

## Fix 1: Add Proper Error Handling to IAT Hooking

**File: `src/iat_utils.cpp`** (needs modification)

Add validation and error handling:

```cpp
bool HookIAT(HMODULE hModule, const char* dllName, const char* funcName, 
             void* pHook, void** ppOriginal) {
    if (!hModule || !dllName || !funcName || !pHook) {
        LOG_ERROR("[IAT] Invalid parameters for IAT hook");
        return false;
    }

    // Verify module is valid
    MODULEINFO mi;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &mi, sizeof(mi))) {
        LOG_ERROR("[IAT] GetModuleInformation failed for module 0x%p", hModule);
        return false;
    }

    // Get DOS header
    IMAGE_DOS_HEADER* pDosHeader = (IMAGE_DOS_HEADER*)hModule;
    if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        LOG_ERROR("[IAT] Invalid DOS signature for module 0x%p", hModule);
        return false;
    }

    // ... rest of IAT walking code ...

    // CRITICAL: Add memory protection check before writing
    DWORD oldProtect = 0;
    MEMORY_BASIC_INFORMATION mbi;
    
    // Verify memory is accessible
    if (VirtualQuery((LPCVOID)pFuncAddr, &mbi, sizeof(mbi)) == 0) {
        LOG_ERROR("[IAT] VirtualQuery failed for address 0x%p", pFuncAddr);
        return false;
    }
    
    // Check if memory is committed
    if (mbi.State != MEM_COMMIT) {
        LOG_ERROR("[IAT] Memory at 0x%p is not committed (state: 0x%X)", 
                  pFuncAddr, mbi.State);
        return false;
    }
    
    // Try to change protection
    if (!VirtualProtect(pFuncAddr, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DWORD err = GetLastError();
        LOG_ERROR("[IAT] VirtualProtect failed for 0x%p (error: %d)", pFuncAddr, err);
        return false;
    }

    // Save original and patch
    if (ppOriginal && *ppOriginal == nullptr) {
        *ppOriginal = *pFuncAddr;
    }
    
    *pFuncAddr = pHook;
    
    // Restore protection
    DWORD dummy;
    VirtualProtect(pFuncAddr, sizeof(void*), oldProtect, &dummy);
    
    return true;
}
```

## Fix 2: Delay D3D12 Hooking Until Safe

**File: `src/hooks.cpp`**

Modify `InstallD3D12Hooks()` to be more defensive:

```cpp
void InstallD3D12Hooks() {
    if (s_hooksInstalled.exchange(true)) return;
    
    LOG_INFO("Installing D3D12 IAT Hooks...");
    
    // CRITICAL FIX: Check if we're being loaded too early
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        LOG_ERROR("kernel32.dll not available - aborting hook install");
        return;
    }
    
    InstallLoadLibraryHook();
    
    HMODULE hD3D12Already = GetModuleHandleW(L"d3d12.dll");
    if (hD3D12Already && g_OriginalD3D12CreateDevice == nullptr) {
        LOG_INFO("d3d12.dll already in memory, resolving D3D12CreateDevice...");
        g_OriginalD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12Already, "D3D12CreateDevice");
    }
    
    HMODULE hMods[1024];
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded;
    
    if (!EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        LOG_ERROR("EnumProcessModules failed (error: %d) - retrying later", GetLastError());
        // Don't abort - LoadLibrary hooks will catch it later
        s_hooksInstalled.store(false); // Allow retry
        return;
    }
    
    int count = cbNeeded / sizeof(HMODULE);
    LOG_INFO("Scanning %d modules for IAT hooks...", count);
    
    int hooksInstalled = 0;
    int hooksFailed = 0;
    
    for (int i = 0; i < count; i++) {
        char modName[MAX_PATH];
        if (GetModuleBaseNameA(hProcess, hMods[i], modName, sizeof(modName))) {
            // Skip ourselves
            if (_stricmp(modName, "dxgi.dll") == 0) continue;
            
            // CRITICAL FIX: Skip system modules that are likely to be protected
            if (_stricmp(modName, "kernel32.dll") == 0 ||
                _stricmp(modName, "ntdll.dll") == 0 ||
                _stricmp(modName, "kernelbase.dll") == 0) {
                continue;
            }
            
            // Try to hook, but don't fail if it doesn't work
            __try {
                if (HookIAT(hMods[i], "d3d12.dll", "D3D12CreateDevice", 
                           (void*)Hooked_D3D12CreateDevice, 
                           (void**)&g_OriginalD3D12CreateDevice)) {
                    LOG_INFO("Hooked D3D12CreateDevice in module: %s", modName);
                    hooksInstalled++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                LOG_ERROR("Exception while hooking module: %s (code: 0x%X)", 
                         modName, GetExceptionCode());
                hooksFailed++;
            }
        }
    }
    
    LOG_INFO("IAT Hook results: %d installed, %d failed", hooksInstalled, hooksFailed);
    InstallGetProcAddressHook();
}
```

## Fix 3: Safer InitializeProxy() with Deferred Hooking

**File: `src/proxy.cpp`**

Add a fallback mechanism:

```cpp
bool InitializeProxy() {
    LogStartup("InitializeProxy Entry");
    if (s_CSState.load() != 2) {
        LogStartup("CRITICAL: Proxy Global CS not initialized!");
        return false;
    }

    EnterCriticalSection(&s_InitCS);
    LogStartup("InitializeProxy Lock Acquired");
    
    // ... existing environment variable setup ...
    
    if (g_ProxyState.initialized) {
        LeaveCriticalSection(&s_InitCS);
        LogStartup("InitializeProxy Already Init");
        return true;
    }

    // ... existing DXGI loading code ...
    
    LOG_INFO("Original DXGI loaded successfully.");
    
    // CRITICAL FIX: Wrap hook installation in exception handler
    __try {
        InstallD3D12Hooks();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD exCode = GetExceptionCode();
        LOG_ERROR("EXCEPTION during InstallD3D12Hooks: 0x%X at 0x%p", 
                  exCode, GetExceptionInformation()->ExceptionRecord->ExceptionAddress);
        
        // Don't fail initialization - we can still work with LoadLibrary hooks
        LOG_WARN("Continuing without full IAT hooks - relying on LoadLibrary interception");
    }
    
    g_ProxyState.initialized = true;
    LogStartup("InitializeProxy Success");
    LeaveCriticalSection(&s_InitCS);
    return true;
}
```

## Fix 4: Add Detailed Crash Logging

**File: `src/crash_handler.cpp`**

Already pretty good, but add IAT-specific diagnostics:

```cpp
// Add at end of crash log generation
fprintf(fp, "\nIAT Hook State:\n");
fprintf(fp, "Hooks Installed: %d\n", (int)s_hooksInstalled.load());
fprintf(fp, "Pattern Scan Done: %d\n", (int)g_PatternScanDone.load());
fprintf(fp, "Original D3D12CreateDevice: 0x%p\n", g_OriginalD3D12CreateDevice);
fprintf(fp, "Proxy Initialized: %d\n", (int)g_ProxyState.initialized);
```

## Fix 5: User Workaround (Immediate)

**Create a batch file to test without hooks:**

```batch
@echo off
echo Testing DLSS 4 Proxy with minimal hooks...
set DLSS_SAFE_MODE=1
start "" "C:\Path\To\ACValhalla.exe"
```

Then in `InstallD3D12Hooks()`, add at the top:

```cpp
// Check for safe mode
char safeModeEnv[32];
if (GetEnvironmentVariableA("DLSS_SAFE_MODE", safeModeEnv, sizeof(safeModeEnv)) > 0) {
    LOG_WARN("DLSS_SAFE_MODE enabled - skipping aggressive IAT hooks");
    s_hooksInstalled.store(true); // Prevent retry
    return;
}
```

## Testing Steps

1. Apply Fix 1 and Fix 2 first (safest)
2. Rebuild with `build.bat`
3. Test installation
4. Check `dlss4_proxy.log` and `startup_trace.log` for detailed error messages
5. If still crashing, apply Fix 3 (exception handling)
6. If desperate, use Fix 5 (safe mode)

## Additional Debug Info Needed

To further diagnose, we need:
1. Full contents of `dlss4_crash.log`
2. Full contents of `startup_trace.log`
3. Contents of `dlss4_proxy.log` (if any)
4. System info (Windows version, GPU, driver version)
5. What overlays/tools are running (Discord, MSI Afterburner, etc.)
