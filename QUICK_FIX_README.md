# Quick Fix for Crash on Startup (0xC0000005)

## What's Happening

Your crash is caused by the DLSS proxy attempting to hook the game's D3D12 device creation, but failing when it tries to modify protected memory in the Import Address Table (IAT). This is a common issue with aggressive DLL proxies.

## Immediate Solutions (No Recompile Needed)

### Solution 1: Disable All Overlays (HIGHEST SUCCESS RATE)

**These are KNOWN to cause crashes with DLSS proxy mods:**

1. **Ubisoft Connect Overlay** ← Most likely culprit!
   - Open Ubisoft Connect
   - Settings → General
   - **UNCHECK** "Enable in-game overlay for supported games"
   - Restart Ubisoft Connect

2. **Discord Overlay**
   - Discord Settings → Activity Settings → Activity Status
   - Turn OFF "Enable in-game overlay"

3. **MSI Afterburner / RivaTuner Statistics Server**
   - Close completely (not just minimize)
   - Check system tray

4. **GeForce Experience / NVIDIA Overlay**
   - Open GeForce Experience → Settings → General
   - Turn OFF "In-Game Overlay"

5. **Steam Overlay** (if using Steam version)
   - Right-click AC Valhalla in Steam Library
   - Properties → General
   - **UNCHECK** "Enable Steam Overlay while in-game"

6. **Other recording software**
   - OBS Studio, Fraps, XSplit, etc.
   - Close them completely

### Solution 2: Update Graphics Driver

Make sure you have **NVIDIA Driver 566.03 or newer** (latest Game Ready)

Download from: https://www.nvidia.com/Download/index.aspx

### Solution 3: Run as Administrator

Right-click `ACValhalla.exe` → Properties → Compatibility → **Check** "Run this program as administrator"

## If Still Crashing: Apply Code Fixes

### Step 1: Backup Original File

```powershell
Copy-Item "src\iat_utils.cpp" "src\iat_utils.cpp.BACKUP"
```

### Step 2: Replace with Fixed Version

```powershell
# In the ACV-DLSS-4.5 folder
Copy-Item "src\iat_utils_FIXED.cpp" "src\iat_utils.cpp" -Force
```

### Step 3: Rebuild

```powershell
.\build.bat
```

### Step 4: Reinstall

```powershell
.\INSTALL.bat
```

## Diagnostic Logs

After attempting to launch (even if it crashes), check these files in your game folder:

1. **startup_trace.log** - Shows early initialization
2. **dlss4_proxy.log** - Main runtime log
3. **dlss4_crash.log** - Crash details (if crash handler triggered)

**Upload these to the GitHub issue if you need more help!**

## Understanding the Fix

The original `iat_utils.cpp` doesn't properly validate memory before writing. The fixed version:

1. ✅ Validates module information before hooking
2. ✅ Checks memory is readable/writable before patching
3. ✅ Uses exception handlers to catch access violations
4. ✅ Skips protected system modules (ntdll.dll, kernelbase.dll)
5. ✅ Provides detailed logging for diagnosis
6. ✅ Has sanity limits to prevent infinite loops
7. ✅ Gracefully fails instead of crashing

## Technical Details

**Your crash:**
- Exception: `0xC0000005` (ACCESS_VIOLATION)
- Type: **Write** attempt
- Address: `0x00007FF80F14AE00`
- Module: **Unknown**

This means the hook code tried to write to an address that:
- Doesn't belong to any loaded module, OR
- Is in a protected memory region (probably NVIDIA driver or Ubisoft DRM code)

The fix prevents writing to invalid/protected addresses by:
1. Using `VirtualQuery()` to check memory state before writing
2. Using `VirtualProtect()` with proper error handling
3. Wrapping writes in `__try/__except` blocks
4. Skipping modules that are known to be problematic

## Still Need Help?

Post in the GitHub issue with:
1. Your GPU model
2. Driver version (`nvidia-smi` or GeForce Experience)
3. Windows version
4. Contents of the three log files mentioned above
5. What overlays you disabled

---

**Note:** If the fixed version still crashes, there may be deeper compatibility issues with your system's memory protection or anti-cheat software. In that case, this mod may not be compatible with your setup until NVIDIA releases official DLSS 4 support for AC Valhalla.
