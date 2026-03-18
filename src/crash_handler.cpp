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
#include "crash_handler.h"
#include <cstdio>
#include <dbghelp.h>
#include <psapi.h>
#include <ctime>
#include <string>
#include <shellapi.h>
#include <atomic>
#include <charconv>

// Link dependencies moved to CMakeLists.txt (psapi, dbghelp)

static PVOID g_Handler = nullptr;
static std::atomic<bool> g_LogOpened(false);

namespace {
    struct DumpFilterContext {
        ULONG64 mainBase = 0;
        ULONG32 mainSize = 0;
        ULONG64 selfBase = 0;
        ULONG32 selfSize = 0;
    };

    bool GetModuleRange(HMODULE module, ULONG64& baseOut, ULONG32& sizeOut) {
        if (!module) return false;
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return false;
        baseOut = reinterpret_cast<ULONG64>(info.lpBaseOfDll);
        sizeOut = static_cast<ULONG32>(info.SizeOfImage);
        return true;
    }

    BOOL CALLBACK MiniDumpFilterCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output) {
        if (!param || !input || !output) return TRUE;
        const DumpFilterContext* ctx = reinterpret_cast<const DumpFilterContext*>(param);
        switch (input->CallbackType) {
            case IncludeModuleCallback:
                if (input->IncludeModule.BaseOfImage == ctx->mainBase ||
                    input->IncludeModule.BaseOfImage == ctx->selfBase) {
                    return TRUE;
                }
                return FALSE;
            case ModuleCallback:
                if (input->Module.BaseOfImage != ctx->mainBase &&
                    input->Module.BaseOfImage != ctx->selfBase) {
                    output->ModuleWriteFlags &= ~ModuleWriteDataSeg;
                }
                return TRUE;
            default:
                return TRUE;
        }
    }

    // ========================================================================
    // VEH-Safe Module Resolution via VirtualQuery
    // ========================================================================
    // GetModuleHandleExA is NOT safe inside a VEH handler because it acquires
    // the OS Loader Lock (LdrpLoaderLock).  If the crash occurred during
    // LoadLibrary/FreeLibrary, the loader lock is already held → deadlock.
    //
    // VirtualQuery is a safe Ring-0 kernel syscall that returns the
    // AllocationBase, which for memory-mapped PE images is the module's HMODULE.
    // ========================================================================

    HMODULE SafeGetModuleFromAddress(const void* address) noexcept {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) return nullptr;
        return static_cast<HMODULE>(mbi.AllocationBase);
    }

    // ========================================================================
    // VEH-Safe formatting helpers using C++17 std::to_chars
    // ========================================================================
    // std::to_chars is guaranteed by the standard to be:
    //   - allocation-free
    //   - locale-independent
    //   - async-signal-safe
    // This replaces the custom UnsafeHex/UnsafeAppend bit-math helpers.
    // ========================================================================

    // Append a string literal to the buffer, return updated position.
    int SafeAppend(char* buf, int pos, int maxLen, const char* str) noexcept {
        while (*str && pos < maxLen - 1) buf[pos++] = *str++;
        return pos;
    }

    // Append a hex-formatted uint64 using std::to_chars, return updated position.
    int SafeAppendHex(char* buf, int pos, int maxLen, DWORD64 val) noexcept {
        // Prefix with "0x" is handled by the caller.
        if (pos >= maxLen - 1) return pos;
        auto [ptr, ec] = std::to_chars(buf + pos, buf + maxLen - 1, val, 16);
        if (ec == std::errc{}) {
            // Uppercase the hex digits for consistency with original output
            for (char* p = buf + pos; p < ptr; ++p) {
                if (*p >= 'a' && *p <= 'f') *p -= 32; // NOLINT: ASCII math
            }
            return static_cast<int>(ptr - buf);
        }
        return pos; // Conversion failed — leave buffer unchanged
    }
}

void OpenCrashLog() {
    if (g_LogOpened.exchange(true)) return;
    ShellExecuteA(nullptr, "open", "dlss4_crash.log", nullptr, nullptr, SW_SHOW);
}

void OpenMainLog() {
    if (g_LogOpened.exchange(true)) return;
    ShellExecuteA(nullptr, "open", "dlss4_proxy.log", nullptr, nullptr, SW_SHOW);
}

// Per-thread crash buffer — prevents data corruption when multiple threads
// crash simultaneously (e.g. GPU device-removed cascading segfaults).
// alignas(64) ensures the buffer doesn't share a cache line with other data.
alignas(64) thread_local char t_crashBuf[4096];

LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    // Only catch serious errors
    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && 
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Use Win32 WriteFile (async-signal-safe) instead of CRT fopen/fprintf
    HANDLE hFile = CreateFileA("dlss4_crash.log", GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        int pos = 0;
        constexpr int maxLen = sizeof(t_crashBuf);

        pos = SafeAppend(t_crashBuf, pos, maxLen, "=== DLSS 4 PROXY CRASH REPORT ===\r\n");
        pos = SafeAppend(t_crashBuf, pos, maxLen, "Exception Code: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen, code);
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\nAddress: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen,
                         reinterpret_cast<DWORD64>(pExceptionInfo->ExceptionRecord->ExceptionAddress));
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\n");

        // Identify module using VirtualQuery (safe — bypasses loader lock)
        HMODULE hModule = SafeGetModuleFromAddress(pExceptionInfo->ExceptionRecord->ExceptionAddress);
        char moduleName[MAX_PATH] = {};
        if (hModule) {
            GetModuleFileNameA(hModule, moduleName, MAX_PATH);
        }
        pos = SafeAppend(t_crashBuf, pos, maxLen, "Module: ");
        pos = SafeAppend(t_crashBuf, pos, maxLen, moduleName[0] ? moduleName : "Unknown");
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\n");

        if (code == EXCEPTION_ACCESS_VIOLATION) {
            pos = SafeAppend(t_crashBuf, pos, maxLen, "Access Violation: ");
            pos = SafeAppend(t_crashBuf, pos, maxLen,
                pExceptionInfo->ExceptionRecord->ExceptionInformation[0] ? "Write" : "Read");
            pos = SafeAppend(t_crashBuf, pos, maxLen, " at 0x");
            pos = SafeAppendHex(t_crashBuf, pos, maxLen,
                             static_cast<DWORD64>(pExceptionInfo->ExceptionRecord->ExceptionInformation[1]));
            pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\n");
        }

        #ifdef _WIN64
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\nRegisters:\r\nRIP: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen, pExceptionInfo->ContextRecord->Rip);
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\nRSP: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen, pExceptionInfo->ContextRecord->Rsp);
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\nRAX: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen, pExceptionInfo->ContextRecord->Rax);
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\nRBX: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen, pExceptionInfo->ContextRecord->Rbx);
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\nRCX: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen, pExceptionInfo->ContextRecord->Rcx);
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\nRDX: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen, pExceptionInfo->ContextRecord->Rdx);
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\n");
        #else
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\nRegisters:\r\nEIP: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen, pExceptionInfo->ContextRecord->Eip);
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\nESP: 0x");
        pos = SafeAppendHex(t_crashBuf, pos, maxLen, pExceptionInfo->ContextRecord->Esp);
        pos = SafeAppend(t_crashBuf, pos, maxLen, "\r\n");
        #endif

        // Write pre-formatted buffer with a single WriteFile call (async-signal-safe)
        DWORD bytesWritten = 0;
        WriteFile(hFile, t_crashBuf, static_cast<DWORD>(pos), &bytesWritten, nullptr);

        // Minidump — MiniDumpWriteDump is explicitly documented as safe in VEH
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
        dumpInfo.ThreadId = GetCurrentThreadId();
        dumpInfo.ExceptionPointers = pExceptionInfo;
        dumpInfo.ClientPointers = FALSE;

        DumpFilterContext filterCtx{};
        HMODULE mainModule = GetModuleHandleA(nullptr);
        GetModuleRange(mainModule, filterCtx.mainBase, filterCtx.mainSize);

        // Resolve our own module using VirtualQuery (safe — no loader lock)
        HMODULE selfModule = SafeGetModuleFromAddress(reinterpret_cast<const void*>(&VectoredHandler));
        if (selfModule) {
            GetModuleRange(selfModule, filterCtx.selfBase, filterCtx.selfSize);
        }

        // Write filtered minidump directly (no encryption — DPAPI is not safe
        // inside a VEH handler, and encrypted dumps can't be shared for debugging)
        HANDLE hDumpFile = CreateFileA("dlss4_crash.dmp", GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hDumpFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_CALLBACK_INFORMATION cbInfo{};
            cbInfo.CallbackParam = &filterCtx;
            cbInfo.CallbackRoutine = MiniDumpFilterCallback;
            MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpFilterMemory);
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, dumpType, &dumpInfo, nullptr, &cbInfo);
            CloseHandle(hDumpFile);
        }

        CloseHandle(hFile);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler() {
    if (!g_Handler) {
        g_Handler = AddVectoredExceptionHandler(1, VectoredHandler); // 1 = Call first
    }
}

void UninstallCrashHandler() {
    if (g_Handler) {
        RemoveVectoredExceptionHandler(g_Handler);
        g_Handler = nullptr;
    }
}
