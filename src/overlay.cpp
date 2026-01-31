#include "overlay.h"
#include "streamline_integration.h"
#include "config_manager.h"
#include "logger.h"
#include <commctrl.h>
#include <stdio.h>
#include <cmath>

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

// Colors (ImGui Style)
#define COL_BG      RGB(30, 30, 30)
#define COL_HEADER  RGB(10, 15, 20) // Valhalla Dark
#define COL_BTN     RGB(40, 50, 60)
#define COL_TEXT    RGB(220, 220, 220)
#define COL_ACCENT  RGB(212, 175, 55) // Gold

OverlayUI& OverlayUI::Get() {
    static OverlayUI instance;
    return instance;
}

void OverlayUI::Initialize(HMODULE hModule) {
    if (m_initialized) return;
    m_hModule = hModule;
    
    // Create Resources
    m_brBack = CreateSolidBrush(COL_BG);
    m_brHeader = CreateSolidBrush(COL_HEADER);
    m_brButton = CreateSolidBrush(COL_BTN);
    m_hFontUI = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_hFontHeader = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    m_hThread = CreateThread(NULL, 0, UIThreadEntry, this, 0, NULL);
    m_initialized = true;
}

DWORD WINAPI OverlayUI::UIThreadEntry(LPVOID lpParam) {
    OverlayUI* pThis = (OverlayUI*)lpParam;
    pThis->UIThreadLoop();
    return 0;
}

void OverlayUI::UIThreadLoop() {
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
    WNDCLASSEXW wcVig = {0}; wcVig.cbSize = sizeof(WNDCLASSEXW); wcVig.lpfnWndProc = WindowProc; wcVig.hInstance = m_hModule; wcVig.lpszClassName = L"DLSS4ProxyVignette"; wcVig.hbrBackground = CreateSolidBrush(RGB(255, 100, 0)); RegisterClassExW(&wcVig);
    
    CreateOverlayWindow();
    CreateFPSWindow();
    CreateVignetteWindow(); 
    
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void OverlayUI::CreateOverlayWindow() {
    int width = 360;
    int height = 530;
    int x = 50; int y = 50;

    // WS_POPUP only - No standard Windows borders/caption
    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 
        L"DLSS4ProxyOverlay", L"DLSS 4.5",
        WS_POPUP | WS_VISIBLE,
        x, y, width, height, NULL, NULL, m_hModule, NULL
    );

    int padding = 15; int cy = 40; // Start below header
    int contentWidth = width - 2*padding;

    // Controls
    auto AddLabel = [&](const wchar_t* text) {
        HWND h = CreateWindowW(L"STATIC", text, WS_VISIBLE | WS_CHILD | SS_LEFT, padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
        cy += 22;
    };

    AddLabel(L"DLSS Quality Mode:");
    m_hComboDLSS = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, padding, cy, contentWidth, 200, m_hwnd, (HMENU)ID_COMBO_DLSS, m_hModule, NULL);
    SendMessage(m_hComboDLSS, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    const wchar_t* modes[] = { L"Off", L"Max Performance", L"Balanced", L"Max Quality", L"Ultra Quality", L"DLAA" };
    for (const wchar_t* mode : modes) SendMessageW(m_hComboDLSS, CB_ADDSTRING, 0, (LPARAM)mode);
    cy += 40;

    AddLabel(L"DLSS Preset:");
    m_hComboPreset = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, padding, cy, contentWidth, 200, m_hwnd, (HMENU)ID_COMBO_PRESET, m_hModule, NULL);
    SendMessage(m_hComboPreset, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    const wchar_t* presets[] = { L"Default", L"Preset A", L"Preset B", L"Preset C", L"Preset D", L"Preset E", L"Preset F", L"Preset G" };
    for (const wchar_t* preset : presets) SendMessageW(m_hComboPreset, CB_ADDSTRING, 0, (LPARAM)preset);
    cy += 40;

    AddLabel(L"Frame Generation:");
    m_hCheckFG = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, padding, cy, contentWidth, 200, m_hwnd, (HMENU)ID_CHECK_FG, m_hModule, NULL);
    SendMessage(m_hCheckFG, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    const wchar_t* fgModes[] = { L"Off", L"2x (DLSS-G)", L"3x (DLSS-MFG)", L"4x (DLSS-MFG)" };
    for (const wchar_t* m : fgModes) SendMessageW(m_hCheckFG, CB_ADDSTRING, 0, (LPARAM)m);
    SendMessageW(m_hCheckFG, CB_SETCURSEL, 3, 0); cy += 40;

    AddLabel(L"Sharpness:");
    m_hSliderSharpness = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, padding, cy, contentWidth, 30, m_hwnd, (HMENU)ID_SLIDER_SHARP, m_hModule, NULL);
    SendMessageW(m_hSliderSharpness, TBM_SETRANGE, TRUE, MAKELONG(0, 100)); SendMessageW(m_hSliderSharpness, TBM_SETPOS, TRUE, 50); cy += 40;

    AddLabel(L"Texture Detail (LOD Bias):");
    m_hSliderLOD = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, padding, cy, contentWidth, 30, m_hwnd, (HMENU)ID_SLIDER_LOD, m_hModule, NULL);
    SendMessageW(m_hSliderLOD, TBM_SETRANGE, TRUE, MAKELONG(0, 30)); SendMessageW(m_hSliderLOD, TBM_SETPOS, TRUE, 10); cy += 45;

    m_hLabelFPS = CreateWindowW(L"STATIC", L"FPS: ...", WS_VISIBLE | WS_CHILD | SS_CENTER, padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL);
    SendMessage(m_hLabelFPS, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += 30;

    m_hLabelCamera = CreateWindowW(L"STATIC", L"Camera: ...", WS_VISIBLE | WS_CHILD | SS_CENTER, padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL);
    SendMessage(m_hLabelCamera, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += 30;

    // Use OWNERDRAW for Button to style it
    m_hBtnExpand = CreateWindowW(L"BUTTON", L"Advanced Settings >>", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, padding, cy, contentWidth, 30, m_hwnd, (HMENU)ID_BTN_EXPAND, m_hModule, NULL);
    cy += 40;

    m_hCheckReflex = CreateWindowW(L"BUTTON", L"NVIDIA Reflex Boost", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, 25, m_hwnd, (HMENU)ID_CHECK_REFLEX, m_hModule, NULL);
    SendMessage(m_hCheckReflex, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += 30;
    
    m_hCheckHUDFix = CreateWindowW(L"BUTTON", L"HUD Masking", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, 25, m_hwnd, (HMENU)ID_CHECK_HUD, m_hModule, NULL);
    SendMessage(m_hCheckHUDFix, WM_SETFONT, (WPARAM)m_hFontUI, TRUE);
    cy += 30;
    
    UpdateControls();
    ShowWindow(m_hwnd, SW_HIDE); // Start Hidden
}

// ... FPS and Vignette methods remain similar ...
void OverlayUI::CreateVignetteWindow() {
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN); int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    m_hwndVignette = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE, L"DLSS4ProxyVignette", L"", WS_POPUP, 0, 0, w, h, NULL, NULL, m_hModule, NULL);
    CreateWindowW(L"STATIC", L"MODDED BY SERGREY", WS_VISIBLE|WS_CHILD|SS_RIGHT, w-450, h-80, 400, 50, m_hwndVignette, NULL, m_hModule, NULL);
    SetLayeredWindowAttributes(m_hwndVignette, 0, 80, LWA_ALPHA); 
}
void OverlayUI::ToggleVignette() { m_showVignette = !m_showVignette; if(m_hwndVignette) ShowWindow(m_hwndVignette, m_showVignette ? SW_SHOW : SW_HIDE); }
void OverlayUI::DrawVignette() {}

