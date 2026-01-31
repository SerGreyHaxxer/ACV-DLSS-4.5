#pragma once
#include <windows.h>
#include <functional>
#include <vector>
#include <string>

struct KeyCallback {
    int vKey;
    std::function<void()> callback;
    bool wasPressed;
    std::string name;
};

class InputHandler {
public:
    static InputHandler& Get();
    
    void RegisterHotkey(int vKey, std::function<void()> callback, const char* name);
    void ProcessInput();

private:
    InputHandler() = default;
    std::vector<KeyCallback> m_callbacks;
};
