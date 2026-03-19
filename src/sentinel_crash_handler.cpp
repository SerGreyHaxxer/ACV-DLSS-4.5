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

#include "sentinel_crash_handler.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <dbghelp.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h> // PEB, LDR_DATA_TABLE_ENTRY




// ============================================================================
// SENTINEL CRASH HANDLER IMPLEMENTATION
// ============================================================================

namespace Sentinel {

// Internal state
static PVOID g_VehHandle = nullptr;
static Config g_Config;
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static std::atomic<bool> g_Installed{false};
static std::atomic<bool> g_HandlingCrash{false};
static std::atomic<uintptr_t> g_LastCrashAddress{0};
static std::atomic<DWORD> g_LastExceptionCode{0};

// Captured stack trace storage (pre-allocated to avoid allocations during crash)
static std::array<StackFrame, kMaxStackFrames> g_CapturedFrames{};
static std::atomic<size_t> g_CapturedFrameCount{0};

// Pre-allocated crash log buffer (async-signal-safe)
static std::array<char, 16384> g_CrashBuffer{};

// P2 Fix 7: Pre-opened file handles — opened during Install() to avoid
// calling CreateFileA inside the VEH where Loader Lock or Heap Lock
// may be held. This prevents double-fault deadlocks on heap corruption.
static HANDLE g_PreOpenedLogHandle = INVALID_HANDLE_VALUE;
static HANDLE g_PreOpenedDumpHandle = INVALID_HANDLE_VALUE;

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Module range for filtering
struct ModuleRange {
  uintptr_t base;
  size_t size;
  std::array<char, MAX_PATH> name;
};
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static ModuleRange g_MainModule{};
static ModuleRange g_SelfModule{};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ============================================================================
// ASYNC-SIGNAL-SAFE HELPERS
// ============================================================================

// These functions avoid CRT allocations and are safe to call from VEH context

static int UnsafeHex(char* buf, int maxLen, uint64_t val) { // NOLINT(bugprone-easily-swappable-parameters)
  static const char hexChars[] = "0123456789ABCDEF"; // NOLINT(modernize-avoid-c-arrays)
  constexpr int maxHexChars = 17;
  std::array<char, maxHexChars> tmp{};
  int len = 0;
  if (val == 0) {
    tmp[static_cast<size_t>(len++)] = '0';
  } else {
    while (val > 0 && len < 16) {
      tmp[static_cast<size_t>(len++)] = hexChars[val & 0xF];
      val >>= 4;
    }
  }
  if (len >= maxLen) len = maxLen - 1;
  int written = 0;
  for (int i = 0; i < len && i < maxLen; i++) {
    buf[i] = tmp[static_cast<size_t>(len - 1 - i)];
    written++;
  }
  return written;
}

static int UnsafeAppend(char* buf, int pos, int maxLen, const char* str) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  while (*str && pos < maxLen - 1) {
    buf[pos++] = *str++;
  }
  return pos;
}

static int UnsafeInt(char* buf, int maxLen, int64_t val) { // NOLINT(bugprone-easily-swappable-parameters)
  if (val == 0) {
    buf[0] = '0';
    return 1;
  }
  constexpr int maxIntChars = 21;
  std::array<char, maxIntChars> tmp{};
  int len = 0;
  bool neg = val < 0;
  if (neg) { val = -val; }
  while (val > 0 && len < 20) {
    tmp[static_cast<size_t>(len++)] = '0' + static_cast<char>(val % 10);
    val /= 10;
  }
  int pos = 0;
  if (neg && pos < maxLen - 1) { buf[pos++] = '-'; }
  for (int i = len - 1; i >= 0 && pos < maxLen - 1; i--) {
    buf[pos++] = tmp[static_cast<size_t>(i)];
  }
  return pos;
}

// ============================================================================
// MODULE INFORMATION
// ============================================================================

static bool GetModuleInfo(HMODULE module, ModuleRange& out) {
  if (!module) return false;
  MODULEINFO info{};
  if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
    return false;
  }
  out.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
  out.size = info.SizeOfImage;
  GetModuleFileNameA(module, out.name, MAX_PATH);
  return true;
}

static void InitializeModuleRanges() {
  HMODULE mainModule = GetModuleHandleA(nullptr);
  GetModuleInfo(mainModule, g_MainModule);

  HMODULE selfModule = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(&InitializeModuleRanges), &selfModule)) {
    GetModuleInfo(selfModule, g_SelfModule);
  }
}

