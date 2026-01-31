# SDK Path Fix Summary

## Problem
Your NVIDIA Streamline SDK was located in your Downloads folder, but the build system was looking for it in:
- `external/streamline/lib/` (old location)

## Solution
Updated both build systems to point to your actual SDK location:
```
C:\Users\serge\Downloads\streamline-sdk-v2.10.3
```

## Changes Made

### 1. Moved Stub Headers (Critical!)
The stub headers in the project root were conflicting with the real SDK.
- **Moved to:** `backup_stub_headers/`
- This ensures the compiler uses the REAL NVIDIA headers

### 2. Updated `CMakeLists.txt`
- Added SDK path detection with helpful error message
- Updated include path to SDK include folder
- Updated library link path to SDK lib folder

### 3. Updated `build.bat`
- Added SDK detection at start of build
- Added include path: `/I"%USERPROFILE%\Downloads\streamline-sdk-v2.10.3\include"`
- Added library link: `"%USERPROFILE%\Downloads\streamline-sdk-v2.10.3\lib\x64\sl.interposer.lib"`

### 4. Created `deploy_mod.ps1`
- PowerShell script to deploy all required DLLs to game folder
- Usage: ` .\deploy_mod.ps1 -GamePath "C:\Path\To\Game"`

## How to Build

### Option 1: Using CMake (Recommended)
```batch
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### Option 2: Using build.bat
```batch
# Run from Visual Studio Developer Command Prompt
build.bat
```

## Required DLLs for Deployment
After building, copy these files to your AC Valhalla game folder:
```
bin\dxgi.dll                    (Your proxy)
bin\sl.interposer.dll           (Streamline core)
bin\sl.common.dll              (Streamline common)
bin\sl.dlss.dll                (DLSS Super Resolution)
bin\sl.dlss_g.dll              (DLSS Frame Generation)
bin\sl.reflex.dll              (NVIDIA Reflex)
bin\nvngx_dlss.dll             (NGX DLSS backend)
bin\nvngx_dlssg.dll            (NGX Frame Gen backend)
```

Or use the deployment script:
```powershell
.\deploy_mod.ps1
```

## Controls (In-Game)
- **F5** - Open DLSS Control Panel
- **F6** - Toggle FPS Counter
- **F7** - Toggle Vignette
- **F8** - Debug Camera Status

## Troubleshooting

### "NVIDIA Streamline SDK not found!"
- Download the SDK from: https://developer.nvidia.com/rtx/streamline
- Extract to: `C:\Users\serge\Downloads\streamline-sdk-v2.10.3`
- Or update the path in `CMakeLists.txt` and `build.bat`

### Build errors about missing headers
- Make sure the stub headers were moved to `backup_stub_headers/`
- Verify the SDK include folder exists: `%USERPROFILE%\Downloads\streamline-sdk-v2.10.3\include`

### DLSS not working in game
1. Check `dlss4_proxy.log` for errors
2. Verify all 8 DLLs are in the game folder
3. Make sure you have an RTX GPU (20 series or newer)
4. Update GPU drivers to 560.94 or newer
