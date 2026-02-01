#include <vector>
#include <string>
#include <sstream>
#include "overlay.h"
#include "streamline_integration.h"
#include "d3d12_wrappers.h"
#include "config_manager.h"
#include "resource_detector.h"
#include "logger.h"
#include <commctrl.h>
#include <stdio.h>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

// Control IDs
#define ID_COMBO_DLSS 101
#define ID_COMBO_PRESET 102
#define ID_CHECK_FG   103
#define ID_SLIDER_SHARP 104
#define ID_SLIDER_LOD   105
#define ID_BTN_EXPAND   106
#define ID_CHECK_REFLEX 107
#define ID_CHECK_HUD    108
#define ID_CHECK_DEBUG  109
#define ID_COMBO_LOGVERB 110
#define ID_CHECK_QUIETSCAN 111
#define ID_CHECK_DEBUGMODE 112
#define ID_BTN_RESET   113
#define ID_CHECK_SHOWFPS 114
#define ID_CHECK_SHOWVIG 115
#define ID_SLIDER_VIG_INT 116
#define ID_SLIDER_VIG_RAD 117
#define ID_SLIDER_VIG_SOFT 118

// Colors (ImGui Style)
#define COL_BG      RGB(30, 30, 30)
#define COL_HEADER  RGB(10, 15, 20) // Valhalla Dark
#define COL_BTN     RGB(40, 50, 60)
#define COL_TEXT    RGB(220, 220, 220)
#define COL_ACCENT  RGB(212, 175, 55) // Gold
#define COL_PANEL   RGB(24, 24, 24)
#define COL_PANEL_D RGB(18, 18, 18)

static const wchar_t* kSectionGeneral = L"General";
static const wchar_t* kSectionQuality = L"Quality";
static const wchar_t* kSectionOverlay = L"Overlay";

static bool IsValidWindowPos(int x, int y) {
    return x >= -2000 && x <= 2000 && y >= -2000 && y <= 2000;
}

OverlayUI& OverlayUI::Get() {
    static OverlayUI instance;
    return instance;
}

