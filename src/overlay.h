#pragma once
#include <windows.h>
#include <string>

class OverlayUI {
public:
    static OverlayUI& Get();
    
    void Initialize(HMODULE hModule);
    void ToggleVisibility();
    void SetFPS(float gameFps, float totalFps); 
    
    // New FPS & Vignette
    void ToggleFPS(); 
    void ToggleVignette();
    void SetCameraStatus(bool hasCamera, float jitterX, float jitterY);
    void ToggleDebugMode(bool enabled);
    bool IsVisible() const { return m_visible; }
    void UpdateControls();

private:
    OverlayUI() = default;
    
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static DWORD WINAPI UIThreadEntry(LPVOID lpParam);
    void UIThreadLoop();
    
    void CreateOverlayWindow();
    void CreateFPSWindow(); 
    void CreateVignetteWindow();
    void DrawFPSOverlay();  
    void DrawVignette();
    void UpdateVignette(bool force);
    void UpdateValueLabels();
    void CreateTooltips();
    void AddTooltip(HWND target, const wchar_t* text);
    void ReleaseVignetteResources();
    int Scale(int value) const;

    HMODULE m_hModule = nullptr;
    HWND m_hwnd = nullptr;     // Main Menu
    HWND m_hwndFPS = nullptr;  // FPS Counter
    HWND m_hwndVignette = nullptr; // Signature Effect
    HANDLE m_hThread = nullptr;
    
    // UI Resources (GDI)
    HBRUSH m_brBack = nullptr;
    HBRUSH m_brHeader = nullptr;
    HBRUSH m_brButton = nullptr;
    HBRUSH m_brPanel = nullptr;
    HBRUSH m_brPanelDark = nullptr;
    HFONT m_hFontUI = nullptr;
    HFONT m_hFontHeader = nullptr;
    HFONT m_hFontSection = nullptr;
    HFONT m_hFontSmall = nullptr;
    HFONT m_hFontFPS = nullptr;
    HWND m_hTooltips = nullptr;
    UINT m_dpi = 96;
    float m_scale = 1.0f;
    
    // Controls
    HWND m_hComboDLSS = nullptr;
    HWND m_hComboPreset = nullptr;
    HWND m_hCheckFG = nullptr;
    HWND m_hSliderSharpness = nullptr;
    HWND m_hSliderLOD = nullptr;
    HWND m_hLabelFPS = nullptr;
    HWND m_hLabelCamera = nullptr;
    HWND m_hLabelSharpnessVal = nullptr;
    HWND m_hLabelLODVal = nullptr;
    HWND m_hCheckShowFPS = nullptr;
    HWND m_hCheckShowVignette = nullptr;
    HWND m_hSliderVignetteIntensity = nullptr;
    HWND m_hSliderVignetteRadius = nullptr;
    HWND m_hSliderVignetteSoftness = nullptr;
    HWND m_hLabelVignetteIntensityVal = nullptr;
    HWND m_hLabelVignetteRadiusVal = nullptr;
    HWND m_hLabelVignetteSoftnessVal = nullptr;
    HWND m_hLabelHotkeys = nullptr;
    
    // Expandable Section
    HWND m_hBtnExpand = nullptr;
    HWND m_hGrpAdvanced = nullptr;
    HWND m_hCheckReflex = nullptr;
    HWND m_hCheckHUDFix = nullptr;
    HWND m_hCheckDebug = nullptr;
    HWND m_hLabelLogVerbosity = nullptr;
    HWND m_hComboLogVerbosity = nullptr;
    HWND m_hCheckQuietScan = nullptr;
    HWND m_hCheckDebugMode = nullptr;
    HWND m_hBtnReset = nullptr;
    
    bool m_visible = false;
    bool m_showFPS = false;
    bool m_showVignette = false; 
    bool m_showDebug = false;
    bool m_expanded = false; 
    bool m_initialized = false;
    bool m_vignetteDirty = true;
    
    float m_cachedTotalFPS = 0.0f;
    float m_cachedJitterX = 0.0f;
    float m_cachedJitterY = 0.0f;
    bool m_cachedCamera = false;
    
    // Debug Window
    HWND m_hwndDebug = nullptr;
    HWND m_hwndDebugPanel = nullptr;
    HWND m_hLabelCameraScore = nullptr;
    HWND m_hLabelCameraAge = nullptr;
    HBITMAP m_hVignetteBitmap = nullptr;
    HDC m_hVignetteDC = nullptr;
    void* m_vignetteBits = nullptr;
    int m_vignetteW = 0;
    int m_vignetteH = 0;
    int m_vignetteX = 0;
    int m_vignetteY = 0;
    void CreateDebugWindow();
    void UpdateDebugInfo();
    void UpdateDebugPanel();
};
