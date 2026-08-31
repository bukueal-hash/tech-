// Vector / matrix math suite — Pillar 1 (docs/aplus-plan.md)
// Covers Vector2, Vector3, matrix3x4_t, and the RAD2DEG/DEG2RAD macros.
// Testable in isolation: no DMA, no ImGui, no process.

#include "tests_main.hpp"
#include "Core/Vector.hpp"

#pragma warning(push)
#pragma warning(disable : 4201 4244 4459)
#include "Core/Engine.h"
#pragma warning(pop)

// doctest's own headers trip C5285 (std::tuple specialization) under /W4 on
// the VS 18.x compiler — ThirdParty must never fail the build.
#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

// doctest needs to stringify these for failure messages
namespace doctest {
template <>
struct StringMaker<Vector2>
{
    static String convert(const Vector2& v)
    {
        return String(toString(v.x).c_str()) + "," + String(toString(v.y).c_str());
    }
};
template <>
struct StringMaker<Vector3>
{
    static String convert(const Vector3& v)
    {
        return String(toString(v.x).c_str()) + "," + String(toString(v.y).c_str()) + "," + String(toString(v.z).c_str());
    }
};
} // namespace doctest

namespace {

// Approximate equality; both operands are finite doubles.
bool approx(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) <= eps;
}

} // namespace

TEST_CASE("Vector2 arithmetic")
{
    Vector2 a(3.0, 4.0);
    Vector2 b(1.0, 2.0);

    CHECK(a.Distance(b) == doctest::Approx(std::sqrt(8.0)));
    // Vector2 has no operator== (Vector3 does) — compare components
    {
        Vector2 s = a + b;
        CHECK(s.x == doctest::Approx(4.0));
        CHECK(s.y == doctest::Approx(6.0));
    }
    {
        Vector2 d = a - b;
        CHECK(d.x == doctest::Approx(2.0));
        CHECK(d.y == doctest::Approx(2.0));
    }

    Vector2 fl = a.flip();
    CHECK(fl.x == doctest::Approx(a.y));
    CHECK(fl.y == doctest::Approx(a.x));

    // default ctor zeros
    Vector2 z;
    CHECK(z.x == 0.0);
    CHECK(z.y == 0.0);
}

TEST_CASE("Vector3 arithmetic and distance")
{
    Vector3 a(1.0, 2.0, 2.0);   // length 3
    Vector3 b(4.0, 0.0, 0.0);

    CHECK(a.Length() == doctest::Approx(3.0));
    CHECK(a.Dot(b) == doctest::Approx(4.0));
    CHECK(a.Distance(b) == doctest::Approx(std::sqrt(17.0))); // diff (3,-2,-2)
    CHECK(a.DistTo(b) == doctest::Approx(std::sqrt(17.0)));

    CHECK(a + b == Vector3(5.0, 2.0, 2.0));
    CHECK(b - a == Vector3(3.0, -2.0, -2.0));
    CHECK(a * 2.0 == Vector3(2.0, 4.0, 4.0));
    CHECK(a / 2.0 == Vector3(0.5, 1.0, 1.0));

    // operator== is exact (double equality) — document intent
    CHECK(a == Vector3(1.0, 2.0, 2.0));
    CHECK(a != b);

    Vector3 c = a;
    c += b;
    CHECK(c == Vector3(5.0, 2.0, 2.0));
    c -= b;
    CHECK(c == a);

    // Empty
    CHECK(Vector3().Empty());
    CHECK_FALSE(a.Empty());
}

TEST_CASE("matrix3x4_t layout and access")
{
    matrix3x4_t m(
        1.0, 2.0, 3.0, 4.0,
        5.0, 6.0, 7.0, 8.0,
        9.0, 10.0, 11.0, 12.0);

    // element access via operator[]
    CHECK(m[0][0] == doctest::Approx(1.0));
    CHECK(m[1][3] == doctest::Approx(8.0));
    CHECK(m[2][2] == doctest::Approx(11.0));

    // Base() is a flat 12-double array, row-major
    const double* flat = m.Base();
    for (int i = 0; i < 12; ++i)
        CHECK(flat[i] == doctest::Approx(static_cast<double>(i + 1)));
}

