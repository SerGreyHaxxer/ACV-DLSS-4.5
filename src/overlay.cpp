#include "overlay.h"
#include "streamline_integration.h"
#include "logger.h"
#include <commctrl.h>
#include <cmath>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")

#define ID_COMBO_DLSS 101
#define ID_CHECK_FG   102
#define ID_SLIDER_SHARP 103
#define ID_SLIDER_LOD   104
#define ID_BTN_EXPAND   105
#define ID_CHECK_REFLEX 106
#define ID_CHECK_HUD    107

static const UINT kVignetteTimerId = 1;

OverlayUI& OverlayUI::Get() {
    static OverlayUI instance;
    return instance;
}

void OverlayUI::Initialize(HMODULE hModule) {
    if (m_initialized) return;
    m_hModule = hModule;
    m_hThread = CreateThread(NULL, 0, UIThreadEntry, this, 0, NULL);
    m_initialized = true;
}

DWORD WINAPI OverlayUI::UIThreadEntry(LPVOID lpParam) {
    OverlayUI* pThis = (OverlayUI*)lpParam;
    pThis->UIThreadLoop();
    return 0;
}

void OverlayUI::UIThreadLoop() {
    // 1. Control Panel Class
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = m_hModule;
    wc.lpszClassName = L"DLSS4ProxyOverlay";
    wc.hbrBackground = CreateSolidBrush(RGB(10, 15, 20)); // Dark Blue
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);
    
    // 2. FPS Counter Class
    WNDCLASSEXW wcFPS = {0};
    wcFPS.cbSize = sizeof(WNDCLASSEXW);
    wcFPS.lpfnWndProc = WindowProc; 
    wcFPS.hInstance = m_hModule;
    wcFPS.lpszClassName = L"DLSS4ProxyFPS";
    wcFPS.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wcFPS);

    // 3. Vignette Class (Orange Background)
    WNDCLASSEXW wcVig = {0};
    wcVig.cbSize = sizeof(WNDCLASSEXW);
    wcVig.lpfnWndProc = WindowProc;
    wcVig.hInstance = m_hModule;
    wcVig.lpszClassName = L"DLSS4ProxyVignette";
    wcVig.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wcVig);
    
    CreateOverlayWindow();
    CreateFPSWindow();
    CreateVignetteWindow(); 
    
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void OverlayUI::CreateVignetteWindow() {
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);

    // Create window covering entire virtual screen
    m_hwndVignette = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, 
        L"DLSS4ProxyVignette", L"", WS_POPUP, x, y, w, h, NULL, NULL, m_hModule, NULL
    );
    
    // Alpha Blend: 80/255 (approx 30% opacity)
    SetLayeredWindowAttributes(m_hwndVignette, RGB(0, 0, 0), (BYTE)m_vignetteAlpha, LWA_ALPHA | LWA_COLORKEY); 
    
    m_showVignette = false;
    ShowWindow(m_hwndVignette, SW_HIDE);
}

void OverlayUI::ToggleVignette() {
    m_showVignette = !m_showVignette;
    if (m_hwndVignette) {
        ShowWindow(m_hwndVignette, m_showVignette ? SW_SHOW : SW_HIDE);
        // Force update just in case
        UpdateWindow(m_hwndVignette);
        if (m_showVignette) {
            SetTimer(m_hwndVignette, kVignetteTimerId, 33, NULL);
        } else {
            KillTimer(m_hwndVignette, kVignetteTimerId);
        }
    }
}

void OverlayUI::DrawVignette() {
    if (!m_hwndVignette || !m_showVignette) return;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwndVignette, &ps);
    if (!hdc) return;

    RECT rect;
    GetClientRect(m_hwndVignette, &rect);
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;

    HBRUSH clearBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(hdc, &rect, clearBrush);

    const int thickness = 90;
    int alpha = m_vignetteAlpha;
    if (alpha < 10) alpha = 10;
    if (alpha > 200) alpha = 200;

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)alpha, 0 };

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
    HBRUSH orangeBrush = CreateSolidBrush(RGB(255, 100, 0));

    RECT rTop = { 0, 0, w, thickness };
    RECT rBottom = { 0, h - thickness, w, h };
    RECT rLeft = { 0, thickness, thickness, h - thickness };
    RECT rRight = { w - thickness, thickness, w, h - thickness };
    FillRect(memDC, &rTop, orangeBrush);
    FillRect(memDC, &rBottom, orangeBrush);
    FillRect(memDC, &rLeft, orangeBrush);
    FillRect(memDC, &rRight, orangeBrush);

    AlphaBlend(hdc, 0, 0, w, h, memDC, 0, 0, w, h, bf);

    DeleteObject(orangeBrush);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(m_hwndVignette, &ps);
}

void OverlayUI::CreateFPSWindow() {
    m_hwndFPS = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, 
        L"DLSS4ProxyFPS",
        L"",
        WS_POPUP,
        20, 20, 400, 80,
        NULL, NULL, m_hModule, NULL
    );
    SetLayeredWindowAttributes(m_hwndFPS, 0, 200, LWA_ALPHA);
}

