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

#include "ghost_hook.h"

#include <tlhelp32.h>

// ============================================================================
// GHOST HOOK IMPLEMENTATION
// ============================================================================
// Uses Hardware Breakpoints (Dr0-Dr3) to intercept function calls without
// modifying any code in memory. This is undetectable by integrity checks.
//
// How it works:
// 1. Set Dr0-Dr3 to target addresses
// 2. Configure Dr7 for execution breakpoints
// 3. VEH catches EXCEPTION_SINGLE_STEP
// 4. Execute user callback
// 5. Resume execution (or skip function)
// ============================================================================

namespace Ghost {

// Thread-local to track if we're inside a hook callback (prevent recursion)
static thread_local bool tl_InsideCallback = false;
static thread_local int tl_DisabledSlot = -1;

// ============================================================================
// DR7 REGISTER HELPERS
// ============================================================================

// Dr7 bit layout for x86/x64:
// Bits 0,2,4,6: Local enable for Dr0-Dr3
// Bits 1,3,5,7: Global enable for Dr0-Dr3 (not used in user mode)
// Bits 16-17: Condition for Dr0 (00=execute, 01=write, 11=read/write)
// Bits 18-19: Length for Dr0 (00=1 byte for execute)
// Bits 20-21, 22-23: Condition/Length for Dr1
// Bits 24-25, 26-27: Condition/Length for Dr2
// Bits 28-29, 30-31: Condition/Length for Dr3

static constexpr DWORD64 DR7_LOCAL_ENABLE_DR0 = (1ULL << 0);
static constexpr DWORD64 DR7_LOCAL_ENABLE_DR1 = (1ULL << 2);
static constexpr DWORD64 DR7_LOCAL_ENABLE_DR2 = (1ULL << 4);
static constexpr DWORD64 DR7_LOCAL_ENABLE_DR3 = (1ULL << 6);

static constexpr DWORD64 DR7_CONDITION_EXECUTE = 0; // 00 = break on execute
static constexpr DWORD64 DR7_LENGTH_1BYTE = 0;      // 00 = 1 byte (for execute)

// Get enable bit for a slot
static DWORD64 GetEnableBit(int slot) {
  switch (slot) {
    case 0: return DR7_LOCAL_ENABLE_DR0;
    case 1: return DR7_LOCAL_ENABLE_DR1;
    case 2: return DR7_LOCAL_ENABLE_DR2;
    case 3: return DR7_LOCAL_ENABLE_DR3;
    default: return 0;
  }
}

// Get condition/length bits position for a slot
static int GetConditionBitPos(int slot) {
  return 16 + (slot * 4);
}

// Build Dr7 value for all active slots
// Phase 3: Added explicit clearing of condition/length bits for
// disabled/inactive slots as defense-in-depth against stale Dr7 state.
static DWORD64 BuildDr7(const HookSlot* slots, int disabledSlot = -1) {
  DWORD64 dr7 = 0;

  for (int i = 0; i < static_cast<int>(kMaxHooks); i++) {
    int pos = GetConditionBitPos(i);
    if (slots[i].active && i != disabledSlot) {
      dr7 |= GetEnableBit(i);
      // Set condition=execute (00), length=1 (00) at bits 16+i*4
      dr7 &= ~(0xFULL << pos); // Clear condition/length bits
                               // 00 for execute, 00 for 1-byte (both zero, so nothing to OR)
    } else {
      // Defense-in-depth: explicitly clear condition/length bits for
      // inactive/disabled slots to prevent stale Dr7 state from leaking.
      dr7 &= ~(0xFULL << pos);
    }
  }

  return dr7;
}

// ============================================================================
// HOOK MANAGER IMPLEMENTATION
// ============================================================================

HookManager& HookManager::Get() {
  static HookManager instance;
  return instance;
}

HookManager::~HookManager() {
  Shutdown();
}

bool HookManager::Initialize() {
  if (m_Initialized) return true;

  // Initialize critical section
  if (!m_LockInitialized) {
    InitializeCriticalSection(&m_Lock);
    m_LockInitialized = true;
  }

  // Initialize all slots as inactive
  for (size_t i = 0; i < kMaxHooks; i++) {
    m_Slots[i] = {};
  }

  // Install VEH (priority 1 = first handler)
  m_VehHandle = AddVectoredExceptionHandler(1, VehHandler);
  if (!m_VehHandle) {
    return false;
  }

  m_Initialized = true;
  return true;
}

void HookManager::Shutdown() {
  if (!m_Initialized) return;

  // Clear all breakpoints first
  ClearAllBreakpoints();

  // Remove VEH
  if (m_VehHandle) {
    RemoveVectoredExceptionHandler(m_VehHandle);
    m_VehHandle = nullptr;
  }

  // Clear all slots
  EnterCriticalSection(&m_Lock);
  for (size_t i = 0; i < kMaxHooks; i++) {
    m_Slots[i] = {};
  }
  LeaveCriticalSection(&m_Lock);

  m_Initialized = false;
}

bool HookManager::IsInitialized() const {
  return m_Initialized;
}

int HookManager::InstallHook(uintptr_t address, HookCallback callback, void* userData) {
  if (!m_Initialized) return -1;
  if (!callback) return -1;
  if (address == 0) return -1;

  EnterCriticalSection(&m_Lock);

  int slot = FindFreeSlot();
  if (slot < 0) {
    LeaveCriticalSection(&m_Lock);
    return -1; // No free slots
  }

  // Check if already hooked
  if (FindHookByAddress(address) >= 0) {
    LeaveCriticalSection(&m_Lock);
    return -1; // Already hooked
  }

  // Fill slot
  m_Slots[slot].address = address;
  m_Slots[slot].callback = std::move(callback);
  m_Slots[slot].userData = userData;
  m_Slots[slot].active = true;
  m_DesiredAddr[slot].store(address, std::memory_order_release);

  LeaveCriticalSection(&m_Lock);

  // Apply to all threads
  if (!ApplyBreakpointsToAllThreads()) {
    // Rollback
    EnterCriticalSection(&m_Lock);
    m_Slots[slot] = {};
    LeaveCriticalSection(&m_Lock);
    return -1;
  }

  return slot;
}

bool HookManager::RemoveHook(int hookId) {
  if (!m_Initialized) return false;
  if (hookId < 0 || hookId >= static_cast<int>(kMaxHooks)) return false;

  EnterCriticalSection(&m_Lock);
  if (!m_Slots[hookId].active) {
    LeaveCriticalSection(&m_Lock);
    return false;
  }

  m_Slots[hookId] = {};
  LeaveCriticalSection(&m_Lock);

  // Re-apply breakpoints (will remove the cleared one)
  ApplyBreakpointsToAllThreads();

  return true;
}

bool HookManager::RemoveHookByAddress(uintptr_t address) {
  EnterCriticalSection(&m_Lock);
  int slot = FindHookByAddress(address);
  LeaveCriticalSection(&m_Lock);

  if (slot < 0) return false;
  return RemoveHook(slot);
}

const HookSlot* HookManager::GetHookSlot(int hookId) const {
  if (hookId < 0 || hookId >= static_cast<int>(kMaxHooks)) return nullptr;
  return &m_Slots[hookId];
}

int HookManager::GetActiveHookCount() const {
  int count = 0;
  for (size_t i = 0; i < kMaxHooks; i++) {
    if (m_Slots[i].active) count++;
  }
  return count;
}

bool HookManager::IsAddressHooked(uintptr_t address) const {
  return FindHookByAddress(address) >= 0;
}

void HookManager::DisableHook(int hookId) {
  if (hookId < 0 || hookId >= static_cast<int>(kMaxHooks)) return;
  tl_DisabledSlot = hookId;
}

void HookManager::EnableHook(int hookId) {
  if (tl_DisabledSlot == hookId) {
    tl_DisabledSlot = -1;
  }
}

void HookManager::SwapRotatingSlots(int slotA, uintptr_t addrA, HookCallback cbA, int slotB, uintptr_t addrB,
                                    HookCallback cbB) {
  if (!m_Initialized) return;
  if (slotA < 0 || slotA >= static_cast<int>(kMaxHooks)) return;
  if (slotB < 0 || slotB >= static_cast<int>(kMaxHooks)) return;

  EnterCriticalSection(&m_Lock);

  // Update slot A
  if (addrA != 0) {
    m_Slots[slotA].address = addrA;
    m_Slots[slotA].callback = std::move(cbA);
    m_Slots[slotA].userData = nullptr;
    m_Slots[slotA].active = true;
  } else {
    m_Slots[slotA] = {};
  }

  // Update slot B
  if (addrB != 0) {
    m_Slots[slotB].address = addrB;
    m_Slots[slotB].callback = std::move(cbB);
    m_Slots[slotB].userData = nullptr;
    m_Slots[slotB].active = true;
  } else {
    m_Slots[slotB] = {};
  }

  LeaveCriticalSection(&m_Lock);

  // Update atomic desired addresses for lazy propagation.
  // The VEH handler will pick these up on the next breakpoint hit
  // and update each thread's Dr registers — ZERO thread suspension.
  m_DesiredAddr[slotA].store(addrA, std::memory_order_release);
  m_DesiredAddr[slotB].store(addrB, std::memory_order_release);
}

HookManager::Stats HookManager::GetStats() const {
  return m_Stats;
}

// ============================================================================
// VEH HANDLER
// ============================================================================

LONG WINAPI HookManager::VehHandler(PEXCEPTION_POINTERS exInfo) {
  // Only handle single-step exceptions (hardware breakpoints)
  if (exInfo->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Phase 0: Handle single-step resume (TF was set to re-enable a disabled slot)
  if (tl_DisabledSlot >= 0) {
    HookManager& mgr = Get();
    if (mgr.m_Initialized) {
      // Re-enable the previously disabled breakpoint
      EnterCriticalSection(&mgr.m_Lock);
      exInfo->ContextRecord->Dr7 = BuildDr7(mgr.m_Slots);
      LeaveCriticalSection(&mgr.m_Lock);
    }
    tl_DisabledSlot = -1;
    // Clear TF so we don't keep single-stepping
    exInfo->ContextRecord->EFlags &= ~0x100;
    exInfo->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  // Prevent recursion — track blocked re-entries for diagnostics
  if (tl_InsideCallback) {
    HookManager& mgrRef = Get();
    mgrRef.m_Stats.recursionBlocked++;
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HookManager& mgr = Get();
  if (!mgr.m_Initialized) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Get the faulting address
#ifdef _WIN64
  uintptr_t faultAddr = exInfo->ContextRecord->Rip;
#else
  uintptr_t faultAddr = exInfo->ContextRecord->Eip;
#endif

  // Find matching hook
  EnterCriticalSection(&mgr.m_Lock);
  int hookId = mgr.FindHookByAddress(faultAddr);

  if (hookId < 0 || hookId == tl_DisabledSlot) {
    LeaveCriticalSection(&mgr.m_Lock);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Copy callback info (to call outside lock)
  HookCallback callback = mgr.m_Slots[hookId].callback;
  void* userData = mgr.m_Slots[hookId].userData;
  LeaveCriticalSection(&mgr.m_Lock);

  mgr.m_Stats.totalHits++;

  // Execute callback
  bool continueToOriginal = true;
  if (callback) {
    tl_InsideCallback = true;
    try {
      continueToOriginal = callback(exInfo->ContextRecord, userData);
      mgr.m_Stats.callbacksExecuted++;
    } catch (...) {
      // Callback threw - continue to original
      continueToOriginal = true;
    }
    tl_InsideCallback = false;
  }

  if (!continueToOriginal) {
    mgr.m_Stats.skippedCalls++;
  }

  // Phase 0: Full Dr6/Dr7 context sanitization
  // Clear the debug status register to acknowledge the exception
  exInfo->ContextRecord->Dr6 = 0;

  // ---- Lazy propagation of rotating slots ----
  // Update Dr0-Dr3 to match the desired addresses. This is how
  // SwapRotatingSlots changes propagate to each thread WITHOUT
  // ever suspending them. Each thread self-updates on its next
  // breakpoint hit (Present/ExecuteCommandLists fire frequently).
  {
    uintptr_t d0 = mgr.m_DesiredAddr[0].load(std::memory_order_acquire);
    uintptr_t d1 = mgr.m_DesiredAddr[1].load(std::memory_order_acquire);
    uintptr_t d2 = mgr.m_DesiredAddr[2].load(std::memory_order_acquire);
    uintptr_t d3 = mgr.m_DesiredAddr[3].load(std::memory_order_acquire);
    exInfo->ContextRecord->Dr0 = d0;
    exInfo->ContextRecord->Dr1 = d1;
    exInfo->ContextRecord->Dr2 = d2;
    exInfo->ContextRecord->Dr3 = d3;
  }

  if (continueToOriginal) {
    // Temporarily disable the breakpoint for this hook and set TF (Trap Flag)
    // so we get a single-step exception after executing the original instruction.
    // On the single-step, we re-enable the breakpoint.
    tl_DisabledSlot = hookId;

    // Rebuild Dr7 with this slot disabled
    EnterCriticalSection(&mgr.m_Lock);
    exInfo->ContextRecord->Dr7 = BuildDr7(mgr.m_Slots, hookId);
    LeaveCriticalSection(&mgr.m_Lock);

    // Set Trap Flag (bit 8) for single-step
    exInfo->ContextRecord->EFlags |= 0x100;
  } else {
    // Not continuing — still rebuild Dr7 to reflect current state
    EnterCriticalSection(&mgr.m_Lock);
    exInfo->ContextRecord->Dr7 = BuildDr7(mgr.m_Slots);
    LeaveCriticalSection(&mgr.m_Lock);
  }

  return EXCEPTION_CONTINUE_EXECUTION;
}

// ============================================================================
// THREAD ENUMERATION & BREAKPOINT APPLICATION
// ============================================================================

bool HookManager::ApplyBreakpointsToAllThreads() {
  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (hSnapshot == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD currentPid = GetCurrentProcessId();
  DWORD currentTid = GetCurrentThreadId();

  THREADENTRY32 te;
  te.dwSize = sizeof(te);

  bool success = true;

  if (Thread32First(hSnapshot, &te)) {
    do {
      if (te.th32OwnerProcessID == currentPid) {
        if (te.th32ThreadID == currentTid) {
          // Apply to current thread directly
          CONTEXT ctx{};
          ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

          // Get current debug registers
          // For current thread, we need to use NtGetContextThread
          // or work with the current context
          HANDLE hThread = GetCurrentThread();
          if (GetThreadContext(hThread, &ctx)) {
            EnterCriticalSection(&m_Lock);
            ctx.Dr0 = m_Slots[0].active ? m_Slots[0].address : 0;
            ctx.Dr1 = m_Slots[1].active ? m_Slots[1].address : 0;
            ctx.Dr2 = m_Slots[2].active ? m_Slots[2].address : 0;
            ctx.Dr3 = m_Slots[3].active ? m_Slots[3].address : 0;
            ctx.Dr6 = 0;
            ctx.Dr7 = BuildDr7(m_Slots, tl_DisabledSlot);
            LeaveCriticalSection(&m_Lock);

            SetThreadContext(hThread, &ctx);
          }
        } else {
          HANDLE hThread =
              OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
          if (hThread) {
            if (!ApplyBreakpointsToThread(hThread)) {
              success = false;
            }
            CloseHandle(hThread);
          }
        }
      }
    } while (Thread32Next(hSnapshot, &te));
  }

  CloseHandle(hSnapshot);
  return success;
}

bool HookManager::ApplyBreakpointsToThread(HANDLE hThread) {
  // Suspend thread to safely modify context
  if (SuspendThread(hThread) == (DWORD)-1) {
    return false;
  }

  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

  bool success = false;
  if (GetThreadContext(hThread, &ctx)) {
    EnterCriticalSection(&m_Lock);

    // Set debug registers
    ctx.Dr0 = m_Slots[0].active ? m_Slots[0].address : 0;
    ctx.Dr1 = m_Slots[1].active ? m_Slots[1].address : 0;
    ctx.Dr2 = m_Slots[2].active ? m_Slots[2].address : 0;
    ctx.Dr3 = m_Slots[3].active ? m_Slots[3].address : 0;
    ctx.Dr6 = 0; // Clear debug status
    ctx.Dr7 = BuildDr7(m_Slots);

    LeaveCriticalSection(&m_Lock);

    success = SetThreadContext(hThread, &ctx) != FALSE;
  }

  ResumeThread(hThread);
  return success;
}

void HookManager::ClearAllBreakpoints() {
  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (hSnapshot == INVALID_HANDLE_VALUE) return;

  DWORD currentPid = GetCurrentProcessId();
  THREADENTRY32 te;
  te.dwSize = sizeof(te);

  if (Thread32First(hSnapshot, &te)) {
    do {
      if (te.th32OwnerProcessID == currentPid) {
        HANDLE hThread =
            OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (hThread) {
          SuspendThread(hThread);

          CONTEXT ctx{};
          ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
          if (GetThreadContext(hThread, &ctx)) {
            ctx.Dr0 = 0;
            ctx.Dr1 = 0;
            ctx.Dr2 = 0;
            ctx.Dr3 = 0;
            ctx.Dr6 = 0;
            ctx.Dr7 = 0;
            SetThreadContext(hThread, &ctx);
          }

          ResumeThread(hThread);
          CloseHandle(hThread);
        }
      }
    } while (Thread32Next(hSnapshot, &te));
  }

  CloseHandle(hSnapshot);
}

int HookManager::FindFreeSlot() const {
  for (size_t i = 0; i < kMaxHooks; i++) {
    if (!m_Slots[i].active) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int HookManager::FindHookByAddress(uintptr_t address) const {
  for (size_t i = 0; i < kMaxHooks; i++) {
    if (m_Slots[i].active && m_Slots[i].address == address) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

uintptr_t GetReturnAddress(const CONTEXT* context) {
  if (!context) return 0;
#ifdef _WIN64
  // Return address is at [RSP]
  return *reinterpret_cast<uintptr_t*>(context->Rsp);
#else
  // Return address is at [ESP]
  return *reinterpret_cast<uintptr_t*>(context->Esp);
#endif
}

void SetReturnValue(CONTEXT* context, uintptr_t value) {
  if (!context) return;
#ifdef _WIN64
  context->Rax = value;
#else
  context->Eax = static_cast<DWORD>(value);
#endif
}

void SkipFunction(CONTEXT* context, uintptr_t returnValue) {
  if (!context) return;

  // Set return value
  SetReturnValue(context, returnValue);

#ifdef _WIN64
  // Get return address and pop it
  uintptr_t retAddr = *reinterpret_cast<uintptr_t*>(context->Rsp);
  context->Rsp += 8;      // Pop return address
  context->Rip = retAddr; // Jump to return address
#else
  uintptr_t retAddr = *reinterpret_cast<uintptr_t*>(context->Esp);
  context->Esp += 4; // Pop return address
  context->Eip = static_cast<DWORD>(retAddr);
#endif
}

#ifdef _WIN64
uintptr_t GetArg1(const CONTEXT* context) {
  return context ? context->Rcx : 0;
}
uintptr_t GetArg2(const CONTEXT* context) {
  return context ? context->Rdx : 0;
}
uintptr_t GetArg3(const CONTEXT* context) {
  return context ? context->R8 : 0;
}
uintptr_t GetArg4(const CONTEXT* context) {
  return context ? context->R9 : 0;
}
#else
uintptr_t GetArg1(const CONTEXT* context) {
  return context ? *reinterpret_cast<uintptr_t*>(context->Esp + 4) : 0;
}
uintptr_t GetArg2(const CONTEXT* context) {
  return context ? *reinterpret_cast<uintptr_t*>(context->Esp + 8) : 0;
}
uintptr_t GetArg3(const CONTEXT* context) {
  return context ? *reinterpret_cast<uintptr_t*>(context->Esp + 12) : 0;
}
uintptr_t GetArg4(const CONTEXT* context) {
  return context ? *reinterpret_cast<uintptr_t*>(context->Esp + 16) : 0;
}
#endif

} // namespace Ghost
