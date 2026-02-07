/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "integrity_checker.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <psapi.h>
#include <sstream>
#include <wincrypt.h>


namespace fs = std::filesystem;

namespace Integrity {

// ============================================================================
// CONSTANTS
// ============================================================================

static constexpr wchar_t kGameExeName[] = L"ACValhalla.exe";
static constexpr wchar_t kProxyDllName[] = L"dxgi.dll";
static constexpr wchar_t kStartupLogName[] = L"tensorboot_startups.log";

static constexpr int kMaxStartupsInWindow = 5;    // Max startups allowed
static constexpr int kStartupWindowSeconds = 60;  // Time window in seconds
static constexpr ULONGLONG kMinDiskSpaceMB = 500; // Minimum disk space

// Required Streamline DLLs
static const wchar_t* kStreamlineDlls[] = {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", nullptr};

// Optional Streamline DLLs (frame gen)
static const wchar_t* kOptionalDlls[] = {L"sl.dlss_g.dll", L"nvngx_dlss.dll", L"nvngx_dlssg.dll", nullptr};

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

std::wstring GetBootDirectory() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  fs::path p(path);
  return p.parent_path().wstring();
}

std::wstring FindGameDirectory() {
  // First check if we're in the game directory
  std::wstring bootDir = GetBootDirectory();
  if (fs::exists(fs::path(bootDir) / kGameExeName)) {
    return bootDir;
  }

  // Check common Steam locations
  const wchar_t* steamPaths[] = {L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Assassin's Creed Valhalla",
                                 L"D:\\Steam\\steamapps\\common\\Assassin's Creed Valhalla",
                                 L"E:\\Steam\\steamapps\\common\\Assassin's Creed Valhalla",
                                 L"D:\\Games\\Assassin's Creed Valhalla",
                                 L"C:\\Games\\Assassin's Creed Valhalla",
                                 nullptr};

  for (int i = 0; steamPaths[i]; i++) {
    fs::path p = fs::path(steamPaths[i]) / kGameExeName;
    if (fs::exists(p)) {
      return steamPaths[i];
    }
  }

  return L"";
}

// ============================================================================
// PE VALIDATION
// ============================================================================

bool ValidatePEFile(const wchar_t* path, std::wstring& error) {
  HANDLE hFile =
      CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    error = L"Cannot open file";
    return false;
  }

  IMAGE_DOS_HEADER dosHeader;
  DWORD bytesRead;

  if (!ReadFile(hFile, &dosHeader, sizeof(dosHeader), &bytesRead, nullptr) || bytesRead != sizeof(dosHeader)) {
    CloseHandle(hFile);
    error = L"Cannot read DOS header";
    return false;
  }

  if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
    CloseHandle(hFile);
    error = L"Invalid DOS signature";
    return false;
  }

  SetFilePointer(hFile, dosHeader.e_lfanew, nullptr, FILE_BEGIN);

  DWORD ntSignature;
  if (!ReadFile(hFile, &ntSignature, sizeof(ntSignature), &bytesRead, nullptr) || bytesRead != sizeof(ntSignature)) {
    CloseHandle(hFile);
    error = L"Cannot read NT signature";
    return false;
  }

  if (ntSignature != IMAGE_NT_SIGNATURE) {
    CloseHandle(hFile);
    error = L"Invalid PE signature";
    return false;
  }

  IMAGE_FILE_HEADER fileHeader;
  if (!ReadFile(hFile, &fileHeader, sizeof(fileHeader), &bytesRead, nullptr) || bytesRead != sizeof(fileHeader)) {
    CloseHandle(hFile);
    error = L"Cannot read file header";
    return false;
  }

  CloseHandle(hFile);

  // Check for DLL vs EXE
  if (wcsstr(path, L".dll") || wcsstr(path, L".DLL")) {
    if (!(fileHeader.Characteristics & IMAGE_FILE_DLL)) {
      error = L"File is not a valid DLL";
      return false;
    }
  }

  return true;
}

