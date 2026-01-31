#pragma once
#include <windows.h>

// Hook an import in the Import Address Table (IAT) of a specific module
// hModule: The module to hook (NULL for current executable)
// targetModule: The name of the DLL containing the function (e.g., "d3d12.dll")
// targetFunction: The name of the function to hook (e.g., "D3D12CreateDevice")
// newFunction: Pointer to your hook function
// originalFunction: [Optional] Output pointer to store the original function address
bool HookIAT(HMODULE hModule, const char* targetModule, const char* targetFunction, void* newFunction, void** originalFunction);
void HookAllModulesIAT(const char* targetModule, const char* targetFunction, void* newFunction, void** originalFunction);
