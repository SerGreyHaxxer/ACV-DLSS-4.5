# DLSS 4.5 Mod for Assassin's Creed Valhalla

**Unlock 200+ FPS with DLSS 4.5 & Frame Generation.**

![Version](https://img.shields.io/badge/Version-4.5-blue) ![Status](https://img.shields.io/badge/Status-Working-brightgreen)

This mod replaces the game's standard TAA with **NVIDIA DLSS 4.5** (AI Upscaling) and adds **Frame Generation (DLSS-G)**, allowing you to multiply your FPS by up to 4x on supported RTX cards.

---

## [!] Easy Installation Guide

Follow these steps to install the mod. **No building or coding required!**

### Step 1: Download Required Files
1.  **Download this Mod:** Click **Code -> Download ZIP** above and extract it.
2.  **Download NVIDIA Streamline SDK:**
    *   Go to the [NVIDIA Developer Streamline Page](https://developer.nvidia.com/rtx/streamline).
    *   Download **Streamline SDK 2.4.0 or newer** (tested with 2.10.3).
    *   **CRITICAL:** Just leave the downloaded ZIP file in your **Downloads** folder. The installer will find it automatically.

### Step 2: Install
1.  Open the folder where you extracted this mod.
2.  Double-click **`Install.bat`**.
3.  A blue window will appear. It will:
    *   Find your game installation automatically.
    *   Extract the necessary files from the SDK ZIP in your Downloads.
    *   Copy everything to the game folder.
4.  **Registry Fix:** The script will ask to import a Registry file (`EnableNvidiaSigOverride.reg`).
    *   You **MUST select YES**.
    *   *This allows the game to load the custom DLSS files.*

### Step 3: Configure Game Settings
1.  Launch **Assassin's Creed Valhalla**.
2.  Go to **Options** -> **Screen**.
3.  Set **Window Mode** to **Borderless Windowed** (Required for the menu overlay).
4.  Set **Resolution Scale** to **50%** (or 67% for Quality).
    *   *The mod will take this lower resolution and upscale it back to native 4K/1440p using DLSS.*
5.  **Press F5** to open the Mod Menu.
    *   Enable **Frame Generation**.
    *   Set your desired multiplier (e.g., 2x, 3x, or 4x).
    *   Configure **Vignette** and **FPS overlay** in the Overlay section.
    *   **F6** toggles FPS overlay, **F7** toggles vignette.

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

**"Streamline SDK not found"**
*   Make sure you downloaded the SDK ZIP from NVIDIA and it is in your `Downloads` folder.

**The F5 Menu doesn't open**
*   Make sure you are in **Borderless Windowed** mode.
*   Open the game folder and check `dlss4_proxy.log` for errors.

**Game crashes on startup**
*   Disable **Ubisoft Connect Overlay** (Settings -> General -> Uncheck "Enable in-game overlay").
*   Disable **Discord Overlay** and **MSI Afterburner**.

---

**Credits:** SerGreyHaxxer, NVIDIA Streamline SDK.
