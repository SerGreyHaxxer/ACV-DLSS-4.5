/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once
#include <windows.h>

#include <string>
#include <vector>


// ============================================================================
// INTEGRITY CHECKER - Pre-flight validation for TensorBoot
// ============================================================================

namespace Integrity {

// Result of a single check
struct CheckResult {
  bool passed;
  std::wstring name;
  std::wstring message;
  int severity; // 0=info, 1=warning, 2=error
};

// Run all pre-flight checks
// Returns true if all critical checks pass
bool RunAllChecks(std::vector<CheckResult>& results);

// Individual check functions
bool CheckGameExecutable(CheckResult& result);
bool CheckProxyDll(CheckResult& result);
bool CheckStreamlineDlls(CheckResult& result);
bool CheckDiskSpace(CheckResult& result);
bool CheckPreviousCrash(CheckResult& result);
bool CheckAdminRights(CheckResult& result);
bool CheckAntiVirusExclusion(CheckResult& result);

// PE validation
bool ValidatePEFile(const wchar_t* path, std::wstring& error);
bool Is64BitPE(const wchar_t* path);

// Hash verification
bool ComputeFileSHA256(const wchar_t* path, std::wstring& hashOut);
bool VerifyFileHash(const wchar_t* path, const wchar_t* expectedHash);

// Startup loop detection
bool IsInStartupLoop();
void RecordStartup();
void ClearStartupHistory();

// Get game directory (searches common locations)
std::wstring FindGameDirectory();

// Get current directory (where TensorBoot.exe is located)
std::wstring GetBootDirectory();

// Safe Mode
bool EnterSafeMode();
bool IsSafeMode();

// Configuration backup/restore
bool BackupConfig(const std::wstring& gameDir);
bool RestoreConfig(const std::wstring& gameDir);
bool HasConfigBackup(const std::wstring& gameDir);

// Auto-repair
struct RepairResult {
    bool success;
    std::wstring message;
    int filesRepaired;
};
RepairResult AutoRepairMissingDlls(const std::wstring& gameDir);

} // namespace Integrity