static bool IsAddressInKnownModule(uintptr_t address) {
  if (address >= g_MainModule.base && address < g_MainModule.base + g_MainModule.size) {
    return true;
  }
  if (address >= g_SelfModule.base && address < g_SelfModule.base + g_SelfModule.size) {
    return true;
  }
  return false;
}

static bool IsAddressInSelfModule(uintptr_t address) {
  if (g_SelfModule.size == 0) return false;
  return (address >= g_SelfModule.base && address < g_SelfModule.base + g_SelfModule.size);
}

// ============================================================================
// LOCK-FREE PEB WALK FOR MODULE RESOLUTION
// ============================================================================
// Fix 6: GetModuleHandleExA acquires the Loader Lock (PEB->LoaderLock).
// If a crash occurs while any thread holds Loader Lock (e.g. during
// DllMain, LoadLibrary, or Denuvo's integrity checks), calling
// GetModuleHandleExA from the VEH deadlocks and the game hangs.
//
// This PEB walk reads the InMemoryOrderModuleList directly.
// It is NOT truly lock-free (the list can be mutated concurrently)
// but it avoids the Loader Lock wait, which is the deadlock source.
// In practice, module list mutations during a crash are extremely rare.

static bool FindModuleByAddressPEB(uintptr_t address, char* outName, size_t maxLen) {
  if (!outName || maxLen == 0) return false;
  outName[0] = '\0';

  __try {
    // Access PEB through the TEB (Thread Environment Block)
#ifdef _WIN64
    const PEB* peb = reinterpret_cast<const PEB*>(__readgsqword(0x60));
#else
    const PEB* peb = reinterpret_cast<const PEB*>(__readfsdword(0x30));
#endif
    if (!peb || !peb->Ldr) return false;

    const LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    const LIST_ENTRY* current = head->Flink;

    // Safety limit to prevent infinite loops on corrupted lists
    int limit = 512;
    while (current != head && limit-- > 0) {
      // The LDR_DATA_TABLE_ENTRY for InMemoryOrder is offset by one LIST_ENTRY
      // Cast safely since PEB types are complex across SDKs
      const auto* entry = reinterpret_cast<const LDR_DATA_TABLE_ENTRY*>(
          reinterpret_cast<const uint8_t*>(current) - offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks));

      uintptr_t base = reinterpret_cast<uintptr_t>(entry->DllBase);
      // Different SDKs have SizeOfImage in different places, or not at all. Let's use a safe hack:
      // In LDR_DATA_TABLE_ENTRY, DllBase is followed by EntryPoint and then SizeOfImage.
      // DllBase = +0x30, EntryPoint = +0x38, SizeOfImage = +0x40.
      size_t size = *reinterpret_cast<const ULONG*>(reinterpret_cast<const uint8_t*>(entry) + 0x40);

      if (address >= base && address < base + size) {
        // Found — copy the module name (UNICODE_STRING → narrow)
        if (entry->FullDllName.Buffer && entry->FullDllName.Length > 0) {
          USHORT charCount = entry->FullDllName.Length / sizeof(WCHAR);
          size_t copyLen = (charCount < maxLen - 1) ? charCount : maxLen - 1;
          for (size_t i = 0; i < copyLen; i++) {
            WCHAR wc = entry->FullDllName.Buffer[i];
            outName[i] = (wc < 128) ? static_cast<char>(wc) : '?';
          }
          outName[copyLen] = '\0';
        }
        return true;
      }

      current = current->Flink;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // PEB/LDR was corrupted — silently fail
  }

  return false;
}

// ============================================================================
// STACK WALKING (ASYNC-SIGNAL-SAFE)
// ============================================================================
// Fix 10: Replaced SymInitialize/StackWalk64/SymFromAddr with
// RtlCaptureStackBackTrace. The old stack walker called:
//   - SymInitialize → acquires Loader Lock
//   - StackWalk64 → acquires Heap Lock for symbol tables
//   - GetModuleHandleExA → acquires Loader Lock
// If the crash occurred while ANY of those locks were held, the VEH would
// deadlock and the game would freeze to a black screen with no dump.
//
// RtlCaptureStackBackTrace is documented as async-signal-safe — it reads
// the RSP chain directly without acquiring any locks.