bool Is64BitPE(const wchar_t* path) {
  HANDLE hFile =
      CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return false;

  IMAGE_DOS_HEADER dosHeader;
  DWORD bytesRead;
  ReadFile(hFile, &dosHeader, sizeof(dosHeader), &bytesRead, nullptr);

  SetFilePointer(hFile, dosHeader.e_lfanew + sizeof(DWORD), nullptr, FILE_BEGIN);

  IMAGE_FILE_HEADER fileHeader;
  ReadFile(hFile, &fileHeader, sizeof(fileHeader), &bytesRead, nullptr);

  CloseHandle(hFile);

  return fileHeader.Machine == IMAGE_FILE_MACHINE_AMD64;
}

// ============================================================================
// HASH VERIFICATION
// ============================================================================

bool ComputeFileSHA256(const wchar_t* path, std::wstring& hashOut) {
  HANDLE hFile =
      CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return false;

  HCRYPTPROV hProv = 0;
  HCRYPTHASH hHash = 0;
  bool success = false;

  if (CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
    if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
      BYTE buffer[65536];
      DWORD bytesRead;

      while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        if (!CryptHashData(hHash, buffer, bytesRead, 0)) {
          break;
        }
      }

      BYTE hash[32];
      DWORD hashLen = sizeof(hash);
      if (CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
        std::wostringstream oss;
        oss << std::hex << std::setfill(L'0');
        for (DWORD i = 0; i < hashLen; i++) {
          oss << std::setw(2) << static_cast<int>(hash[i]);
        }
        hashOut = oss.str();
        success = true;
      }

      CryptDestroyHash(hHash);
    }
    CryptReleaseContext(hProv, 0);
  }

  CloseHandle(hFile);
  return success;
}

bool VerifyFileHash(const wchar_t* path, const wchar_t* expectedHash) {
  std::wstring actualHash;
  if (!ComputeFileSHA256(path, actualHash)) return false;
  return _wcsicmp(actualHash.c_str(), expectedHash) == 0;
}

// ============================================================================
// STARTUP LOOP DETECTION
// ============================================================================

bool IsInStartupLoop() {
  fs::path logPath = fs::path(GetBootDirectory()) / kStartupLogName;

  if (!fs::exists(logPath)) return false;

  std::wifstream file(logPath);
  if (!file.is_open()) return false;

  auto now = std::chrono::system_clock::now();
  auto windowStart = now - std::chrono::seconds(kStartupWindowSeconds);
  auto windowStartTime = std::chrono::system_clock::to_time_t(windowStart);

  int recentStartups = 0;
  std::wstring line;

  while (std::getline(file, line)) {
    try {
      time_t timestamp = std::stoll(line);
      if (timestamp > windowStartTime) {
        recentStartups++;
      }
    } catch (...) {
      continue;
    }
  }

  return recentStartups >= kMaxStartupsInWindow;
}

void RecordStartup() {
  fs::path logPath = fs::path(GetBootDirectory()) / kStartupLogName;
  std::wofstream file(logPath, std::ios::app);
  if (file.is_open()) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::system_clock::to_time_t(now);
    file << timestamp << L"\n";
  }
}

void ClearStartupHistory() {
  fs::path logPath = fs::path(GetBootDirectory()) / kStartupLogName;
  if (fs::exists(logPath)) {
    fs::remove(logPath);
  }
}

// ============================================================================
// INDIVIDUAL CHECKS
// ============================================================================

bool CheckGameExecutable(CheckResult& result) {
  result.name = L"Game Executable";

  std::wstring gameDir = FindGameDirectory();
  if (gameDir.empty()) {
    result.passed = false;
    result.message = L"Cannot find ACValhalla.exe. Please run TensorBoot from the game folder.";
    result.severity = 2;
    return false;
  }

  fs::path exePath = fs::path(gameDir) / kGameExeName;

  std::wstring error;
  if (!ValidatePEFile(exePath.c_str(), error)) {
    result.passed = false;
    result.message = L"Game executable validation failed: " + error;
    result.severity = 2;
    return false;
  }

  if (!Is64BitPE(exePath.c_str())) {
    result.passed = false;
    result.message = L"Game executable is not 64-bit.";
    result.severity = 2;
    return false;
  }

  result.passed = true;
  result.message = L"Found valid ACValhalla.exe";
  result.severity = 0;
  return true;
}

