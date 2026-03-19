/*
 * Unit Tests: Sampler Interceptor (ApplyLodBias)
 *
 * Tests the LOD bias application logic for D3D12 sampler descriptors.
 * Validates clamping, zero-bias passthrough, and additive behavior.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "sampler_interceptor.h"

// ============================================================================
// Helper: Create a default sampler desc
// ============================================================================
static D3D12_SAMPLER_DESC MakeDefaultSampler(float existingBias = 0.0f) {
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.MipLODBias = existingBias;
    desc.MaxAnisotropy = 16;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    return desc;
}

// ============================================================================
// TESTS
// ============================================================================

TEST_CASE("ApplyLodBias: zero bias returns desc unchanged", "[sampler]") {
    SamplerInterceptor_SetTargetLODBias(0.0f);
    auto desc = MakeDefaultSampler(0.0f);

    auto result = ApplyLodBias(desc);

    REQUIRE(result.MipLODBias == 0.0f);
    REQUIRE(result.Filter == desc.Filter);
    REQUIRE(result.MaxAnisotropy == desc.MaxAnisotropy);
}

TEST_CASE("ApplyLodBias: positive bias increases MipLODBias", "[sampler]") {
    SamplerInterceptor_SetTargetLODBias(1.5f);
    auto desc = MakeDefaultSampler(0.0f);

    auto result = ApplyLodBias(desc);

    REQUIRE_THAT(result.MipLODBias, Catch::Matchers::WithinAbs(1.5f, 0.01f));
}

TEST_CASE("ApplyLodBias: negative bias decreases MipLODBias", "[sampler]") {
    SamplerInterceptor_SetTargetLODBias(-2.0f);
    auto desc = MakeDefaultSampler(0.0f);

    auto result = ApplyLodBias(desc);

    REQUIRE_THAT(result.MipLODBias, Catch::Matchers::WithinAbs(-2.0f, 0.01f));
}

TEST_CASE("ApplyLodBias: additive with existing bias", "[sampler]") {
    SamplerInterceptor_SetTargetLODBias(1.0f);
    auto desc = MakeDefaultSampler(0.5f); // existing bias = 0.5

    auto result = ApplyLodBias(desc);

    // 0.5 + 1.0 = 1.5
    REQUIRE_THAT(result.MipLODBias, Catch::Matchers::WithinAbs(1.5f, 0.01f));
}

TEST_CASE("ApplyLodBias: clamps at D3D12 upper limit (+15.99)", "[sampler]") {
    SamplerInterceptor_SetTargetLODBias(3.0f); // max allowed by SetTargetLODBias
    auto desc = MakeDefaultSampler(15.0f); // existing bias near limit

    auto result = ApplyLodBias(desc);

    // 15.0 + 3.0 = 18.0, should be clamped to 15.99
    REQUIRE(result.MipLODBias <= 15.99f);
}

TEST_CASE("ApplyLodBias: clamps at D3D12 lower limit (-16.0)", "[sampler]") {
    SamplerInterceptor_SetTargetLODBias(-3.0f); // min allowed
    auto desc = MakeDefaultSampler(-14.0f); // existing bias near limit

    auto result = ApplyLodBias(desc);

    // -14.0 + (-3.0) = -17.0, should be clamped to -16.0
    REQUIRE(result.MipLODBias >= -16.0f);
}

TEST_CASE("ApplyLodBias: very small bias below threshold does nothing", "[sampler]") {
    SamplerInterceptor_SetTargetLODBias(0.0005f); // below 0.001 threshold
    auto desc = MakeDefaultSampler(2.0f);

    auto result = ApplyLodBias(desc);

    // Bias below threshold → no modification
    REQUIRE(result.MipLODBias == 2.0f);
}

TEST_CASE("ApplyLodBias: preserves all other sampler properties", "[sampler]") {
    SamplerInterceptor_SetTargetLODBias(1.0f);
    auto desc = MakeDefaultSampler(0.0f);
    desc.MinLOD = 2.0f;
    desc.MaxLOD = 10.0f;

    auto result = ApplyLodBias(desc);

    REQUIRE(result.Filter == D3D12_FILTER_ANISOTROPIC);
    REQUIRE(result.AddressU == D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    REQUIRE(result.MaxAnisotropy == 16);
    REQUIRE(result.MinLOD == 2.0f);
    REQUIRE(result.MaxLOD == 10.0f);

    // Reset for other tests
    SamplerInterceptor_SetTargetLODBias(0.0f);
}
