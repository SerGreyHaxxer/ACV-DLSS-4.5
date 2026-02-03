# DLSS 4.5 Mod for Assassin's Creed Valhalla

**Boost FPS with DLSS 4.5 & Frame Generation (hardware dependent).**

![DLSS 4.5 Control Panel](assets/Screenshot-2026-02-02-153102.png)

![Version](https://img.shields.io/badge/Version-4.5-blue) ![Status](https://img.shields.io/badge/Status-Working-brightgreen)

This mod replaces the game's standard TAA with **NVIDIA DLSS 4.5** (AI Upscaling), adds **Frame Generation (DLSS-G)**, and supports **DeepDVC (RTX Dynamic Vibrance)** for adaptive color enhancement.

---

## Install (Exact Steps)

1) Download this repo as ZIP and extract it (example: `C:\Mods\ACV-DLSS-4.5`).  
2) Download the **NVIDIA Streamline SDK** (tested **2.10.3**) and leave the ZIP or extracted folder in **Downloads**.  
   Examples:  
   * `C:\Users\You\Downloads\streamline-sdk-v2.10.3.zip`  
   * `C:\Users\You\Downloads\streamline-sdk-v2.10.3\bin\x64`  
3) Double‑click **Install.bat** and approve the UAC prompt (registry import).  
   If prompted for the game path, drag **ACValhalla.exe** into the window and press Enter.  
   Example: `C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla\ACValhalla.exe`  
4) Launch the game → **Screen** → set **Borderless Windowed** and **Resolution Scale 50%**.  
5) Press **F5** to open the Control Panel. Run **Setup Wizard** or use the preset buttons.  
6) Settings are saved to **dlss_settings.ini** in the game folder.

## Hotkeys

* **F5**: Toggle Control Panel  
* **F6**: Toggle FPS Overlay  
* **F7**: Toggle Vignette Overlay  
* **F8**: Debug Camera Status (log + beep)  
* **F9**: Debug DLSS‑G Status (log)  

## Features (what each control does)

**General**  
* **DLSS Quality Mode**: Off / Performance / Balanced / Quality / Ultra / DLAA.  
* **DLSS Preset**: NVIDIA preset tuning selection.  
* **Preset buttons**: Apply a full recommended bundle instantly.  

**Ray Reconstruction**  
* **Enable DLSS Ray Reconstruction**: Replaces denoising with RR.  
* **RR Preset**: Per‑quality RR tuning.  
* **RR Denoiser Strength**: Strength used by RR options.  

**DeepDVC (RTX Dynamic Vibrance)**  
* **Enable DeepDVC**: Enables dynamic vibrance.  
* **Intensity / Saturation Boost**: Base effect strength.  
* **Adaptive Vibrance**: Adjusts based on scene luminance.  
* **Adaptive Strength/Min/Max/Smoothing**: Controls adaptation range and speed.  

**Frame Generation**  
* **Frame Generation**: Off / 2x / 3x / 4x (RTX 40‑series only).  

**Smart Frame Generation**  
* **Enable Smart FG**: Auto‑manages FG based on gameplay.  
* **Auto‑disable when FPS is high + Threshold**: Turns FG off above a target FPS.  
* **Scene‑change detection + Sensitivity**: Temporarily disables FG on rapid scene changes.  
* **FG Interpolation Quality**: `0` forces interpolated‑only mode; higher values keep normal output.  

**Quality**  
* **Sharpness**: DLSS sharpening.  
* **Texture Detail (LOD Bias)**: Shifts texture mip bias in real time.  
* **Auto Motion Vector Scale / MV Scale X/Y**: Auto or manual motion‑vector scaling.  

**Overlay**  
* **Show FPS Overlay**: In‑game FPS overlay.  
* **Show Vignette + Intensity/Radius/Softness/Color**: Adjustable vignette overlay.  

**Tools**  
* **Reset to Defaults**: Restores default settings.  
* **Setup Wizard**: First‑time recommendations based on GPU.  
* **Status row**: Live support/status for DLSS, FG, Camera, DeepDVC.  

## What makes this mod unique

* Automatic camera/matrix detection and jitter tracking to feed Streamline without engine changes.  
* DXGI proxy approach (no injector required).  
* Full real‑time control panel with instant slider updates.  
* DeepDVC with adaptive vibrance plus Smart FG safeguards.  

## Uninstall

1) Delete **dxgi.dll** from the game folder.  
2) Delete the copied **sl.\*** and **nvngx_\*** DLLs in the same folder.  
3) (Optional) delete **dlss_settings.ini** and **EnableNvidiaSigOverride.reg**.  

---

## Building from Source

**Prerequisites:**
*   Visual Studio 2019 or 2022 (C++ Desktop Development workload)
*   [NVIDIA Streamline SDK](https://developer.nvidia.com/rtx/streamline)
*   [NVIDIA NVAPI](https://github.com/NVIDIA/nvapi)

**Steps:**
1.  Clone the repository.
2.  Place the Streamline SDK in `external/streamline` (or `Downloads`).
3.  Place NVAPI in `external/nvapi` (or `Downloads`).
4.  Run `build.bat` from a Developer Command Prompt (or just double-click it, it will try to find VS).
5.  The output DLL will be in `bin/dxgi.dll`.

---

## Requirements

| Component | Requirement |
| :--- | :--- |
| **GPU (Frame Gen)** | NVIDIA RTX 40-series or newer (DLSS-G) |
| **GPU (Upscaling)** | NVIDIA RTX 20-series or 30-series |
| **OS** | Windows 10 or 11 (64-bit) |
| **Driver** | 560.00 or newer recommended |

---

## Troubleshooting

**"Streamline SDK not found"**  
* Make sure the SDK ZIP or extracted folder is in your `Downloads` folder.  
* Re‑run **Install.bat** after placing it there.

**The menu doesn't open**  
* Ensure **Borderless Windowed** is set and press **F5**.  
* Check `dlss4_proxy.log` in the game folder for errors.

**Game crashes on startup**  
* Disable **Ubisoft Connect Overlay** (Settings → General → uncheck “Enable in‑game overlay”).  
* Disable **Discord Overlay** and **MSI Afterburner**.

---

**Credits:** SerGreyHaxxer, NVIDIA Streamline SDK.