bool CheckProxyDll(CheckResult& result) {
  result.name = L"Proxy DLL";

  std::wstring gameDir = FindGameDirectory();
  if (gameDir.empty()) {
    result.passed = false;
    result.message = L"Cannot find game directory.";
    result.severity = 2;
    return false;
  }

  fs::path dllPath = fs::path(gameDir) / kProxyDllName;

  if (!fs::exists(dllPath)) {
    result.passed = false;
    result.message = L"dxgi.dll not found. Please copy it to the game folder.";
    result.severity = 2;
    return false;
  }

  std::wstring error;
  if (!ValidatePEFile(dllPath.c_str(), error)) {
    result.passed = false;
    result.message = L"Proxy DLL validation failed: " + error;
    result.severity = 2;
    return false;
  }

  if (!Is64BitPE(dllPath.c_str())) {
    result.passed = false;
    result.message = L"Proxy DLL is not 64-bit.";
    result.severity = 2;
    return false;
  }

  result.passed = true;
  result.message = L"Proxy DLL validated";
  result.severity = 0;
  return true;
}

bool CheckStreamlineDlls(CheckResult& result) {
  result.name = L"Streamline DLLs";

  std::wstring gameDir = FindGameDirectory();
  if (gameDir.empty()) {
    result.passed = false;
    result.message = L"Cannot find game directory.";
    result.severity = 2;
    return false;
  }

  std::vector<std::wstring> missing;
  std::vector<std::wstring> invalid;

  // Check required DLLs
  for (int i = 0; kStreamlineDlls[i]; i++) {
    fs::path dllPath = fs::path(gameDir) / kStreamlineDlls[i];
    if (!fs::exists(dllPath)) {
      missing.push_back(kStreamlineDlls[i]);
    } else {
      std::wstring error;
      if (!ValidatePEFile(dllPath.c_str(), error)) {
        invalid.push_back(kStreamlineDlls[i]);
      }
    }
  }

  if (!missing.empty()) {
    result.passed = false;
    result.message = L"Missing required Streamline DLLs: ";
    for (const auto& dll : missing) {
      result.message += dll + L" ";
    }
    result.severity = 2;
    return false;
  }

  if (!invalid.empty()) {
    result.passed = false;
    result.message = L"Invalid Streamline DLLs: ";
    for (const auto& dll : invalid) {
      result.message += dll + L" ";
    }
    result.severity = 2;
    return false;
  }

  // Check optional DLLs
  std::vector<std::wstring> missingOptional;
  for (int i = 0; kOptionalDlls[i]; i++) {
    fs::path dllPath = fs::path(gameDir) / kOptionalDlls[i];
    if (!fs::exists(dllPath)) {
      missingOptional.push_back(kOptionalDlls[i]);
    }
  }

  if (!missingOptional.empty()) {
    result.passed = true;
    result.message = L"Required DLLs OK. Missing optional: ";
    for (const auto& dll : missingOptional) {
      result.message += dll + L" ";
    }
    result.severity = 1;
    return true;
  }

  result.passed = true;
  result.message = L"All Streamline DLLs present and valid";
  result.severity = 0;
  return true;
}

bool CheckDiskSpace(CheckResult& result) {
  result.name = L"Disk Space";

  std::wstring gameDir = FindGameDirectory();
  if (gameDir.empty()) {
    result.passed = true; // Can't check, assume OK
    result.message = L"Could not determine game directory.";
    result.severity = 1;
    return true;
  }

  ULARGE_INTEGER freeBytesAvailable;
  if (!GetDiskFreeSpaceExW(gameDir.c_str(), &freeBytesAvailable, nullptr, nullptr)) {
    result.passed = true;
    result.message = L"Could not check disk space.";
    result.severity = 1;
    return true;
  }

  ULONGLONG freeMB = freeBytesAvailable.QuadPart / (1024 * 1024);

  if (freeMB < kMinDiskSpaceMB) {
    result.passed = false;
    result.message = L"Low disk space: " + std::to_wstring(freeMB) + L" MB free.";
    result.severity = 2;
    return false;
  }

  result.passed = true;
  result.message = std::to_wstring(freeMB) + L" MB available";
  result.severity = 0;
  return true;
}

