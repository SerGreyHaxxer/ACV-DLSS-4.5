/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
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

    // Non-copyable, non-movable singleton
    InputHandler(const InputHandler&) = delete;
    InputHandler& operator=(const InputHandler&) = delete;
    InputHandler(InputHandler&&) = delete;
    InputHandler& operator=(InputHandler&&) = delete;
    
    void RegisterHotkey(int vKey, std::function<void()> callback, const char* name);
    void UpdateHotkey(const char* name, int vKey);
    void ClearHotkeys();
    [[nodiscard]] std::string GetKeyName(int vKey) const;
    
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
    // Lock hierarchy level 4 â€” same tier as Config
    // (SwapChain=1 > Hooks=2 > Resources=3 > Config/Input=4 > Logging=5).
    std::mutex m_callbackMutex;
    HHOOK m_hHook = nullptr;
    HMODULE m_selfModule = nullptr; // Prevent premature unload
};

