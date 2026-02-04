#include "iat_utils.h"
#include "logger.h"
#include <dbghelp.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <set>

#pragma comment(lib, "dbghelp.lib")

// FIXED VERSION: Added comprehensive error checking and logging

bool HookIAT(HMODULE hModule, const char* targetModule, const char* targetFunction, void* newFunction, void** originalFunction) {
    if (!hModule) hModule = GetModuleHandle(NULL);
    if (!hModule || !targetModule || !targetFunction || !newFunction) {
        LOG_ERROR("[IAT] Invalid parameters (module=0x%p, targetMod=%s, func=%s, new=0x%p)", 
                  hModule, targetModule ? targetModule : "NULL", 
                  targetFunction ? targetFunction : "NULL", newFunction);
        return false;
    }

    // CRITICAL FIX 1: Verify module information is accessible
    MODULEINFO mi;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &mi, sizeof(mi))) {
        LOG_ERROR("[IAT] GetModuleInformation failed for module 0x%p (error: %d)", hModule, GetLastError());
        return false;
    }

    // CRITICAL FIX 2: Verify module has valid PE headers
    __try {
        IMAGE_DOS_HEADER* pDosHeader = (IMAGE_DOS_HEADER*)hModule;
        if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            LOG_ERROR("[IAT] Invalid DOS signature for module 0x%p", hModule);
            return false;
        }
        
        IMAGE_NT_HEADERS* pNtHeaders = (IMAGE_NT_HEADERS*)((BYTE*)hModule + pDosHeader->e_lfanew);
        if (pNtHeaders->Signature != IMAGE_NT_SIGNATURE) {
            LOG_ERROR("[IAT] Invalid PE signature for module 0x%p", hModule);
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[IAT] Exception reading PE headers for module 0x%p", hModule);
        return false;
    }

    ULONG size = 0;
    PIMAGE_IMPORT_DESCRIPTOR pImportDesc = (PIMAGE_IMPORT_DESCRIPTOR)ImageDirectoryEntryToData(
        hModule, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &size);

    if (!pImportDesc) {
        // Not an error - module just doesn't import anything (or already resolved)
        return false;
    }

    bool found = false;
    int entriesScanned = 0;
    const int MAX_ENTRIES = 1000; // Sanity limit

    __try {
        while (pImportDesc->Name && entriesScanned++ < MAX_ENTRIES) {
            char* moduleName = (char*)((BYTE*)hModule + pImportDesc->Name);
            
            // CRITICAL FIX 3: Validate string pointer before comparing
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(moduleName, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
                LOG_WARN("[IAT] Skipping invalid module name pointer 0x%p", moduleName);
                pImportDesc++;
                continue;
            }
            
            if (_stricmp(moduleName, targetModule) == 0) {
                PIMAGE_THUNK_DATA pThunk = (PIMAGE_THUNK_DATA)((BYTE*)hModule + pImportDesc->FirstThunk);
                PIMAGE_THUNK_DATA pOrigThunk = (PIMAGE_THUNK_DATA)((BYTE*)hModule + pImportDesc->OriginalFirstThunk);

                if (!pOrigThunk) pOrigThunk = pThunk;

                // CRITICAL FIX 4: Validate thunk pointers
                if (VirtualQuery(pThunk, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
                    LOG_WARN("[IAT] Invalid thunk pointer 0x%p in %s", pThunk, moduleName);
                    pImportDesc++;
                    continue;
                }

                int thunkCount = 0;
                const int MAX_THUNKS = 500; // Sanity limit

                while (pThunk->u1.Function && thunkCount++ < MAX_THUNKS) {
                    if (!(pOrigThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                        PIMAGE_IMPORT_BY_NAME pImport = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hModule + pOrigThunk->u1.AddressOfData);
                        
                        // CRITICAL FIX 5: Validate import name pointer
                        if (VirtualQuery(pImport, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
                            pThunk++;
                            pOrigThunk++;
                            continue;
                        }
                        
                        if (strcmp(pImport->Name, targetFunction) == 0) {
                            if (originalFunction && *originalFunction == nullptr) {
                                *originalFunction = (void*)pThunk->u1.Function;
                            }

                            if (pThunk->u1.Function != (ULONG_PTR)newFunction) {
                                // CRITICAL FIX 6: Comprehensive memory protection handling
                                MEMORY_BASIC_INFORMATION thunkMbi;
                                if (VirtualQuery(&pThunk->u1.Function, &thunkMbi, sizeof(thunkMbi)) == 0) {
                                    LOG_ERROR("[IAT] VirtualQuery failed for thunk at 0x%p", &pThunk->u1.Function);
                                    pThunk++;
                                    pOrigThunk++;
                                    continue;
                                }
                                
                                if (thunkMbi.State != MEM_COMMIT) {
                                    LOG_ERROR("[IAT] Thunk memory not committed (state: 0x%X) at 0x%p", 
                                             thunkMbi.State, &pThunk->u1.Function);
                                    pThunk++;
                                    pOrigThunk++;
                                    continue;
                                }
                                
                                if (thunkMbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
                                    LOG_ERROR("[IAT] Thunk memory not accessible (protect: 0x%X) at 0x%p", 
                                             thunkMbi.Protect, &pThunk->u1.Function);
                                    pThunk++;
                                    pOrigThunk++;
                                    continue;
                                }
                                
                                DWORD oldProtect;
                                if (!VirtualProtect(&pThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                                    DWORD err = GetLastError();
                                    LOG_ERROR("[IAT] VirtualProtect(RW) failed at 0x%p (error: %d, currentProtect: 0x%X)", 
                                             &pThunk->u1.Function, err, thunkMbi.Protect);
                                    pThunk++;
                                    pOrigThunk++;
                                    continue;
                                }
                                
                                // CRITICAL FIX 7: Protected write
                                __try {
                                    pThunk->u1.Function = (ULONG_PTR)newFunction;
                                } __except (EXCEPTION_EXECUTE_HANDLER) {
                                    LOG_ERROR("[IAT] Exception writing thunk at 0x%p (code: 0x%X)", 
                                             &pThunk->u1.Function, GetExceptionCode());
                                    DWORD dummy;
                                    VirtualProtect(&pThunk->u1.Function, sizeof(void*), oldProtect, &dummy);
                                    pThunk++;
                                    pOrigThunk++;
                                    continue;
                                }
                                
                                DWORD restoreProtect = 0;
                                if (!VirtualProtect(&pThunk->u1.Function, sizeof(void*), oldProtect, &restoreProtect)) {
                                    LOG_WARN("[IAT] Failed to restore protection at 0x%p (not critical)", &pThunk->u1.Function);
                                }
                                
                                found = true;
                                LOG_INFO("[IAT] Successfully hooked %s!%s at 0x%p -> 0x%p", 
                                        targetModule, targetFunction, *originalFunction, newFunction);
                            }
                        }
                    }
                    pThunk++;
                    pOrigThunk++;
                }
            }
            pImportDesc++;
        }
        
        if (entriesScanned >= MAX_ENTRIES) {
            LOG_WARN("[IAT] Hit entry scan limit (%d) - possible infinite loop avoided", MAX_ENTRIES);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[IAT] Exception during IAT walk (code: 0x%X)", GetExceptionCode());
        return false;
    }
    
    return found;
}

void HookAllModulesIAT(const char* targetModule, const char* targetFunction, void* newFunction, void** originalFunction) {
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded = 0;
    
    // CRITICAL FIX 8: Better error handling for module enumeration
    if (!EnumProcessModules(hProcess, nullptr, 0, &cbNeeded)) {
        LOG_ERROR("[IAT] EnumProcessModules(query) failed (error: %d)", GetLastError());
        return;
    }
    
    if (cbNeeded == 0) {
        LOG_WARN("[IAT] No modules to enumerate");
        return;
    }
    
    std::vector<HMODULE> hMods(cbNeeded / sizeof(HMODULE));
    if (!EnumProcessModules(hProcess, hMods.data(), cbNeeded, &cbNeeded)) {
        LOG_ERROR("[IAT] EnumProcessModules(enumerate) failed (error: %d)", GetLastError());
        return;
    }
    
    int moduleCount = cbNeeded / sizeof(HMODULE);
    int hooked = 0;
    int failed = 0;
    
    LOG_INFO("[IAT] Scanning %d modules for %s!%s", moduleCount, targetModule, targetFunction);
    
    for (int i = 0; i < moduleCount; i++) {
        char modName[MAX_PATH];
        if (GetModuleBaseNameA(hProcess, hMods[i], modName, sizeof(modName))) {
            // Skip dangerous system modules
            if (_stricmp(modName, "ntdll.dll") == 0 ||
                _stricmp(modName, "kernelbase.dll") == 0) {
                LOG_DEBUG("[IAT] Skipping protected system module: %s", modName);
                continue;
            }
            
            if (HookIAT(hMods[i], targetModule, targetFunction, newFunction, originalFunction)) {
                hooked++;
            } else {
                failed++;
            }
        }
    }
    
    LOG_INFO("[IAT] HookAllModulesIAT complete: %d hooked, %d failed/skipped", hooked, failed);
}
