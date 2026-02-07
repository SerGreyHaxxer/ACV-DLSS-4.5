#include <catch2/catch_test_macros.hpp>
#include "sentinel_crash_handler.h"

TEST_CASE("Sentinel crash handler lifecycle", "[sentinel]") {
    // Start clean
    Sentinel::Uninstall();

    SECTION("Install and uninstall") {
        REQUIRE(Sentinel::Install() == true);
        REQUIRE(Sentinel::IsInstalled() == true);
        Sentinel::Uninstall();
        REQUIRE(Sentinel::IsInstalled() == false);
    }

    SECTION("Double install returns false") {
        REQUIRE(Sentinel::Install() == true);
        REQUIRE(Sentinel::Install() == false);
        Sentinel::Uninstall();
    }

    SECTION("Uninstall when not installed is safe") {
        REQUIRE(Sentinel::IsInstalled() == false);
        Sentinel::Uninstall(); // Should not crash
        REQUIRE(Sentinel::IsInstalled() == false);
    }

    SECTION("Config is stored") {
        Sentinel::Config cfg{};
        cfg.enableFullMemoryDump = true;
        cfg.enableStackWalk = false;
        Sentinel::Install(cfg);

        auto& stored = Sentinel::GetConfig();
        REQUIRE(stored.enableFullMemoryDump == true);
        REQUIRE(stored.enableStackWalk == false);
        Sentinel::Uninstall();
    }

    SECTION("Last crash address initially zero") {
        REQUIRE(Sentinel::GetLastCrashAddress() == 0);
    }

    SECTION("Last exception code initially zero") {
        REQUIRE(Sentinel::GetLastExceptionCode() == 0);
    }

    SECTION("GetCapturedStackTrace with zero frames") {
        Sentinel::StackFrame frames[4];
        size_t count = Sentinel::GetCapturedStackTrace(frames, 4);
        REQUIRE(count == 0);
    }
}
