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
    bool IsVisible() const { return m_visible; }

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
    void UpdateControls(); 

    HMODULE m_hModule = nullptr;
    HWND m_hwnd = nullptr;     // Main Menu
    HWND m_hwndFPS = nullptr;  // FPS Counter
    HWND m_hwndVignette = nullptr; // Signature Effect
    HANDLE m_hThread = nullptr;
    
    // Controls
    HWND m_hComboDLSS = nullptr;
    HWND m_hCheckFG = nullptr;
    HWND m_hSliderSharpness = nullptr;
    HWND m_hSliderLOD = nullptr;
    HWND m_hLabelFPS = nullptr;
    
    // Expandable Section
    HWND m_hBtnExpand = nullptr;
    HWND m_hGrpAdvanced = nullptr;
    HWND m_hCheckReflex = nullptr;
    HWND m_hCheckHUDFix = nullptr;
    
    bool m_visible = false;
    bool m_showFPS = false;
    bool m_showVignette = false; 
    bool m_expanded = false; 
    bool m_initialized = false;
    
    float m_cachedTotalFPS = 0.0f;
    int m_vignetteAlpha = 80;
};
