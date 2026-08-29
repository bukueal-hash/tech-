// Bone / transform math suite — Pillar 1 (docs/aplus-plan.md)
// Exercises the REAL code from Core/BoneMath.hpp (extracted verbatim from
// Cache.hpp + SteamDecrypt.hpp): quaternion ops, FTransform→matrix, and the
// CL-1341255 SIMD bone-array-pointer decrypt.

#include "tests_main.hpp"
#include "Core/BoneMath.hpp"

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

#include <cmath>
#include <cstring>

namespace {

bool approx(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) <= eps;
}

bool approxVec(const Vector3& a, const Vector3& b, double eps = 1e-9)
{
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

// Scalar reference for BoneMath::DecryptBoneArrayPointer.
// PSHUFB {05,00,04,06,07,02,03,01}, ROL32=22 per dword, ROL64=50 low qword.
uint64_t refDecryptBoneArrayPointer(const uint8_t seed[16])
{
    uint8_t sh[16] = {};
    sh[0] = seed[5]; sh[1] = seed[0]; sh[2] = seed[4]; sh[3] = seed[6];
    sh[4] = seed[7]; sh[5] = seed[2]; sh[6] = seed[3]; sh[7] = seed[1];

    auto rol32 = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    auto rol64 = [](uint64_t x, int n) { return (x << n) | (x >> (64 - n)); };

    uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
    std::memcpy(&d0, sh + 0, 4);
    std::memcpy(&d1, sh + 4, 4);
    std::memcpy(&d2, sh + 8, 4);
    std::memcpy(&d3, sh + 12, 4);
    d0 = rol32(d0, 22); d1 = rol32(d1, 22);
    d2 = rol32(d2, 22); d3 = rol32(d3, 22);
    return rol64((static_cast<uint64_t>(d0)) | (static_cast<uint64_t>(d1) << 32), 50);
}

} // namespace

