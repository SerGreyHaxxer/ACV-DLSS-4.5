# Assassin's Creed Valhalla - DLSS 4.5 & Frame Generation Mod

![Status](https://img.shields.org/badge/Status-Working-brightgreen) ![Version](https://img.shields.org/badge/Version-4.5-blue) ![DirectX](https://img.shields.org/badge/DX-12-blue)

This mod injects **NVIDIA DLSS 4.5** (Upscaling) and **Frame Generation** (up to 4x) into *Assassin's Creed Valhalla*. It replaces the game's native anti-aliasing with DLSS/DLAA and uses Streamline to generate intermediate frames for smoother gameplay.

## 🌟 Key Features

*   **Frame Generation:** Multiplies your FPS by 2x, 3x, or **4x**.
*   **True DLSS Scaling:** Supports rendering at lower resolutions (50%-67%) and upscaling to Native 4K with AI quality better than native TAA.
*   **DLAA Mode:** Run at native resolution with AI Anti-Aliasing for ultimate sharpness.
*   **NVIDIA Reflex:** Low-Latency mode enabled to minimize input lag.
*   **In-Game Control Panel (F1):** Adjust settings live without restarting.
    *   Change Frame Gen Multiplier.
    *   Toggle DLSS Modes (Quality, Balanced, Performance).
    *   Adjust Sharpness & Texture Detail (LOD Bias).
*   **Live FPS Overlay (F2):** Custom Valhalla-themed counter showing Base vs. Total FPS.

## 📥 Installation

### Method 1: Automatic (Recommended)
1.  Download this repository.
2.  Run **`complete_setup.ps1`** (Right-click -> Run with PowerShell).
3.  Follow the prompts. It will find your game and install everything.

### Method 2: Manual
1.  Copy `dxgi.dll` from the `bin` folder to your game directory (where `ACValhalla.exe` is).
2.  Ensure you have the NVIDIA Streamline DLLs (`sl.interposer.dll`, `sl.common.dll`) and DLSS DLLs (`nvngx_dlss.dll`, `nvngx_dlssg.dll`) in the game folder.
3.  Launch the game.

## 🎮 How to Use

1.  **Launch** *Assassin's Creed Valhalla*.
2.  **Press F1** to open the **Control Panel**.
    *   Set **Frame Gen** to **4x** for maximum smoothness.
    *   Set **DLSS Mode** to **Quality** or **Balanced**.
3.  **For Performance Boost:**
    *   Go to Game Options -> Screen.
    *   Lower **Resolution Scale** to **67%** (Quality) or **50%** (Performance).
    *   *The mod will detect this and upscale it back to native resolution.*
4.  **Press F2** to toggle the **FPS Counter**.
    *   Format: `Base FPS -> Generated FPS` (e.g., `60 -> 240 FPS`).

## 🛠️ Controls

| Hotkey | Action |
| :--- | :--- |
| **F1** | Toggle Control Panel Overlay |
| **F2** | Toggle Real FPS Counter |

## ⚙️ Advanced Settings (Overlay)

*   **Sharpness:** Controls the DLSS edge sharpening filter (0.0 - 1.0).
*   **LOD Bias:** Controls texture sharpness. Use negative values (slider left) if textures look blurry.
*   **Reflex Boost:** Aggressive latency reduction (found in "Advanced >>" section).

## ⚠️ Troubleshooting

*   **Game Crashing?** Ensure you don't have other overlays (RiverTuner/Afterburner) hooking DX12 aggressively.
*   **Laggy Input?** Make sure **NVIDIA Reflex** is checked in the Advanced section (F1).
*   **UI Flickering?** Try enabling "HUD Masking" in Advanced settings.

## 📝 Credits
*   Mod created by **SerGreyHaxxer**.
*   Powered by NVIDIA Streamline SDK.