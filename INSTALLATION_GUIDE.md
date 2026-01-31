# 📖 Complete Installation Guide

## What Was Created

This package now includes a complete **one-click installation system**. Here's what each file does:

### 🎯 Main Installation Files

| File | Description | When to Use |
|------|-------------|-------------|
| **`INSTALL.bat`** | Double-click installer | **EASIEST - Use this!** |
| **`BuildAndDeploy.ps1`** | PowerShell build & deploy | When you want full control |
| **`QUICKSTART.md`** | Quick reference | Cheat sheet |

### 🔧 Build System Files

| File | Description |
|------|-------------|
| `CMakeLists.txt` | CMake configuration (updated for your SDK path) |
| `build.bat` | Batch file build (updated for your SDK path) |
| `build.ps1` | PowerShell build script |

### 📦 Deployment Files

| File | Description |
|------|-------------|
| `deploy_mod.ps1` | Copy files to game folder only |
| `install.ps1` | Alternative install script |
| `install_mod.bat` | Simple file copy batch |

---

## Step-by-Step Installation

### Method 1: One-Click Install (Easiest) ⭐

1. **Download NVIDIA Streamline SDK**
   - Visit: https://developer.nvidia.com/rtx/streamline
   - Download version 2.10.3 or newer
   - Extract the ZIP to your Downloads folder
   - Should be at: `C:\Users\[YourName]\Downloads\streamline-sdk-v2.10.3`

2. **Run the Installer**
   - Double-click **`INSTALL.bat`**
   - If Windows asks about "Windows protected your PC", click "More info" → "Run anyway"
   - The script will:
     - ✅ Verify the SDK is present
     - 🔍 Find your AC Valhalla installation (Steam or Ubisoft)
     - 🔨 Build the mod
     - 📦 Copy all 8 required files
     - 📝 Show you instructions

3. **Play!**
   - Launch AC Valhalla
   - Press **F5** in-game to open the menu

### Method 2: PowerShell (More Control)

```powershell
# Run with auto-detection
.\BuildAndDeploy.ps1

# Or specify game location
.\BuildAndDeploy.ps1 -CustomGamePath "D:\Games\AC Valhalla"

# Create desktop shortcut too
.\BuildAndDeploy.ps1 -CreateShortcut
```

### Method 3: Manual Build (Advanced)

#### Using CMake (Recommended for developers)

```batch
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

#### Using build.bat

```batch
# Must run from "Developer Command Prompt for VS 2022"
build.bat
```

Then manually copy files from `bin\` to your game folder.

---

## What Gets Installed

The following files are copied to your AC Valhalla folder:

```
📁 AC Valhalla Folder
├── ACValhalla.exe          (original game)
├── dxgi.dll                ← YOUR MOD (proxy)
├── sl.interposer.dll       ← Streamline core
├── sl.common.dll           ← Streamline common
├── sl.dlss.dll             ← DLSS Super Resolution
├── sl.dlss_g.dll           ← DLSS Frame Generation
├── sl.reflex.dll           ← NVIDIA Reflex
├── nvngx_dlss.dll          ← NGX backend
├── nvngx_dlssg.dll         ← NGX Frame Gen backend
└── dlss_settings.ini       ← Your settings (created after first run)
```

---

## In-Game Controls

| Key | What It Does |
|-----|--------------|
| **F5** | Open DLSS Control Panel (set quality, frame gen, etc.) |
| **F6** | Toggle FPS counter (shows real vs displayed FPS) |
| **F7** | Toggle orange vignette (visual confirmation mod is working) |
| **F8** | Debug info (shows camera status - useful for troubleshooting) |

---

## Recommended Settings for Maximum FPS

1. **Press F5** to open the menu
2. Set **Frame Generation** to:
   - `4x` if you have RTX 50 series (5090, 5080, etc.)
   - `3x` if you have RTX 40 series (4090, 4080, etc.)
   - `2x` if you have RTX 20/30 series
3. Set **DLSS Mode** to `Performance`
4. **Close menu** and open game settings
5. Go to **Screen** → **Resolution Scale**
6. Set to **50%**
7. Enjoy 200+ FPS! 🎉

---

## Troubleshooting

### "Streamline SDK not found"

**Problem:** The script can't find the NVIDIA SDK

**Solution:**
1. Download from https://developer.nvidia.com/rtx/streamline
2. Extract to Downloads folder
3. The folder should be named `streamline-sdk-v2.10.3` (or similar)

### "ACValhalla.exe not found"

**Problem:** Auto-detection failed

**Solution:**
```powershell
.\BuildAndDeploy.ps1 -CustomGamePath "C:\Your\Actual\Game\Path"
```

### "Build failed"

**Problem:** Missing compiler

**Solution:**
1. Install Visual Studio 2022 Community (free)
2. During install, select "Desktop development with C++"
3. Or install just "Build Tools for Visual Studio 2022"

### Game crashes on startup

**Problem:** Conflicting overlays

**Solution:**
1. Disable Ubisoft Connect overlay
2. Disable Discord overlay
3. Disable RivaTuner / MSI Afterburner
4. Disable GeForce Experience overlay

### DLSS menu doesn't appear (F5 does nothing)

**Problem:** Mod not loading

**Solution:**
1. Check that all 8 DLL files are in the game folder
2. Run game in **Borderless Windowed** mode
3. Check `dlss4_proxy.log` in game folder for errors

### Low FPS / No improvement

**Problem:** Settings not optimal

**Solution:**
1. Make sure Resolution Scale in game is 50% (not 100%)
2. Check F5 menu shows "Frame Gen: 4x" (or your desired setting)
3. Verify you have an RTX GPU (GTX cards don't support DLSS)

---

## Uninstallation

To remove the mod, delete these files from your AC Valhalla folder:

```
dxgi.dll
sl.interposer.dll
sl.common.dll
sl.dlss.dll
sl.dlss_g.dll
sl.reflex.dll
nvngx_dlss.dll
nvngx_dlssg.dll
dlss_settings.ini (optional)
dlss4_proxy.log (optional)
```

---

## Support

If you still have issues:
1. Check `dlss4_proxy.log` in your game folder
2. Check `build_log.txt` if build failed
3. Create an issue on GitHub with these logs

---

## Summary

**Easiest way to install:**
1. Download Streamline SDK → Extract to Downloads
2. Double-click `INSTALL.bat`
3. Launch game, press F5
4. Set Frame Gen to 4x, DLSS to Performance
5. Set game Resolution Scale to 50%
6. Play at 200+ FPS! 🚀
