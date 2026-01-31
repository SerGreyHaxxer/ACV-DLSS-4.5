#include "input_handler.h"
#include "logger.h"

// Global static for the hook procedure
static InputHandler* g_pInputHandler = nullptr;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;
            if (g_pInputHandler) {
                g_pInputHandler->HandleKey(pKey->vkCode);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

InputHandler& InputHandler::Get() {
    static InputHandler instance;
    return instance;
}

void InputHandler::RegisterHotkey(int vKey, std::function<void()> callback, const char* name) {
    m_callbacks.push_back({vKey, callback, false, std::string(name)});
    LOG_INFO("Registered Hotkey: %s (Key: %d)", name, vKey);
}

void InputHandler::InstallHook() {
    if (m_hHook) return;
    g_pInputHandler = this;
    HMODULE selfModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(&LowLevelKeyboardProc), &selfModule);
    m_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, selfModule, 0);
    LOG_INFO("Global Keyboard Hook Installed");
}

void InputHandler::UninstallHook() {
    if (m_hHook) {
        UnhookWindowsHookEx(m_hHook);
        m_hHook = nullptr;
    }
}

void InputHandler::HandleKey(int vKey) {
    for (auto& cb : m_callbacks) {
        if (cb.vKey == vKey) {
            LOG_INFO("Global Hotkey Triggered: %s", cb.name.c_str());
            if (cb.callback) cb.callback();
        }
    }
}

void InputHandler::ProcessInput() {
    for (auto& cb : m_callbacks) {
        SHORT state = GetAsyncKeyState(cb.vKey);
        bool isDown = (state & 0x8000) != 0;
        if (isDown && !cb.wasPressed) {
            cb.wasPressed = true;
            LOG_INFO("Polled Hotkey Triggered: %s", cb.name.c_str());
            if (cb.callback) cb.callback();
        } else if (!isDown) {
            cb.wasPressed = false;
        }
    }
}
