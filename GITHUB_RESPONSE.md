# Investigation Results & Proposed Fixes

## Root Cause Analysis

After analyzing the code, I've identified the crash cause:

**Problem:** Access Violation (0xC0000005) at address `0x00007FF80F14AE00` during IAT (Import Address Table) hooking in `src/iat_utils.cpp`.

**Why it crashes:**
1. The hook code attempts to modify the IAT to intercept `D3D12CreateDevice`
2. It doesn't validate that the memory is writable before attempting the write
3. The target address likely belongs to:
   - NVIDIA driver code (protected memory)
   - Ubisoft's DRM/anti-tamper system
   - Another overlay that loaded first

## Immediate Workarounds (No Code Changes)

### 🔥 Most Likely Fix: Disable Ubisoft Connect Overlay

The Ubisoft overlay is notorious for conflicting with DXGI proxy mods:

1. Open **Ubisoft Connect**
2. **Settings** → **General**
3. **UNCHECK** "Enable in-game overlay for supported games"
4. Restart Ubisoft Connect completely
5. Try launching AC Valhalla again

### Also Disable These:
- Discord overlay
- MSI Afterburner / RivaTuner
- GeForce Experience overlay
- Steam overlay (if applicable)
- Any screen recording software

## Code Fixes Required

I've created a comprehensive fix for `src/iat_utils.cpp` that adds:

### Critical Changes:

1. **Memory validation before writes**
   ```cpp
   MEMORY_BASIC_INFORMATION mbi;
   if (VirtualQuery(&pThunk->u1.Function, &mbi, sizeof(mbi)) == 0 || 
       mbi.State != MEM_COMMIT) {
       LOG_ERROR("Thunk memory not accessible");
       continue; // Skip instead of crash
   }
   ```

2. **Exception handling around writes**
   ```cpp
   __try {
       pThunk->u1.Function = (ULONG_PTR)newFunction;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
       LOG_ERROR("Exception writing thunk");
       continue; // Skip instead of crash
   }
   ```

3. **Skip protected system modules**
   ```cpp
   if (_stricmp(modName, "ntdll.dll") == 0 ||
       _stricmp(modName, "kernelbase.dll") == 0) {
       LOG_DEBUG("Skipping protected system module");
       continue;
   }
   ```

4. **Sanity limits to prevent infinite loops**
   ```cpp
   const int MAX_ENTRIES = 1000;
   const int MAX_THUNKS = 500;
   ```

5. **Detailed logging for diagnosis**

### Files Modified:

**`src/iat_utils.cpp`** (see [iat_utils_FIXED.cpp](./src/iat_utils_FIXED.cpp) in the repo)

**Key improvements:**
- ✅ Validates module PE headers before hooking
- ✅ Uses `VirtualQuery()` to check memory state
- ✅ Wraps writes in exception handlers
- ✅ Gracefully skips problematic modules
- ✅ Provides detailed error logging
- ✅ Prevents infinite loops with sanity limits

## Additional Defensive Fixes

### `src/hooks.cpp` - InstallD3D12Hooks()

Add exception handling wrapper:

```cpp
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
```

### `src/proxy.cpp` - InitializeProxy()

Wrap hook installation:

```cpp
__try {
    InstallD3D12Hooks();
} __except (EXCEPTION_EXECUTE_HANDLER) {
    LOG_ERROR("EXCEPTION during InstallD3D12Hooks: 0x%X", GetExceptionCode());
    LOG_WARN("Continuing without full IAT hooks");
}
```

## Testing Needed

After applying fixes:

1. Build with `build.bat`
2. Run `INSTALL.bat`
3. Launch AC Valhalla
4. Check for these files in game folder:
   - `startup_trace.log`
   - `dlss4_proxy.log`
   - `dlss4_crash.log` (if still crashing)

Post the contents of these logs to help further diagnose.

## System Info Needed

To help diagnose further:

- GPU model: ?
- Driver version: ?
- Windows version: ?
- What overlays are running: ?
- Are you running any anti-cheat or DRM software: ?

## Why This Fix is Better

**Original code:**
- Blindly writes to IAT without validation
- No error handling
- Crashes if memory is protected

**Fixed code:**
- Validates every memory access
- Uses exception handlers
- Skips problematic modules
- Logs detailed error info
- Gracefully degrades instead of crashing

---

## Files to Download

I've created these files in the repo:

1. **[FIXES.md](./FIXES.md)** - Detailed technical explanation
2. **[src/iat_utils_FIXED.cpp](./src/iat_utils_FIXED.cpp)** - Fixed IAT hooking code
3. **[QUICK_FIX_README.md](./QUICK_FIX_README.md)** - User-friendly guide

## Next Steps

**For users experiencing crashes:**
1. Try disabling overlays first (especially Ubisoft Connect)
2. If still crashing, wait for the code fix to be merged
3. Post your log files to help diagnose

**For the developer:**
1. Review the proposed fixes
2. Test on multiple systems
3. Consider adding a "safe mode" that skips aggressive IAT hooks
4. Add runtime detection for problematic overlays

---

*Hope this helps! Let me know if you need more details on any part of the fix.*