bool CheckPreviousCrash(CheckResult& result) {
  result.name = L"Previous Crash";

  std::wstring gameDir = FindGameDirectory();
  if (gameDir.empty()) {
    result.passed = true;
    result.message = L"No crash data found.";
    result.severity = 0;
    return true;
  }

  fs::path crashLog = fs::path(gameDir) / L"dlss4_sentinel.log";
  fs::path crashDump = fs::path(gameDir) / L"dlss4_sentinel.dmp";

  if (fs::exists(crashLog) || fs::exists(crashDump)) {
    result.passed = true; // Warning, not failure
    result.message = L"Crash files from previous session detected. Check dlss4_sentinel.log for details.";
    result.severity = 1;
    return true;
  }

  result.passed = true;
  result.message = L"No previous crash detected";
  result.severity = 0;
  return true;
}

bool CheckAdminRights(CheckResult& result) {
  result.name = L"Admin Rights";

  BOOL isAdmin = FALSE;
  PSID adminGroup = nullptr;
  SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;

  if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                               &adminGroup)) {
    CheckTokenMembership(nullptr, adminGroup, &isAdmin);
    FreeSid(adminGroup);
  }

  if (isAdmin) {
    result.passed = true;
    result.message = L"Warning: Running as Administrator is not recommended.";
    result.severity = 1;
    return true;
  }

  result.passed = true;
  result.message = L"Running as standard user (recommended)";
  result.severity = 0;
  return true;
}

bool CheckAntiVirusExclusion(CheckResult& result) {
  result.name = L"Antivirus";

  // We can't reliably check AV exclusions, just provide a warning
  result.passed = true;
  result.message = L"Consider adding game folder to antivirus exclusions for best performance.";
  result.severity = 0;
  return true;
}

// ============================================================================
// MAIN CHECK RUNNER
// ============================================================================

bool RunAllChecks(std::vector<CheckResult>& results) {
  results.clear();

  bool allPassed = true;

  // Run each check
  CheckResult result;

  if (!CheckGameExecutable(result)) allPassed = false;
  results.push_back(result);

  if (!CheckProxyDll(result)) allPassed = false;
  results.push_back(result);

  if (!CheckStreamlineDlls(result)) allPassed = false;
  results.push_back(result);

  if (!CheckDiskSpace(result)) allPassed = false;
  results.push_back(result);

  CheckPreviousCrash(result);
  results.push_back(result);

  CheckAdminRights(result);
  results.push_back(result);

  CheckAntiVirusExclusion(result);
  results.push_back(result);

  // Check for startup loop
  if (IsInStartupLoop()) {
    CheckResult loopResult;
    loopResult.name = L"Startup Loop";
    loopResult.passed = false;
    loopResult.message = L"Too many startup attempts detected. The mod may be causing crashes.";
    loopResult.severity = 2;
    results.push_back(loopResult);
    allPassed = false;
  }

  return allPassed;
}

// ============================================================================
// SAFE MODE
// ============================================================================

static constexpr wchar_t kSafeModeFlag[] = L"tensorboot_safemode.flag";

bool EnterSafeMode() {
    fs::path flagPath = fs::path(GetBootDirectory()) / kSafeModeFlag;
    std::wofstream file(flagPath);
    if (file.is_open()) {
        file << L"safe_mode_active\n";
        return true;
    }
    return false;
}

bool IsSafeMode() {
    fs::path flagPath = fs::path(GetBootDirectory()) / kSafeModeFlag;
    return fs::exists(flagPath);
}

// ============================================================================
// CONFIGURATION BACKUP/RESTORE
// ============================================================================

static constexpr wchar_t kConfigFileName[] = L"dlss_settings.toml";
static constexpr wchar_t kConfigBackupName[] = L"dlss_settings.toml.backup";
static constexpr wchar_t kLegacyConfigName[] = L"dlss_settings.ini";
static constexpr wchar_t kLegacyBackupName[] = L"dlss_settings.ini.backup";

