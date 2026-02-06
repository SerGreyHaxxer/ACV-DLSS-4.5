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

// Do NOT include dxgi headers here to avoid redefinition conflicts
// We use GetProcAddress to dynamically load the original functions

// ============================================================================
// DXGI PROXY FUNCTION TYPES
// ============================================================================
// These are the original function signatures from the system DXGI.dll
// We forward calls to these after (optionally) intercepting them.

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_DXGIDeclareAdapterRemovalSupport)();
typedef HRESULT(WINAPI* PFN_DXGIGetDebugInterface1)(UINT Flags, REFIID riid, void** pDebug);

// ============================================================================
// PROXY STATE
// ============================================================================

struct DXGIProxyState {
    HMODULE hOriginalDXGI = nullptr;
    
    // Original function pointers
    PFN_CreateDXGIFactory       pfnCreateDXGIFactory = nullptr;
    PFN_CreateDXGIFactory1      pfnCreateDXGIFactory1 = nullptr;
    PFN_CreateDXGIFactory2      pfnCreateDXGIFactory2 = nullptr;
    PFN_DXGIDeclareAdapterRemovalSupport pfnDXGIDeclareAdapterRemovalSupport = nullptr;
    PFN_DXGIGetDebugInterface1  pfnDXGIGetDebugInterface1 = nullptr;

    // Additional Passthroughs
    void* pfnApplyCompatResolutionQuirking = nullptr;
    void* pfnCompatString = nullptr;
    void* pfnCompatValue = nullptr;
    void* pfnDXGIDumpJournal = nullptr;
    void* pfnDXGIReportAdapterConfiguration = nullptr;
    void* pfnDXGIDisableVBlankVirtualization = nullptr;
    void* pfnD3DKMTCloseAdapter = nullptr;
    void* pfnD3DKMTDestroyAllocation = nullptr;
    void* pfnD3DKMTDestroyContext = nullptr;
    void* pfnD3DKMTDestroyDevice = nullptr;
    void* pfnD3DKMTDestroySynchronizationObject = nullptr;
    void* pfnD3DKMTQueryAdapterInfo = nullptr;
    void* pfnD3DKMTSetDisplayPrivateDriverFormat = nullptr;
    void* pfnD3DKMTSignalSynchronizationObject = nullptr;
    void* pfnD3DKMTUnlock = nullptr;
    void* pfnD3DKMTWaitForSynchronizationObject = nullptr;
    void* pfnOpenAdapter10 = nullptr;
    void* pfnOpenAdapter10_2 = nullptr;
    void* pfnSetAppCompatStringPointer = nullptr;
    
    bool initialized = false;
};

// Global proxy state
extern DXGIProxyState g_ProxyState;

// ============================================================================
// PROXY FUNCTIONS
// ============================================================================

bool InitializeProxy();
void ShutdownProxy();

void InitProxyGlobal();
void CleanupProxyGlobal();

// Note: The actual exported functions are declared in proxy.cpp with 
// extern "C" __declspec(dllexport) to avoid conflicts with SDK headers

