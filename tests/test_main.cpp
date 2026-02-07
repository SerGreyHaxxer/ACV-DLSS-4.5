// Catch2 v3 auto-generates main when linking Catch2::Catch2WithMain
// This file exists for any global test setup/teardown

#include <catch2/catch_test_macros.hpp>

// Stub for LogStartup used by pattern_scanner.cpp (defined in proxy.cpp in main build)
extern "C" void LogStartup(const char*) {}
