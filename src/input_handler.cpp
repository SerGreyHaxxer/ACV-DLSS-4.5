#include "input_handler.h"
#include "logger.h"
#include <stdio.h>

// Global static for the hook procedure
static InputHandler* g_pInputHandler = nullptr;
static std::mutex g_inputHandlerMutex;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;
            InputHandler* handler = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_inputHandlerMutex);
                handler = g_pInputHandler;
            }
            if (handler) {
                handler->HandleKey(pKey->vkCode);
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
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callbacks.push_back({vKey, callback, false, std::string(name)});
    LOG_DEBUG("Registered Hotkey: %s (Key: %d)", name, vKey);
}

void InputHandler::UpdateHotkey(const char* name, int vKey) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    for (auto& cb : m_callbacks) {
        if (cb.name == name) {
            cb.vKey = vKey;
            cb.wasPressed = false;
            LOG_DEBUG("Updated Hotkey: %s (Key: %d)", name, vKey);
            return;
        }
    }
}

void InputHandler::ClearHotkeys() {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callbacks.clear();
}

const char* InputHandler::GetKeyName(int vKey) const {
    thread_local char buf[64];
    UINT scan = MapVirtualKeyA(vKey, MAPVK_VK_TO_VSC);
    LONG lParam = (scan << 16);
    if (GetKeyNameTextA(lParam, buf, sizeof(buf)) > 0) return buf;
    snprintf(buf, sizeof(buf), "Key %d", vKey);
    return buf;
}

void InputHandler::InstallHook() {
    if (m_hHook) return;
    {
        std::lock_guard<std::mutex> lock(g_inputHandlerMutex);
        g_pInputHandler = this;
    }
    HMODULE selfModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(&LowLevelKeyboardProc), &selfModule);
    m_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, selfModule, 0);
    LOG_INFO("Global Keyboard Hook Installed");
    if (selfModule) {
        FreeLibrary(selfModule);
    }
}

void InputHandler::UninstallHook() {
    if (m_hHook) {
        UnhookWindowsHookEx(m_hHook);
        m_hHook = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_inputHandlerMutex);
        g_pInputHandler = nullptr;
    }
}

void InputHandler::HandleKey(int vKey) {
    std::vector<std::function<void()>> callbacksToRun;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        for (auto& cb : m_callbacks) {
            if (cb.vKey == vKey) {
                if (cb.wasPressed) continue;
                LOG_DEBUG("Global Hotkey Triggered: %s", cb.name.c_str());
                if (cb.callback) callbacksToRun.push_back(cb.callback);
                cb.wasPressed = true;
            }
        }
    }
    for (auto& cb : callbacksToRun) {
        cb();
    }
}

void InputHandler::ProcessInput() {
    std::vector<std::function<void()>> callbacksToRun;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        for (auto& cb : m_callbacks) {
            SHORT state = GetAsyncKeyState(cb.vKey);
            bool isDown = (state & 0x8000) != 0;
            if (isDown && !cb.wasPressed) {
                cb.wasPressed = true;
                LOG_DEBUG("Polled Hotkey Triggered: %s", cb.name.c_str());
                if (cb.callback) callbacksToRun.push_back(cb.callback);
            } else if (!isDown) {
                cb.wasPressed = false;
            }
        }
    }
    for (auto& cb : callbacksToRun) {
        cb();
    }
}
