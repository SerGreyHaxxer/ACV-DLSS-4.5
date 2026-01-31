#include "iat_utils.h"
#include <dbghelp.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <set>

#pragma comment(lib, "dbghelp.lib")

bool HookIAT(HMODULE hModule, const char* targetModule, const char* targetFunction, void* newFunction, void** originalFunction) {
    if (!hModule) hModule = GetModuleHandle(NULL);
    if (!hModule || !targetModule || !targetFunction || !newFunction) return false;

    ULONG size = 0;
    PIMAGE_IMPORT_DESCRIPTOR pImportDesc = (PIMAGE_IMPORT_DESCRIPTOR)ImageDirectoryEntryToData(
        hModule, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &size);

    if (!pImportDesc) return false;

    bool found = false;
    while (pImportDesc->Name) {
        char* moduleName = (char*)((BYTE*)hModule + pImportDesc->Name);
        if (_stricmp(moduleName, targetModule) == 0) {
            PIMAGE_THUNK_DATA pThunk = (PIMAGE_THUNK_DATA)((BYTE*)hModule + pImportDesc->FirstThunk);
            PIMAGE_THUNK_DATA pOrigThunk = (PIMAGE_THUNK_DATA)((BYTE*)hModule + pImportDesc->OriginalFirstThunk);

            if (!pOrigThunk) pOrigThunk = pThunk;

            while (pThunk->u1.Function) {
                if (!(pOrigThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    PIMAGE_IMPORT_BY_NAME pImport = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hModule + pOrigThunk->u1.AddressOfData);
                    if (strcmp(pImport->Name, targetFunction) == 0) {
                        if (originalFunction && *originalFunction == nullptr) {
                            *originalFunction = (void*)pThunk->u1.Function;
                        }

                        if (pThunk->u1.Function != (ULONG_PTR)newFunction) {
                            DWORD oldProtect;
                            if (VirtualProtect(&pThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                                pThunk->u1.Function = (ULONG_PTR)newFunction;
                                VirtualProtect(&pThunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                                found = true;
                            }
                        }
                    }
                }
                pThunk++;
                pOrigThunk++;
            }
        }
        pImportDesc++;
    }
    return found;
}

void HookAllModulesIAT(const char* targetModule, const char* targetFunction, void* newFunction, void** originalFunction) {
    HMODULE hMods[1024];
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded;

    if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        for (unsigned int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            HookIAT(hMods[i], targetModule, targetFunction, newFunction, originalFunction);
        }
    }
}
