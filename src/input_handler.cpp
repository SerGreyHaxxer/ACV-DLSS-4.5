#include "input_handler.h"
#include "logger.h"

InputHandler& InputHandler::Get() {
    static InputHandler instance;
    return instance;
}

void InputHandler::RegisterHotkey(int vKey, std::function<void()> callback, const char* name) {
    m_callbacks.push_back({vKey, callback, false, std::string(name)});
    LOG_INFO("Registered Hotkey: %s (Key: %d)", name, vKey);
}

void InputHandler::ProcessInput() {
    for (auto& cb : m_callbacks) {
        bool isPressed = (GetAsyncKeyState(cb.vKey) & 0x8000) != 0;
        
        if (isPressed && !cb.wasPressed) {
            // Key Down Event
            LOG_INFO("Hotkey Pressed: %s", cb.name.c_str());
            if (cb.callback) cb.callback();
        }
        
        cb.wasPressed = isPressed;
    }
}
