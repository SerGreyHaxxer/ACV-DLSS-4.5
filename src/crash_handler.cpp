#include "crash_handler.h"
#include <cstdio>
#include <dbghelp.h>
#include <psapi.h>
#include <ctime>
#include <vector>
#include <string>
#include <sstream>
#include <shellapi.h>
#include <atomic>
#include <wincrypt.h>

// Link dependencies moved to CMakeLists.txt (psapi, dbghelp, crypt32)

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

    bool EncryptDumpFile(const char* inputPath, const char* outputPath) {
        HANDLE inputFile = CreateFileA(inputPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (inputFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(inputFile, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > static_cast<LONGLONG>(MAXDWORD)) {
            CloseHandle(inputFile);
            return false;
        }
        std::vector<BYTE> buffer(static_cast<size_t>(fileSize.QuadPart));
        DWORD bytesRead = 0;
        if (!ReadFile(inputFile, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) || bytesRead != buffer.size()) {
            CloseHandle(inputFile);
            return false;
        }
        CloseHandle(inputFile);

        DATA_BLOB inputBlob{};
        inputBlob.cbData = bytesRead;
        inputBlob.pbData = buffer.data();
        DATA_BLOB outputBlob{};
        if (!CryptProtectData(&inputBlob, L"DLSS4 Crash Dump", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outputBlob)) {
            return false;
        }

        HANDLE outputFile = CreateFileA(outputPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (outputFile == INVALID_HANDLE_VALUE) {
            LocalFree(outputBlob.pbData);
            return false;
        }
        DWORD bytesWritten = 0;
        bool ok = WriteFile(outputFile, outputBlob.pbData, outputBlob.cbData, &bytesWritten, nullptr) && bytesWritten == outputBlob.cbData;
        CloseHandle(outputFile);
        LocalFree(outputBlob.pbData);
        return ok;
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

// Pre-allocated buffer for crash log — avoids CRT allocations inside VEH handler
static char g_crashBuf[4096];

// Async-signal-safe int-to-hex helper — no CRT dependency
static int UnsafeHex(char* buf, int maxLen, DWORD64 val) {
    static const char hexChars[] = "0123456789ABCDEF";
    char tmp[17];
    int len = 0;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val > 0 && len < 16) { tmp[len++] = hexChars[val & 0xF]; val >>= 4; } }
    if (len >= maxLen) len = maxLen - 1;
    for (int i = 0; i < len && i < maxLen; i++) buf[i] = tmp[len - 1 - i];
    return len;
}

// Async-signal-safe string append helper
static int UnsafeAppend(char* buf, int pos, int maxLen, const char* str) {
    while (*str && pos < maxLen - 1) buf[pos++] = *str++;
    return pos;
}

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
        constexpr int maxLen = sizeof(g_crashBuf);

        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "=== DLSS 4 PROXY CRASH REPORT ===\r\n");
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "Exception Code: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos, code);
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\nAddress: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos,
                         reinterpret_cast<DWORD64>(pExceptionInfo->ExceptionRecord->ExceptionAddress));
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\n");

        // Identify module (GetModuleHandleExA is safe in VEH)
        HMODULE hModule = nullptr;
        char moduleName[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(pExceptionInfo->ExceptionRecord->ExceptionAddress), &hModule)) {
            GetModuleFileNameA(hModule, moduleName, MAX_PATH);
        }
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "Module: ");
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, moduleName[0] ? moduleName : "Unknown");
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\n");

        if (code == EXCEPTION_ACCESS_VIOLATION) {
            pos = UnsafeAppend(g_crashBuf, pos, maxLen, "Access Violation: ");
            pos = UnsafeAppend(g_crashBuf, pos, maxLen,
                pExceptionInfo->ExceptionRecord->ExceptionInformation[0] ? "Write" : "Read");
            pos = UnsafeAppend(g_crashBuf, pos, maxLen, " at 0x");
            pos += UnsafeHex(g_crashBuf + pos, maxLen - pos,
                             static_cast<DWORD64>(pExceptionInfo->ExceptionRecord->ExceptionInformation[1]));
            pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\n");
        }

        #ifdef _WIN64
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\nRegisters:\r\nRIP: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos, pExceptionInfo->ContextRecord->Rip);
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\nRSP: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos, pExceptionInfo->ContextRecord->Rsp);
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\nRAX: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos, pExceptionInfo->ContextRecord->Rax);
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\nRBX: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos, pExceptionInfo->ContextRecord->Rbx);
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\nRCX: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos, pExceptionInfo->ContextRecord->Rcx);
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\nRDX: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos, pExceptionInfo->ContextRecord->Rdx);
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\n");
        #else
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\nRegisters:\r\nEIP: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos, pExceptionInfo->ContextRecord->Eip);
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\nESP: 0x");
        pos += UnsafeHex(g_crashBuf + pos, maxLen - pos, pExceptionInfo->ContextRecord->Esp);
        pos = UnsafeAppend(g_crashBuf, pos, maxLen, "\r\n");
        #endif

        // Write pre-formatted buffer with a single WriteFile call (async-signal-safe)
        DWORD bytesWritten = 0;
        WriteFile(hFile, g_crashBuf, static_cast<DWORD>(pos), &bytesWritten, nullptr);

        // Minidump — MiniDumpWriteDump is explicitly documented as safe in VEH
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
        dumpInfo.ThreadId = GetCurrentThreadId();
        dumpInfo.ExceptionPointers = pExceptionInfo;
        dumpInfo.ClientPointers = FALSE;

        DumpFilterContext filterCtx{};
        HMODULE mainModule = GetModuleHandleA(nullptr);
        GetModuleRange(mainModule, filterCtx.mainBase, filterCtx.mainSize);
        HMODULE selfModule = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&VectoredHandler), &selfModule)) {
            GetModuleRange(selfModule, filterCtx.selfBase, filterCtx.selfSize);
        }

        // Write filtered minidump directly (skip encryption in crash path — CryptProtectData is not async-safe)
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
