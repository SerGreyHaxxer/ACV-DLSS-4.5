/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * TensorBoot - Safe Mode Bootstrapper for AC Valhalla DLSS Proxy
 * ================================================================
 * This launcher performs pre-flight checks before starting the game:
 *   - Validates game executable and proxy DLL integrity
 *   - Checks for required Streamline DLLs
 *   - Detects startup loops to prevent infinite crash cycles
 *   - Provides clear error messages for common issues
 *
 * Usage:
 *   TensorBoot.exe              Launch with pre-flight checks
 *   TensorBoot.exe --silent     Skip checks and launch immediately
 *   TensorBoot.exe --check      Run checks only, don't launch
 *   TensorBoot.exe --clear      Clear startup loop history
 */

#include "integrity_checker.h"

#include <iostream>
#include <string>
#include <vector>


// ANSI color codes for console output
namespace Color {
const char* Reset = "\033[0m";
const char* Red = "\033[91m";
const char* Green = "\033[92m";
const char* Yellow = "\033[93m";
const char* Cyan = "\033[96m";
const char* Bold = "\033[1m";
} // namespace Color

void EnableVirtualTerminal() {
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD dwMode = 0;
  GetConsoleMode(hOut, &dwMode);
  SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void PrintBanner() {
  std::cout << Color::Cyan << Color::Bold;
  std::cout << R"(
  _____                           ____              _   
 |_   _|__ _ __  ___  ___  _ __  | __ )  ___   ___ | |_ 
   | |/ _ \ '_ \/ __|/ _ \| '__| |  _ \ / _ \ / _ \| __|
   | |  __/ | | \__ \ (_) | |    | |_) | (_) | (_) | |_ 
   |_|\___|_| |_|___/\___/|_|    |____/ \___/ \___/ \__|
                                                        
)" << Color::Reset;
  std::cout << "  AC Valhalla Safe Mode Bootstrapper v1.0.0\n";
  std::cout << "  DLSS 4.5 Mod for Assassin's Creed Valhalla\n";
  std::cout << "  ============================================\n\n";
}

void PrintResult(const Integrity::CheckResult& result) {
  const char* icon;
  const char* color;

  if (result.passed) {
    if (result.severity == 0) {
      icon = "[OK]";
      color = Color::Green;
    } else {
      icon = "[!!]";
      color = Color::Yellow;
    }
  } else {
    icon = "[XX]";
    color = Color::Red;
  }

  // Convert wide string to narrow for output
  char nameBuf[256] = {0};
  char msgBuf[1024] = {0};
  WideCharToMultiByte(CP_UTF8, 0, result.name.c_str(), -1, nameBuf, sizeof(nameBuf), NULL, NULL);
  WideCharToMultiByte(CP_UTF8, 0, result.message.c_str(), -1, msgBuf, sizeof(msgBuf), NULL, NULL);

  std::cout << color << icon << " " << nameBuf << ": " << Color::Reset << msgBuf << "\n";
}

bool LaunchGame(const std::wstring& gameDir) {
  std::wstring exePath = gameDir + L"\\ACValhalla.exe";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  // Record startup before launching
  Integrity::RecordStartup();

  if (!CreateProcessW(exePath.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, gameDir.c_str(), &si, &pi)) {
    std::cout << Color::Red << "[ERROR] Failed to launch game. Error code: " << GetLastError() << Color::Reset << "\n";
    return false;
  }

  std::cout << Color::Green << "\n[OK] Game launched successfully!\n" << Color::Reset;
  std::cout << "     Press F5 in-game to open the DLSS Control Panel.\n";

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return true;
}

void PrintUsage() {
  std::cout << "Usage: TensorBoot.exe [options]\n\n";
  std::cout << "Options:\n";
  std::cout << "  --silent    Skip pre-flight checks and launch immediately\n";
  std::cout << "  --check     Run checks only, don't launch the game\n";
  std::cout << "  --clear     Clear startup loop history\n";
  std::cout << "  --repair    Auto-repair missing DLLs\n";
  std::cout << "  --backup    Backup current configuration\n";
  std::cout << "  --restore   Restore configuration from backup\n";
  std::cout << "  --help      Show this help message\n";
}

