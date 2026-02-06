// ============================================================================
// DLSS 4 PROXY DLL - MAIN ENTRY POINT
// ============================================================================
// This DLL proxies dxgi.dll to inject NVIDIA DLSS 4 functionality into
// Assassin's Creed Valhalla (or any DirectX 12 game).
//
// Features:
//   - DLSS 4 Super Resolution (transformer-based upscaling)
//   - DLSS 4 Ray Reconstruction (RT denoising)
//   - DLSS 4 Frame Generation (DLSS-G, up to 4x where supported)
//
// Usage:
//   1. Compile this project to dxgi.dll
//   2. Place in AC Valhalla game folder (next to ACValhalla.exe)
//   3. Place nvngx_dlss.dll and nvngx_dlssg.dll in the same folder
//   4. Run the game - look for dlss4_proxy.log for debug output
//
// Author: acerthyracer
// Version: 1.0.0
// ============================================================================

#include "src/crash_handler.h"
#include "src/dlss4_config.h"
#include "src/hooks.h"
#include "src/logger.h"
#include "src/proxy.h"
#include <stdio.h> // Added for startup logging
#include <windows.h>


// Simple startup logger to debug early crashes
extern "C" void LogStartup(const char *msg);

// ============================================================================
// DLL ENTRY POINT
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  switch (ul_reason_for_call) {
  case DLL_PROCESS_ATTACH:
    // DEBUG: Show message box to confirm DLL is loading
    // MessageBoxA(NULL, "DLSS Proxy DLL Loading", "Debug", MB_OK);
    LogStartup("DLL_PROCESS_ATTACH Entry");

    // Initialize Proxy Global State (CS)
    try {
      InitProxyGlobal();
    } catch (...) {
      LogStartup("EXCEPTION during InitProxyGlobal");
      return FALSE;
    }

    // Disable thread notifications for performance
    DisableThreadLibraryCalls(hModule);
    LogStartup("Thread Library Calls Disabled");

    LogStartup("Logger deferred until first DXGI call");

    LogStartup("DLL_PROCESS_ATTACH Exit");
    break;

  case DLL_THREAD_ATTACH:
  case DLL_THREAD_DETACH:
    break;

  case DLL_PROCESS_DETACH:
    LogStartup("DLL_PROCESS_DETACH Entry");
    LogStartup("DLSS 4 Proxy DLL Unloading...");

    if (lpReserved != nullptr) {
      LogStartup("Process termination detected; skipping cleanup to maintain "
                 "stability");
      break;
    }

    // Only cleanup if we are actually being unloaded, not just terminating
    // However, many games re-load DLLs or use multiple instances.
    // For stealth and persistence, we should be careful here.

    // Cleanup in reverse order
    CleanupHooks();
    LogStartup("Hooks Cleanup");

    ShutdownProxy();
    LogStartup("Proxy Shutdown");

    CleanupProxyGlobal();

    UninstallCrashHandler();
    LogStartup("Crash Handler Uninstalled");

    Logger::Instance().Close(false);
    LogStartup("Logger Closed");
    break;
  }
  return TRUE;
}
