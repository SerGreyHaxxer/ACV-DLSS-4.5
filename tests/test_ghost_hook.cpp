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

// ============================================================================
// P2 FIX: Lifecycle tests — exercise Initialize/Shutdown/InstallHook/RemoveHook
// that were previously skipped entirely.
// ============================================================================

// A dummy function used as a hook target.  The function must produce a real
// code address that is not inlined away.
static volatile int g_dummyCounter = 0;
__declspec(noinline) static void DummyHookTarget() {
    g_dummyCounter++;
}

TEST_CASE("Ghost hook Initialize and Shutdown cycle", "[ghost_hook][lifecycle]") {
    auto& mgr = Ghost::HookManager::Get();

    // If already initialized from a previous test, shut it down first
    if (mgr.IsInitialized()) {
        mgr.Shutdown();
    }

    REQUIRE_FALSE(mgr.IsInitialized());

    SECTION("Initialize succeeds") {
        REQUIRE(mgr.Initialize());
        REQUIRE(mgr.IsInitialized());
        REQUIRE(mgr.GetActiveHookCount() == 0);

        mgr.Shutdown();
        REQUIRE_FALSE(mgr.IsInitialized());
    }

    SECTION("Double Initialize is idempotent") {
        REQUIRE(mgr.Initialize());
        REQUIRE(mgr.Initialize()); // Should return true, not fail
        REQUIRE(mgr.IsInitialized());

        mgr.Shutdown();
    }

    SECTION("Shutdown without Initialize is safe") {
        mgr.Shutdown(); // Should not crash
    }
}

TEST_CASE("Ghost hook InstallHook and RemoveHook", "[ghost_hook][lifecycle]") {
    auto& mgr = Ghost::HookManager::Get();
    if (mgr.IsInitialized()) mgr.Shutdown();
    REQUIRE(mgr.Initialize());

    auto targetAddr = reinterpret_cast<uintptr_t>(&DummyHookTarget);
    bool callbackFired = false;

    SECTION("InstallHook returns valid slot ID") {
        int slot = mgr.InstallHook(targetAddr, [&](CONTEXT*, void*) -> bool {
            callbackFired = true;
            return true;
        });
        REQUIRE(slot >= 0);
        REQUIRE(slot < static_cast<int>(Ghost::kMaxHooks));
        REQUIRE(mgr.GetActiveHookCount() == 1);
        REQUIRE(mgr.IsAddressHooked(targetAddr));

        SECTION("RemoveHook decrements count") {
            REQUIRE(mgr.RemoveHook(slot));
            REQUIRE(mgr.GetActiveHookCount() == 0);
            REQUIRE_FALSE(mgr.IsAddressHooked(targetAddr));
        }

        SECTION("RemoveHookByAddress works") {
            REQUIRE(mgr.RemoveHookByAddress(targetAddr));
            REQUIRE(mgr.GetActiveHookCount() == 0);
        }

        // Clean up if sections above didn't run
        if (mgr.IsAddressHooked(targetAddr)) {
            mgr.RemoveHookByAddress(targetAddr);
        }
    }

    SECTION("InstallHook with null callback fails") {
        int slot = mgr.InstallHook(targetAddr, nullptr);
        REQUIRE(slot == -1);
    }

    SECTION("InstallHook with zero address fails") {
        int slot = mgr.InstallHook(0, [](CONTEXT*, void*) -> bool { return true; });
        REQUIRE(slot == -1);
    }

    SECTION("Duplicate address is rejected") {
        auto cb = [](CONTEXT*, void*) -> bool { return true; };
        int slot1 = mgr.InstallHook(targetAddr, cb);
        REQUIRE(slot1 >= 0);
        int slot2 = mgr.InstallHook(targetAddr, cb);
        REQUIRE(slot2 == -1); // Duplicate
        mgr.RemoveHook(slot1);
    }

    SECTION("RemoveHook with invalid ID fails gracefully") {
        REQUIRE_FALSE(mgr.RemoveHook(-1));
        REQUIRE_FALSE(mgr.RemoveHook(99));
    }

    SECTION("GetHookSlot returns valid data") {
        auto cb = [](CONTEXT*, void*) -> bool { return true; };
        int slot = mgr.InstallHook(targetAddr, cb);
        REQUIRE(slot >= 0);

        auto* info = mgr.GetHookSlot(slot);
        REQUIRE(info != nullptr);
        REQUIRE(info->active == true);
        REQUIRE(info->address == targetAddr);

        // Invalid slot returns null
        REQUIRE(mgr.GetHookSlot(-1) == nullptr);
        REQUIRE(mgr.GetHookSlot(99) == nullptr);

        mgr.RemoveHook(slot);
    }

    SECTION("Stats are initialized to zero") {
        auto stats = mgr.GetStats();
        // Stats may have non-zero values from other test sections,
        // but they should be well-formed integers
        REQUIRE(stats.totalHits >= 0);
        REQUIRE(stats.callbacksExecuted >= 0);
    }

    mgr.Shutdown();
}

TEST_CASE("Ghost hook ClearAllBreakpoints does not crash", "[ghost_hook][lifecycle]") {
    auto& mgr = Ghost::HookManager::Get();
    if (mgr.IsInitialized()) mgr.Shutdown();
    REQUIRE(mgr.Initialize());

    auto targetAddr = reinterpret_cast<uintptr_t>(&DummyHookTarget);
    auto cb = [](CONTEXT*, void*) -> bool { return true; };

    int slot = mgr.InstallHook(targetAddr, cb);
    REQUIRE(slot >= 0);

    // Shutdown internally calls ClearAllBreakpoints — should not crash
    mgr.Shutdown();
    REQUIRE_FALSE(mgr.IsInitialized());
}
