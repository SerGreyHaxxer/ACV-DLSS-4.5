#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <optional>

class PatternScanner {
public:
    static std::optional<uintptr_t> Scan(const std::string& moduleName, const std::string& pattern);
    static std::optional<uintptr_t> Scan(uintptr_t start, size_t length, const std::string& pattern);

private:
    static std::vector<int> ParsePattern(const std::string& pattern);
};