void OverlayUI::ToggleFPS() {
    m_showFPS = !m_showFPS;
    if (m_hwndFPS) ShowWindow(m_hwndFPS, m_showFPS ? SW_SHOW : SW_HIDE);
}

void OverlayUI::DrawFPSOverlay() {
    if (!m_hwndFPS || !m_showFPS) return;
    HDC hdc = GetDC(m_hwndFPS);
    RECT rect; GetClientRect(m_hwndFPS, &rect);
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rect, bg); DeleteObject(bg);
    
    SetTextColor(hdc, RGB(212, 175, 55));
    SetBkMode(hdc, TRANSPARENT);
    HFONT hFont = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
    HFONT hOld = (HFONT)SelectObject(hdc, hFont);
    
    wchar_t buf[64];
    int mult = StreamlineIntegration::Get().GetFrameGenMultiplier();
    if (mult < 1) mult = 1;
    float baseFps = m_cachedTotalFPS / (float)mult;
    swprintf_s(buf, L"%.0f -> %.0f FPS", baseFps, m_cachedTotalFPS);
    
    DrawTextW(hdc, buf, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, hOld); DeleteObject(hFont); ReleaseDC(m_hwndFPS, hdc);
}

void OverlayUI::SetFPS(float gameFps, float totalFps) {
    m_cachedTotalFPS = totalFps;
    if (m_hLabelFPS) {
        wchar_t buf[128];
        swprintf_s(buf, L"Game: %.1f FPS | DLSS 4.5: %.1f FPS", gameFps, totalFps);
        SetWindowTextW(m_hLabelFPS, buf);
    }
    DrawFPSOverlay();
}

void OverlayUI::CreateOverlayWindow() {
    int width = 360;
    int height = 480;
    int x = 50; int y = 50;

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 
        L"DLSS4ProxyOverlay", L"DLSS 4.5 Control Panel (F1)",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_BORDER,
        x, y, width, height, NULL, NULL, m_hModule, NULL
    );

    int padding = 15; int cy = 15; int contentWidth = width - 2*padding;

    CreateWindowW(L"STATIC", L"Performance Settings", WS_VISIBLE | WS_CHILD | SS_CENTER, padding, cy, contentWidth, 25, m_hwnd, NULL, m_hModule, NULL); cy += 40;
    CreateWindowW(L"STATIC", L"DLSS Quality Mode:", WS_VISIBLE | WS_CHILD, padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL); cy += 22;
    m_hComboDLSS = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, padding, cy, contentWidth, 200, m_hwnd, (HMENU)ID_COMBO_DLSS, m_hModule, NULL);
    const wchar_t* modes[] = { L"Off", L"Max Performance", L"Balanced", L"Max Quality", L"Ultra Quality", L"DLAA" };
    for (const wchar_t* mode : modes) SendMessageW(m_hComboDLSS, CB_ADDSTRING, 0, (LPARAM)mode);
    SendMessageW(m_hComboDLSS, CB_SETCURSEL, 5, 0); cy += 45;

    CreateWindowW(L"STATIC", L"Frame Generation:", WS_VISIBLE | WS_CHILD, padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL); cy += 22;
    m_hCheckFG = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, padding, cy, contentWidth, 200, m_hwnd, (HMENU)ID_CHECK_FG, m_hModule, NULL);
    const wchar_t* fgModes[] = { L"Off", L"2x (DLSS-G)", L"3x (DLSS-MFG)", L"4x (DLSS-MFG)" };
    for (const wchar_t* m : fgModes) SendMessageW(m_hCheckFG, CB_ADDSTRING, 0, (LPARAM)m);
    SendMessageW(m_hCheckFG, CB_SETCURSEL, 3, 0); cy += 45;

    CreateWindowW(L"STATIC", L"Sharpness:", WS_VISIBLE | WS_CHILD, padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL); cy += 22;
    m_hSliderSharpness = CreateWindowW(TRACKBAR_CLASSW, L"Sharpness", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, padding, cy, contentWidth, 30, m_hwnd, (HMENU)ID_SLIDER_SHARP, m_hModule, NULL);
    SendMessageW(m_hSliderSharpness, TBM_SETRANGE, TRUE, MAKELONG(0, 100)); SendMessageW(m_hSliderSharpness, TBM_SETPOS, TRUE, 50); cy += 45;

    CreateWindowW(L"STATIC", L"Texture Detail (Neg LOD):", WS_VISIBLE | WS_CHILD, padding, cy, contentWidth, 20, m_hwnd, NULL, m_hModule, NULL); cy += 22;
    m_hSliderLOD = CreateWindowW(TRACKBAR_CLASSW, L"LOD", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, padding, cy, contentWidth, 30, m_hwnd, (HMENU)ID_SLIDER_LOD, m_hModule, NULL);
    SendMessageW(m_hSliderLOD, TBM_SETRANGE, TRUE, MAKELONG(0, 30)); SendMessageW(m_hSliderLOD, TBM_SETPOS, TRUE, 10); cy += 50;

    m_hLabelFPS = CreateWindowW(L"STATIC", L"Waiting for game...", WS_VISIBLE | WS_CHILD | SS_CENTER | WS_BORDER, padding, cy, contentWidth, 30, m_hwnd, NULL, m_hModule, NULL); cy += 45;
    m_hBtnExpand = CreateWindowW(L"BUTTON", L"Advanced Settings >>", WS_VISIBLE | WS_CHILD, padding, cy, contentWidth, 30, m_hwnd, (HMENU)ID_BTN_EXPAND, m_hModule, NULL); cy += 35;

    m_hCheckReflex = CreateWindowW(L"BUTTON", L"NVIDIA Reflex Boost", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, 25, m_hwnd, (HMENU)ID_CHECK_REFLEX, m_hModule, NULL);
    SendMessageW(m_hCheckReflex, BM_SETCHECK, BST_CHECKED, 0); cy += 30;
    m_hCheckHUDFix = CreateWindowW(L"BUTTON", L"Experimental HUD Masking", WS_CHILD | BS_AUTOCHECKBOX, padding, cy, contentWidth, 25, m_hwnd, (HMENU)ID_CHECK_HUD, m_hModule, NULL); cy += 30;
}

