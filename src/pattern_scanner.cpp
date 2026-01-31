#include "pattern_scanner.h"
#include "logger.h"
#include <psapi.h>
#include <sstream>
#include <iomanip>
#include <stdio.h> // for sprintf

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
    sprintf_s(msg, "[SCAN] Scanning module: %s", moduleName.c_str());
    LogStartup(msg);

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

    uintptr_t start = (uintptr_t)modInfo.lpBaseOfDll;
    size_t size = modInfo.SizeOfImage;

    sprintf_s(msg, "[SCAN] Search Range: 0x%p - 0x%p (%zu bytes)", (void*)start, (void*)(start + size), size);
    LogStartup(msg);

    return Scan(start, size, pattern);
}

std::optional<uintptr_t> PatternScanner::Scan(uintptr_t start, size_t length, const std::string& pattern) {
    std::vector<int> patternBytes = ParsePattern(pattern);
    uint8_t* scanStart = (uint8_t*)start;
    size_t patternLength = patternBytes.size();
    
    // Simple log for pattern
    LogStartup("[SCAN] Starting memory scan...");

    for (size_t i = 0; i < length - patternLength; ++i) {
        bool found = true;
        for (size_t j = 0; j < patternLength; ++j) {
            if (patternBytes[j] != -1 && patternBytes[j] != scanStart[i + j]) {
                found = false;
                break;
            }
        }
        if (found) {
            char msg[256];
            sprintf_s(msg, "[SCAN] SUCCESS! Found at offset: +0x%zX", i);
            LogStartup(msg);
            return start + i;
        }
    }

    LogStartup("[SCAN] FAILED: Pattern not found.");
    return std::nullopt;
}
