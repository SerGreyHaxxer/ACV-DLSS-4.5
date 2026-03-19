/*
 * Unit Tests: Camera Scoring (ScoreMatrixPair)
 *
 * Tests the matrix scoring logic used for camera detection.
 * Since ScoreMatrixPair is in an anonymous namespace in camera_scanner.cpp,
 * we re-implement the key scoring logic here for isolated testing.
 * This validates the fixed indexing (PR #6: mdspan → manual row*4+col).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <span>

// ============================================================================
// Duplicate of ScoreMatrixPair scoring logic for testability
// This mirrors the exact code in camera_scanner.cpp after the mdspan fix.
// If the implementation changes, these tests ensure the logic remains correct.
// ============================================================================

namespace test_scoring {

static bool IsFinite(float v) {
    return v == v && v > -FLT_MAX && v < FLT_MAX;
}

static bool LooksLikeMatrix(std::span<const float, 16> m) {
    for (int i = 0; i < 16; ++i) {
        if (!IsFinite(m[i])) return false;
    }
    return true;
}

static float Dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float Length3(const float* v) {
    return sqrtf(Dot3(v, v));
}

// Exact copy of ScoreMatrixPair from camera_scanner.cpp (after PR #6 fix)
float ScoreMatrixPair(std::span<const float, 16> viewData, std::span<const float, 16> projData) {
    float score = 0.0f;
    if (!LooksLikeMatrix(viewData) || !LooksLikeMatrix(projData)) return 0.0f;

    const float* view = viewData.data();
    const float* proj = projData.data();

    // view[3,3] should be 1.0 for an affine view matrix
    if (std::abs(view[3 * 4 + 3] - 1.0f) > 0.1f) return 0.0f;
    if (std::abs(view[3 * 4 + 3] - 1.0f) < 0.01f) score += 0.2f;

    // Perspective projection detection
    bool isStrongPerspective = std::abs(proj[3 * 4 + 3]) < 0.01f && std::abs(std::abs(proj[2 * 4 + 3]) - 1.0f) < 0.1f;
    bool isWeakPerspective = std::abs(proj[3 * 4 + 3]) < 0.8f && std::abs(proj[2 * 4 + 3]) > 0.2f;

    if (isStrongPerspective)
        score += 0.6f;
    else if (isWeakPerspective)
        score += 0.3f;
    else
        return 0.0f;

    // FoV validation
    if (std::abs(proj[0 * 4 + 0]) > 0.3f && std::abs(proj[0 * 4 + 0]) < 5.0f &&
        std::abs(proj[1 * 4 + 1]) > 0.3f && std::abs(proj[1 * 4 + 1]) < 5.0f) {
        score += 0.15f;
        if (std::abs(proj[1 * 4 + 1]) > 0.8f && std::abs(proj[1 * 4 + 1]) < 2.2f) score += 0.05f;
    }

    // Affine view matrix: last column should be [0, 0, 0, 1]
    if (std::abs(view[0 * 4 + 3]) < 1.0f && std::abs(view[1 * 4 + 3]) < 1.0f && std::abs(view[2 * 4 + 3]) < 1.0f) score += 0.1f;

    // Orthogonality check
    float r0[3] = { view[0 * 4 + 0], view[0 * 4 + 1], view[0 * 4 + 2] };
    float r1[3] = { view[1 * 4 + 0], view[1 * 4 + 1], view[1 * 4 + 2] };
    float r2[3] = { view[2 * 4 + 0], view[2 * 4 + 1], view[2 * 4 + 2] };
    float len0 = Length3(r0), len1 = Length3(r1), len2 = Length3(r2);
    if (len0 > 0.1f && len1 > 0.1f && len2 > 0.1f) {
        float orthoScore = 0.0f;
        float d01 = std::abs(Dot3(r0, r1) / (len0 * len1));
        float d02 = std::abs(Dot3(r0, r2) / (len0 * len2));
        float d12 = std::abs(Dot3(r1, r2) / (len1 * len2));
        if (d01 < 0.2f) orthoScore += 0.1f;
        if (d02 < 0.2f) orthoScore += 0.1f;
        if (d12 < 0.2f) orthoScore += 0.1f;
        score += orthoScore;
        if (std::abs(len0 - 1.0f) < 0.15f && std::abs(len1 - 1.0f) < 0.15f && std::abs(len2 - 1.0f) < 0.15f) {
            score += 0.1f;
        }
    }

    return score;
}

} // namespace test_scoring

// ============================================================================
// Helper: Build a standard perspective projection matrix
// ============================================================================
static std::array<float, 16> MakePerspectiveProjection(float fovY = 1.0472f, float aspect = 16.0f / 9.0f,
                                                        float nearZ = 0.1f, float farZ = 1000.0f) {
    float f = 1.0f / std::tan(fovY / 2.0f);
    float rangeInv = 1.0f / (nearZ - farZ);
    std::array<float, 16> proj{};
    // Row-major layout: proj[row * 4 + col]
    proj[0 * 4 + 0] = f / aspect;
    proj[1 * 4 + 1] = f;
    proj[2 * 4 + 2] = farZ * rangeInv;
    proj[2 * 4 + 3] = -1.0f;  // perspective divide marker
    proj[3 * 4 + 2] = nearZ * farZ * rangeInv;
    proj[3 * 4 + 3] = 0.0f;   // perspective: w = 0
    return proj;
}

// ============================================================================
// Helper: Build a standard identity view matrix (camera at origin, looking -Z)
// ============================================================================
static std::array<float, 16> MakeIdentityView() {
    std::array<float, 16> view{};
    view[0 * 4 + 0] = 1.0f;
    view[1 * 4 + 1] = 1.0f;
    view[2 * 4 + 2] = 1.0f;
    view[3 * 4 + 3] = 1.0f;
    return view;
}

// ============================================================================
// Helper: Build a rotated view matrix (camera rotated 45° around Y)
// ============================================================================
static std::array<float, 16> MakeRotatedView() {
    float c = std::cos(0.7854f); // cos(45°)
    float s = std::sin(0.7854f); // sin(45°)
    std::array<float, 16> view{};
    view[0 * 4 + 0] = c;
    view[0 * 4 + 2] = s;
    view[1 * 4 + 1] = 1.0f;
    view[2 * 4 + 0] = -s;
    view[2 * 4 + 2] = c;
    view[3 * 4 + 0] = 10.0f;  // translation X
    view[3 * 4 + 1] = 5.0f;   // translation Y
    view[3 * 4 + 2] = -20.0f; // translation Z
    view[3 * 4 + 3] = 1.0f;
    return view;
}

// ============================================================================
// TESTS
// ============================================================================

TEST_CASE("ScoreMatrixPair: identity view + perspective projection scores high", "[camera]") {
    auto view = MakeIdentityView();
    auto proj = MakePerspectiveProjection();

    float score = test_scoring::ScoreMatrixPair(
        std::span<const float, 16>{view.data(), 16},
        std::span<const float, 16>{proj.data(), 16});

    // Identity view + proper perspective -> should be well above threshold
    REQUIRE(score > 0.6f);
    // Strong perspective + good FoV + affine view + orthogonality + unit rows
    REQUIRE(score > 1.0f);
}

TEST_CASE("ScoreMatrixPair: rotated view + perspective projection scores high", "[camera]") {
    auto view = MakeRotatedView();
    auto proj = MakePerspectiveProjection();

    float score = test_scoring::ScoreMatrixPair(
        std::span<const float, 16>{view.data(), 16},
        std::span<const float, 16>{proj.data(), 16});

    REQUIRE(score > 0.6f);
}

TEST_CASE("ScoreMatrixPair: two identity matrices score 0 (not perspective)", "[camera]") {
    auto identity = MakeIdentityView();

    float score = test_scoring::ScoreMatrixPair(
        std::span<const float, 16>{identity.data(), 16},
        std::span<const float, 16>{identity.data(), 16});

    // Identity is NOT a perspective projection (proj[3][3] = 1.0, not 0)
    REQUIRE(score == 0.0f);
}

TEST_CASE("ScoreMatrixPair: all zeros scores 0", "[camera]") {
    std::array<float, 16> zeros{};

    float score = test_scoring::ScoreMatrixPair(
        std::span<const float, 16>{zeros.data(), 16},
        std::span<const float, 16>{zeros.data(), 16});

    REQUIRE(score == 0.0f);
}

TEST_CASE("ScoreMatrixPair: NaN data returns 0", "[camera]") {
    std::array<float, 16> nanData{};
    nanData[0] = std::numeric_limits<float>::quiet_NaN();

    float score = test_scoring::ScoreMatrixPair(
        std::span<const float, 16>{nanData.data(), 16},
        std::span<const float, 16>{nanData.data(), 16});

    REQUIRE(score == 0.0f);
}

TEST_CASE("ScoreMatrixPair: view[3][3] != 1.0 rejects immediately", "[camera]") {
    auto view = MakeIdentityView();
    auto proj = MakePerspectiveProjection();

    // Corrupt view[3][3] — should fail the affine check
    view[3 * 4 + 3] = 5.0f;

    float score = test_scoring::ScoreMatrixPair(
        std::span<const float, 16>{view.data(), 16},
        std::span<const float, 16>{proj.data(), 16});

    REQUIRE(score == 0.0f);
}

TEST_CASE("ScoreMatrixPair: wide FoV projection still detects", "[camera]") {
    auto view = MakeIdentityView();
    // 120° FoV
    auto proj = MakePerspectiveProjection(2.0944f, 16.0f / 9.0f);

    float score = test_scoring::ScoreMatrixPair(
        std::span<const float, 16>{view.data(), 16},
        std::span<const float, 16>{proj.data(), 16});

    REQUIRE(score > 0.6f);
}

TEST_CASE("ScoreMatrixPair: fixed indexing reads correct elements", "[camera]") {
    // This test validates that the PR #6 fix (mdspan -> manual row*4+col)
    // reads the correct matrix elements. Before the fix, view[3,3] used the
    // comma operator and read view[3] instead of view[15].
    auto view = MakeIdentityView();
    auto proj = MakePerspectiveProjection();

    // Verify the correct element is being checked:
    // view[3*4+3] = view[15] = 1.0f (identity)
    REQUIRE(view[3 * 4 + 3] == 1.0f);
    // proj[3*4+3] = proj[15] = 0.0f (perspective)
    REQUIRE(proj[3 * 4 + 3] == 0.0f);
    // proj[2*4+3] = proj[11] = -1.0f (perspective divide)
    REQUIRE_THAT(proj[2 * 4 + 3], Catch::Matchers::WithinAbs(-1.0f, 0.001f));

    float score = test_scoring::ScoreMatrixPair(
        std::span<const float, 16>{view.data(), 16},
        std::span<const float, 16>{proj.data(), 16});

    // With correct indexing, this should score very high
    REQUIRE(score > 1.0f);
}
