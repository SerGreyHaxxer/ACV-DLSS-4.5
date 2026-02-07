#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "cpp26/reflection.h"
#include <string>
#include <vector>

// Test struct for reflection
struct TestReflectStruct {
    int intField = 42;
    float floatField = 3.14f;
    bool boolField = true;
};

// Register with reflection macros
namespace cpp26::reflect {
REFLECT_STRUCT_BEGIN(TestReflectStruct)
    REFLECT_FIELD(int, intField, 42, ui::slider_int(0, 100), "General")
    REFLECT_FIELD(float, floatField, 3.14f, ui::slider_float(0.0f, 10.0f), "General")
    REFLECT_FIELD(bool, boolField, true, ui::checkbox(), "General")
REFLECT_STRUCT_END()

inline void InitTestReflection() {
    REFLECT_INIT(TestReflectStruct);
}
} // namespace cpp26::reflect

static void EnsureReflectionInit() {
    static bool done = false;
    if (!done) {
        cpp26::reflect::InitTestReflection();
        done = true;
    }
}

TEST_CASE("Reflection field count", "[reflection]") {
    EnsureReflectionInit();
    REQUIRE(cpp26::reflect::StructInfo<TestReflectStruct>::fieldCount == 3);
}

TEST_CASE("Reflection field names match", "[reflection]") {
    EnsureReflectionInit();
    auto& fields = cpp26::reflect::StructInfo<TestReflectStruct>::fields;
    REQUIRE(fields[0].name == "intField");
    REQUIRE(fields[1].name == "floatField");
    REQUIRE(fields[2].name == "boolField");
}

TEST_CASE("Reflection field types match", "[reflection]") {
    EnsureReflectionInit();
    auto& fields = cpp26::reflect::StructInfo<TestReflectStruct>::fields;
    REQUIRE(fields[0].type == cpp26::reflect::FieldType::Int);
    REQUIRE(fields[1].type == cpp26::reflect::FieldType::Float);
    REQUIRE(fields[2].type == cpp26::reflect::FieldType::Bool);
}

TEST_CASE("Reflection getInt/setInt work", "[reflection]") {
    EnsureReflectionInit();
    TestReflectStruct obj;
    auto* field = cpp26::reflect::StructInfo<TestReflectStruct>::getField("intField");
    REQUIRE(field != nullptr);

    REQUIRE(field->getInt(&obj) == 42);
    field->setInt(&obj, 99);
    REQUIRE(obj.intField == 99);
    REQUIRE(field->getInt(&obj) == 99);
}

TEST_CASE("Reflection getFloat/setFloat work", "[reflection]") {
    EnsureReflectionInit();
    TestReflectStruct obj;
    auto* field = cpp26::reflect::StructInfo<TestReflectStruct>::getField("floatField");
    REQUIRE(field != nullptr);

    REQUIRE(field->getFloat(&obj) == Catch::Approx(3.14f));
    field->setFloat(&obj, 2.718f);
    REQUIRE(obj.floatField == Catch::Approx(2.718f));
    REQUIRE(field->getFloat(&obj) == Catch::Approx(2.718f));
}

TEST_CASE("Reflection getBool/setBool work", "[reflection]") {
    EnsureReflectionInit();
    TestReflectStruct obj;
    auto* field = cpp26::reflect::StructInfo<TestReflectStruct>::getField("boolField");
    REQUIRE(field != nullptr);

    REQUIRE(field->getBool(&obj) == true);
    field->setBool(&obj, false);
    REQUIRE(obj.boolField == false);
    REQUIRE(field->getBool(&obj) == false);
}

TEST_CASE("Reflection forEachField iterates all fields", "[reflection]") {
    EnsureReflectionInit();
    std::vector<std::string> names;
    cpp26::reflect::forEachField<TestReflectStruct>(
        [&](const cpp26::reflect::FieldInfo& f) {
            names.emplace_back(f.name);
        });

    REQUIRE(names.size() == 3);
    REQUIRE(names[0] == "intField");
    REQUIRE(names[1] == "floatField");
    REQUIRE(names[2] == "boolField");
}

TEST_CASE("Reflection forEachFieldInCategory filters correctly", "[reflection]") {
    EnsureReflectionInit();
    std::vector<std::string> generalFields;
    cpp26::reflect::forEachFieldInCategory<TestReflectStruct>(
        "General",
        [&](const cpp26::reflect::FieldInfo& f) {
            generalFields.emplace_back(f.name);
        });
    REQUIRE(generalFields.size() == 3);

    // Non-existent category should yield nothing
    std::vector<std::string> noFields;
    cpp26::reflect::forEachFieldInCategory<TestReflectStruct>(
        "NonExistent",
        [&](const cpp26::reflect::FieldInfo& f) {
            noFields.emplace_back(f.name);
        });
    REQUIRE(noFields.empty());
}

TEST_CASE("Reflection getField by name returns correct field", "[reflection]") {
    EnsureReflectionInit();

    auto* intF = cpp26::reflect::StructInfo<TestReflectStruct>::getField("intField");
    REQUIRE(intF != nullptr);
    REQUIRE(intF->name == "intField");
    REQUIRE(intF->type == cpp26::reflect::FieldType::Int);

    auto* floatF = cpp26::reflect::StructInfo<TestReflectStruct>::getField("floatField");
    REQUIRE(floatF != nullptr);
    REQUIRE(floatF->type == cpp26::reflect::FieldType::Float);

    auto* missing = cpp26::reflect::StructInfo<TestReflectStruct>::getField("noSuchField");
    REQUIRE(missing == nullptr);
}
