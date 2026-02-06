#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

#include "error_types.h"

class PatternScanner {
public:
    static ScanResult<uintptr_t> Scan(const std::string& moduleName, const std::string& pattern);
    static ScanResult<uintptr_t> Scan(uintptr_t start, size_t length, const std::string& pattern);

private:
    static std::vector<int> ParsePattern(const std::string& pattern);
};
