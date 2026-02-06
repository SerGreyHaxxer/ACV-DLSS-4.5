#include "input_handler.h"
#include "logger.h"
#include "streamline_integration.h"
#include <array>
#include <format>

// Global static for the hook procedure
static InputHandler* g_pInputHandler = nullptr;
static std::mutex g_inputHandlerMutex;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            KBDLLHOOKSTRUCT* pKey = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
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
    LOG_DEBUG("Registered Hotkey: {} (Key: {})", name, vKey);
}

void InputHandler::UpdateHotkey(const char* name, int vKey) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    for (auto& cb : m_callbacks) {
        if (cb.name == name) {
            cb.vKey = vKey;
            cb.wasPressed = false;
            LOG_DEBUG("Updated Hotkey: {} (Key: {})", name, vKey);
            return;
        }
    }
}

void InputHandler::ClearHotkeys() {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_callbacks.clear();
}

std::string InputHandler::GetKeyName(int vKey) const {
    std::array<char, 64> buf{};
    UINT scan = MapVirtualKeyA(vKey, MAPVK_VK_TO_VSC);
    LONG lParam = (scan << 16);
    if (GetKeyNameTextA(lParam, buf.data(), static_cast<int>(buf.size())) > 0) {
        return std::string(buf.data());
    }
    return std::format("Key {}", vKey);
}

void InputHandler::InstallHook() {
    if (m_hHook) return;
    {
        std::lock_guard<std::mutex> lock(g_inputHandlerMutex);
        g_pInputHandler = this;
    }
    // Get a ref-counted handle to our module so it stays loaded while the hook is active
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCSTR>(&LowLevelKeyboardProc), &m_selfModule)) {
        DWORD err = GetLastError();
        LOG_ERROR("GetModuleHandleEx failed (error {}), keyboard hook unavailable", err);
        m_selfModule = nullptr;
        // Fall through — SetWindowsHookEx can still work with a null module for
        // low-level hooks, but log the warning so we know something is off.
    }
    m_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, m_selfModule, 0);
    if (m_hHook) {
        LOG_INFO("Global Keyboard Hook Installed");
    } else {
        DWORD err = GetLastError();
        LOG_ERROR("SetWindowsHookEx FAILED (error {}). Hotkeys will use fallback polling.", err);
        // Clean up the global pointer so we don't leave a dangling reference
        {
            std::lock_guard<std::mutex> lock(g_inputHandlerMutex);
            g_pInputHandler = nullptr;
        }
        if (m_selfModule) {
            FreeLibrary(m_selfModule);
            m_selfModule = nullptr;
        }
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
    // Release the ref-counted module handle now that the hook is removed
    if (m_selfModule) {
        FreeLibrary(m_selfModule);
        m_selfModule = nullptr;
    }
}

void InputHandler::HandleKey(int vKey) {
    StreamlineIntegration::Get().ReflexMarker(sl::PCLMarker::eControllerInputSample);
    std::vector<std::function<void()>> callbacksToRun;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        for (auto& cb : m_callbacks) {
            if (cb.vKey == vKey) {
                if (cb.wasPressed) continue;
                LOG_DEBUG("Global Hotkey Triggered: {}", cb.name);
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
    bool anyPressed = false;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        for (auto& cb : m_callbacks) {
            SHORT state = GetAsyncKeyState(cb.vKey);
            bool isDown = (state & 0x8000) != 0;
            if (isDown) anyPressed = true;
            if (isDown && !cb.wasPressed) {
                cb.wasPressed = true;
                LOG_DEBUG("Polled Hotkey Triggered: {}", cb.name);
                if (cb.callback) callbacksToRun.push_back(cb.callback);
            } else if (!isDown) {
                cb.wasPressed = false;
            }
        }
    }
    if (anyPressed) {
        StreamlineIntegration::Get().ReflexMarker(sl::PCLMarker::eControllerInputSample);
    }
    for (auto& cb : callbacksToRun) {
        cb();
    }
}
