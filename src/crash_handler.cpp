#include "crash_handler.h"
#include <stdio.h>
#include <dbghelp.h>
#include <time.h>
#include <vector>
#include <string>
#include <sstream>

#pragma comment(lib, "dbghelp.lib")

static PVOID g_Handler = nullptr;

LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    // Only catch serious errors
    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && 
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Open log file directly (no mutex dependencies to avoid deadlock during crash)
    FILE* fp;
    if (fopen_s(&fp, "dlss4_crash.log", "w") == 0) {
        fprintf(fp, "=== DLSS 4 PROXY CRASH REPORT ===\n");
        
        time_t t = time(NULL);
        fprintf(fp, "Time: %lld\n", t);
        fprintf(fp, "Exception Code: 0x%08X\n", code);
        fprintf(fp, "Address: 0x%p\n", pExceptionInfo->ExceptionRecord->ExceptionAddress);
        
        // Identify module
        HMODULE hModule = NULL;
        char moduleName[MAX_PATH] = "Unknown";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, 
            (LPCSTR)pExceptionInfo->ExceptionRecord->ExceptionAddress, &hModule)) {
            GetModuleFileNameA(hModule, moduleName, MAX_PATH);
        }
        fprintf(fp, "Module: %s\n", moduleName);
        
        if (code == EXCEPTION_ACCESS_VIOLATION) {
            fprintf(fp, "Access Violation: %s at 0x%p\n", 
                pExceptionInfo->ExceptionRecord->ExceptionInformation[0] ? "Write" : "Read",
                (void*)pExceptionInfo->ExceptionRecord->ExceptionInformation[1]);
        }

        fprintf(fp, "\nRegisters:\n");
        #ifdef _WIN64
        fprintf(fp, "RAX: %p  RBX: %p\n", (void*)pExceptionInfo->ContextRecord->Rax, (void*)pExceptionInfo->ContextRecord->Rbx);
        fprintf(fp, "RCX: %p  RDX: %p\n", (void*)pExceptionInfo->ContextRecord->Rcx, (void*)pExceptionInfo->ContextRecord->Rdx);
        fprintf(fp, "RSI: %p  RDI: %p\n", (void*)pExceptionInfo->ContextRecord->Rsi, (void*)pExceptionInfo->ContextRecord->Rdi);
        fprintf(fp, "RBP: %p  RSP: %p\n", (void*)pExceptionInfo->ContextRecord->Rbp, (void*)pExceptionInfo->ContextRecord->Rsp);
        fprintf(fp, "RIP: %p\n", (void*)pExceptionInfo->ContextRecord->Rip);
        #endif

        // Write Minidump
        HANDLE hDumpFile = CreateFileA("dlss4_crash.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hDumpFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
            dumpInfo.ThreadId = GetCurrentThreadId();
            dumpInfo.ExceptionPointers = pExceptionInfo;
            dumpInfo.ClientPointers = FALSE;

            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, MiniDumpNormal, &dumpInfo, NULL, NULL);
            CloseHandle(hDumpFile);
            fprintf(fp, "\nMinidump written to dlss4_crash.dmp\n");
        } else {
            fprintf(fp, "\nFailed to create minidump file.\n");
        }

        fclose(fp);
    }

    // Don't swallow the exception, let the game/OS handle it (or show the usual error dialog)
    // But since we are debugging, maybe we terminate?
    // Let's continue search so the game's crash reporter (if any) can also run.
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