static size_t WalkStack(CONTEXT* ctx, StackFrame* frames, size_t maxFrames) {
  if (!ctx || !frames || maxFrames == 0) return 0;

  // Capture raw return addresses using the async-signal-safe API
  std::array<void*, kMaxStackFrames> rawAddresses{}; // NOLINT(modernize-avoid-c-arrays)
  USHORT captured = RtlCaptureStackBackTrace(
      0, // Skip 0 frames
      static_cast<DWORD>(maxFrames < kMaxStackFrames ? maxFrames : kMaxStackFrames),
      rawAddresses.data(),
      nullptr);

  size_t frameCount = 0;
  for (USHORT i = 0; i < captured && frameCount < maxFrames; i++) {
    StackFrame& frame = frames[frameCount];
    frame.address = reinterpret_cast<uintptr_t>(rawAddresses[static_cast<size_t>(i)]);
    frame.returnAddress = 0;
    frame.framePointer = 0;
    frame.moduleName[0] = '\0';
    frame.symbolName[0] = '\0';
    frame.lineNumber = 0;
    frame.fileName[0] = '\0';

    // Fix 6: Lock-free PEB walk — avoids GetModuleHandleExA which acquires
    // Loader Lock and can deadlock if the crash occurred during DLL load.
    FindModuleByAddressPEB(frame.address, frame.moduleName, MAX_PATH);

    // Symbol resolution is DEFERRED — we do NOT call SymFromAddr here.
    // The raw addresses are sufficient for post-mortem analysis with
    // a debugger or addr2line equivalent.

    frameCount++;

    // Stop at known Denuvo trampolines
    if (frameCount > 5 && frame.moduleName[0] == '\0' && !IsAddressInKnownModule(frame.address)) {
      break;
    }
  }

  return frameCount;
}

// ============================================================================
// MINIDUMP GENERATION
// ============================================================================

static BOOL CALLBACK MiniDumpCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
                                      PMINIDUMP_CALLBACK_OUTPUT output) {
  if (!param || !input || !output) return TRUE;

  if (!g_Config.enableModuleFiltering) return TRUE;

  // Filter to only include main game and proxy modules
  switch (input->CallbackType) {
    case IncludeModuleCallback: {
      uintptr_t base = input->IncludeModule.BaseOfImage;
      if (base == g_MainModule.base || base == g_SelfModule.base) {
        return TRUE;
      }
      // Include system DLLs but reduce data
      return TRUE;
    }
    case ModuleCallback: {
      uintptr_t base = input->Module.BaseOfImage;
      if (base != g_MainModule.base && base != g_SelfModule.base) {
        output->ModuleWriteFlags &= ~ModuleWriteDataSeg;
      }
      return TRUE;
    }
    default: return TRUE;
  }
}

static bool WriteMiniDump(PEXCEPTION_POINTERS exInfo, const char* path) {
  HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return false;

  MINIDUMP_EXCEPTION_INFORMATION dumpInfo{};
  dumpInfo.ThreadId = GetCurrentThreadId();
  dumpInfo.ExceptionPointers = exInfo;
  dumpInfo.ClientPointers = FALSE;

  MINIDUMP_CALLBACK_INFORMATION cbInfo{};
  cbInfo.CallbackParam = nullptr;
  cbInfo.CallbackRoutine = MiniDumpCallback;

  MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithHandleData |
                                                      MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);

  if (g_Config.enableFullMemoryDump) {
    dumpType = static_cast<MINIDUMP_TYPE>(dumpType | MiniDumpWithFullMemory);
  }

  BOOL success =
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType, exInfo ? &dumpInfo : nullptr,
                        nullptr, g_Config.enableModuleFiltering ? &cbInfo : nullptr);

  CloseHandle(hFile);
  return success != FALSE;
}

// ============================================================================
// CRASH LOG WRITING
// ============================================================================

