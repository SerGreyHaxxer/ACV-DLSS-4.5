#include <catch2/catch_test_macros.hpp>
#include "ghost_hook.h"

// NOTE: Ghost hook lifecycle tests (Initialize/Shutdown/InstallHook) require real
// OS-level VEH and debug register manipulation via thread enumeration.
// These hang in unit test environments due to thread suspend/resume.
// Only helper functions and singleton access are tested here.

TEST_CASE("Ghost hook singleton access", "[ghost_hook]") {
    auto& mgr1 = Ghost::HookManager::Get();
    auto& mgr2 = Ghost::HookManager::Get();
    REQUIRE(&mgr1 == &mgr2);
}

TEST_CASE("Ghost hook helper functions", "[ghost_hook]") {
    SECTION("GetReturnAddress with null context returns 0") {
        REQUIRE(Ghost::GetReturnAddress(nullptr) == 0);
    }

    SECTION("SetReturnValue with null context is safe") {
        Ghost::SetReturnValue(nullptr, 42); // Should not crash
    }

    SECTION("SkipFunction with null context is safe") {
        Ghost::SkipFunction(nullptr, 0); // Should not crash
    }

    SECTION("GetArg1-4 with null context return 0") {
        REQUIRE(Ghost::GetArg1(nullptr) == 0);
        REQUIRE(Ghost::GetArg2(nullptr) == 0);
        REQUIRE(Ghost::GetArg3(nullptr) == 0);
        REQUIRE(Ghost::GetArg4(nullptr) == 0);
    }
}

TEST_CASE("Ghost hook constants", "[ghost_hook]") {
    REQUIRE(Ghost::kMaxHooks == 4);
}