TEST_CASE("BoneMath::DecryptBoneArrayPointer matches scalar reference")
{
    // Deterministic set of seeds (no RNG).
    const uint8_t seeds[][16] = {
        { 0xDE, 0xAD, 0xBE, 0xEF, 0x13, 0x37, 0x00, 0x01,
          0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
        { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
          0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10 },
        { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
          0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F },
    };
    for (const auto& seed : seeds) {
        const uint64_t expected = refDecryptBoneArrayPointer(seed);
        const uint64_t actual = BoneMath::DecryptBoneArrayPointer(seed);
        CAPTURE(expected);
        CHECK(actual == expected);
    }
}

TEST_CASE("BoneMath::DecryptBoneArrayPointer edge cases")
{
    uint8_t zero[16] = {};
    CHECK(BoneMath::DecryptBoneArrayPointer(zero) == 0u);

    uint8_t ff[16];
    std::memset(ff, 0xFF, sizeof(ff));
    // All-ones: shuffle keeps 8 x 0xFF, ROL32/ROL64 of all-ones are all-ones.
    CHECK(BoneMath::DecryptBoneArrayPointer(ff) == 0xFFFFFFFFFFFFFFFFULL);

    uint8_t a[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t b[16];
    std::memcpy(b, a, sizeof(a));
    CHECK(BoneMath::DecryptBoneArrayPointer(a) == BoneMath::DecryptBoneArrayPointer(b));
}

TEST_CASE("FQuat identity and multiplication")
{
    const FQuat id{ 0.0, 0.0, 0.0, 1.0 };
    const FQuat q{ 0.1, 0.2, 0.3, 0.4 };

    const FQuat r = id.Multiply(q);
    CHECK(approx(r.x, q.x));
    CHECK(approx(r.y, q.y));
    CHECK(approx(r.z, q.z));
    CHECK(approx(r.w, q.w));

    // id rotates nothing.
    const Vector3 v(1.0, 2.0, 3.0);
    const Vector3 rv = id.RotateVector(v);
    CHECK(approxVec(rv, v));
}

TEST_CASE("FQuat rotation by 90 degrees around Z")
{
    const double s = std::sin(3.14159265358979323846 / 4.0);
    const FQuat q{ 0.0, 0.0, s, s };

    const Vector3 r = q.RotateVector(Vector3(1.0, 0.0, 0.0));
    CHECK(approxVec(r, Vector3(0.0, 1.0, 0.0), 1e-9));  // +X → +Y
}

TEST_CASE("FQuat rotation by 180 degrees around Z")
{
    const FQuat q{ 0.0, 0.0, 1.0, 0.0 };
    const Vector3 r = q.RotateVector(Vector3(1.0, 0.0, 0.0));
    CHECK(approxVec(r, Vector3(-1.0, 0.0, 0.0), 1e-9));  // +X → −X
}

TEST_CASE("FQuat: quaternion*vector equals matrix*vector consistency")
{
    // q=(0,0,sin45,cos45) rotating +X by +90° must match ToMatrixWithScale.
    const double s = std::sin(3.14159265358979323846 / 4.0);
    const FQuat q{ 0.0, 0.0, s, s };
    const FTransform t{ q, { 0.0, 0.0, 0.0 }, {}, { 1.0, 1.0, 1.0 }, {} };
    const D3DMATRIX m = t.ToMatrixWithScale();

    // m * (1,0,0,1) should be (cos90, sin90, 0, 1) = (0,1,0,1).
    const float wx = m._11 * 1.f + m._21 * 0.f + m._31 * 0.f + m._41;
    const float wy = m._12 * 1.f + m._22 * 0.f + m._32 * 0.f + m._42;
    const float wz = m._13 * 1.f + m._23 * 0.f + m._33 * 0.f + m._43;
    CHECK(approx(wx, 0.f, 1e-4f));
    CHECK(approx(wy, 1.f, 1e-4f));
    CHECK(approx(wz, 0.f, 1e-4f));
}

TEST_CASE("FTransform::ToMatrixWithScale translation + identity rotation")
{
    const FTransform t{ { 0.0, 0.0, 0.0, 1.0 }, { 10.0, -20.0, 30.0 }, {},
                        { 1.0, 1.0, 1.0 }, {} };
    const D3DMATRIX m = t.ToMatrixWithScale();

    CHECK(approx(m._11, 1.f, 1e-4f));
    CHECK(approx(m._22, 1.f, 1e-4f));
    CHECK(approx(m._33, 1.f, 1e-4f));
    CHECK(approx(m._44, 1.f, 1e-4f));
    CHECK(approx(m._41, 10.f, 1e-4f));
    CHECK(approx(m._42, -20.f, 1e-4f));
    CHECK(approx(m._43, 30.f, 1e-4f));
    // No shearing / non-diagonal entries for identity rotation.
    CHECK(approx(m._12, 0.f, 1e-4f));
    CHECK(approx(m._21, 0.f, 1e-4f));
    CHECK(approx(m._31, 0.f, 1e-4f));
}

TEST_CASE("FTransform::ToMatrixWithScale scale applied on diagonal")
{
    const FTransform t{ { 0.0, 0.0, 0.0, 1.0 }, {}, {},
                        { 2.0, 3.0, 4.0 }, {} };
    const D3DMATRIX m = t.ToMatrixWithScale();
    CHECK(approx(m._11, 2.f, 1e-4f));
    CHECK(approx(m._22, 3.f, 1e-4f));
    CHECK(approx(m._33, 4.f, 1e-4f));
}

TEST_CASE("FTransform layout matches engine stride")
{
    // FTransform is 0x60 bytes total (matches BoneList's stride constant).
    static_assert(sizeof(FTransform) == 0x60, "FTransform size must be 0x60");
    static_assert(offsetof(FTransform, Rotation) == 0x0);
    static_assert(offsetof(FTransform, Translation) == 0x20);
    static_assert(offsetof(FTransform, Scale3D) == 0x40);
    static_assert(sizeof(FQuat) == 0x20, "FQuat must be 4 doubles");
}