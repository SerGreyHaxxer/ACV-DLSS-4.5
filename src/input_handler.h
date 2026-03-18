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
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

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

    bool HasHookInstalled() const { return m_hHook != nullptr; }

    // Wait-free key queue — called by the OS LL hook thread.
    // Stores a pending flag for the virtual key so the render/timer thread
    // can safely process it later via ProcessInput().
    void QueueKey(int vKey) noexcept {
        if (vKey >= 0 && vKey < 256) {
            m_pendingKeys[static_cast<size_t>(vKey)].store(true, std::memory_order_release);
        }
    }

    // C++20 inline static atomic — wait-free access from the OS hook thread.
    // No mutex, no allocation, no chance of Windows timing out the LL hook.
    static inline std::atomic<InputHandler*> s_instance{nullptr};

private:
    InputHandler() = default;

    // Safely invokes registered callbacks for a given vKey.
    // Only called from ProcessInput() on the timer/render thread.
    void HandleKey(int vKey);

    std::vector<KeyCallback> m_callbacks;
    // Protects m_callbacks during RegisterHotkey/UpdateHotkey/ClearHotkeys
    // (configuration-time only — never held in the OS hook or hot path).
    std::mutex m_callbackMutex;

    // Lock-free array indexed by vKey (0-255).
    // The OS hook thread sets entries; ProcessInput() drains them.
    std::array<std::atomic<bool>, 256> m_pendingKeys{};

    HHOOK m_hHook = nullptr;
    HMODULE m_selfModule = nullptr; // Prevent premature unload
};
