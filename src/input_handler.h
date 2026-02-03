#pragma once
#include <windows.h>
#include <functional>
#include <vector>
#include <string>
#include <mutex>

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
    void UpdateHotkey(const char* name, int vKey);
    void ClearHotkeys();
    const char* GetKeyName(int vKey) const;
    
    // Global Hook
    void InstallHook();
    void UninstallHook();
    void ProcessInput();
    
    // Internal
    void HandleKey(int vKey);
    bool HasHookInstalled() const { return m_hHook != nullptr; }

private:
    InputHandler() = default;
    std::vector<KeyCallback> m_callbacks;
    std::mutex m_callbackMutex;
    HHOOK m_hHook = nullptr;
};
