// Look arrow suite - arrow geometry + ControlRotation sanity gates.
//
// The arrow must point the same way the radar's ally arrows do (0 = camera
// forward), and a free-cam rotation outside the view clamps must be rejected
// instead of spinning the arrow.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/LookArrow.hpp"

TEST_CASE("NormalizeDeg folds into (-180, 180]")
{
    CHECK(LookArrow::NormalizeDeg(0.0) == doctest::Approx(0.0));
    CHECK(LookArrow::NormalizeDeg(270.0) == doctest::Approx(-90.0));
    CHECK(LookArrow::NormalizeDeg(-270.0) == doctest::Approx(90.0));
    CHECK(LookArrow::NormalizeDeg(540.0) == doctest::Approx(180.0));
}

TEST_CASE("ArrowAngleRad is the aim yaw relative to the camera")
{
    // aiming where the camera looks -> 0 (arrow up)
    CHECK(LookArrow::ArrowAngleRad(90.0, 90.0) == doctest::Approx(0.0));
    // aiming 90 deg right of the camera -> +pi/2 (clockwise = right)
    CHECK(LookArrow::ArrowAngleRad(180.0, 90.0) == doctest::Approx(LookArrow::kPi / 2));
    // wrap-around: -170 vs 170 is 20 deg apart, not 340
    CHECK(LookArrow::ArrowAngleRad(-170.0, 170.0) == doctest::Approx(20.0 * LookArrow::kPi / 180.0));
}

TEST_CASE("ArrowPoints: angle 0 points up (screen y decreases)")
{
    LookArrow::Point tip, left, right;
    LookArrow::ArrowPoints(100.f, 100.f, 0.0, 8.f, tip, left, right);
    CHECK(tip.x == doctest::Approx(100.f));
    CHECK(tip.y == doctest::Approx(92.f));
    // base sits behind the center
    CHECK(left.y > tip.y);
    CHECK(right.y > tip.y);
    CHECK(left.x < right.x);
}

TEST_CASE("ArrowPoints: +90 deg points right (screen x increases)")
{
    LookArrow::Point tip, left, right;
    LookArrow::ArrowPoints(0.f, 0.f, LookArrow::kPi / 2, 8.f, tip, left, right);
    CHECK(tip.x == doctest::Approx(8.f));
    CHECK(tip.y == doctest::Approx(0.f));
}

TEST_CASE("AimPitchValid enforces the view clamps when known")
{
    // no usable clamps: only finiteness/range matters
    CHECK(LookArrow::AimPitchValid(30.0, 0.0, 0.0));
    CHECK_FALSE(LookArrow::AimPitchValid(300.0, 0.0, 0.0));
    CHECK_FALSE(LookArrow::AimPitchValid(NAN, 0.0, 0.0));  // NaN pitch
    // UE-style -90..90 clamps with slack
    CHECK(LookArrow::AimPitchValid(45.0, -90.0, 90.0));
    CHECK(LookArrow::AimPitchValid(-95.0, -90.0, 90.0));          // inside slack
    CHECK_FALSE(LookArrow::AimPitchValid(120.0, -90.0, 90.0));    // free-cam garbage
}