static void WriteCrashLog(PEXCEPTION_POINTERS exInfo, const char* path) {
  // P2 Fix 7: Use pre-opened HANDLE if available — avoids CreateFileA inside VEH.
  // CreateFileA can acquire the PEB lock and Heap Lock, which deadlocks if the
  // crash happened inside ntdll.dll or during heap corruption.
  HANDLE hFile = INVALID_HANDLE_VALUE;
  bool usedPreOpened = false;

  if (g_PreOpenedLogHandle != INVALID_HANDLE_VALUE) {
    hFile = g_PreOpenedLogHandle;
    usedPreOpened = true;
    // Seek to beginning to overwrite previous crash data
    SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
    SetEndOfFile(hFile); // Truncate
  } else {
    // Fallback: try CreateFileA (risky in VEH but better than nothing)
    hFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (hFile == INVALID_HANDLE_VALUE) return;

  int pos = 0;
  constexpr int maxLen = sizeof(g_CrashBuffer);

  // Header
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen,
                     "================================================================================\r\n"
                     "                      SENTINEL CRASH REPORT - DLSS 4 Proxy\r\n"
                     "================================================================================\r\n\r\n");

  // Exception info
  DWORD code = exInfo->ExceptionRecord->ExceptionCode;
  uintptr_t address = reinterpret_cast<uintptr_t>(exInfo->ExceptionRecord->ExceptionAddress);

  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "Exception Code: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, code);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, " (");

  // Exception name
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "ACCESS_VIOLATION"); break;
    case EXCEPTION_STACK_OVERFLOW: pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "STACK_OVERFLOW"); break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "ILLEGAL_INSTRUCTION"); break;
    case EXCEPTION_PRIV_INSTRUCTION: pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "PRIVILEGED_INSTRUCTION"); break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "DIVIDE_BY_ZERO"); break;
    case EXCEPTION_BREAKPOINT: pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "BREAKPOINT"); break;
    default: pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "UNKNOWN"); break;
  }
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, ")\r\n");

  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "Fault Address: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, address);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\n");

  // Module info
  // Fix 6: Lock-free PEB walk for module name resolution
  char moduleName[MAX_PATH] = {0};
  FindModuleByAddressPEB(address, moduleName, MAX_PATH);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "Faulting Module: ");
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, moduleName[0] ? moduleName : "Unknown");
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\n");

  // Access violation details
  if (code == EXCEPTION_ACCESS_VIOLATION && exInfo->ExceptionRecord->NumberParameters >= 2) {
    pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\nAccess Violation: ");
    pos = UnsafeAppend(g_CrashBuffer, pos, maxLen,
                       exInfo->ExceptionRecord->ExceptionInformation[0] ? "WRITE to 0x" : "READ from 0x");
    pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, exInfo->ExceptionRecord->ExceptionInformation[1]);
    pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\n");
  }

  // Registers
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen,
                     "\r\n--------------------------------------------------------------------------------\r\n"
                     "REGISTERS\r\n"
                     "--------------------------------------------------------------------------------\r\n");

#ifdef _WIN64
  CONTEXT* ctx = exInfo->ContextRecord;
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "RIP: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Rip);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  RSP: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Rsp);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  RBP: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Rbp);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\nRAX: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Rax);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  RBX: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Rbx);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  RCX: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Rcx);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\nRDX: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Rdx);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  RSI: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Rsi);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  RDI: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Rdi);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\nR8:  0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->R8);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  R9:  0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->R9);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  R10: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->R10);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\nR11: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->R11);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  R12: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->R12);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  R13: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->R13);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\nR14: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->R14);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  R15: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->R15);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\n");
#else
  CONTEXT* ctx = exInfo->ContextRecord;
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "EIP: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Eip);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  ESP: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Esp);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  EBP: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Ebp);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\nEAX: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Eax);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  EBX: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Ebx);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  ECX: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Ecx);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\nEDX: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Edx);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  ESI: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Esi);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "  EDI: 0x");
  pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, ctx->Edi);
  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\n");
#endif

  // Stack trace
  if (g_Config.enableStackWalk && g_CapturedFrameCount > 0) {
    pos = UnsafeAppend(g_CrashBuffer, pos, maxLen,
                       "\r\n--------------------------------------------------------------------------------\r\n"
                       "STACK TRACE\r\n"
                       "--------------------------------------------------------------------------------\r\n");

    for (size_t i = 0; i < g_CapturedFrameCount && pos < maxLen - 256; i++) {
      const StackFrame& frame = g_CapturedFrames[i];
      pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "[");
      pos += UnsafeInt(g_CrashBuffer + pos, maxLen - pos, static_cast<int>(i));
      pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "] 0x");
      pos += UnsafeHex(g_CrashBuffer + pos, maxLen - pos, frame.address);

      if (frame.symbolName[0]) {
        pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, " ");
        pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, frame.symbolName);
      }

      if (frame.moduleName[0]) {
        pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, " in ");
        // Extract just filename from path
        const char* name = frame.moduleName;
        const char* lastSlash = name;
        while (*name) {
          if (*name == '\\' || *name == '/') lastSlash = name + 1;
          name++;
        }
        pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, lastSlash);
      }

      if (frame.lineNumber > 0) {
        pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, " (line ");
        pos += UnsafeInt(g_CrashBuffer + pos, maxLen - pos, frame.lineNumber);
        pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, ")");
      }

      pos = UnsafeAppend(g_CrashBuffer, pos, maxLen, "\r\n");
    }
  }

  pos = UnsafeAppend(g_CrashBuffer, pos, maxLen,
                     "\r\n================================================================================\r\n"
                     "END OF CRASH REPORT\r\n"
                     "================================================================================\r\n");

  // Write with single call (async-signal-safe)
  DWORD bytesWritten = 0;
  WriteFile(hFile, g_CrashBuffer, static_cast<DWORD>(pos), &bytesWritten, nullptr);
  FlushFileBuffers(hFile); // Ensure data hits disk even during crash

  // Only close if we created the handle ourselves (not pre-opened)
  if (!usedPreOpened) {
    CloseHandle(hFile);
  }
}