void OverlayUI::ToggleVisibility() {
    if (m_hwnd) {
        m_visible = !m_visible;
        ShowWindow(m_hwnd, m_visible ? SW_SHOW : SW_HIDE);
        if (m_visible) { SetForegroundWindow(m_hwnd); SetFocus(m_hwnd); }
    }
}

void OverlayUI::UpdateControls() {}

LRESULT CALLBACK OverlayUI::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CLOSE) { OverlayUI::Get().ToggleVisibility(); return 0; }
    
    if (uMsg == WM_CTLCOLORSTATIC) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(212, 175, 55));
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    if (uMsg == WM_CTLCOLORBTN) return (LRESULT)GetStockObject(NULL_BRUSH);
    if (uMsg == WM_TIMER && wParam == kVignetteTimerId) {
        OverlayUI& ui = OverlayUI::Get();
        double t = (double)GetTickCount64() / 1000.0;
        int baseAlpha = 80;
        int amp = 30;
        ui.m_vignetteAlpha = baseAlpha + (int)(amp * (0.5 + 0.5 * sin(t * 1.2)));
        if (ui.m_hwndVignette && ui.m_showVignette) {
            SetLayeredWindowAttributes(ui.m_hwndVignette, RGB(0, 0, 0), (BYTE)ui.m_vignetteAlpha, LWA_ALPHA | LWA_COLORKEY);
            InvalidateRect(ui.m_hwndVignette, NULL, TRUE);
        }
        return 0;
    }
    if (uMsg == WM_PAINT && hwnd == OverlayUI::Get().m_hwndVignette) {
        OverlayUI::Get().DrawVignette();
        return 0;
    }

    if (uMsg == WM_COMMAND) {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (id == ID_COMBO_DLSS && code == CBN_SELCHANGE) {
            int idx = SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
            StreamlineIntegration::Get().SetDLSSMode(idx);
        } else if (id == ID_CHECK_FG && code == CBN_SELCHANGE) {
            int idx = SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
            int multiplier = (idx == 1) ? 2 : (idx == 2 ? 3 : (idx == 3 ? 4 : 0));
            StreamlineIntegration::Get().SetFrameGenMultiplier(multiplier);
        } else if (id == ID_BTN_EXPAND) {
            OverlayUI& ui = OverlayUI::Get();
            ui.m_expanded = !ui.m_expanded;
            SetWindowTextW(ui.m_hBtnExpand, ui.m_expanded ? L"<< Less Settings" : L"Advanced Settings >>");
            int show = ui.m_expanded ? SW_SHOW : SW_HIDE;
            ShowWindow(ui.m_hCheckReflex, show); ShowWindow(ui.m_hCheckHUDFix, show);
            RECT rect; GetWindowRect(ui.m_hwnd, &rect);
            SetWindowPos(ui.m_hwnd, NULL, 0, 0, rect.right - rect.left, ui.m_expanded ? 560 : 480, SWP_NOMOVE | SWP_NOZORDER);
        }
    }
    if (uMsg == WM_HSCROLL) {
        HWND hSlider = (HWND)lParam;
        int pos = SendMessageW(hSlider, TBM_GETPOS, 0, 0);
        if (hSlider == OverlayUI::Get().m_hSliderSharpness) StreamlineIntegration::Get().SetSharpness((float)pos / 100.0f);
        else if (hSlider == OverlayUI::Get().m_hSliderLOD) StreamlineIntegration::Get().SetLODBias(-((float)pos / 10.0f));
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
