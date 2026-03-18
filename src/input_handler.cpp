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
#include "input_handler.h"
#include "logger.h"
#include "streamline_integration.h"
#include <array>
#include <format>

// ============================================================================
// OS-Level Keyboard Hook — 100% Wait-Free
// ============================================================================
// Windows calls WH_KEYBOARD_LL hooks synchronously on the OS message pump
// thread for every keystroke.  If this callback blocks (e.g. on a mutex),
// Windows will silently unhook the application after LowLevelHooksTimeout
// (~300ms), permanently breaking hotkeys.
//
// Solution: Zero locks.  Zero heap allocations.  Atomically queue the vKey
// and let the timer/render thread process it safely in ProcessInput().
// ============================================================================

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            // Wait-free atomic load — no mutex, no chance of OS timeout.
            if (auto* handler = InputHandler::s_instance.load(std::memory_order_acquire)) {
                KBDLLHOOKSTRUCT* pKey = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
                handler->QueueKey(static_cast<int>(pKey->vkCode));
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

    // Atomically publish this instance (wait-free for the hook proc).
    s_instance.store(this, std::memory_order_release);

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
        // Clean up the atomic pointer so we don't leave a dangling reference
        s_instance.store(nullptr, std::memory_order_release);
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
    // Atomically clear (wait-free).
    s_instance.store(nullptr, std::memory_order_release);
    // Release the ref-counted module handle now that the hook is removed
    if (m_selfModule) {
        FreeLibrary(m_selfModule);
        m_selfModule = nullptr;
    }
}

void InputHandler::HandleKey(int vKey) {
    // Called from ProcessInput() on the timer thread — safe for game logic.
    StreamlineIntegration::Get().ReflexMarker(sl::PCLMarker::eControllerInputSample);
    std::vector<std::function<void()>> callbacksToRun;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        for (auto& cb : m_callbacks) {
            if (cb.vKey == vKey) {
                if (cb.wasPressed) continue;
                LOG_DEBUG("Hotkey Triggered: {}", cb.name);
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
    // ---- Phase 1: Drain the lock-free pending key queue ----
    // Keys queued by the OS LL hook via QueueKey() are processed here
    // on the timer thread, safely outside the OS hook context.
    for (int i = 0; i < 256; ++i) {
        if (m_pendingKeys[static_cast<size_t>(i)].exchange(false, std::memory_order_acquire)) {
            HandleKey(i);
        }
    }

    // ---- Phase 2: GetAsyncKeyState polling fallback ----
    // This catches keys even if the LL hook was never installed or was
    // silently removed by Windows. It also handles key-up transitions.
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