void OverlayUI::Initialize(HMODULE hModule) {
    if (m_initialized) return;
    m_hModule = hModule;
    m_dpi = GetDpiForSystem();
    m_scale = m_dpi / 96.0f;
    
    // Create Resources
    m_brBack = CreateSolidBrush(COL_BG);
    m_brHeader = CreateSolidBrush(COL_HEADER);
    m_brButton = CreateSolidBrush(COL_BTN);
    m_brPanel = CreateSolidBrush(COL_PANEL);
    m_brPanelDark = CreateSolidBrush(COL_PANEL_D);
    m_hFontUI = CreateFontW(Scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_hFontHeader = CreateFontW(Scale(18), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_hFontSection = CreateFontW(Scale(15), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_hFontSmall = CreateFontW(Scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_hFontFPS = CreateFontW(Scale(42), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Arial");

    m_hThread = CreateThread(NULL, 0, UIThreadEntry, this, 0, NULL);
    m_initialized = true;
}

void OverlayUI::ToggleDebugMode(bool enabled) {
    m_showDebug = enabled;
    if (m_hCheckDebug) SendMessageW(m_hCheckDebug, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    if (m_hwndDebug) ShowWindow(m_hwndDebug, enabled ? SW_SHOW : SW_HIDE);
}

DWORD WINAPI OverlayUI::UIThreadEntry(LPVOID lpParam) {
    OverlayUI* pThis = (OverlayUI*)lpParam;
    pThis->UIThreadLoop();
    return 0;
}

void OverlayUI::UIThreadLoop() {
    ModConfig& cfg = ConfigManager::Get().Data();
    m_showFPS = cfg.showFPS;
    m_showVignette = cfg.showVignette;
    m_showDebug = cfg.debugMode;

    // 1. Control Panel Class (Custom Painted)
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = m_hModule;
    wc.lpszClassName = L"DLSS4ProxyOverlay";
    wc.hbrBackground = m_brBack;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_DROPSHADOW; // Add drop shadow
    RegisterClassExW(&wc);
    
    // 2. FPS & Vignette classes (Standard)
    WNDCLASSEXW wcFPS = {0}; wcFPS.cbSize = sizeof(WNDCLASSEXW); wcFPS.lpfnWndProc = WindowProc; wcFPS.hInstance = m_hModule; wcFPS.lpszClassName = L"DLSS4ProxyFPS"; wcFPS.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassExW(&wcFPS);
    WNDCLASSEXW wcVig = {0}; wcVig.cbSize = sizeof(WNDCLASSEXW); wcVig.lpfnWndProc = WindowProc; wcVig.hInstance = m_hModule; wcVig.lpszClassName = L"DLSS4ProxyVignette"; wcVig.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassExW(&wcVig);
    
    // 3. Debug Window Class
    WNDCLASSEXW wcDbg = {0}; wcDbg.cbSize = sizeof(WNDCLASSEXW); wcDbg.lpfnWndProc = WindowProc; wcDbg.hInstance = m_hModule; wcDbg.lpszClassName = L"DLSS4ProxyDebug"; 
    wcDbg.hbrBackground = CreateSolidBrush(RGB(20, 20, 20)); // Dark background
    RegisterClassExW(&wcDbg);

    CreateOverlayWindow();
    CreateFPSWindow();
    CreateVignetteWindow(); 
    CreateDebugWindow(); 
    if (m_hwndFPS) ShowWindow(m_hwndFPS, m_showFPS ? SW_SHOW : SW_HIDE);
    if (m_hwndVignette) {
        ShowWindow(m_hwndVignette, m_showVignette ? SW_SHOW : SW_HIDE);
        if (m_showVignette) UpdateVignette(true);
    }
    if (m_hwndDebug) ShowWindow(m_hwndDebug, m_showDebug ? SW_SHOW : SW_HIDE);
    
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void OverlayUI::CreateOverlayWindow() {
    ModConfig& cfg = ConfigManager::Get().Data();
    int width = Scale(420);
    int height = Scale(640);
    int x = cfg.uiPosX;
    int y = cfg.uiPosY;
    if (!IsValidWindowPos(x, y)) { x = 50; y = 50; }

    // WS_POPUP only - No standard Windows borders/caption
    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 
        L"DLSS4ProxyOverlay", L"DLSS 4.5",
        WS_POPUP | WS_VISIBLE,
        x, y, width, height, NULL, NULL, m_hModule, NULL
    );

    int padding = Scale(18); int cy = Scale(44); // Start below header
    int contentWidth = width - 2*padding;

    // Controls
    auto AddLabel = [&](const wchar_t* text) {
        HWND h = CreateWindowW(L"STATIC", text, WS_VISIBLE | WS_CHILD | SS_LEFT, padding, cy, contentWidth, Scale(20), m_hwnd, NULL, m_hModule, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)m_hFontSection, TRUE);
        cy += Scale(24);
    };
    auto AddSmallLabel = [&](const wchar_t* text) {
        HWND h = CreateWindowW(L"STATIC", text, WS_VISIBLE | WS_CHILD | SS_LEFT, padding, cy, contentWidth, Scale(18), m_hwnd, NULL, m_hModule, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
        cy += Scale(20);
        return h;
    };
    auto AddValueLabel = [&](int x, int y, int w) {
        HWND h = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_RIGHT, x, y, w, Scale(18), m_hwnd, NULL, m_hModule, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
        return h;
    };

    AddLabel(kSectionGeneral);
    AddSmallLabel(L"DLSS Quality Mode:");
    m_hComboDLSS = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, padding, cy, contentWidth, Scale(240), m_hwnd, (HMENU)ID_COMBO_DLSS, m_hModule, NULL);
    SendMessage(m_hComboDLSS, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    const wchar_t* modes[] = { L"Off", L"Max Performance", L"Balanced", L"Max Quality", L"Ultra Quality", L"DLAA" };
    for (const wchar_t* mode : modes) SendMessageW(m_hComboDLSS, CB_ADDSTRING, 0, (LPARAM)mode);
    cy += Scale(42);

    AddSmallLabel(L"DLSS Preset:");
    m_hComboPreset = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, padding, cy, contentWidth, Scale(240), m_hwnd, (HMENU)ID_COMBO_PRESET, m_hModule, NULL);
    SendMessage(m_hComboPreset, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    const wchar_t* presets[] = { L"Default", L"Preset A", L"Preset B", L"Preset C", L"Preset D", L"Preset E", L"Preset F", L"Preset G" };
    for (const wchar_t* preset : presets) SendMessageW(m_hComboPreset, CB_ADDSTRING, 0, (LPARAM)preset);
    cy += Scale(42);

    AddSmallLabel(L"Frame Generation:");
    m_hCheckFG = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, padding, cy, contentWidth, Scale(240), m_hwnd, (HMENU)ID_CHECK_FG, m_hModule, NULL);
    SendMessage(m_hCheckFG, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    const wchar_t* fgModes[] = { L"Off", L"2x (DLSS-G)", L"3x (DLSS-MFG)", L"4x (DLSS-MFG)" };
    for (const wchar_t* m : fgModes) SendMessageW(m_hCheckFG, CB_ADDSTRING, 0, (LPARAM)m);
    SendMessageW(m_hCheckFG, CB_SETCURSEL, 3, 0); cy += Scale(46);

    AddLabel(kSectionQuality);
    AddSmallLabel(L"Sharpness:");
    m_hSliderSharpness = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, padding, cy, contentWidth - Scale(80), Scale(32), m_hwnd, (HMENU)ID_SLIDER_SHARP, m_hModule, NULL);
    SendMessageW(m_hSliderSharpness, TBM_SETRANGE, TRUE, MAKELONG(0, 100)); SendMessageW(m_hSliderSharpness, TBM_SETPOS, TRUE, 50);
    m_hLabelSharpnessVal = AddValueLabel(padding + contentWidth - Scale(70), cy + Scale(4), Scale(70));
    cy += Scale(44);

    AddSmallLabel(L"Texture Detail (LOD Bias):");
    m_hSliderLOD = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, padding, cy, contentWidth - Scale(80), Scale(32), m_hwnd, (HMENU)ID_SLIDER_LOD, m_hModule, NULL);
    SendMessageW(m_hSliderLOD, TBM_SETRANGE, TRUE, MAKELONG(0, 30)); SendMessageW(m_hSliderLOD, TBM_SETPOS, TRUE, 10);
    m_hLabelLODVal = AddValueLabel(padding + contentWidth - Scale(70), cy + Scale(4), Scale(70));
    cy += Scale(52);

    m_hLabelFPS = CreateWindowW(L"STATIC", L"FPS: ...", WS_VISIBLE | WS_CHILD | SS_CENTER, padding, cy, contentWidth, Scale(20), m_hwnd, NULL, m_hModule, NULL);
    SendMessage(m_hLabelFPS, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    cy += Scale(26);

    m_hLabelCamera = CreateWindowW(L"STATIC", L"Camera: ...", WS_VISIBLE | WS_CHILD | SS_CENTER, padding, cy, contentWidth, Scale(20), m_hwnd, NULL, m_hModule, NULL);
    SendMessage(m_hLabelCamera, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    cy += Scale(30);

    m_hwndDebugPanel = CreateWindowW(L"STATIC", L"", WS_VISIBLE | WS_CHILD, padding, cy, contentWidth, Scale(56), m_hwnd, NULL, m_hModule, NULL);
    m_hLabelCameraScore = CreateWindowW(L"STATIC", L"Camera Score: ...", WS_VISIBLE | WS_CHILD | SS_LEFT, padding + Scale(8), cy + Scale(4), contentWidth - Scale(16), Scale(20), m_hwnd, NULL, m_hModule, NULL);
    m_hLabelCameraAge = CreateWindowW(L"STATIC", L"Camera Age: ...", WS_VISIBLE | WS_CHILD | SS_LEFT, padding + Scale(8), cy + Scale(28), contentWidth - Scale(16), Scale(20), m_hwnd, NULL, m_hModule, NULL);
    SendMessage(m_hLabelCameraScore, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    SendMessage(m_hLabelCameraAge, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    SendMessage(m_hwndDebugPanel, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    cy += Scale(68);

    // Use OWNERDRAW for Button to style it
    m_hBtnExpand = CreateWindowW(L"BUTTON", L"Advanced Settings >>", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, padding, cy, contentWidth, Scale(30), m_hwnd, (HMENU)ID_BTN_EXPAND, m_hModule, NULL);
    cy += Scale(40);

    m_hCheckReflex = CreateWindowW(L"BUTTON", L"NVIDIA Reflex Boost", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, Scale(24), m_hwnd, (HMENU)ID_CHECK_REFLEX, m_hModule, NULL);
    SendMessage(m_hCheckReflex, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += Scale(28);
    
    m_hCheckHUDFix = CreateWindowW(L"BUTTON", L"HUD Masking", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, Scale(24), m_hwnd, (HMENU)ID_CHECK_HUD, m_hModule, NULL);
    SendMessage(m_hCheckHUDFix, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += Scale(28);

    m_hCheckDebug = CreateWindowW(L"BUTTON", L"Show Resource Debug Info", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, Scale(24), m_hwnd, (HMENU)ID_CHECK_DEBUG, m_hModule, NULL);
    SendMessage(m_hCheckDebug, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += Scale(28);

    m_hLabelLogVerbosity = CreateWindowW(L"STATIC", L"Logging Verbosity:", WS_VISIBLE | WS_CHILD | SS_LEFT, padding, cy, contentWidth, Scale(20), m_hwnd, NULL, m_hModule, NULL);
    SendMessage(m_hLabelLogVerbosity, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += Scale(22);
    m_hComboLogVerbosity = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, padding, cy, contentWidth, Scale(200), m_hwnd, (HMENU)ID_COMBO_LOGVERB, m_hModule, NULL);
    SendMessage(m_hComboLogVerbosity, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    const wchar_t* logModes[] = { L"Quiet", L"Normal", L"Verbose" };
    for (const wchar_t* mode : logModes) SendMessageW(m_hComboLogVerbosity, CB_ADDSTRING, 0, (LPARAM)mode);
    cy += Scale(40);

    m_hCheckQuietScan = CreateWindowW(L"BUTTON", L"Quiet Resource Scan Logs", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, Scale(24), m_hwnd, (HMENU)ID_CHECK_QUIETSCAN, m_hModule, NULL);
    SendMessage(m_hCheckQuietScan, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += Scale(28);

    m_hCheckDebugMode = CreateWindowW(L"BUTTON", L"Debug Mode (Verbose + Debug Window)", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, Scale(24), m_hwnd, (HMENU)ID_CHECK_DEBUGMODE, m_hModule, NULL);
    SendMessage(m_hCheckDebugMode, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += Scale(28);

    AddLabel(kSectionOverlay);
    m_hCheckShowFPS = CreateWindowW(L"BUTTON", L"Show FPS Overlay (F6)", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, Scale(24), m_hwnd, (HMENU)ID_CHECK_SHOWFPS, m_hModule, NULL);
    SendMessage(m_hCheckShowFPS, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += Scale(28);
    m_hCheckShowVignette = CreateWindowW(L"BUTTON", L"Show Vignette (F7)", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, Scale(24), m_hwnd, (HMENU)ID_CHECK_SHOWVIG, m_hModule, NULL);
    SendMessage(m_hCheckShowVignette, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += Scale(28);

    AddSmallLabel(L"Vignette Intensity:");
    m_hSliderVignetteIntensity = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, padding, cy, contentWidth - Scale(80), Scale(32), m_hwnd, (HMENU)ID_SLIDER_VIG_INT, m_hModule, NULL);
    SendMessageW(m_hSliderVignetteIntensity, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    m_hLabelVignetteIntensityVal = AddValueLabel(padding + contentWidth - Scale(70), cy + Scale(4), Scale(70));
    cy += Scale(44);

    AddSmallLabel(L"Vignette Radius:");
    m_hSliderVignetteRadius = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, padding, cy, contentWidth - Scale(80), Scale(32), m_hwnd, (HMENU)ID_SLIDER_VIG_RAD, m_hModule, NULL);
    SendMessageW(m_hSliderVignetteRadius, TBM_SETRANGE, TRUE, MAKELONG(20, 100));
    m_hLabelVignetteRadiusVal = AddValueLabel(padding + contentWidth - Scale(70), cy + Scale(4), Scale(70));
    cy += Scale(44);

    AddSmallLabel(L"Vignette Softness:");
    m_hSliderVignetteSoftness = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, padding, cy, contentWidth - Scale(80), Scale(32), m_hwnd, (HMENU)ID_SLIDER_VIG_SOFT, m_hModule, NULL);
    SendMessageW(m_hSliderVignetteSoftness, TBM_SETRANGE, TRUE, MAKELONG(5, 100));
    m_hLabelVignetteSoftnessVal = AddValueLabel(padding + contentWidth - Scale(70), cy + Scale(4), Scale(70));
    cy += Scale(48);

    m_hLabelHotkeys = CreateWindowW(L"STATIC", L"Hotkeys: F5 Menu  |  F6 FPS  |  F7 Vignette", WS_VISIBLE | WS_CHILD | SS_CENTER, padding, cy, contentWidth, Scale(18), m_hwnd, NULL, m_hModule, NULL);
    SendMessage(m_hLabelHotkeys, WM_SETFONT, (WPARAM)m_hFontSmall, TRUE);
    cy += Scale(24);

    m_hBtnReset = CreateWindowW(L"BUTTON", L"Reset to Defaults", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, padding, cy, contentWidth, Scale(28), m_hwnd, (HMENU)ID_BTN_RESET, m_hModule, NULL);
    cy += Scale(34);
    
    UpdateControls();
    UpdateDebugPanel();
    ShowWindow(m_hCheckReflex, SW_HIDE);
    ShowWindow(m_hCheckHUDFix, SW_HIDE);
    ShowWindow(m_hCheckDebug, SW_HIDE);
    ShowWindow(m_hLabelLogVerbosity, SW_HIDE);
    ShowWindow(m_hComboLogVerbosity, SW_HIDE);
    ShowWindow(m_hCheckQuietScan, SW_HIDE);
    ShowWindow(m_hCheckDebugMode, SW_HIDE);
    ShowWindow(m_hBtnReset, SW_HIDE);
    CreateTooltips();
        if (cfg.uiExpanded) {
            m_expanded = true;
            SetWindowTextW(m_hBtnExpand, L"<< Collapse");
            int show = SW_SHOW;
            ShowWindow(m_hCheckReflex, show);
            ShowWindow(m_hCheckHUDFix, show);
            ShowWindow(m_hCheckDebug, show);
            ShowWindow(m_hLabelLogVerbosity, show);
            ShowWindow(m_hComboLogVerbosity, show);
            ShowWindow(m_hCheckQuietScan, show);
            ShowWindow(m_hCheckDebugMode, show);
            ShowWindow(m_hBtnReset, show);
            ShowWindow(m_hCheckShowFPS, show);
            ShowWindow(m_hCheckShowVignette, show);
            ShowWindow(m_hSliderVignetteIntensity, show);
            ShowWindow(m_hSliderVignetteRadius, show);
            ShowWindow(m_hSliderVignetteSoftness, show);
            ShowWindow(m_hLabelVignetteIntensityVal, show);
            ShowWindow(m_hLabelVignetteRadiusVal, show);
            ShowWindow(m_hLabelVignetteSoftnessVal, show);
            ShowWindow(m_hLabelHotkeys, show);
        } else {
            ShowWindow(m_hCheckShowFPS, SW_HIDE);
            ShowWindow(m_hCheckShowVignette, SW_HIDE);
            ShowWindow(m_hSliderVignetteIntensity, SW_HIDE);
            ShowWindow(m_hSliderVignetteRadius, SW_HIDE);
            ShowWindow(m_hSliderVignetteSoftness, SW_HIDE);
            ShowWindow(m_hLabelVignetteIntensityVal, SW_HIDE);
            ShowWindow(m_hLabelVignetteRadiusVal, SW_HIDE);
            ShowWindow(m_hLabelVignetteSoftnessVal, SW_HIDE);
            ShowWindow(m_hLabelHotkeys, SW_HIDE);
        }
    ShowWindow(m_hwnd, cfg.uiVisible ? SW_SHOW : SW_HIDE);
    m_visible = cfg.uiVisible;
}

// ... FPS and Vignette methods remain similar ...
void OverlayUI::CreateVignetteWindow() {
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    m_hwndVignette = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE, L"DLSS4ProxyVignette", L"", WS_POPUP, 0, 0, w, h, NULL, NULL, m_hModule, NULL);
    UpdateVignette(true);
}
void OverlayUI::ToggleVignette() {
    m_showVignette = !m_showVignette;
    ConfigManager::Get().Data().showVignette = m_showVignette;
    ConfigManager::Get().Save();
    if (m_hCheckShowVignette) SendMessageW(m_hCheckShowVignette, BM_SETCHECK, m_showVignette ? BST_CHECKED : BST_UNCHECKED, 0);
    m_vignetteDirty = true;
    if(m_hwndVignette) ShowWindow(m_hwndVignette, m_showVignette ? SW_SHOW : SW_HIDE);
}
void OverlayUI::DrawVignette() { UpdateVignette(false); }

void OverlayUI::CreateFPSWindow() {
    m_hwndFPS = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE, L"DLSS4ProxyFPS", L"", WS_POPUP, Scale(20), Scale(20), Scale(320), Scale(80), NULL, NULL, m_hModule, NULL);
    SetLayeredWindowAttributes(m_hwndFPS, 0, 210, LWA_ALPHA);
}
void OverlayUI::ToggleFPS() {
    m_showFPS = !m_showFPS;
    ConfigManager::Get().Data().showFPS = m_showFPS;
    ConfigManager::Get().Save();
    if (m_hCheckShowFPS) SendMessageW(m_hCheckShowFPS, BM_SETCHECK, m_showFPS ? BST_CHECKED : BST_UNCHECKED, 0);
    if(m_hwndFPS) ShowWindow(m_hwndFPS, m_showFPS ? SW_SHOW : SW_HIDE);
}
void OverlayUI::DrawFPSOverlay() {
    if(!m_hwndFPS || !m_showFPS) return;
    HDC hdc = GetDC(m_hwndFPS); RECT rect; GetClientRect(m_hwndFPS, &rect);
    HBRUSH bg = CreateSolidBrush(RGB(0,0,0)); FillRect(hdc, &rect, bg); DeleteObject(bg);
    SetTextColor(hdc, RGB(212, 175, 55)); SetBkMode(hdc, TRANSPARENT);
    HFONT hOld = (HFONT)SelectObject(hdc, m_hFontFPS);
    wchar_t buf[64]; 
    int mult = StreamlineIntegration::Get().GetFrameGenMultiplier(); if(mult<1) mult=1;
    swprintf_s(buf, L"%.0f -> %.0f FPS", m_cachedTotalFPS/(float)mult, m_cachedTotalFPS);
    DrawTextW(hdc, buf, -1, &rect, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(hdc, hOld); ReleaseDC(m_hwndFPS, hdc);
}

void OverlayUI::CreateDebugWindow() {
    // Large window on the right side
    int w = 500;
    int h = 600;
    int x = GetSystemMetrics(SM_CXSCREEN) - w - 20;
    int y = 20;
    
    m_hwndDebug = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, 
        L"DLSS4ProxyDebug", L"", 
        WS_POPUP, x, y, w, h, NULL, NULL, m_hModule, NULL
    );
    SetLayeredWindowAttributes(m_hwndDebug, 0, 220, LWA_ALPHA); // Semi-transparent
}

void OverlayUI::UpdateDebugInfo() {
    if (!m_hwndDebug || !m_showDebug) return;
    
    std::string debugInfo = ResourceDetector::Get().GetDebugInfo();
    
    HDC hdc = GetDC(m_hwndDebug);
    RECT rect; GetClientRect(m_hwndDebug, &rect);
    
    // Clear
    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20)); 
    FillRect(hdc, &rect, bg); 
    DeleteObject(bg);
    
    SetTextColor(hdc, RGB(0, 255, 0)); // Green text
    SetBkMode(hdc, TRANSPARENT);
    
    HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    HFONT hOld = (HFONT)SelectObject(hdc, hFont);
    
    // Convert string to wstring for DrawTextW
    std::wstring wText;
    if (!debugInfo.empty()) {
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &debugInfo[0], (int)debugInfo.size(), NULL, 0);
        wText.resize(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &debugInfo[0], (int)debugInfo.size(), &wText[0], size_needed);
    } else {
        wText = L"No debug info available yet...";
    }
    
    RECT textRect = rect;
    textRect.left += 10; textRect.top += 10;
    DrawTextW(hdc, wText.c_str(), -1, &textRect, DT_LEFT);
    
    SelectObject(hdc, hOld); 
    DeleteObject(hFont); 
    ReleaseDC(m_hwndDebug, hdc);
}

void OverlayUI::UpdateDebugPanel() {
    if (!m_hLabelCameraScore || !m_hLabelCameraAge) return;
    float score = 0.0f;
    uint64_t frame = 0;
    bool hasStats = GetLastCameraStats(score, frame);
    uint32_t currentFrame = StreamlineIntegration::Get().GetFrameCount();
    uint64_t age = hasStats && currentFrame >= frame ? (currentFrame - frame) : 0;
    wchar_t scoreBuf[64];
    wchar_t ageBuf[64];
    if (hasStats) {
    swprintf_s(scoreBuf, L"Camera Score: %.2f", score);
    swprintf_s(ageBuf, L"Camera Age: %llu frames", static_cast<unsigned long long>(age));
    } else {
        swprintf_s(scoreBuf, L"Camera Score: N/A");
        swprintf_s(ageBuf, L"Camera Age: N/A");
    }
    SetWindowTextW(m_hLabelCameraScore, scoreBuf);
    SetWindowTextW(m_hLabelCameraAge, ageBuf);
}

void OverlayUI::SetFPS(float gameFps, float totalFps) { 
    m_cachedTotalFPS = totalFps; 
    static uint64_t s_lastOverlay = 0;
    uint64_t nowOverlay = GetTickCount64();
    if (nowOverlay - s_lastOverlay >= 200 || m_vignetteDirty) {
        DrawFPSOverlay(); 
        DrawVignette();
        s_lastOverlay = nowOverlay;
    }
    UpdateDebugPanel();
    static uint64_t s_lastDebugUpdate = 0;
    uint64_t now = GetTickCount64();
    if (m_showDebug && (now - s_lastDebugUpdate) >= 500) { UpdateDebugInfo(); s_lastDebugUpdate = now; }
    if(m_hLabelFPS) { wchar_t buf[64]; swprintf_s(buf, L"%.0f FPS", totalFps); SetWindowTextW(m_hLabelFPS, buf); }
    if (m_hLabelCamera) {
        wchar_t buf[128];
        swprintf_s(buf, L"Camera: %s (J %.3f, %.3f)", m_cachedCamera ? L"OK" : L"Missing", m_cachedJitterX, m_cachedJitterY);
        SetWindowTextW(m_hLabelCamera, buf);
    }
}

void OverlayUI::SetCameraStatus(bool hasCamera, float jitterX, float jitterY) {
    m_cachedCamera = hasCamera;
    m_cachedJitterX = jitterX;
    m_cachedJitterY = jitterY;
    if (m_hLabelCamera) {
        wchar_t buf[128];
        swprintf_s(buf, L"Camera: %s (J %.3f, %.3f)", hasCamera ? L"OK" : L"Missing", jitterX, jitterY);
        SetWindowTextW(m_hLabelCamera, buf);
    }
}
void OverlayUI::ToggleVisibility() {
    if(m_hwnd) {
        m_visible = !m_visible;
        ModConfig& cfg = ConfigManager::Get().Data();
        cfg.uiVisible = m_visible;
        ConfigManager::Get().Save();
        ShowWindow(m_hwnd, m_visible?SW_SHOW:SW_HIDE);
        if(m_visible){ SetForegroundWindow(m_hwnd); SetFocus(m_hwnd); }
    }
}
void OverlayUI::UpdateControls() {
    ModConfig& cfg = ConfigManager::Get().Data();
    StreamlineIntegration& sli = StreamlineIntegration::Get();

    // 1. Hardware Support Checks
    bool dlssSup = sli.IsDLSSSupported();
    bool fgSup = sli.IsFrameGenSupported();
    bool reflexSup = sli.IsReflexSupported();

    // 2. Enable/Disable Controls
    if (m_hComboDLSS) EnableWindow(m_hComboDLSS, dlssSup);
    if (m_hCheckFG) EnableWindow(m_hCheckFG, fgSup);
    if (m_hCheckReflex) EnableWindow(m_hCheckReflex, reflexSup);

    // 3. Update Values (Prefer Runtime State)
    if (m_hComboDLSS) SendMessageW(m_hComboDLSS, CB_SETCURSEL, sli.GetDLSSModeIndex(), 0);
    if (m_hComboPreset) SendMessageW(m_hComboPreset, CB_SETCURSEL, sli.GetDLSSPresetIndex(), 0);
    
    if (m_hCheckFG) {
        int fgMult = sli.GetFrameGenMultiplier(); // Use runtime value (0 if unsupported)
        int fgIndex = 0;
        if (fgMult == 2) fgIndex = 1;
        else if (fgMult == 3) fgIndex = 2;
        else if (fgMult == 4) fgIndex = 3;
        SendMessageW(m_hCheckFG, CB_SETCURSEL, fgIndex, 0);
    }

    if (m_hSliderSharpness) SendMessageW(m_hSliderSharpness, TBM_SETPOS, TRUE, (LPARAM)std::lround(cfg.sharpness * 100.0f));
    if (m_hSliderLOD) SendMessageW(m_hSliderLOD, TBM_SETPOS, TRUE, (LPARAM)std::lround(-cfg.lodBias * 10.0f));
    
    if (m_hCheckReflex) {
        // If not supported, force unchecked visually
        bool checked = reflexSup ? cfg.reflexEnabled : false; 
        SendMessageW(m_hCheckReflex, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    
    if (m_hCheckHUDFix) SendMessageW(m_hCheckHUDFix, BM_SETCHECK, cfg.hudFixEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    if (m_hComboLogVerbosity) SendMessageW(m_hComboLogVerbosity, CB_SETCURSEL, cfg.logVerbosity, 0);
    if (m_hCheckQuietScan) SendMessageW(m_hCheckQuietScan, BM_SETCHECK, cfg.quietResourceScan ? BST_CHECKED : BST_UNCHECKED, 0);
    if (m_hCheckDebugMode) SendMessageW(m_hCheckDebugMode, BM_SETCHECK, cfg.debugMode ? BST_CHECKED : BST_UNCHECKED, 0);
    if (m_hCheckDebug) SendMessageW(m_hCheckDebug, BM_SETCHECK, m_showDebug ? BST_CHECKED : BST_UNCHECKED, 0);
    if (m_hCheckShowFPS) SendMessageW(m_hCheckShowFPS, BM_SETCHECK, cfg.showFPS ? BST_CHECKED : BST_UNCHECKED, 0);
    if (m_hCheckShowVignette) SendMessageW(m_hCheckShowVignette, BM_SETCHECK, cfg.showVignette ? BST_CHECKED : BST_UNCHECKED, 0);
    if (m_hSliderVignetteIntensity) SendMessageW(m_hSliderVignetteIntensity, TBM_SETPOS, TRUE, (LPARAM)std::lround(cfg.vignetteIntensity * 100.0f));
    if (m_hSliderVignetteRadius) SendMessageW(m_hSliderVignetteRadius, TBM_SETPOS, TRUE, (LPARAM)std::lround(cfg.vignetteRadius * 100.0f));
    if (m_hSliderVignetteSoftness) SendMessageW(m_hSliderVignetteSoftness, TBM_SETPOS, TRUE, (LPARAM)std::lround(cfg.vignetteSoftness * 100.0f));
    if (m_hwndFPS) ShowWindow(m_hwndFPS, cfg.showFPS ? SW_SHOW : SW_HIDE);
    if (m_hwndVignette) ShowWindow(m_hwndVignette, cfg.showVignette ? SW_SHOW : SW_HIDE);
    m_showFPS = cfg.showFPS;
    m_showVignette = cfg.showVignette;
    if (m_showVignette) m_vignetteDirty = true;
    UpdateValueLabels();
}

LRESULT CALLBACK OverlayUI::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    OverlayUI& ui = OverlayUI::Get();
    
    // CUSTOM PAINTING
    if (uMsg == WM_PAINT && hwnd == ui.m_hwnd) {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect; GetClientRect(hwnd, &rect);
        
        // Background
        FillRect(hdc, &rect, ui.m_brBack);
        
        // Header
        RECT headerRect = rect; headerRect.bottom = ui.Scale(32);
        FillRect(hdc, &headerRect, ui.m_brHeader);
        
        // Header Text
        SetTextColor(hdc, COL_ACCENT); SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, ui.m_hFontHeader);
        RECT textRect = headerRect; textRect.left += 10;
        DrawTextW(hdc, L"DLSS 4.5 CONTROL PANEL", -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT generalRect = { ui.Scale(12), ui.Scale(40), rect.right - ui.Scale(12), ui.Scale(210) };
        RECT qualityRect = { ui.Scale(12), ui.Scale(220), rect.right - ui.Scale(12), ui.Scale(365) };
        RECT overlayRect = { ui.Scale(12), ui.Scale(375), rect.right - ui.Scale(12), rect.bottom - ui.Scale(12) };
        FillRect(hdc, &generalRect, ui.m_brPanel);
        FillRect(hdc, &qualityRect, ui.m_brPanelDark);
        FillRect(hdc, &overlayRect, ui.m_brPanel);
        
        // Border
        HBRUSH border = CreateSolidBrush(COL_ACCENT);
        FrameRect(hdc, &rect, border);
        DeleteObject(border);
        
        EndPaint(hwnd, &ps);
        return 0;
    }
    
    // DRAG HEADER TO MOVE
    if (uMsg == WM_NCHITTEST && hwnd == ui.m_hwnd) {
        LRESULT hit = DefWindowProcW(hwnd, uMsg, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            if (pt.y < ui.Scale(32)) return HTCAPTION; // Allow drag on top header
        }
        return hit;
    }

    if (uMsg == WM_DRAWITEM && (UINT)wParam == ID_BTN_EXPAND) {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        FillRect(pDIS->hDC, &pDIS->rcItem, ui.m_brButton);
        SetTextColor(pDIS->hDC, COL_TEXT); SetBkMode(pDIS->hDC, TRANSPARENT);
        wchar_t buf[64]; GetWindowTextW(pDIS->hwndItem, buf, 64);
        DrawTextW(pDIS->hDC, buf, -1, &pDIS->rcItem, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(pDIS->itemState & ODS_SELECTED) {
            RECT r = pDIS->rcItem; InflateRect(&r, -1, -1);
            DrawFocusRect(pDIS->hDC, &r);
        }
        return TRUE;
    }

    if (uMsg == WM_CTLCOLORSTATIC || uMsg == WM_CTLCOLORBTN) {
        HDC hdc = (HDC)wParam; SetTextColor(hdc, COL_TEXT); SetBkMode(hdc, TRANSPARENT);
        if ((HWND)lParam == ui.m_hwndDebugPanel) return (LRESULT)ui.m_brPanelDark;
        return (LRESULT)ui.m_brBack;
    }

    if (uMsg == WM_COMMAND) {
        int id = LOWORD(wParam); int code = HIWORD(wParam);
        if (id == ID_COMBO_DLSS && code == CBN_SELCHANGE) {
            int idx = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0); StreamlineIntegration::Get().SetDLSSModeIndex(idx);
            ConfigManager::Get().Data().dlssMode = idx; ConfigManager::Get().Save();
        } else if (id == ID_COMBO_PRESET && code == CBN_SELCHANGE) {
            int idx = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0); StreamlineIntegration::Get().SetDLSSPreset(idx);
            ConfigManager::Get().Data().dlssPreset = idx; ConfigManager::Get().Save();
        } else if (id == ID_CHECK_FG && code == CBN_SELCHANGE) {
            int idx = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
            StreamlineIntegration::Get().SetFrameGenMultiplier((idx==1)?2:(idx==2?3:(idx==3?4:0)));
            ConfigManager::Get().Data().frameGenMultiplier = (idx==1)?2:(idx==2?3:(idx==3?4:0)); ConfigManager::Get().Save();
        } else if (id == ID_BTN_EXPAND) {
            ui.m_expanded = !ui.m_expanded;
            ConfigManager::Get().Data().uiExpanded = ui.m_expanded;
            ConfigManager::Get().Save();
            SetWindowTextW(ui.m_hBtnExpand, ui.m_expanded ? L"<< Collapse" : L"Advanced Settings >>");
            int show = ui.m_expanded ? SW_SHOW : SW_HIDE;
            ShowWindow(ui.m_hCheckReflex, show); 
            ShowWindow(ui.m_hCheckHUDFix, show);
            ShowWindow(ui.m_hCheckDebug, show);
            ShowWindow(ui.m_hLabelLogVerbosity, show);
            ShowWindow(ui.m_hComboLogVerbosity, show);
            ShowWindow(ui.m_hCheckQuietScan, show);
            ShowWindow(ui.m_hCheckDebugMode, show);
            ShowWindow(ui.m_hBtnReset, show);
            ShowWindow(ui.m_hCheckShowFPS, show);
            ShowWindow(ui.m_hCheckShowVignette, show);
            ShowWindow(ui.m_hSliderVignetteIntensity, show);
            ShowWindow(ui.m_hSliderVignetteRadius, show);
            ShowWindow(ui.m_hSliderVignetteSoftness, show);
            ShowWindow(ui.m_hLabelVignetteIntensityVal, show);
            ShowWindow(ui.m_hLabelVignetteRadiusVal, show);
            ShowWindow(ui.m_hLabelVignetteSoftnessVal, show);
            ShowWindow(ui.m_hLabelHotkeys, show);
            RECT rect; GetWindowRect(ui.m_hwnd, &rect);
            SetWindowPos(ui.m_hwnd, NULL, 0, 0, rect.right-rect.left, ui.m_expanded ? ui.Scale(860) : ui.Scale(620), SWP_NOMOVE|SWP_NOZORDER);
        } else if (id == ID_CHECK_REFLEX && code == BN_CLICKED) {
            bool enabled = SendMessageW(ui.m_hCheckReflex, BM_GETCHECK, 0, 0) == BST_CHECKED;
            StreamlineIntegration::Get().SetReflexEnabled(enabled);
            ConfigManager::Get().Data().reflexEnabled = enabled; ConfigManager::Get().Save();
        } else if (id == ID_CHECK_HUD && code == BN_CLICKED) {
            bool enabled = SendMessageW(ui.m_hCheckHUDFix, BM_GETCHECK, 0, 0) == BST_CHECKED;
            StreamlineIntegration::Get().SetHUDFixEnabled(enabled);
            ConfigManager::Get().Data().hudFixEnabled = enabled; ConfigManager::Get().Save();
        } else if (id == ID_CHECK_DEBUG && code == BN_CLICKED) {
            ui.m_showDebug = SendMessageW(ui.m_hCheckDebug, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ShowWindow(ui.m_hwndDebug, ui.m_showDebug ? SW_SHOW : SW_HIDE);
        } else if (id == ID_COMBO_LOGVERB && code == CBN_SELCHANGE) {
            int idx = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
            ConfigManager::Get().Data().logVerbosity = idx;
            ConfigManager::Get().Save();
        } else if (id == ID_CHECK_QUIETSCAN && code == BN_CLICKED) {
            bool enabled = SendMessageW(ui.m_hCheckQuietScan, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ConfigManager::Get().Data().quietResourceScan = enabled;
            ConfigManager::Get().Save();
        } else if (id == ID_CHECK_DEBUGMODE && code == BN_CLICKED) {
            bool enabled = SendMessageW(ui.m_hCheckDebugMode, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ConfigManager::Get().Data().debugMode = enabled;
            if (enabled) ConfigManager::Get().Data().logVerbosity = 2;
            ui.ToggleDebugMode(enabled);
            ConfigManager::Get().Save();
            ui.UpdateControls();
        } else if (id == ID_BTN_RESET) {
            ConfigManager::Get().ResetToDefaults();
            ModConfig& cfg = ConfigManager::Get().Data();
            StreamlineIntegration::Get().SetDLSSModeIndex(cfg.dlssMode);
            StreamlineIntegration::Get().SetDLSSPreset(cfg.dlssPreset);
            StreamlineIntegration::Get().SetFrameGenMultiplier(cfg.frameGenMultiplier);
            StreamlineIntegration::Get().SetSharpness(cfg.sharpness);
            StreamlineIntegration::Get().SetLODBias(cfg.lodBias);
            StreamlineIntegration::Get().SetReflexEnabled(cfg.reflexEnabled);
            StreamlineIntegration::Get().SetHUDFixEnabled(cfg.hudFixEnabled);
            ui.ToggleDebugMode(cfg.debugMode);
            ui.m_showFPS = cfg.showFPS;
            ui.m_showVignette = cfg.showVignette;
            ShowWindow(ui.m_hwndFPS, cfg.showFPS ? SW_SHOW : SW_HIDE);
            ShowWindow(ui.m_hwndVignette, cfg.showVignette ? SW_SHOW : SW_HIDE);
            ui.UpdateVignette(true);
            ui.UpdateControls();
        } else if (id == ID_CHECK_SHOWFPS && code == BN_CLICKED) {
            bool enabled = SendMessageW(ui.m_hCheckShowFPS, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ui.m_showFPS = enabled;
            ConfigManager::Get().Data().showFPS = enabled;
            ConfigManager::Get().Save();
            ShowWindow(ui.m_hwndFPS, enabled ? SW_SHOW : SW_HIDE);
        } else if (id == ID_CHECK_SHOWVIG && code == BN_CLICKED) {
            bool enabled = SendMessageW(ui.m_hCheckShowVignette, BM_GETCHECK, 0, 0) == BST_CHECKED;
            ui.m_showVignette = enabled;
            ConfigManager::Get().Data().showVignette = enabled;
            ConfigManager::Get().Save();
            ShowWindow(ui.m_hwndVignette, enabled ? SW_SHOW : SW_HIDE);
            ui.UpdateVignette(true);
        }
    }
    if (uMsg == WM_HSCROLL) {
        HWND hSlider = (HWND)lParam; int pos = SendMessageW(hSlider, TBM_GETPOS, 0, 0);
        if(hSlider == ui.m_hSliderSharpness) { StreamlineIntegration::Get().SetSharpness((float)pos/100.0f); ConfigManager::Get().Data().sharpness = (float)pos / 100.0f; ConfigManager::Get().Save(); ui.UpdateValueLabels(); }
        else if(hSlider == ui.m_hSliderLOD) { StreamlineIntegration::Get().SetLODBias(-((float)pos/10.0f)); ConfigManager::Get().Data().lodBias = -((float)pos / 10.0f); ConfigManager::Get().Save(); ui.UpdateValueLabels(); }
        else if(hSlider == ui.m_hSliderVignetteIntensity) { ConfigManager::Get().Data().vignetteIntensity = (float)pos / 100.0f; ConfigManager::Get().Save(); ui.UpdateValueLabels(); ui.UpdateVignette(true); }
        else if(hSlider == ui.m_hSliderVignetteRadius) { ConfigManager::Get().Data().vignetteRadius = (float)pos / 100.0f; ConfigManager::Get().Save(); ui.UpdateValueLabels(); ui.UpdateVignette(true); }
        else if(hSlider == ui.m_hSliderVignetteSoftness) { ConfigManager::Get().Data().vignetteSoftness = (float)pos / 100.0f; ConfigManager::Get().Save(); ui.UpdateValueLabels(); ui.UpdateVignette(true); }
    }
    if (uMsg == WM_DISPLAYCHANGE) {
        ui.UpdateVignette(true);
    }
    if (uMsg == WM_MOVE && hwnd == ui.m_hwnd) {
        RECT rect; GetWindowRect(ui.m_hwnd, &rect);
        ConfigManager::Get().Data().uiPosX = rect.left;
        ConfigManager::Get().Data().uiPosY = rect.top;
        ConfigManager::Get().Save();
    }
    if (uMsg == WM_DPICHANGED && hwnd == ui.m_hwnd) {
        ui.m_dpi = HIWORD(wParam);
        ui.m_scale = ui.m_dpi / 96.0f;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int OverlayUI::Scale(int value) const {
    return (int)std::lround(value * m_scale);
}

void OverlayUI::UpdateValueLabels() {
    ModConfig& cfg = ConfigManager::Get().Data();
    if (m_hLabelSharpnessVal) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.2f", cfg.sharpness);
        SetWindowTextW(m_hLabelSharpnessVal, buf);
    }
    if (m_hLabelLODVal) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.2f", cfg.lodBias);
        SetWindowTextW(m_hLabelLODVal, buf);
    }
    if (m_hLabelVignetteIntensityVal) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.2f", cfg.vignetteIntensity);
        SetWindowTextW(m_hLabelVignetteIntensityVal, buf);
    }
    if (m_hLabelVignetteRadiusVal) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.2f", cfg.vignetteRadius);
        SetWindowTextW(m_hLabelVignetteRadiusVal, buf);
    }
    if (m_hLabelVignetteSoftnessVal) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.2f", cfg.vignetteSoftness);
        SetWindowTextW(m_hLabelVignetteSoftnessVal, buf);
    }
}

void OverlayUI::CreateTooltips() {
    if (m_hTooltips) return;
    m_hTooltips = CreateWindowExW(0, TOOLTIPS_CLASS, NULL, WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hwnd, NULL, m_hModule, NULL);
    if (!m_hTooltips) return;
    SendMessageW(m_hTooltips, TTM_SETMAXTIPWIDTH, 0, Scale(300));
    AddTooltip(m_hComboDLSS, L"Select the DLSS quality mode.");
    AddTooltip(m_hComboPreset, L"Overrides the DLSS preset for image quality tuning.");
    AddTooltip(m_hCheckFG, L"Frame generation multiplier (requires RTX 40/50).");
    AddTooltip(m_hSliderSharpness, L"Adjust sharpening strength (0.0-1.0).");
    AddTooltip(m_hSliderLOD, L"Negative values improve texture detail.");
    AddTooltip(m_hCheckShowFPS, L"Shows the FPS overlay.");
    AddTooltip(m_hCheckShowVignette, L"Shows the vignette overlay.");
    AddTooltip(m_hSliderVignetteIntensity, L"Vignette darkness at corners.");
    AddTooltip(m_hSliderVignetteRadius, L"Inner radius before darkening starts.");
    AddTooltip(m_hSliderVignetteSoftness, L"How soft the vignette falloff is.");
}

void OverlayUI::AddTooltip(HWND target, const wchar_t* text) {
    if (!m_hTooltips || !target || !text) return;
    TOOLINFOW ti = {0};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = m_hwnd;
    ti.uId = (UINT_PTR)target;
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(m_hTooltips, TTM_ADDTOOL, 0, (LPARAM)&ti);
}

void OverlayUI::ReleaseVignetteResources() {
    if (m_hVignetteDC) {
        DeleteDC(m_hVignetteDC);
        m_hVignetteDC = nullptr;
    }
    if (m_hVignetteBitmap) {
        DeleteObject(m_hVignetteBitmap);
        m_hVignetteBitmap = nullptr;
    }
    m_vignetteBits = nullptr;
}

void OverlayUI::UpdateVignette(bool force) {
    if (!m_hwndVignette) return;
    if (!m_showVignette) return;
    ModConfig& cfg = ConfigManager::Get().Data();
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if (!force && !m_vignetteDirty && w == m_vignetteW && h == m_vignetteH && x == m_vignetteX && y == m_vignetteY) {
        return;
    }
    if (force || w != m_vignetteW || h != m_vignetteH || x != m_vignetteX || y != m_vignetteY) {
        ReleaseVignetteResources();
        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        m_hVignetteDC = CreateCompatibleDC(NULL);
        m_hVignetteBitmap = CreateDIBSection(m_hVignetteDC, &bmi, DIB_RGB_COLORS, &m_vignetteBits, NULL, 0);
        SelectObject(m_hVignetteDC, m_hVignetteBitmap);
        m_vignetteW = w;
        m_vignetteH = h;
        m_vignetteX = x;
        m_vignetteY = y;
    }
    if (!m_vignetteBits || !m_hVignetteDC) return;
    uint32_t* pixels = reinterpret_cast<uint32_t*>(m_vignetteBits);
    float cx = (w - 1) * 0.5f;
    float cy = (h - 1) * 0.5f;
    float maxR = std::min(cx, cy);
    float radius = std::clamp(cfg.vignetteRadius, 0.2f, 1.0f);
    float softness = std::clamp(cfg.vignetteSoftness, 0.05f, 1.0f);
    float intensity = std::clamp(cfg.vignetteIntensity, 0.0f, 1.0f);
    float inner = maxR * radius;
    float outer = std::max(inner + 1.0f, maxR * std::min(radius + softness, 1.0f));
    float range = outer - inner;
    for (int y = 0; y < h; ++y) {
        float dy = y - cy;
        for (int x = 0; x < w; ++x) {
            float dx = x - cx;
            float dist = std::sqrt(dx * dx + dy * dy);
            float t = (dist - inner) / range;
            t = std::clamp(t, 0.0f, 1.0f);
            float alpha = intensity * t;
            uint8_t a = (uint8_t)std::lround(alpha * 255.0f);
            pixels[y * w + x] = (a << 24);
        }
    }
    POINT pos = {x, y};
    SIZE size = {w, h};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m_hwndVignette, NULL, &pos, &size, m_hVignetteDC, &src, 0, &blend, ULW_ALPHA);
    m_vignetteDirty = false;
}