bool BackupConfig(const std::wstring& gameDir) {
    bool anyBacked = false;
    
    // Backup TOML config
    fs::path configPath = fs::path(gameDir) / kConfigFileName;
    fs::path backupPath = fs::path(gameDir) / kConfigBackupName;
    if (fs::exists(configPath)) {
        std::error_code ec;
        fs::copy_file(configPath, backupPath, fs::copy_options::overwrite_existing, ec);
        if (!ec) anyBacked = true;
    }
    
    // Backup legacy INI config
    fs::path iniPath = fs::path(gameDir) / kLegacyConfigName;
    fs::path iniBackup = fs::path(gameDir) / kLegacyBackupName;
    if (fs::exists(iniPath)) {
        std::error_code ec;
        fs::copy_file(iniPath, iniBackup, fs::copy_options::overwrite_existing, ec);
        if (!ec) anyBacked = true;
    }
    
    return anyBacked;
}

bool RestoreConfig(const std::wstring& gameDir) {
    bool anyRestored = false;
    
    fs::path backupPath = fs::path(gameDir) / kConfigBackupName;
    fs::path configPath = fs::path(gameDir) / kConfigFileName;
    if (fs::exists(backupPath)) {
        std::error_code ec;
        fs::copy_file(backupPath, configPath, fs::copy_options::overwrite_existing, ec);
        if (!ec) anyRestored = true;
    }
    
    fs::path iniBackup = fs::path(gameDir) / kLegacyBackupName;
    fs::path iniPath = fs::path(gameDir) / kLegacyConfigName;
    if (fs::exists(iniBackup)) {
        std::error_code ec;
        fs::copy_file(iniBackup, iniPath, fs::copy_options::overwrite_existing, ec);
        if (!ec) anyRestored = true;
    }
    
    return anyRestored;
}

bool HasConfigBackup(const std::wstring& gameDir) {
    return fs::exists(fs::path(gameDir) / kConfigBackupName) ||
           fs::exists(fs::path(gameDir) / kLegacyBackupName);
}

// ============================================================================
// AUTO-REPAIR
// ============================================================================

RepairResult AutoRepairMissingDlls(const std::wstring& gameDir) {
    RepairResult result{true, L"", 0};
    
    // Check for proxy DLL
    fs::path proxyDll = fs::path(gameDir) / kProxyDllName;
    if (!fs::exists(proxyDll)) {
        // Check if it's in the TensorBoot directory
        fs::path bootDir = GetBootDirectory();
        fs::path sourceDll = fs::path(bootDir) / kProxyDllName;
        if (fs::exists(sourceDll)) {
            std::error_code ec;
            fs::copy_file(sourceDll, proxyDll, ec);
            if (!ec) {
                result.filesRepaired++;
                result.message += L"Copied dxgi.dll from TensorBoot directory. ";
            } else {
                result.success = false;
                result.message += L"Failed to copy dxgi.dll: " + 
                    std::wstring(ec.message().begin(), ec.message().end()) + L". ";
            }
        } else {
            result.success = false;
            result.message += L"dxgi.dll not found in TensorBoot directory. ";
        }
    }
    
    // Check for required Streamline DLLs - look in common locations
    fs::path bootDir = GetBootDirectory();
    for (int i = 0; kStreamlineDlls[i]; i++) {
        fs::path targetDll = fs::path(gameDir) / kStreamlineDlls[i];
        if (!fs::exists(targetDll)) {
            // Try to find it next to TensorBoot
            fs::path sourceDll = fs::path(bootDir) / kStreamlineDlls[i];
            if (fs::exists(sourceDll)) {
                std::error_code ec;
                fs::copy_file(sourceDll, targetDll, ec);
                if (!ec) {
                    result.filesRepaired++;
                    result.message += std::wstring(L"Copied ") + kStreamlineDlls[i] + L". ";
                }
            }
        }
    }
    
    if (result.filesRepaired == 0 && result.success) {
        result.message = L"All required files are present. No repairs needed.";
    }
    
    return result;
}

} // namespace Integrity
