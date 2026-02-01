# DLSS 4.5 Mod for Assassin's Creed Valhalla

**Unlock 200+ FPS with DLSS 4.5 & Frame Generation.**

![Version](https://img.shields.io/badge/Version-4.5-blue) ![Status](https://img.shields.io/badge/Status-Working-brightgreen)

This mod replaces the game's standard TAA with **NVIDIA DLSS 4.5** (AI Upscaling) and adds **Frame Generation (DLSS-G)**, allowing you to multiply your FPS by up to 4x on supported RTX cards.

---

## [!] Comprehensive Installation Guide

Follow these steps exactly to build and install the mod.

### Step 1: Prepare Your PC (Prerequisites)
Before you start, ensure you have the necessary tools installed.

1.  **Install Visual Studio 2022 (Community Edition):**
    *   Download for free from [Microsoft](https://visualstudio.microsoft.com/vs/community/).
    *   During installation, look for the "Workloads" tab.
    *   Check **"Desktop development with C++"**.
    *   Click **Install**.
    *   *(This is required to compile the mod code)*.

2.  **Download NVIDIA Streamline SDK:**
    *   Go to the [NVIDIA Developer Streamline Page](https://developer.nvidia.com/rtx/streamline).
    *   Download **Streamline SDK 2.4.0 or newer** (tested with 2.10.3).
    *   **CRITICAL:** Extract the downloaded ZIP file into your **Downloads** folder.
    *   *The build script will automatically look for it there.*

### Step 2: Download the Mod
1.  Click the green **Code** button at the top of this page.
2.  Select **Download ZIP**.
3.  Extract the ZIP file to a folder on your computer (e.g., `C:\Mods\ACV-DLSS`).

### Step 3: Build the Mod
1.  Open the folder where you extracted the mod.
2.  Double-click **`build.bat`**.
3.  A black window will appear. It will:
    *   Find your Visual Studio installation.
    *   Find the Streamline SDK in your Downloads folder.
    *   Compile the code into `dxgi.dll`.
4.  **Success:** You should see "BUILD SUCCESSFUL" in green text.
    *   *If it fails, check that you installed the C++ Workload in Visual Studio.*

### Step 4: Automatic Installation
1.  Double-click **`Install.bat`**.
2.  A blue window will appear. It will try to automatically find your *Assassin's Creed Valhalla* game folder.
    *   **If found:** It will copy all necessary DLL files (`dxgi.dll`, `nvngx_dlss.dll`, etc.) to the game folder.
    *   **If NOT found:** It will ask you to drag and drop your `ACValhalla.exe` file onto the window.
3.  **Registry Fix:** The script will ask to import a Registry file (`EnableNvidiaSigOverride.reg`).
    *   You **MUST select YES**.
    *   *This allows the game to load the custom DLSS files without a digital signature error.*

### Step 5: Configure the Game
1.  Launch **Assassin's Creed Valhalla**.
2.  Go to **Options** -> **Screen**.
3.  Set **Window Mode** to **Borderless Windowed** (Required for the menu overlay).
4.  Set **Resolution Scale** to **50%** (or your preferred DLSS quality level).
    *   *50% Scale = DLSS Performance*
    *   *67% Scale = DLSS Quality*
    *   *The mod will take this lower resolution and upscale it back to native 4K/1440p.*
5.  **Press F5** to open the Mod Menu.
    *   Enable **Frame Generation**.
    *   Set your desired multiplier (e.g., 2x, 3x, or 4x).

---

## [?] Requirements

| Component | Requirement |
| :--- | :--- |
| **GPU (Frame Gen)** | NVIDIA RTX 40-series or 50-series |
| **GPU (Upscaling)** | NVIDIA RTX 20-series or 30-series |
| **OS** | Windows 10 or 11 (64-bit) |
| **Driver** | 560.00 or newer recommended |

---

## [X] Troubleshooting

**The F5 Menu doesn't open**
*   Make sure you are in **Borderless Windowed** mode.
*   Open the game folder and check `dlss4_proxy.log` for errors.

**"Missing ACValhalla.exe"**
*   Run `Install.bat` again and drag-drop your game exe when prompted.

**Game crashes on startup**
*   Disable **Ubisoft Connect Overlay** (Settings -> General -> Uncheck "Enable in-game overlay").
*   Disable **Discord Overlay**.
*   Disable **MSI Afterburner / RivaTuner**.

**I have low FPS**
*   Did you set **Resolution Scale** to 50%? If it's at 100%, you are rendering at native resolution + DLAA, which is heavy.

---

**Credits:** SerGreyHaxxer, NVIDIA Streamline SDK.