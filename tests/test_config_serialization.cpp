#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "config_manager.h"
#include <toml++/toml.hpp>
#include <sstream>

// Re-implement the serialization helpers for testing (mirrors config_manager.cpp)
template <typename T>
static void TestSerialize(toml::table& tbl, const T& obj, const char* section) {
    toml::table sec;
    cpp26::reflect::forEachField<T>([&](const cpp26::reflect::FieldInfo& f) {
        if (f.type == cpp26::reflect::FieldType::Int)
            sec.insert_or_assign(f.name, f.getInt(&obj));
        else if (f.type == cpp26::reflect::FieldType::Float)
            sec.insert_or_assign(f.name, static_cast<double>(f.getFloat(&obj)));
        else if (f.type == cpp26::reflect::FieldType::Bool)
            sec.insert_or_assign(f.name, f.getBool(&obj));
    });
    tbl.insert_or_assign(section, sec);
}

template <typename T>
static void TestDeserialize(const toml::table& tbl, T& obj, const char* section) {
    if (!tbl.contains(section)) return;
    const auto* sec = tbl[section].as_table();
    if (!sec) return;
    cpp26::reflect::forEachField<T>([&](const cpp26::reflect::FieldInfo& f) {
        if (f.type == cpp26::reflect::FieldType::Int) {
            if (auto v = sec->get(f.name); v && v->is_integer())
                f.setInt(&obj, static_cast<int>(v->as_integer()->get()));
        } else if (f.type == cpp26::reflect::FieldType::Float) {
            if (auto v = sec->get(f.name); v && v->is_floating_point())
                f.setFloat(&obj, static_cast<float>(v->as_floating_point()->get()));
            else if (auto v2 = sec->get(f.name); v2 && v2->is_integer())
                f.setFloat(&obj, static_cast<float>(v2->as_integer()->get()));
        } else if (f.type == cpp26::reflect::FieldType::Bool) {
            if (auto v = sec->get(f.name); v && v->is_boolean())
                f.setBool(&obj, v->as_boolean()->get());
        }
    });
}

TEST_CASE("Config serialization roundtrip", "[config]") {
    // Ensure reflection is initialized
    cpp26::reflect::InitReflection();

    SECTION("DLSSConfig roundtrip") {
        DLSSConfig original;
        original.mode = 3;
        original.preset = 2;
        original.sharpness = 0.75f;
        original.lodBias = -2.5f;

        toml::table tbl;
        TestSerialize(tbl, original, "dlss");

        DLSSConfig loaded;
        TestDeserialize(tbl, loaded, "dlss");

        REQUIRE(loaded.mode == 3);
        REQUIRE(loaded.preset == 2);
        REQUIRE_THAT(loaded.sharpness, Catch::Matchers::WithinAbs(0.75, 0.001));
        REQUIRE_THAT(loaded.lodBias, Catch::Matchers::WithinAbs(-2.5, 0.001));
    }

    SECTION("FrameGenConfig roundtrip") {
        FrameGenConfig original;
        original.multiplier = 2;
        original.smartEnabled = true;
        original.autoDisableFps = 90.0f;

        toml::table tbl;
        TestSerialize(tbl, original, "fg");

        FrameGenConfig loaded;
        TestDeserialize(tbl, loaded, "fg");

        REQUIRE(loaded.multiplier == 2);
        REQUIRE(loaded.smartEnabled == true);
        REQUIRE_THAT(loaded.autoDisableFps, Catch::Matchers::WithinAbs(90.0, 0.1));
    }

    SECTION("Default values preserved when section missing") {
        toml::table emptyTbl;
        DLSSConfig defaults;
        TestDeserialize(emptyTbl, defaults, "nonexistent");

        REQUIRE(defaults.mode == 5);
        REQUIRE(defaults.preset == 0);
        REQUIRE_THAT(defaults.sharpness, Catch::Matchers::WithinAbs(0.5, 0.001));
    }

    SECTION("HDRConfig full roundtrip") {
        HDRConfig original;
        original.enabled = true;
        original.peakNits = 2000.0f;
        original.paperWhiteNits = 300.0f;

        toml::table tbl;
        TestSerialize(tbl, original, "hdr");

        HDRConfig loaded;
        TestDeserialize(tbl, loaded, "hdr");

        REQUIRE(loaded.enabled == true);
        REQUIRE_THAT(loaded.peakNits, Catch::Matchers::WithinAbs(2000.0, 0.1));
        REQUIRE_THAT(loaded.paperWhiteNits, Catch::Matchers::WithinAbs(300.0, 0.1));
    }

    SECTION("Full ModConfig roundtrip via TOML string") {
        ModConfig original;
        original.dlss.mode = 2;
        original.fg.multiplier = 3;
        original.dvc.enabled = true;
        original.hdr.enabled = true;
        original.ui.showFPS = true;

        toml::table tbl;
        TestSerialize(tbl, original.dlss, "dlss");
        TestSerialize(tbl, original.fg, "fg");
        TestSerialize(tbl, original.dvc, "dvc");
        TestSerialize(tbl, original.hdr, "hdr");
        TestSerialize(tbl, original.ui, "ui");

        // Serialize to string and parse back
        std::stringstream ss;
        ss << tbl;
        auto parsed = toml::parse(ss.str());

        ModConfig loaded;
        TestDeserialize(parsed, loaded.dlss, "dlss");
        TestDeserialize(parsed, loaded.fg, "fg");
        TestDeserialize(parsed, loaded.dvc, "dvc");
        TestDeserialize(parsed, loaded.hdr, "hdr");
        TestDeserialize(parsed, loaded.ui, "ui");

        REQUIRE(loaded.dlss.mode == 2);
        REQUIRE(loaded.fg.multiplier == 3);
        REQUIRE(loaded.dvc.enabled == true);
        REQUIRE(loaded.hdr.enabled == true);
        REQUIRE(loaded.ui.showFPS == true);
    }
}