int wmain(int argc, wchar_t* argv[]) {
  EnableVirtualTerminal();

  bool silentMode = false;
  bool checkOnly = false;
  bool clearHistory = false;
  bool repairMode = false;
  bool backupMode = false;
  bool restoreMode = false;

  // Parse arguments
  for (int i = 1; i < argc; i++) {
    std::wstring arg = argv[i];
    if (arg == L"--silent" || arg == L"-s") {
      silentMode = true;
    } else if (arg == L"--check" || arg == L"-c") {
      checkOnly = true;
    } else if (arg == L"--clear") {
      clearHistory = true;
    } else if (arg == L"--repair" || arg == L"-r") {
      repairMode = true;
    } else if (arg == L"--backup") {
      backupMode = true;
    } else if (arg == L"--restore") {
      restoreMode = true;
    } else if (arg == L"--help" || arg == L"-h") {
      PrintBanner();
      PrintUsage();
      return 0;
    }
  }

  // Clear history if requested
  if (clearHistory) {
    Integrity::ClearStartupHistory();
    std::cout << Color::Green << "[OK] Startup history cleared.\n" << Color::Reset;
    if (argc == 2) return 0; // Only --clear was passed
  }

  // Repair mode
  if (repairMode) {
    std::wstring gameDir = Integrity::FindGameDirectory();
    if (gameDir.empty()) {
      std::cout << Color::Red << "[ERROR] Cannot find game directory for repair.\n" << Color::Reset;
      return 1;
    }
    auto result = Integrity::AutoRepairMissingDlls(gameDir);
    char msgBuf[1024] = {0};
    WideCharToMultiByte(CP_UTF8, 0, result.message.c_str(), -1, msgBuf, sizeof(msgBuf), NULL, NULL);
    if (result.success) {
      std::cout << Color::Green << "[OK] Repair: " << msgBuf << " (" << result.filesRepaired << " files)\n" << Color::Reset;
    } else {
      std::cout << Color::Red << "[ERROR] Repair: " << msgBuf << "\n" << Color::Reset;
    }
    if (argc == 2) return result.success ? 0 : 1;
  }

  // Backup mode
  if (backupMode) {
    std::wstring gameDir = Integrity::FindGameDirectory();
    if (!gameDir.empty() && Integrity::BackupConfig(gameDir)) {
      std::cout << Color::Green << "[OK] Configuration backed up.\n" << Color::Reset;
    } else {
      std::cout << Color::Yellow << "[!!] No configuration found to backup.\n" << Color::Reset;
    }
    if (argc == 2) return 0;
  }

  // Restore mode
  if (restoreMode) {
    std::wstring gameDir = Integrity::FindGameDirectory();
    if (!gameDir.empty() && Integrity::RestoreConfig(gameDir)) {
      std::cout << Color::Green << "[OK] Configuration restored from backup.\n" << Color::Reset;
    } else {
      std::cout << Color::Yellow << "[!!] No backup found to restore.\n" << Color::Reset;
    }
    if (argc == 2) return 0;
  }

  PrintBanner();

  // Find game directory
  std::wstring gameDir = Integrity::FindGameDirectory();
  if (gameDir.empty()) {
    std::cout << Color::Red << "[ERROR] Cannot find game directory.\n" << Color::Reset;
    std::cout << "        Please run TensorBoot.exe from the AC Valhalla folder\n";
    std::cout << "        (next to ACValhalla.exe).\n";
    return 1;
  }

  // Display game directory
  char gameDirBuf[MAX_PATH] = {0};
  WideCharToMultiByte(CP_UTF8, 0, gameDir.c_str(), -1, gameDirBuf, MAX_PATH, NULL, NULL);
  std::cout << "Game Directory: " << gameDirBuf << "\n\n";

  // Silent mode - just launch
  if (silentMode) {
    std::cout << Color::Yellow << "[!!] Silent mode - skipping pre-flight checks\n" << Color::Reset;
    return LaunchGame(gameDir) ? 0 : 1;
  }

  // Run pre-flight checks
  std::cout << Color::Bold << "Running Pre-Flight Checks...\n" << Color::Reset;
  std::cout << "--------------------------------------------\n";

  std::vector<Integrity::CheckResult> results;
  bool allPassed = Integrity::RunAllChecks(results);

  // Print results
  for (const auto& result : results) {
    PrintResult(result);
  }

  std::cout << "--------------------------------------------\n";

  // Count issues
  int errors = 0, warnings = 0;
  for (const auto& result : results) {
    if (!result.passed)
      errors++;
    else if (result.severity > 0)
      warnings++;
  }

  if (errors > 0) {
    std::cout << Color::Red << "\n[XX] " << errors << " error(s) found. ";
    if (!checkOnly) {
      std::cout << "Cannot launch game.";
    }
    std::cout << Color::Reset << "\n";
    std::cout << "     Please fix the issues above and try again.\n";
    return 1;
  }

  if (warnings > 0) {
    std::cout << Color::Yellow << "\n[!!] " << warnings << " warning(s). " << Color::Reset;
    std::cout << "Proceeding anyway...\n";
  } else {
    std::cout << Color::Green << "\n[OK] All checks passed!\n" << Color::Reset;
  }

  // Check for safe mode
  if (Integrity::IsInStartupLoop()) {
    std::cout << Color::Yellow << "\n[!!] Startup loop detected! Entering safe mode...\n" << Color::Reset;
    Integrity::EnterSafeMode();
    // Backup config before potentially resetting
    Integrity::BackupConfig(gameDir);
    std::cout << "     Configuration backed up. You may need to reset settings.\n";
    std::cout << "     Use --restore to recover your configuration.\n";
  }

  // Check only mode - don't launch
  if (checkOnly) {
    std::cout << "\n     Check-only mode - not launching game.\n";
    return 0;
  }

  // Launch the game
  std::cout << "\nLaunching Assassin's Creed Valhalla...\n";
  return LaunchGame(gameDir) ? 0 : 1;
}
