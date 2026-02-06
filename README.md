<div align="center">

# 🎮 DLSS 4.5 Mod for Assassin's Creed Valhalla

### **Unlock Next-Gen Graphics & Up to 4× Frame Generation**

[![Version](https://img.shields.io/badge/Version-4.5.0-blue?style=for-the-badge)](https://github.com/acerthyracer/tensor-curie)
[![Status](https://img.shields.io/badge/Status-Working-brightgreen?style=for-the-badge)](#)
[![RTX](https://img.shields.io/badge/NVIDIA-RTX%20Optimized-76B900?style=for-the-badge&logo=nvidia)](https://nvidia.com)

<img src="assets/Screenshot-2026-02-02-153102.png" width="700" alt="DLSS 4.5 Control Panel"/>

**Transform your Viking adventure with AI-powered upscaling, frame generation, and real-time visual enhancements.**

[🚀 Quick Install](#-quick-install-2-minutes) • [✨ Features](#-features) • [🎛️ Overlay Guide](#️-the-in-game-overlay) • [❓ FAQ](#-faq)

</div>

---

## 🌟 What Makes This Mod Unique

Unlike traditional DLSS mods that require engine modifications or complex injectors, **Tensor-Curie** uses an innovative approach:

| Feature | Traditional Mods | Tensor-Curie |
|---------|-----------------|--------------|
| **Installation** | Manual DLL injection, registry hacks | One-click installer |
| **Camera Detection** | Requires engine source access | Automatic camera matrix detection |
| **Motion Vectors** | Manual memory addresses | AI-powered resource scanning |
| **Configuration** | INI file editing | Beautiful in-game overlay |
| **Frame Generation** | Usually unsupported | Full DLSS-G 2×/3×/4× support |
| **Updates** | Breaks on game patches | Auto-adapts to game updates |

### 🧠 How It Works

```
┌────────────────────────────────────────────────────────────────┐
│                    Tensor-Curie Architecture                    │
├─────────────────┬──────────────────────────────────────────────┤
│  DXGI Proxy     │  Intercepts graphics calls without injection │
│  ─────────────  │  ─────────────────────────────────────────── │
│  D3D12 Wrapper  │  Wraps device/swapchain for resource access  │
│  ─────────────  │  ─────────────────────────────────────────── │
│  AI Scanner     │  Detects depth/motion buffers automatically  │
│  ─────────────  │  ─────────────────────────────────────────── │
│  Streamline SDK │  NVIDIA's official DLSS 4 integration layer  │
│  ─────────────  │  ─────────────────────────────────────────── │
│  ImGui Overlay  │  Real-time control panel with live updates   │
└─────────────────┴──────────────────────────────────────────────┘
```

---

## 🚀 Quick Install (2 Minutes)

### For Players (Pre-Built Release)

**Step 1: Download**
```
📁 Download the latest release ZIP from the Releases page
```

**Step 2: Extract to Game Folder**
```
📂 Extract all files to:
   C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla\
```

**Step 3: Run the Installer**
```
🖱️ Double-click Install.ps1 (or right-click → "Run with PowerShell")
```

**Step 4: Launch Game**
```
🎮 Start AC Valhalla → Press F5 to open the Control Panel
```

> **💡 Pro Tip:** Set the game to **Borderless Windowed** and **Resolution Scale 50%** for best DLSS performance.

### What Gets Installed

| File | Purpose |
|------|---------|
| `dxgi.dll` | Main proxy DLL (the mod itself) |
| `sl.interposer.dll` | NVIDIA Streamline loader |
| `sl.common.dll` | Streamline common library |
| `sl.dlss.dll` | DLSS upscaling module |
| `sl.dlss_g.dll` | Frame Generation module |
| `nvngx_dlss.dll` | NVIDIA NGX runtime |
| `dlss_settings.ini` | Your saved preferences |

---

## ✨ Features

### 🖼️ DLSS 4.5 AI Upscaling
Render at lower resolution, let AI reconstruct the details.

| Mode | Internal Resolution | Best For |
|------|-------------------|----------|
| **Ultra Performance** | 33% | 4K with older GPUs, 8K displays |
| **Performance** | 50% | 4K gaming sweet spot |
| **Balanced** | 58% | Quality/performance balance |
| **Quality** | 67% | High fidelity with FPS boost |
| **DLAA** | 100% | Maximum quality, native res |

### ⚡ Frame Generation (DLSS-G)
Generate extra frames using AI for up to **4× frame rate boost**.

| Multiplier | Effect | Requirement |
|-----------|--------|-------------|
| **2×** | Doubles your FPS | RTX 40-series |
| **3×** | Triples your FPS | RTX 40-series |
| **4×** | Quadruples your FPS | RTX 40-series |

> **Example:** 30 FPS base game → 120 FPS with 4× Frame Gen

### 🎨 DeepDVC (Dynamic Vibrance)
AI-powered adaptive color enhancement that responds to scene content.

- **Intensity:** Overall effect strength
- **Saturation Boost:** Color vividness increase
- **Adaptive Mode:** Automatically adjusts based on scene brightness

### 🌈 HDR Support
Full HDR output control with real-time adjustments.

- Peak brightness up to 10,000 nits
- Paper white calibration
- Custom exposure and gamma curves
- Advanced tone mapping

### 🔧 Smart Frame Generation
Intelligent frame generation that adapts to your gameplay.

- Auto-disables when FPS is already high (saves power)
- Scene-change detection prevents artifacts during cutscenes
- Configurable thresholds and sensitivity

---

## 🎛️ The In-Game Overlay

Press **F5** to open the beautiful ImGui-based control panel.

### Main Control Panel

```
┌─────────────────────────────────────────────────────────┐
│  🎮 DLSS 4.5 Control Panel                         [×] │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  📊 Status: DLSS ✓  FG ✓  Camera ✓  DeepDVC ✓  HDR ✗   │
│                                                         │
│  ─────────────── General ───────────────                │
│  DLSS Mode:     [▼ Quality        ]                     │
│  DLSS Preset:   [▼ Default        ]                     │
│  Sharpness:     [═══════○═══] 0.50                      │
│                                                         │
│  ─────────────── Frame Gen ───────────────              │
│  Multiplier:    [▼ 3× (Recommended)]                    │
│  Smart FG:      [✓] Enabled                             │
│                                                         │
│  ─────────────── DeepDVC ───────────────                │
│  Enable:        [✓]                                     │
│  Intensity:     [═══○═══════] 0.30                      │
│  Adaptive:      [✓] Auto-adjust to scene                │
│                                                         │
│  [🎯 Apply Preset] [💾 Save] [🔄 Reset] [🧙 Wizard]    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### Overlay Hotkeys

| Key | Function | Description |
|-----|----------|-------------|
| **F5** | Control Panel | Opens/closes the main settings menu |
| **F6** | FPS Counter | Toggles the on-screen FPS display |
| **F7** | Vignette | Toggles cinematic vignette effect |
| **F8** | Camera Debug | Logs camera status (+ audio beep) |
| **F9** | DLSS-G Debug | Logs frame generation status |

### FPS Overlay (F6)

A minimal, non-intrusive FPS counter that shows:
- **Base FPS:** Your actual rendered frame rate
- **Total FPS:** Frame rate after frame generation
- **GPU Load:** Current GPU utilization percentage
- **VRAM Usage:** Video memory consumption

### Vignette Overlay (F7)

Adds a customizable cinematic vignette with:
- **Intensity:** Edge darkening strength (0-100%)
- **Radius:** How far the effect extends from edges
- **Softness:** Edge fade smoothness
- **Color:** Custom tint color (RGB picker)

---

## ⚙️ Configuration

### Settings File Location

```
📂 Game Folder/dlss_settings.ini
```

### Example Configuration

```ini
[DLSS]
qualityMode = 2          ; 0=Off, 1=Perf, 2=Balanced, 3=Quality, 4=Ultra, 5=DLAA
sharpness = 0.5
frameGeneration = 3      ; 0=Off, 1=2x, 2=3x, 3=4x

[DeepDVC]
enabled = true
intensity = 0.3
adaptiveVibrance = true

[Overlay]
showFPS = true
showVignette = false
vignetteIntensity = 0.2
```

---

## 💻 System Requirements

### Minimum (DLSS Upscaling Only)

| Component | Requirement |
|-----------|-------------|
| **GPU** | NVIDIA RTX 20-series or newer |
| **VRAM** | 6 GB |
| **Driver** | 560.00+ |
| **OS** | Windows 10/11 (64-bit) |

### Recommended (Full Features)

| Component | Requirement |
|-----------|-------------|
| **GPU** | NVIDIA RTX 4070 or better |
| **VRAM** | 12 GB |
| **Driver** | 565.00+ |
| **OS** | Windows 11 |

### Feature Support by GPU

| GPU Generation | DLSS Upscaling | Frame Gen | HDR | DeepDVC |
|---------------|----------------|-----------|-----|---------|
| RTX 20-series | ✅ | ❌ | ✅ | ✅ |
| RTX 30-series | ✅ | ❌ | ✅ | ✅ |
| RTX 40-series | ✅ | ✅ 2×/3×/4× | ✅ | ✅ |
| RTX 50-series | ✅ | ✅ 2×/3×/4× | ✅ | ✅ |

---

## ❓ FAQ

### "The menu doesn't open when I press F5"

1. Make sure the game is in **Borderless Windowed** mode
2. Check that `dxgi.dll` exists in the game folder
3. Look for errors in `dlss4_proxy.log`

### "Frame Generation shows as inactive"

1. Ensure you have an RTX 40-series GPU
2. Set Resolution Scale to 50% in game settings
3. Enter gameplay (FG is disabled in menus)
4. Check the status bar in the overlay

### "Game crashes on startup"

1. Disable **Ubisoft Connect Overlay** (Settings → General)
2. Disable **Discord Overlay**
3. Disable **MSI Afterburner** / **RivaTuner**
4. Try running the game as Administrator

### "Controls feel laggy with Frame Gen"

This is input latency from interpolated frames. Try:
1. Enable **NVIDIA Reflex** in the overlay
2. Lower Frame Gen from 4× to 2×
3. Use a higher refresh rate monitor

---

## 🔨 Building from Source

### Prerequisites

- **Visual Studio 2022+** with C++ Desktop workload
- **CMake 3.25+**
- **vcpkg** (will be auto-installed if not present)
- **NVIDIA Streamline SDK 2.10.3+**

### Build & Deploy

Simply run the build script - it handles everything:

```powershell
# From the project root
.\build.ps1

# Or with auto-deploy to game folder
.\build.ps1 -Deploy
```

The script will:
1. ✅ Configure CMake with vcpkg
2. ✅ Build the Release DLL
3. ✅ Copy DLL to `bin/` folder
4. ✅ (Optional) Deploy to game folder with Streamline DLLs

### Manual Build

```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"

# Build
cmake --build build --config Release

# Output: build/Release/dxgi.dll
```

---

## 🗑️ Uninstall

### Quick Uninstall

Delete these files from the game folder:
- `dxgi.dll`
- `sl.*.dll` (all Streamline DLLs)
- `nvngx_*.dll` (all NGX DLLs)
- `dlss_settings.ini` (optional - your settings)
- `dlss4_proxy.log` (optional - log file)

### Complete Uninstall Script

```powershell
# Run from game folder
Remove-Item dxgi.dll, sl.*.dll, nvngx_*.dll, dlss_settings.ini, dlss4_proxy.log -ErrorAction SilentlyContinue
```

---

## 📜 Changelog

### v4.5.0 (Current)
- ✨ Added 4× Frame Generation support
- 🎨 New ImGui overlay with live preview
- 🔧 Automatic camera/motion vector detection
- 🐛 Fixed resource tagging for DLSS-G activation

### v4.0.0
- Initial DLSS 4 support
- DeepDVC integration
- HDR output control

---

## 📄 License

GNU General Public License v3.0 - See [LICENSE](LICENSE) for details.

---

## 🙏 Credits

- **acerthyracer** - Lead Developer
- **NVIDIA** - Streamline SDK, DLSS, NGX
- **ocornut** - Dear ImGui
- **Microsoft** - DirectX 12

---

<div align="center">

**Made with ❤️ for Vikings who demand the best graphics**

[⬆ Back to Top](#-dlss-45-mod-for-assassins-creed-valhalla)

</div>
