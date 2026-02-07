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
#include "pattern_scanner.h"
#include "logger.h"
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <psapi.h>
#include <sstream>


extern "C" void LogStartup(const char *msg);

static std::filesystem::path GetCacheDir() {
  wchar_t buf[MAX_PATH] = {};
  DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (len > 0 && len < MAX_PATH) {
    auto dir = std::filesystem::path(buf) / L"acv-dlss";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!ec) return dir;
  }
  return std::filesystem::current_path();
}

std::vector<int> PatternScanner::ParsePattern(const std::string &pattern) {
  std::vector<int> bytes;
  std::stringstream ss(pattern);
  std::string byteStr;

  while (ss >> byteStr) {
    if (byteStr == "?" || byteStr == "??") {
      bytes.push_back(-1); // Wildcard
    } else {
      bytes.push_back(std::stoi(byteStr, nullptr, 16));
    }
  }
  return bytes;
}

PatternScanResult<uintptr_t> PatternScanner::Scan(const std::string &moduleName,
                                           const std::string &pattern) {
  // 1. Check Cache
  size_t patternHash = std::hash<std::string>{}(pattern);
  std::string cacheFile =
      (GetCacheDir() / ("pattern_cache_" + std::to_string(patternHash) + ".bin")).string();

  HMODULE hModule = GetModuleHandleA(moduleName.c_str());
  if (!hModule) {
    LogStartup("[SCAN] ERROR: Module handle not found!");
    return std::unexpected(ScanError::ModuleNotFound);
  }

  MODULEINFO modInfo;
  if (!GetModuleInformation(GetCurrentProcess(), hModule, &modInfo,
                            sizeof(MODULEINFO))) {
    LogStartup("[SCAN] ERROR: Failed to get module info!");
    return std::unexpected(ScanError::ModuleInfoFailed);
  }

  auto base = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);

  if (std::filesystem::exists(cacheFile)) {
    std::ifstream f(cacheFile, std::ios::binary);
    uint32_t offset = 0;
    if (f.read(reinterpret_cast<char*>(&offset), sizeof(offset))) {
      uintptr_t cachedAddr = base + offset;
      LogStartup(
          std::format("[SCAN] Found cached pattern at relative offset: +0x{:X}",
                      offset)
              .c_str());

      // Validate cached address is readable before using
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery(reinterpret_cast<LPCVOID>(cachedAddr), &mbi, sizeof(mbi)) != 0 &&
          mbi.State == MEM_COMMIT &&
          !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {

        // Verify cache validity (simple byte check of first non-wildcard)
        std::vector<int> patternBytes = ParsePattern(pattern);
        bool valid = true;
        for (size_t i = 0; i < patternBytes.size(); i++) {
          if (patternBytes[i] != -1 &&
              *reinterpret_cast<uint8_t*>(cachedAddr + i) != static_cast<uint8_t>(patternBytes[i])) {
            valid = false;
            break;
          }
        }
        if (valid)
          return cachedAddr;
        LogStartup("[SCAN] Cache invalid (game updated?), rescanning...");
      } else {
        LogStartup("[SCAN] Cache address not readable, rescanning...");
      }
    }
  }

  LogStartup(
      std::format("[SCAN] Scanning module: {} (Base: 0x{:p} Size: 0x{:X})",
                  moduleName, reinterpret_cast<void*>(base), static_cast<size_t>(modInfo.SizeOfImage))
          .c_str());

  auto result = Scan(base, modInfo.SizeOfImage, pattern);

  if (result) {
    // Save to cache
    uint32_t offset = static_cast<uint32_t>(*result - base);
    std::ofstream f(cacheFile, std::ios::binary);
    f.write(reinterpret_cast<char*>(&offset), sizeof(offset));
  }

  return result;
}

PatternScanResult<uintptr_t> PatternScanner::Scan(uintptr_t start, size_t length,
                                           const std::string &pattern) {
  std::vector<int> patternBytes = ParsePattern(pattern);
  size_t patternLength = patternBytes.size();

  if (length < patternLength)
    return std::unexpected(ScanError::PatternNotFound);

  LogStartup("[SCAN] Starting safe memory scan...");

  uintptr_t current = start;
  uintptr_t end = start + length;

  while (current < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi)) == 0) {
      current += 4096;
      continue;
    }

    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) ||
        (mbi.Protect & PAGE_NOACCESS)) {
      current = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      continue;
    }

    uintptr_t regionStart = (std::max)(current, reinterpret_cast<uintptr_t>(mbi.BaseAddress));
    uintptr_t regionEnd =
        (std::min)(end, reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);

    if (regionEnd < regionStart + patternLength) {
      current = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      continue;
    }

    auto* scanStart = reinterpret_cast<uint8_t*>(regionStart);
    size_t scanLength = regionEnd - regionStart;
    if (scanLength < patternLength) {
      current = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
      continue;
    }

    for (size_t i = 0; i <= scanLength - patternLength; ++i) {
      bool found = true;
      for (size_t j = 0; j < patternLength; ++j) {
        if (patternBytes[j] != -1 && patternBytes[j] != scanStart[i + j]) {
          found = false;
          break;
        }
      }
      if (found) {
        uintptr_t foundAddr = regionStart + i;
        LogStartup(
            std::format("[SCAN] SUCCESS! Found at 0x{:p}", reinterpret_cast<void*>(foundAddr))
                .c_str());
        return foundAddr;
      }
    }

    current = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  }

  LogStartup("[SCAN] FAILED: Pattern not found.");
  return std::unexpected(ScanError::PatternNotFound);
}