TEST_CASE("RAD2DEG / DEG2RAD are inverse")
{
    for (double deg : { 0.0, 45.0, 90.0, 180.0, -180.0, 360.0 }) {
        CHECK(RAD2DEG(DEG2RAD(deg)) == doctest::Approx(deg));
        CHECK(DEG2RAD(RAD2DEG(deg)) == doctest::Approx(deg));
    }

    CHECK(DEG2RAD(180.0) == doctest::Approx(3.14159265358979323846));
    CHECK(RAD2DEG(3.14159265358979323846 / 2.0) == doctest::Approx(90.0));
}

TEST_CASE("Vector3 Edge cases")
{
    // dividing by zero produces inf, but must not crash — intentional, so
    // silence C4723 (potential divide by 0) for this block only.
#pragma warning(push)
#pragma warning(disable : 4723)
    Vector3 v(1.0, 2.0, 3.0);
    // NOTE: cannot name a variable `small` — rpcndr.h (via Windows.h) defines
    // `small` as `char`. Use `zeroVec`.
    Vector3 zeroVec(0.0, 0.0, 0.0);
    Vector3 r = v / 0.0;
    CHECK(std::isinf(r.x));
    CHECK(std::isinf(r.y));
    CHECK(std::isinf(r.z));

    // operator/ on a zero vector
    Vector3 q = v / zeroVec.x;   // divide by 0.0
    CHECK(std::isinf(q.x));
#pragma warning(pop)
}

TEST_CASE("ProjectWorldLocationToScreen validates front-facing geometry")
{
    Engine::CameraCache cam{};
    cam.Location = Vector3(0.0, 0.0, 0.0);
    cam.Rotation = Vector3(0.0, 0.0, 0.0);
    cam.FOV = 90.0f;

    Vector3 screen{};
    CHECK(EngineProjection::ProjectWorldLocationToScreen(
        Vector3(100.0, 0.0, 0.0), screen,
        cam.Location, cam.Rotation, cam.FOV,
        1920.0, 1080.0));
    CHECK(screen.x == doctest::Approx(960.0));
    CHECK(screen.y == doctest::Approx(540.0));

    CHECK_FALSE(EngineProjection::ProjectWorldLocationToScreen(
        Vector3(-100.0, 0.0, 0.0), screen,
        cam.Location, cam.Rotation, cam.FOV,
        1920.0, 1080.0));

    // 45 deg yaw to the right: with vertical FOV 90 on a 16:9 viewport,
    // scale = 540/tan(45) = 540, so x = 960 + 100*540/100 = 1500.
    CHECK(EngineProjection::ProjectWorldLocationToScreen(
        Vector3(100.0, 100.0, 100.0), screen,
        cam.Location, cam.Rotation, cam.FOV,
        1920.0, 1080.0));
    CHECK(screen.x == doctest::Approx(1500.0));
    CHECK(screen.y == doctest::Approx(0.0));
}

TEST_CASE("ProjectWorldLocationToScreen rejects invalid camera state")
{
    Engine::CameraCache cam{};
    cam.Location = Vector3(0.0, 0.0, 0.0);
    cam.Rotation = Vector3(0.0, 0.0, 0.0);
    cam.FOV = 0.0f;

    Vector3 screen{};
    CHECK_FALSE(EngineProjection::ProjectWorldLocationToScreen(
        Vector3(100.0, 0.0, 0.0), screen,
        cam.Location, cam.Rotation, cam.FOV,
        1920.0, 1080.0));

    cam.FOV = 180.0f;
    CHECK_FALSE(EngineProjection::ProjectWorldLocationToScreen(
        Vector3(100.0, 0.0, 0.0), screen,
        cam.Location, cam.Rotation, cam.FOV,
        1920.0, 1080.0));
}