void OverlayUI::CreateFPSWindow() {
    m_hwndFPS = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE, L"DLSS4ProxyFPS", L"", WS_POPUP, 20, 20, 400, 80, NULL, NULL, m_hModule, NULL);
    SetLayeredWindowAttributes(m_hwndFPS, 0, 200, LWA_ALPHA);
}
void OverlayUI::ToggleFPS() { m_showFPS = !m_showFPS; if(m_hwndFPS) ShowWindow(m_hwndFPS, m_showFPS ? SW_SHOW : SW_HIDE); }
void OverlayUI::DrawFPSOverlay() {
    if(!m_hwndFPS || !m_showFPS) return;
    HDC hdc = GetDC(m_hwndFPS); RECT rect; GetClientRect(m_hwndFPS, &rect);
    HBRUSH bg = CreateSolidBrush(RGB(0,0,0)); FillRect(hdc, &rect, bg); DeleteObject(bg);
    SetTextColor(hdc, RGB(212, 175, 55)); SetBkMode(hdc, TRANSPARENT);
    HFONT hFont = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Arial");
    HFONT hOld = (HFONT)SelectObject(hdc, hFont);
    wchar_t buf[64]; 
    int mult = StreamlineIntegration::Get().GetFrameGenMultiplier(); if(mult<1) mult=1;
    swprintf_s(buf, L"%.0f -> %.0f FPS", m_cachedTotalFPS/(float)mult, m_cachedTotalFPS);
    DrawTextW(hdc, buf, -1, &rect, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(hdc, hOld); DeleteObject(hFont); ReleaseDC(m_hwndFPS, hdc);
}
void OverlayUI::SetFPS(float gameFps, float totalFps) { m_cachedTotalFPS = totalFps; DrawFPSOverlay(); 
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
void OverlayUI::ToggleVisibility() { if(m_hwnd) { m_visible = !m_visible; ShowWindow(m_hwnd, m_visible?SW_SHOW:SW_HIDE); if(m_visible){ SetForegroundWindow(m_hwnd); SetFocus(m_hwnd); } } }
void OverlayUI::UpdateControls() {
    ModConfig& cfg = ConfigManager::Get().Data();
    if (m_hComboDLSS) SendMessageW(m_hComboDLSS, CB_SETCURSEL, StreamlineIntegration::Get().GetDLSSModeIndex(), 0);
    if (m_hComboPreset) SendMessageW(m_hComboPreset, CB_SETCURSEL, StreamlineIntegration::Get().GetDLSSPresetIndex(), 0);
    if (m_hCheckFG) {
        int fgIndex = 0;
        if (cfg.frameGenMultiplier == 2) fgIndex = 1;
        else if (cfg.frameGenMultiplier == 3) fgIndex = 2;
        else if (cfg.frameGenMultiplier == 4) fgIndex = 3;
        SendMessageW(m_hCheckFG, CB_SETCURSEL, fgIndex, 0);
    }
    if (m_hSliderSharpness) SendMessageW(m_hSliderSharpness, TBM_SETPOS, TRUE, (LPARAM)std::lround(cfg.sharpness * 100.0f));
    if (m_hSliderLOD) SendMessageW(m_hSliderLOD, TBM_SETPOS, TRUE, (LPARAM)std::lround(-cfg.lodBias * 10.0f));
    if (m_hCheckReflex) SendMessageW(m_hCheckReflex, BM_SETCHECK, cfg.reflexEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    if (m_hCheckHUDFix) SendMessageW(m_hCheckHUDFix, BM_SETCHECK, cfg.hudFixEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
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
        RECT headerRect = rect; headerRect.bottom = 30;
        FillRect(hdc, &headerRect, ui.m_brHeader);
        
        // Header Text
        SetTextColor(hdc, COL_ACCENT); SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, ui.m_hFontHeader);
        RECT textRect = headerRect; textRect.left += 10;
        DrawTextW(hdc, L"DLSS 4.5 CONTROL PANEL", -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        
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
            if (pt.y < 30) return HTCAPTION; // Allow drag on top 30px
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
        return (LRESULT)ui.m_brBack;
    }

    if (uMsg == WM_COMMAND) {
        int id = LOWORD(wParam); int code = HIWORD(wParam);
        if (id == ID_COMBO_DLSS && code == CBN_SELCHANGE) {
            int idx = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0); StreamlineIntegration::Get().SetDLSSModeIndex(idx);
        } else if (id == ID_COMBO_PRESET && code == CBN_SELCHANGE) {
            int idx = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0); StreamlineIntegration::Get().SetDLSSPreset(idx);
        } else if (id == ID_CHECK_FG && code == CBN_SELCHANGE) {
            int idx = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
            StreamlineIntegration::Get().SetFrameGenMultiplier((idx==1)?2:(idx==2?3:(idx==3?4:0)));
        } else if (id == ID_BTN_EXPAND) {
            ui.m_expanded = !ui.m_expanded;
            SetWindowTextW(ui.m_hBtnExpand, ui.m_expanded ? L"<< Collapse" : L"Advanced Settings >>");
            int show = ui.m_expanded ? SW_SHOW : SW_HIDE;
            ShowWindow(ui.m_hCheckReflex, show); ShowWindow(ui.m_hCheckHUDFix, show);
            RECT rect; GetWindowRect(ui.m_hwnd, &rect);
            SetWindowPos(ui.m_hwnd, NULL, 0, 0, rect.right-rect.left, ui.m_expanded ? 610 : 510, SWP_NOMOVE|SWP_NOZORDER);
        } else if (id == ID_CHECK_REFLEX && code == BN_CLICKED) {
            bool enabled = SendMessageW(ui.m_hCheckReflex, BM_GETCHECK, 0, 0) == BST_CHECKED;
            StreamlineIntegration::Get().SetReflexEnabled(enabled);
        } else if (id == ID_CHECK_HUD && code == BN_CLICKED) {
            bool enabled = SendMessageW(ui.m_hCheckHUDFix, BM_GETCHECK, 0, 0) == BST_CHECKED;
            StreamlineIntegration::Get().SetHUDFixEnabled(enabled);
        }
    }
    if (uMsg == WM_HSCROLL) {
        HWND hSlider = (HWND)lParam; int pos = SendMessageW(hSlider, TBM_GETPOS, 0, 0);
        if(hSlider == ui.m_hSliderSharpness) StreamlineIntegration::Get().SetSharpness((float)pos/100.0f);
        else if(hSlider == ui.m_hSliderLOD) StreamlineIntegration::Get().SetLODBias(-((float)pos/10.0f));
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
