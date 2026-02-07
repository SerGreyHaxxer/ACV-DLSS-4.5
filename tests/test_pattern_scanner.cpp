#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "error_types.h"
#include "pattern_scanner.h"

TEST_CASE("PatternScanner scans local buffer", "[pattern_scanner]") {
    // Create a known buffer
    std::vector<uint8_t> buffer = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
        0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9,
        0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00
    };

    auto base = reinterpret_cast<uintptr_t>(buffer.data());
    size_t len = buffer.size();

    SECTION("Exact pattern match") {
        auto result = PatternScanner::Scan(base, len, "48 89 5C 24 08");
        REQUIRE(result.has_value());
        REQUIRE(*result == base);
    }

    SECTION("Pattern with wildcards") {
        auto result = PatternScanner::Scan(base, len, "48 89 ?? 24 08");
        REQUIRE(result.has_value());
        REQUIRE(*result == base);
    }

    SECTION("Pattern at offset") {
        auto result = PatternScanner::Scan(base, len, "DE AD BE EF");
        REQUIRE(result.has_value());
        REQUIRE(*result == base + 16);
    }

    SECTION("Pattern not found") {
        auto result = PatternScanner::Scan(base, len, "FF FF FF FF FF");
        REQUIRE(!result.has_value());
        REQUIRE(result.error() == ScanError::PatternNotFound);
    }

    SECTION("Wildcard-only pattern matches first byte") {
        auto result = PatternScanner::Scan(base, len, "?? ?? ??");
        REQUIRE(result.has_value());
        REQUIRE(*result == base);
    }

    SECTION("Pattern too long for buffer") {
        std::vector<uint8_t> tiny = {0x01, 0x02};
        auto result = PatternScanner::Scan(
            reinterpret_cast<uintptr_t>(tiny.data()), tiny.size(),
            "01 02 03 04 05");
        REQUIRE(!result.has_value());
    }
}