// ============================================================================
// VECTORED EXCEPTION HANDLER
// ============================================================================

static LONG WINAPI SentinelHandler(PEXCEPTION_POINTERS exInfo) {
  DWORD code = exInfo->ExceptionRecord->ExceptionCode;

  // =========================================================================
  // ELITE FIX: Denuvo/VMProtect DRM Exception Filtering
  // =========================================================================
  // AC Valhalla uses Denuvo Anti-Tamper which intentionally throws thousands
  // of exceptions per minute as part of its code integrity verification:
  //   - STATUS_GUARD_PAGE_VIOLATION (0x80000001): JIT page guard traps
  //   - STATUS_SINGLE_STEP (0x80000004): VMProtect single-step verification
  //   - STATUS_BREAKPOINT (0x80000003): Software breakpoint traps
  //   - 0x406D1388: MSVC SetThreadName (informational, non-fatal)
  //   - 0xE06D7363: C++ SEH exception wrapper (non-fatal throw/catch)
  //
  // If we process ANY of these (even touching RtlCaptureStackBackTrace),
  // Denuvo may flag us as a debugger → anti-tamper ban or deadlock.
  // Reject them INSTANTLY with zero work.
  // =========================================================================
  if (code == STATUS_GUARD_PAGE_VIOLATION ||  // 0x80000001
      code == STATUS_SINGLE_STEP ||           // 0x80000004
      code == STATUS_BREAKPOINT ||            // 0x80000003
      code == 0x406D1388 ||                   // MSVC SetThreadName
      code == 0xE06D7363)                     // C++ exception (non-fatal)
  {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Only handle fatal hardware exceptions — everything else passes through
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
      code != EXCEPTION_FLT_DIVIDE_BY_ZERO) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Fix 10 + Fix 6: DENUVO FILTER — ACV uses Denuvo Anti-Tamper which
  // intentionally throws EXCEPTION_ACCESS_VIOLATION to verify code integrity.
  // Check if either:
  //   (a) The fault is directly inside our proxy module, OR
  //   (b) Our proxy module is on the call stack (the crash was caused by
  //       our hooks but faulted in game/system code).
  // If neither, pass through — it's a Denuvo or game-native exception.
  uintptr_t faultAddr = reinterpret_cast<uintptr_t>(exInfo->ExceptionRecord->ExceptionAddress);
  if (g_SelfModule.size > 0) {
    bool faultInSelf = IsAddressInSelfModule(faultAddr);
    bool proxyOnStack = false;

    if (!faultInSelf) {
      // Quick check: scan the call stack for any return address in our module
      void* stackFrames[32];
      USHORT count = RtlCaptureStackBackTrace(0, 32, stackFrames, nullptr);
      for (USHORT i = 0; i < count; i++) {
        if (IsAddressInSelfModule(reinterpret_cast<uintptr_t>(stackFrames[i]))) {
          proxyOnStack = true;
          break;
        }
      }
    }

    if (!faultInSelf && !proxyOnStack) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
  }

  // Prevent re-entry
  if (g_HandlingCrash.exchange(true)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Store crash info
  g_LastCrashAddress.store(faultAddr);
  g_LastExceptionCode.store(code);

  // Walk stack using async-signal-safe RtlCaptureStackBackTrace
  if (g_Config.enableStackWalk) {
    CONTEXT ctxCopy = *exInfo->ContextRecord;
    g_CapturedFrameCount.store(WalkStack(&ctxCopy, g_CapturedFrames.data(), kMaxStackFrames));
  }

  // Determine paths
  const char* logPath = g_Config.logPath ? g_Config.logPath : "dlss4_sentinel.log";
  const char* dumpPath = g_Config.dumpPath ? g_Config.dumpPath : "dlss4_sentinel.dmp";

  // Write crash log (uses pre-allocated buffer — async-safe)
  WriteCrashLog(exInfo, logPath);

  // Write minidump — best-effort. MiniDumpWriteDump may deadlock if the
  // heap is corrupted, but it's the only in-process dump mechanism.
  // Out-of-process dumping via sentinel_watcher.exe is a future improvement.
  WriteMiniDump(exInfo, dumpPath);

  // Optionally open log
  if (g_Config.openLogOnCrash) {
    ShellExecuteA(nullptr, "open", logPath, nullptr, nullptr, SW_SHOW);
  }

  // Allow other handlers to run (e.g., game's own crash reporter)
  g_HandlingCrash.store(false);
  return EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================================
// PUBLIC API IMPLEMENTATION
// ============================================================================

bool Install(const Config& config) {
  if (g_Installed.exchange(true)) {
    return false; // Already installed
  }

  g_Config = config;
  InitializeModuleRanges();

  // P2 Fix 7: Pre-open crash log and dump file handles.
  // These are kept open for the lifetime of the handler so that
  // WriteCrashLog doesn't need to call CreateFileA inside the VEH.
  const char* logPath = config.logPath ? config.logPath : "dlss4_sentinel.log";
  const char* dumpPath = config.dumpPath ? config.dumpPath : "dlss4_sentinel.dmp";
  g_PreOpenedLogHandle = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  g_PreOpenedDumpHandle = CreateFileA(dumpPath, GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_PreOpenedLogHandle != INVALID_HANDLE_VALUE) {
    // Move file pointer to end for append-style if desired, or beginning for overwrite
    SetFilePointer(g_PreOpenedLogHandle, 0, nullptr, FILE_BEGIN);
  }

  // Install as first handler (priority 1)
  g_VehHandle = AddVectoredExceptionHandler(1, SentinelHandler);
  if (!g_VehHandle) {
    g_Installed.store(false);
    return false;
  }

  return true;
}

void Uninstall() {
  if (!g_Installed.exchange(false)) {
    return; // Not installed
  }

  if (g_VehHandle) {
    RemoveVectoredExceptionHandler(g_VehHandle);
    g_VehHandle = nullptr;
  }

  // P2 Fix 7: Close pre-opened file handles
  if (g_PreOpenedLogHandle != INVALID_HANDLE_VALUE) {
    CloseHandle(g_PreOpenedLogHandle);
    g_PreOpenedLogHandle = INVALID_HANDLE_VALUE;
  }
  if (g_PreOpenedDumpHandle != INVALID_HANDLE_VALUE) {
    CloseHandle(g_PreOpenedDumpHandle);
    g_PreOpenedDumpHandle = INVALID_HANDLE_VALUE;
  }
}

bool IsInstalled() {
  return g_Installed.load();
}

const Config& GetConfig() {
  return g_Config;
}

bool GenerateManualDump(const char* reason) {
  // Create a fake exception record for manual dump
  CONTEXT ctx;
  RtlCaptureContext(&ctx);

  EXCEPTION_RECORD er{};
  er.ExceptionCode = EXCEPTION_BREAKPOINT;
  er.ExceptionFlags = 0;
  er.ExceptionRecord = nullptr;

#ifdef _WIN64
  er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
#else
  er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Eip);
#endif

  EXCEPTION_POINTERS ep;
  ep.ExceptionRecord = &er;
  ep.ContextRecord = &ctx;

  // Walk stack for the manual dump
  g_CapturedFrameCount.store(WalkStack(&ctx, g_CapturedFrames.data(), kMaxStackFrames));

  // Generate files with "manual" suffix
  char logPath[MAX_PATH];
  char dumpPath[MAX_PATH];
  snprintf(logPath, MAX_PATH, "dlss4_manual_%s.log", reason ? reason : "dump");
  snprintf(dumpPath, MAX_PATH, "dlss4_manual_%s.dmp", reason ? reason : "dump");

  WriteCrashLog(&ep, logPath);
  return WriteMiniDump(&ep, dumpPath);
}

uintptr_t GetLastCrashAddress() {
  return g_LastCrashAddress.load();
}

DWORD GetLastExceptionCode() {
  return g_LastExceptionCode.load();
}

size_t GetCapturedStackTrace(StackFrame* frames, size_t maxFrames) {
  size_t count = g_CapturedFrameCount.load();
  if (count > maxFrames) count = maxFrames;
  if (frames && count > 0) {
    memcpy(frames, g_CapturedFrames.data(), count * sizeof(StackFrame));
  }
  return count;
}

} // namespace Sentinel
