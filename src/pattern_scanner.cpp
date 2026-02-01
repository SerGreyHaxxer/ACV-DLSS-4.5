#include "pattern_scanner.h"
#include "logger.h"
#include <psapi.h>
#include <sstream>
#include <iomanip>
#include <stdio.h>
#include <fstream>
#include <filesystem>

extern "C" void LogStartup(const char* msg);

std::vector<int> PatternScanner::ParsePattern(const std::string& pattern) {
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

std::optional<uintptr_t> PatternScanner::Scan(const std::string& moduleName, const std::string& pattern) {
    char msg[256];
    
    // 1. Check Cache
    size_t patternHash = std::hash<std::string>{}(pattern);
    std::string cacheFile = "pattern_cache_" + std::to_string(patternHash) + ".bin";
    
    HMODULE hModule = GetModuleHandleA(moduleName.c_str());
    if (!hModule) {
        LogStartup("[SCAN] ERROR: Module handle not found!");
        return std::nullopt;
    }

    MODULEINFO modInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(MODULEINFO))) {
        LogStartup("[SCAN] ERROR: Failed to get module info!");
        return std::nullopt;
    }
    
    uintptr_t base = (uintptr_t)modInfo.lpBaseOfDll;
    
    if (std::filesystem::exists(cacheFile)) {
        std::ifstream f(cacheFile, std::ios::binary);
        uint32_t offset = 0;
        if (f.read((char*)&offset, sizeof(offset))) {
            uintptr_t cachedAddr = base + offset;
            sprintf_s(msg, "[SCAN] Found cached pattern at relative offset: +0x%X", offset);
            LogStartup(msg);
            
            // Verify cache validity (simple byte check of first non-wildcard)
            std::vector<int> patternBytes = ParsePattern(pattern);
            bool valid = true;
            for(size_t i=0; i<patternBytes.size(); i++) {
                if (patternBytes[i] != -1 && *(uint8_t*)(cachedAddr + i) != (uint8_t)patternBytes[i]) {
                    valid = false; 
                    break; 
                }
            }
            if (valid) return cachedAddr;
            LogStartup("[SCAN] Cache invalid (game updated?), rescanning...");
        }
    }

    sprintf_s(msg, "[SCAN] Scanning module: %s (Base: 0x%p Size: 0x%zX)", moduleName.c_str(), (void*)base, (size_t)modInfo.SizeOfImage);
    LogStartup(msg);

    auto result = Scan(base, modInfo.SizeOfImage, pattern);
    
    if (result) {
        // Save to cache
        uint32_t offset = (uint32_t)(*result - base);
        std::ofstream f(cacheFile, std::ios::binary);
        f.write((char*)&offset, sizeof(offset));
    }
    
    return result;
}

std::optional<uintptr_t> PatternScanner::Scan(uintptr_t start, size_t length, const std::string& pattern) {
    std::vector<int> patternBytes = ParsePattern(pattern);
    size_t patternLength = patternBytes.size();
    
    if (length < patternLength) return std::nullopt;

    LogStartup("[SCAN] Starting safe memory scan...");

    uintptr_t current = start;
    uintptr_t end = start + length;

    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)current, &mbi, sizeof(mbi)) == 0) {
            current += 4096; // Skip if query fails (shouldn't happen)
            continue;
        }

        // Skip non-commited or guarded memory
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) {
            current = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            continue;
        }
        
        // Skip non-executable if looking for code (heuristic: patterns starting with typical opcodes)
        // But for generality, we scan any readable memory.
        
        uintptr_t regionStart = std::max(current, (uintptr_t)mbi.BaseAddress);
        uintptr_t regionEnd = std::min(end, (uintptr_t)mbi.BaseAddress + mbi.RegionSize);
        
        if (regionEnd < regionStart + patternLength) {
            current = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            continue;
        }

        uint8_t* scanStart = (uint8_t*)regionStart;
        size_t scanLength = regionEnd - regionStart;

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
                char msg[256];
                sprintf_s(msg, "[SCAN] SUCCESS! Found at 0x%p", (void*)foundAddr);
                LogStartup(msg);
                return foundAddr;
            }
        }

        current = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }

    LogStartup("[SCAN] FAILED: Pattern not found.");
    return std::nullopt;
}
