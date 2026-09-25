// Vision cone suite - fan geometry + sight plausibility gates.
//
// The cone polygon is world-space geometry projected to screen later; these
// tests pin the fan shape (apex + arc spanning the half angle around the aim
// direction) and that garbage sight values never pass the gate.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/VisionCone.hpp"

#include <string>

TEST_CASE("FanPoints writes apex + arc samples")
{
    VisionCone::Vec2 pts[10];
    const int n = VisionCone::FanPoints(500.f, 500.f, 0.f, 45.f, 1000.f, pts, 10);
    CHECK(n == 10);  // 1 apex + 2*4+1 arc samples
    CHECK(pts[0].x == doctest::Approx(500.f));
    CHECK(pts[0].y == doctest::Approx(500.f));
    // middle arc sample lies on the aim direction (yaw 0 = +X), at the radius
    const VisionCone::Vec2& mid = pts[n / 2];
    CHECK(mid.x == doctest::Approx(1500.f).epsilon(0.001));
    CHECK(mid.y == doctest::Approx(500.f).epsilon(0.001));
}

TEST_CASE("FanPoints arc endpoints span the half angle")
{
    VisionCone::Vec2 pts[5];
    const int n = VisionCone::FanPoints(0.f, 0.f, 90.f, 30.f, 100.f, pts, 5);
    CHECK(n == 5);
    // yaw 90 = +Y: first endpoint at yaw-halfrange=60deg, last at 120deg
    CHECK(pts[1].x == doctest::Approx(50.f).epsilon(0.001));
    CHECK(pts[1].y == doctest::Approx(86.6025f).epsilon(0.001));
    CHECK(pts[4].x == doctest::Approx(-50.f).epsilon(0.001));
    CHECK(pts[4].y == doctest::Approx(86.6025f).epsilon(0.001));
}

TEST_CASE("FanPoints rejects degenerate inputs")
{
    VisionCone::Vec2 pts[4];
    CHECK(VisionCone::FanPoints(0.f, 0.f, 0.f, 45.f, 0.f, pts, 4) == 0);
    CHECK(VisionCone::FanPoints(0.f, 0.f, 0.f, 45.f, 100.f, nullptr, 4) == 0);
    CHECK(VisionCone::FanPoints(0.f, 0.f, 0.f, 45.f, 100.f, pts, 2) == 0);
}

TEST_CASE("PlausibleSight gates raw DMA values")
{
    CHECK(VisionCone::PlausibleSight(1500.f, 45.f));
    CHECK_FALSE(VisionCone::PlausibleSight(0.f, 45.f));       // unread
    CHECK_FALSE(VisionCone::PlausibleSight(1500.f, 0.f));     // no angle
    CHECK_FALSE(VisionCone::PlausibleSight(999999.f, 45.f));  // absurd radius
    CHECK_FALSE(VisionCone::PlausibleSight(1500.f, 200.f));   // absurd angle
}

TEST_CASE("AlertnessTag names the AIStateService states")
{
    CHECK(std::string(VisionCone::AlertnessTag(0)) == "Idle");
    CHECK(std::string(VisionCone::AlertnessTag(3)) == "Combat");
    CHECK(std::string(VisionCone::AlertnessTag(9)) == "");
}
