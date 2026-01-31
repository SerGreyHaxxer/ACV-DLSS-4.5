# 🚀 Quick Start Guide

## Installation Steps (2 Minutes)

### Step 1: Download Prerequisites
1. **Download this mod** (Code → Download ZIP)
2. **Download NVIDIA Streamline SDK**
   - Go to: https://developer.nvidia.com/rtx/streamline
   - Download v2.10.3 or newer
   - Extract to Downloads folder (it will auto-detect)

### Step 2: Run the Installer
**Double-click** `INSTALL.bat` (or right-click → Run as Administrator)

That's it! The script will:
- ✅ Find your AC Valhalla installation automatically
- 🔨 Build the mod
- 📦 Copy all files

### Step 3: Play!
1. Launch AC Valhalla
2. Press **F5** in-game to open the DLSS menu
3. Set Frame Gen to **4x**
4. Set DLSS to **Performance**
5. In game settings, set **Resolution Scale to 50%**

---

## Alternative: Manual Build

If the automatic installer fails:

### Using PowerShell
```powershell
.\BuildAndDeploy.ps1 -CustomGamePath "C:\Your\Game\Path"
```

### Using CMake
```batch
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

Then copy files from `bin\` or `build\bin\Release\` to your game folder.

---

## Troubleshooting

### "SDK not found"
Make sure you downloaded and extracted the Streamline SDK to Downloads.

### "Game not found"
Use: `.\BuildAndDeploy.ps1 -CustomGamePath "C:\Path\To\Game"`

### Build fails
Install Visual Studio 2022 with "Desktop development with C++" workload.

---

## Files Explained

| File | Purpose |
|------|---------|
| `INSTALL.bat` | Double-click installer (easiest) |
| `BuildAndDeploy.ps1` | PowerShell build & deploy script |
| `build.bat` | Manual batch build (advanced) |
| `CMakeLists.txt` | CMake configuration |
| `deploy_mod.ps1` | Deploy-only script |

---

## Need Help?

Check `README.md` for full documentation or check `dlss4_proxy.log` in the game folder after running.
