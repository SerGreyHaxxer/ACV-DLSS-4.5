#include "overlay.h"
#include "streamline_integration.h"
#include "logger.h"
#include <commctrl.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define ID_COMBO_DLSS 101
#define ID_CHECK_FG   102
#define ID_SLIDER_SHARP 103
#define ID_SLIDER_LOD   104
#define ID_BTN_EXPAND   105
#define ID_CHECK_REFLEX 106
#define ID_CHECK_HUD    107

OverlayUI& OverlayUI::Get() {
    static OverlayUI instance;
    return instance;
}

void OverlayUI::Initialize(HMODULE hModule) {
    if (m_initialized) return;
    m_hModule = hModule;
    
    // Spawn UI Thread
    m_hThread = CreateThread(NULL, 0, UIThreadEntry, this, 0, NULL);
    m_initialized = true;
}

DWORD WINAPI OverlayUI::UIThreadEntry(LPVOID lpParam) {
    OverlayUI* pThis = (OverlayUI*)lpParam;
    pThis->UIThreadLoop();
    return 0;
}

void OverlayUI::UIThreadLoop() {
    // Register Window Class (Must be on same thread as CreateWindow)
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = m_hModule;
    wc.lpszClassName = L"DLSS4ProxyOverlay";
    // Valhalla Dark Blue Background
    wc.hbrBackground = CreateSolidBrush(RGB(10, 15, 20));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    
    RegisterClassExW(&wc);
    
    CreateOverlayWindow();
    CreateFPSWindow(); // Create the FPS overlay too
    
    // Message Loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void OverlayUI::CreateFPSWindow() {
    // Transparent, Layered, Top-Most window for FPS
    // Valhalla Theme: Gold Text
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = DefWindowProcW; // Simple proc
    wc.hInstance = m_hModule;
    wc.lpszClassName = L"DLSS4ProxyFPS";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    
    RegisterClassExW(&wc);

    m_hwndFPS = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, 
        L"DLSS4ProxyFPS",
        L"",
        WS_POPUP,
        20, 20, 400, 80, // Top-Left, Bigger
        NULL, NULL, m_hModule, NULL
    );
    
    // Set Transparency (Alpha 200/255 background)
    SetLayeredWindowAttributes(m_hwndFPS, 0, 200, LWA_ALPHA);
}

void OverlayUI::ToggleFPS() {
    m_showFPS = !m_showFPS;
    if (m_hwndFPS) {
        ShowWindow(m_hwndFPS, m_showFPS ? SW_SHOW : SW_HIDE);
    }
}

void OverlayUI::DrawFPSOverlay() {
    if (!m_hwndFPS || !m_showFPS) return;
    
    HDC hdc = GetDC(m_hwndFPS);
    RECT rect;
    GetClientRect(m_hwndFPS, &rect);
    
    // Clear Background (Black)
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rect, bg);
    DeleteObject(bg);
    
    // Draw Text (Valhalla Gold: RGB 212, 175, 55)
    SetTextColor(hdc, RGB(212, 175, 55));
    SetBkMode(hdc, TRANSPARENT);
    
    // Select a nice font (Size 48 = Huge)
    HFONT hFont = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    
    wchar_t buf[64];
    // Calculate Base from Total and Multiplier
    int mult = StreamlineIntegration::Get().GetFrameGenMultiplier();
    if (mult < 1) mult = 1;
    float baseFps = m_cachedTotalFPS / (float)mult;
    
    swprintf_s(buf, L"%.0f -> %.0f FPS", baseFps, m_cachedTotalFPS);
    
    DrawTextW(hdc, buf, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    
    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
    ReleaseDC(m_hwndFPS, hdc);
}

