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

#include <cstdint>
#include <functional>


// ============================================================================
// GHOST HOOK - Phase 0: Stability & Safety
// ============================================================================
// Hardware Breakpoint (HWBP) based hooking system using Dr0-Dr3 registers.
// This provides undetectable function interception that doesn't modify any
// code in memory, making it invisible to anti-cheat and anti-tamper systems.
// ============================================================================

namespace Ghost {

// Maximum number of simultaneous hooks (limited by x86/x64 debug registers)
constexpr size_t kMaxHooks = 4;

// Hook callback function signature
// Parameters:
//   - context: The thread context at the breakpoint (can be modified to change execution)
//   - userData: User-provided data passed during hook installation
// Return:
//   - true: Continue to original function (after callback)
//   - false: Skip original function (callback handled it)
using HookCallback = std::function<bool(CONTEXT* context, void* userData)>;

// Hook slot information
struct HookSlot {
  uintptr_t address;     // Target address being hooked
  HookCallback callback; // User callback
  void* userData;        // User data passed to callback
  bool active;           // Whether this slot is in use
};

// Ghost Hook Manager singleton
class HookManager {
public:
  static HookManager& Get();

  // Non-copyable, non-movable
  HookManager(const HookManager&) = delete;
  HookManager& operator=(const HookManager&) = delete;
  HookManager(HookManager&&) = delete;
  HookManager& operator=(HookManager&&) = delete;

  // Initialize the ghost hook system (installs VEH)
  bool Initialize();

  // Shutdown and remove all hooks
  void Shutdown();

  // Check if initialized
  bool IsInitialized() const;

  // Install a hardware breakpoint hook
  // Returns hook ID (0-3) on success, -1 on failure
  // address: The address to intercept
  // callback: Function to call when breakpoint is hit
  // userData: Optional user data passed to callback
  int InstallHook(uintptr_t address, HookCallback callback, void* userData = nullptr);

  // Install hook by function pointer
  template <typename T> int InstallHook(T* function, HookCallback callback, void* userData = nullptr) {
    return InstallHook(reinterpret_cast<uintptr_t>(function), callback, userData);
  }

  // Remove a hook by ID
  bool RemoveHook(int hookId);

  // Remove a hook by address
  bool RemoveHookByAddress(uintptr_t address);

  // Get hook slot info
  const HookSlot* GetHookSlot(int hookId) const;

  // Get number of active hooks
  int GetActiveHookCount() const;

  // Check if an address is hooked
  bool IsAddressHooked(uintptr_t address) const;

  // Temporarily disable a hook (useful during original call)
  void DisableHook(int hookId);
  void EnableHook(int hookId);

  // Get statistics
  struct Stats {
    uint64_t totalHits;         // Total times any hook was triggered
    uint64_t callbacksExecuted; // Callbacks that ran
    uint64_t skippedCalls;      // Original calls that were skipped
  };
  Stats GetStats() const;

private:
  HookManager() = default;
  ~HookManager();

  // VEH handler (called on breakpoint)
  static LONG WINAPI VehHandler(PEXCEPTION_POINTERS exInfo);

  // Apply HWBP to all threads
  bool ApplyBreakpointsToAllThreads();

  // Apply HWBP to a single thread
  bool ApplyBreakpointsToThread(HANDLE hThread);

  // Clear all breakpoints from all threads
  void ClearAllBreakpoints();

  // Find free hook slot
  int FindFreeSlot() const;

  // Find hook by address
  int FindHookByAddress(uintptr_t address) const;

  // Internal state
  HookSlot m_Slots[kMaxHooks]{};
  PVOID m_VehHandle = nullptr;
  bool m_Initialized = false;
  Stats m_Stats{};

  // For thread-safe access
  CRITICAL_SECTION m_Lock{};
  bool m_LockInitialized = false;
};

// ============================================================================
// CONVENIENCE MACROS
// ============================================================================

// Helper to call the original function from within a hook callback
// Usage: GHOST_CALL_ORIGINAL(context, ReturnType, arg1, arg2, ...)
// Note: This temporarily removes the breakpoint for the current thread
#define GHOST_DECLARE_ORIGINAL(RetType, Name, ...)                                                                     \
  using Name##_t = RetType(WINAPI*)(__VA_ARGS__);                                                                      \
  static Name##_t Name##_Original = nullptr;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Get the return address from the context (for logging/debugging)
uintptr_t GetReturnAddress(const CONTEXT* context);

// Set the return value in context (for skipping original functions)
void SetReturnValue(CONTEXT* context, uintptr_t value);

// Skip function execution and return immediately
// Call this from your callback and return false
void SkipFunction(CONTEXT* context, uintptr_t returnValue = 0);

// Get function arguments (x64 calling convention)
#ifdef _WIN64
uintptr_t GetArg1(const CONTEXT* context); // RCX
uintptr_t GetArg2(const CONTEXT* context); // RDX
uintptr_t GetArg3(const CONTEXT* context); // R8
uintptr_t GetArg4(const CONTEXT* context); // R9
// Args 5+ are on stack at RSP+0x28, RSP+0x30, etc.
#else
uintptr_t GetArg1(const CONTEXT* context); // [ESP+4]
uintptr_t GetArg2(const CONTEXT* context); // [ESP+8]
uintptr_t GetArg3(const CONTEXT* context); // [ESP+12]
uintptr_t GetArg4(const CONTEXT* context); // [ESP+16]
#endif

} // namespace Ghost
