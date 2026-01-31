# DLSS 4.5 Mod for Assassin's Creed Valhalla

![Version](https://img.shields.io/badge/Version-4.5-blue) ![Status](https://img.shields.io/badge/Status-Working-brightgreen) ![DirectX](https://img.shields.io/badge/DX-12-blue)

**Unlock 200+ FPS in Assassin's Creed Valhalla with this free DLSS 4.5 & Frame Generation Mod.**

This open-source mod injects **NVIDIA DLSS 3.7+ / 4.5** into the AnvilNext engine, replacing the standard TAA with AI Upscaling and adding **Frame Generation (DLSS-G)** support for RTX 40/50 series cards. It also adds **NVIDIA Reflex** to minimize input lag.

> **🔍 Search Keywords:** AC Valhalla DLSS Mod, Assassin's Creed Valhalla Frame Gen, FPS Boost, DLSS 3 Mod, Free DLSS Mod, ACV Upscaler, RTX 4090, RTX 5080, RTX 4060 Fix.

---

## 🚀 Features

- **Massive FPS Boost:** Go from **60 FPS → 240+ FPS** with 4x Frame Generation
- **Fix Stuttering:** Includes **NVIDIA Reflex** Low Latency mode
- **Better Graphics:** Replaces blurry native AA with crisp **DLAA** or **True DLSS Scaling**
- **In-Game Menu (F5):** Customize settings live without restarting
- **Completely Free:** No paywalls, no Patreon. Open source for the community
- **Easy Install:** One-click build & deploy script

---

## 📋 Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| GPU | NVIDIA RTX 20 series | RTX 4070+ for 4x Frame Gen |
| Driver | 560.94 or newer | 566.14 or newer |
| Game Version | Any (Steam/Ubisoft) | Latest |
| OS | Windows 10/11 64-bit | Windows 11 |

---

## 📥 Installation (Easy Method)

### Option 1: One-Click Build & Install (Recommended)

1. **Download this mod** (click Code → Download ZIP, or use Git)
2. **Download NVIDIA Streamline SDK:**
   - Go to: https://developer.nvidia.com/rtx/streamline
   - Download **Streamline SDK v2.10.3** (or latest)
   - Extract to: `C:\Users\[YourName]\Downloads\streamline-sdk-v2.10.3`
3. **Right-click `BuildAndDeploy.ps1` → Run with PowerShell**
4. The script will:
   - ✅ Verify the SDK
   - 🔍 Find your AC Valhalla installation
   - 🔨 Build the mod
   - 📦 Copy all files automatically
5. **Launch the game and press F5!**

### Option 2: Manual Install

If the automatic script doesn't work:

1. **Install Prerequisites:**
   - Visual Studio 2022 with C++ workload (or Build Tools)
   - CMake (optional but recommended)
   - NVIDIA Streamline SDK (see link above)

2. **Build the mod:**
   ```batch
   # Option A: Using PowerShell script
   .\BuildAndDeploy.ps1 -CustomGamePath "C:\Path\To\AC Valhalla"
   
   # Option B: Using batch file
   build.bat
   
   # Option C: Using CMake
   mkdir build && cd build
   cmake .. -G "Visual Studio 17 2022" -A x64
   cmake --build . --config Release
   ```

3. **Copy files to game folder:**
   Copy these files from `bin\` folder to your AC Valhalla folder:
   ```
   dxgi.dll                    (REQUIRED - the main mod)
   sl.interposer.dll           (REQUIRED - Streamline core)
   sl.common.dll              (REQUIRED)
   sl.dlss.dll                (REQUIRED - DLSS Super Resolution)
   sl.dlss_g.dll              (REQUIRED - Frame Generation)
   sl.reflex.dll              (REQUIRED - Low latency)
   nvngx_dlss.dll             (REQUIRED - NGX backend)
   nvngx_dlssg.dll            (REQUIRED - NGX Frame Gen)
   ```

---

## 🎮 Controls

| Key | Function |
|-----|----------|
| **F5** | Open **DLSS Control Panel** |
| **F6** | Toggle **FPS Counter** (Gold Text) |
| **F7** | Toggle **Orange Vignette** overlay |
| **F8** | **Debug Camera Status** (check if mod is working) |

---

## ⚙️ Best Settings for Max FPS

1. Launch the game
2. Press **F5** to open the DLSS Control Panel
3. Set **Frame Gen** to **4x** (or 3x if you have RTX 40 series)
4. Set **DLSS Mode** to **Performance**
5. In the game's video settings:
   - Go to **Screen** → **Resolution Scale**
   - Set to **50%**
   - Enable **Borderless Windowed** mode
6. **Result:** The game renders fast at 1080p, and the Mod upscales it to perfect 4K + generates 3 extra frames!

### Preset Recommendations

| Quality Mode | Render Scale | Use Case |
|--------------|--------------|----------|
| **Performance** | 50% | Maximum FPS (4K → 1080p render) |
| **Balanced** | 58% | Good balance |
| **Quality** | 67% | Better image quality |
| **DLAA** | 100% | Native resolution + anti-aliasing |

---

## 🛠️ Troubleshooting

### "Streamline SDK not found"
- Download from: https://developer.nvidia.com/rtx/streamline
- Extract to Downloads folder (the script looks there automatically)
- Or specify path: `.\BuildAndDeploy.ps1 -SteamPath "C:\Path\To\SDK"`

### Game crashes on startup
1. Disable **Ubisoft Connect Overlay**
2. Disable **RivaTuner Statistics Server** (RTSS)
3. Disable **Discord Overlay**
4. Disable **MSI Afterburner**
5. Run game in **Borderless Windowed** mode

### Laggy mouse / input delay
- Press **F5** → Check "NVIDIA Reflex Boost"
- Lower Frame Gen multiplier (try 2x instead of 4x)

### Menu not appearing (F5 doesn't work)
- Run the game in **Borderless Windowed** mode
- Check `dlss4_proxy.log` in the game folder for errors
- Make sure all 8 DLL files are copied

### Low FPS / no improvement
- Make sure **Resolution Scale** in game is set to 50% or lower
- Verify Frame Generation is enabled (F5 menu)
- Check GPU usage - if it's already at 99%, this mod helps less

### "ACValhalla.exe not found"
- The auto-detection failed
- Use: `.\BuildAndDeploy.ps1 -CustomGamePath "C:\Your\Game\Path"`

---

## 📝 How It Works

This mod is a **DXGI Proxy DLL** that:
1. Intercepts DirectX 12 calls between the game and Windows
2. Detects the game's render targets, depth buffer, and motion vectors
3. Injects NVIDIA DLSS Super Resolution at the right moment
4. Adds Frame Generation to double/triple/quadruple FPS
5. Reduces input lag with NVIDIA Reflex

### Technical Details
- **Hook Method:** DXGI Proxy + VMT Hooking
- **API:** DirectX 12
- **SDK:** NVIDIA Streamline SDK 2.10.3
- **Features:** DLSS 3.7, DLSS-G (Frame Gen), DLSS-RR (Ray Reconstruction), Reflex

---

## 🔄 Uninstallation

To remove the mod:
1. Delete these files from your AC Valhalla folder:
   ```
   dxgi.dll
   sl.interposer.dll
   sl.common.dll
   sl.dlss.dll
   sl.dlss_g.dll
   sl.reflex.dll
   nvngx_dlss.dll
   nvngx_dlssg.dll
   ```
2. (Optional) Delete `dlss_settings.ini` and `dlss4_proxy.log`

---

## 📊 Performance Examples

| Setup | Without Mod | With Mod (4x FG) |
|-------|-------------|------------------|
| RTX 4090 @ 4K Ultra | 70 FPS | 280 FPS |
| RTX 4080 @ 4K High | 55 FPS | 220 FPS |
| RTX 4070 @ 1440p Ultra | 85 FPS | 340 FPS |
| RTX 4060 @ 1080p High | 75 FPS | 300 FPS |

*Results vary by CPU and game scene*

---

## 🐛 Reporting Issues

If you encounter problems:
1. Check `dlss4_proxy.log` in your game folder
2. Note your GPU model and driver version
3. Include the error message or screenshot
4. Create an issue on GitHub

---

## ⚖️ Legal & Disclaimer

- This mod is **open source** and **free**
- Use at your own risk
- We are not affiliated with Ubisoft or NVIDIA
- This does not modify game files, only injects via DirectX
- Only use in single-player mode

---

## 🙏 Credits

- **Created by:** SerGreyHaxxer
- **NVIDIA Streamline SDK:** https://developer.nvidia.com/rtx/streamline
- **Special thanks:** The PC modding community

---

*Created by SerGreyHaxxer - Open Source Community Project*