void OverlayUI::CreateOverlayWindow() {
    int width = 360;  // Wider
    int height = 480; // Taller base height
    
    // Start Top-Left
    int x = 50; 
    int y = 50;

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 
        L"DLSS4ProxyOverlay",
        L"DLSS 4.5 Control Panel (F1)",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_BORDER,
        x, y, width, height,
        NULL, NULL, m_hModule, NULL
    );

    // --- Create Controls ---
    int padding = 15; // More padding
    int cy = 15;
    int contentWidth = width - 2*padding; // 330px usable width

    // Title
    CreateWindowW(L"STATIC", L"Performance Settings", WS_VISIBLE | WS_CHILD | SS_CENTER, 
        padding, cy, contentWidth, 25, m_hwnd, NULL, m_hModule, NULL);
    cy += 40;

    // DLSS Mode
    CreateWindowW(L"STATIC", L"DLSS Quality Mode:", WS_VISIBLE | WS_CHILD, 
        padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL);
    cy += 22;
    
    m_hComboDLSS = CreateWindowW(WC_COMBOBOXW, L"", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        padding, cy, contentWidth, 200, m_hwnd, (HMENU)ID_COMBO_DLSS, m_hModule, NULL);
    
    const wchar_t* modes[] = { L"Off", L"Max Performance", L"Balanced", L"Max Quality", L"Ultra Quality", L"DLAA" };
    for (const wchar_t* mode : modes) {
        SendMessageW(m_hComboDLSS, CB_ADDSTRING, 0, (LPARAM)mode);
    }
    SendMessageW(m_hComboDLSS, CB_SETCURSEL, 5, 0); // Default DLAA (Index 5)
    cy += 45; // More space after combo

    // Frame Gen
    CreateWindowW(L"STATIC", L"Frame Generation:", WS_VISIBLE | WS_CHILD, 
        padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL);
    cy += 22;
    
    m_hCheckFG = CreateWindowW(WC_COMBOBOXW, L"", 
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        padding, cy, contentWidth, 200, m_hwnd, (HMENU)ID_CHECK_FG, m_hModule, NULL);
    
    const wchar_t* fgModes[] = { L"Off", L"2x (DLSS-G)", L"3x (DLSS-MFG)", L"4x (DLSS-MFG)" };
    for (const wchar_t* m : fgModes) SendMessageW(m_hCheckFG, CB_ADDSTRING, 0, (LPARAM)m);
    SendMessageW(m_hCheckFG, CB_SETCURSEL, 3, 0); // Default 4x
    cy += 45;

    // Sharpness
    CreateWindowW(L"STATIC", L"Sharpness:", WS_VISIBLE | WS_CHILD, 
        padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL);
    cy += 22;
    m_hSliderSharpness = CreateWindowW(TRACKBAR_CLASSW, L"Sharpness", 
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        padding, cy, contentWidth, 30, m_hwnd, (HMENU)ID_SLIDER_SHARP, m_hModule, NULL);
    SendMessageW(m_hSliderSharpness, TBM_SETRANGE, TRUE, MAKELONG(0, 100)); // 0-100%
    SendMessageW(m_hSliderSharpness, TBM_SETPOS, TRUE, 50); // 50%
    cy += 45;

    // LOD Bias
    CreateWindowW(L"STATIC", L"Texture Detail (Neg LOD):", WS_VISIBLE | WS_CHILD, 
        padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL);
    cy += 22;
    m_hSliderLOD = CreateWindowW(TRACKBAR_CLASSW, L"LOD", 
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        padding, cy, contentWidth, 30, m_hwnd, (HMENU)ID_SLIDER_LOD, m_hModule, NULL);
    SendMessageW(m_hSliderLOD, TBM_SETRANGE, TRUE, MAKELONG(0, 30)); // 0 to 3.0
    SendMessageW(m_hSliderLOD, TBM_SETPOS, TRUE, 10); // Default -1.0
    cy += 50;

    // FPS Stats
    m_hLabelFPS = CreateWindowW(L"STATIC", L"Waiting for game...", 
        WS_VISIBLE | WS_CHILD | SS_CENTER | WS_BORDER, 
        padding, cy, contentWidth, 30, m_hwnd, NULL, m_hModule, NULL);
    cy += 45;

    // Expand Button
    m_hBtnExpand = CreateWindowW(L"BUTTON", L"Advanced Settings >>", 
        WS_VISIBLE | WS_CHILD,
        padding, cy, contentWidth, 30, m_hwnd, (HMENU)ID_BTN_EXPAND, m_hModule, NULL);
    cy += 45;

    // --- HIDDEN ADVANCED CONTROLS ---
    // Reflex
    m_hCheckReflex = CreateWindowW(L"BUTTON", L"NVIDIA Reflex Boost", 
        WS_CHILD | BS_AUTOCHECKBOX,
        padding, cy, contentWidth, 25, m_hwnd, (HMENU)ID_CHECK_REFLEX, m_hModule, NULL);
    SendMessageW(m_hCheckReflex, BM_SETCHECK, BST_CHECKED, 0); // Default On
    cy += 30;

    // HUD Fix
    m_hCheckHUDFix = CreateWindowW(L"BUTTON", L"Experimental HUD Masking", 
        WS_CHILD | BS_AUTOCHECKBOX,
        padding, cy, contentWidth, 25, m_hwnd, (HMENU)ID_CHECK_HUD, m_hModule, NULL);
    cy += 30;
}

