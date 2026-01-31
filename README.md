# Assassin's Creed Valhalla - DLSS 4.5 & Frame Generation Mod

![Status](https://img.shields.org/badge/Status-Working-brightgreen) ![Version](https://img.shields.org/badge/Version-4.5-blue) ![DirectX](https://img.shields.org/badge/DX-12-blue)

This mod transforms *Assassin's Creed Valhalla* by adding **NVIDIA DLSS 4.5** (AI Upscaling) and **Frame Generation** (up to 4x FPS). It creates intermediate frames to make gameplay buttery smooth and uses AI to sharpen the image better than the game's native anti-aliasing.

## 🌟 What does this do?
*   **Quadruple your FPS:** Turn 60 FPS into **240 FPS** using AI Frame Generation.
*   **Fix Lag:** Enables **NVIDIA Reflex** to minimize mouse/controller delay.
*   **Better Graphics:** Uses DLAA/DLSS to remove jagged edges and shimmering.
*   **Full Control:** Includes an in-game **Control Panel (F1)** to tweak settings live.

---

## 📥 Step 1: Download the Mod
You don't need to be a programmer to use this.

1.  Scroll to the top of this GitHub page.
2.  Click the green button labeled **`<> Code`**.
3.  Select **Download ZIP** from the menu.
4.  Go to your **Downloads** folder and extract the ZIP file (Right-click -> Extract All...) to a folder on your desktop.

---

## 💿 Step 2: Install the Mod

### Option A: The "One-Click" Installer (Recommended)
1.  Open the folder where you extracted the mod.
2.  Right-click the file named **`install_mod.bat`**.
3.  Select **Run as Administrator** (This is needed to copy files to your game folder).
4.  A black window will pop up.
    *   It will automatically try to find your game (Steam or Ubisoft).
    *   If it finds it, it will install everything automatically.
    *   *If it can't find it*, it will ask you to type the location (e.g., `D:\Games\Assassin's Creed Valhalla`).

### Option B: Manual Installation
If the script doesn't work for you, follow these steps:

1.  **Find your Game Folder.**
    *   *Steam:* Right-click game in Library -> Manage -> Browse Local Files.
    *   *Ubisoft:* Open Connect -> AC Valhalla -> Properties -> Open Folder.
    *   *Example Path:* `C:\Program Files (x86)\Steam\steamapps\common\Assassin's Creed Valhalla`
2.  **Copy the Mod.**
    *   Go to the `bin` folder in your download.
    *   Copy the file **`dxgi.dll`**.
    *   Paste it into your Game Folder (right next to `ACValhalla.exe`).
3.  **Install NVIDIA DLLs.**
    *   You need `nvngx_dlss.dll` and `nvngx_dlssg.dll` in the game folder. If you play other modern games (Cyberpunk, Alan Wake), you can copy them from there, or download them from TechPowerUp.

---

## 🎮 Step 3: How to Play & Configure

1.  **Launch the Game.**
2.  **Check the Menu (F1):**
    *   Once the game starts, press **F1**.
    *   You should hear a **Beep** and see a "DLSS 4.5 Control Panel" window.
    *   *Note:* If the window doesn't appear, go to **Options -> Screen** and change "Window Mode" to **Borderless**.
3.  **Enable High FPS:**
    *   In the F1 Menu, look for "Frame Generation".
    *   Select **4x (DLSS-MFG)**.
4.  **Enable True DLSS (Critical Step):**
    *   Set "DLSS Quality Mode" to **Performance** or **Balanced** in the F1 Menu.
    *   Go to the **Game's Settings -> Screen -> Resolution Scale**.
    *   **Lower it to 50% or 60%.**
    *   *Magic:* The mod will detect this low resolution and use AI to upscale it back to 4K. This gives you huge FPS gains because the GPU renders fewer pixels!

---

## 📊 Step 4: Verify FPS

1.  Press **F2** on your keyboard.
2.  Look at the top-left corner. You will see gold text:
    *   `Base: 60 -> Total: 240 FPS`
    *   **Base:** How fast your PC is running the game logic.
    *   **Total:** How many frames you are actually seeing after AI Generation.

---

## ⚙️ Advanced Settings (F1 Menu)

Click the **"Advanced Settings >>"** button at the bottom of the panel.

*   **NVIDIA Reflex Boost:** Keep this **Checked**. It reduces the lag between your mouse click and the action.
*   **Sharpness Slider:** Drag right for a crisper image, left for a softer image.
*   **LOD Bias Slider:** If textures look blurry or "muddy", drag this slider to the **Right** (Negative Bias) to force the game to load high-quality textures.

---

## ⚠️ Common Issues

| Problem | Solution |
| :--- | :--- |
| **Game Crashes on Launch** | You might have another overlay (Afterburner/RivaTuner) fighting the mod. Disable them. |
| **F1 Menu is stuck/can't click** | Switch the game to **Borderless Windowed** mode in Video settings. |
| **Game feels laggy but FPS is high** | Make sure **V-Sync** is **OFF** in game settings. Frame Gen needs unlocked FPS. |
| **Textures look blurry** | Open F1 Menu -> Advanced -> Drag **LOD Bias** slider to the right. |

---
*Modded by SerGreyHaxxer*