void OverlayUI::SetFPS(float gameFps, float totalFps) {
    m_cachedTotalFPS = totalFps;
    
    // Update Control Panel Label
    if (m_hLabelFPS) {
        wchar_t buf[128];
        swprintf_s(buf, L"Game: %.1f FPS | DLSS 4.5: %.1f FPS", gameFps, totalFps);
        SetWindowTextW(m_hLabelFPS, buf);
    }
    
    // Redraw FPS Overlay
    DrawFPSOverlay();
}

void OverlayUI::ToggleVisibility() {
    // Post message to UI thread to toggle logic
    if (m_hwnd) {
        m_visible = !m_visible;
        ShowWindow(m_hwnd, m_visible ? SW_SHOW : SW_HIDE);
        if (m_visible) {
            SetForegroundWindow(m_hwnd);
            SetFocus(m_hwnd);
        }
    }
}

void OverlayUI::UpdateControls() {
    // Sync logic would go here
}

LRESULT CALLBACK OverlayUI::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CLOSE:
            OverlayUI::Get().ToggleVisibility(); // Just hide, don't destroy
            return 0;

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(212, 175, 55)); // Valhalla Gold
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH); // Transparent background for text
        }

        case WM_CTLCOLORBTN: {
            // Note: Buttons are harder to color without owner-draw, but this helps the area around them
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
            
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            
            if (id == ID_COMBO_DLSS && code == CBN_SELCHANGE) {
                int idx = SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
                StreamlineIntegration::Get().SetDLSSMode(idx);
                LOG_INFO("UI: Set DLSS Mode to %d", idx);
            }
            else if (id == ID_CHECK_FG && code == CBN_SELCHANGE) {
                int idx = SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
                // Combo: 0=Off, 1=2x, 2=3x, 3=4x
                int multiplier = 0;
                if (idx == 1) multiplier = 2;
                if (idx == 2) multiplier = 3;
                if (idx == 3) multiplier = 4;
                StreamlineIntegration::Get().SetFrameGenMultiplier(multiplier);
                LOG_INFO("UI: Set FrameGen to %dx", multiplier);
            }
            else if (id == ID_BTN_EXPAND) {
                OverlayUI& ui = OverlayUI::Get();
                ui.m_expanded = !ui.m_expanded;
                
                // Toggle Button Text
                SetWindowTextW(ui.m_hBtnExpand, ui.m_expanded ? L"<< Less Settings" : L"Advanced Settings >>");
                
                // Toggle Controls
                int show = ui.m_expanded ? SW_SHOW : SW_HIDE;
                ShowWindow(ui.m_hCheckReflex, show);
                ShowWindow(ui.m_hCheckHUDFix, show);
                
                // Resize Window
                RECT rect;
                GetWindowRect(ui.m_hwnd, &rect);
                int baseHeight = 480; // New taller base
                int expandedHeight = 560; // New expanded height
                
                SetWindowPos(ui.m_hwnd, NULL, 0, 0, 
                    rect.right - rect.left, 
                    ui.m_expanded ? expandedHeight : baseHeight, 
                    SWP_NOMOVE | SWP_NOZORDER);
            }
            else if (id == ID_CHECK_REFLEX) {
                // Future: Implement Reflex Toggle logic
            }
            break;
        }
        
        case WM_HSCROLL: {
            HWND hSlider = (HWND)lParam;
            int pos = SendMessageW(hSlider, TBM_GETPOS, 0, 0);
            
            if (hSlider == OverlayUI::Get().m_hSliderSharpness) {
                float val = (float)pos / 100.0f;
                StreamlineIntegration::Get().SetSharpness(val);
            }
            else if (hSlider == OverlayUI::Get().m_hSliderLOD) {
                float val = -((float)pos / 10.0f); // 0 to -3.0
                StreamlineIntegration::Get().SetLODBias(val); 
            }
            break;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}