// Radar projection suite - minimap-bounds calibration + north-up projection.
//
// "Map-accurate" means the radar is geographically correct: the whole-map fit
// must land on the true map axes (north up, west left) and a garbage bounds
// block must never calibrate the scale.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/RadarProjection.hpp"

TEST_CASE("BoundsPlausible accepts raid-map extents only")
{
    const RadarProjection::Bounds map{ 0.0, 0.0, 100000.0, 100000.0 };  // 1km map
    CHECK(RadarProjection::BoundsPlausible(map));
    const RadarProjection::Bounds zeroed{};
    CHECK_FALSE(RadarProjection::BoundsPlausible(zeroed));
    const RadarProjection::Bounds tiny{ 0.0, 0.0, 100.0, 100.0 };
    CHECK_FALSE(RadarProjection::BoundsPlausible(tiny));
    const RadarProjection::Bounds inverted{ 100000.0, 100000.0, 0.0, 0.0 };
    CHECK_FALSE(RadarProjection::BoundsPlausible(inverted));
}

TEST_CASE("MapFitted maps the true map axes with north up")
{
    const RadarProjection::Bounds map{ 0.0, 0.0, 10000.0, 10000.0 };
    float x = 99.f, y = 99.f;
    // center of the map -> center of the radar
    CHECK(RadarProjection::MapFitted(5000.0, 5000.0, map, 100.f, x, y));
    CHECK(x == doctest::Approx(0.f));
    CHECK(y == doctest::Approx(0.f));
    // north-east corner -> right AND up (north is up)
    CHECK(RadarProjection::MapFitted(10000.0, 10000.0, map, 100.f, x, y));
    CHECK(x == doctest::Approx(100.f));
    CHECK(y == doctest::Approx(-100.f));
    // south-west corner -> left and down
    CHECK(RadarProjection::MapFitted(0.0, 0.0, map, 100.f, x, y));
    CHECK(x == doctest::Approx(-100.f));
    CHECK(y == doctest::Approx(100.f));
    // outside the map -> no blip
    CHECK_FALSE(RadarProjection::MapFitted(12000.0, 5000.0, map, 100.f, x, y));
}

TEST_CASE("NorthUp fallback keeps metric scale and orientation")
{
    float x = 0.f, y = 0.f;
    // target 500m north of me in a 1000m range -> top edge of the radar
    CHECK(RadarProjection::NorthUp(
        0.0, 50000.0, 0.0, 0.0, 100000.0, 80.f, x, y));
    CHECK(x == doctest::Approx(0.f));
    CHECK(y == doctest::Approx(-40.f));
    // half the range east -> right edge half way out
    CHECK(RadarProjection::NorthUp(
        50000.0, 0.0, 0.0, 0.0, 100000.0, 80.f, x, y));
    CHECK(x == doctest::Approx(40.f));
    // beyond the range -> no blip
    CHECK_FALSE(RadarProjection::NorthUp(
        0.0, 150000.0, 0.0, 0.0, 100000.0, 80.f, x, y));
    // degenerate range/px never divides by zero
    CHECK_FALSE(RadarProjection::NorthUp(
        0.0, 100.0, 0.0, 0.0, 0.0, 80.f, x, y));
    CHECK_FALSE(RadarProjection::NorthUp(
        0.0, 100.0, 0.0, 0.0, 100000.0, 0.f, x, y));
}

TEST_CASE("IsUnderground flags blips well below the local floor")
{
    CHECK(RadarProjection::IsUnderground(-1000.0));
    CHECK_FALSE(RadarProjection::IsUnderground(0.0));
    CHECK_FALSE(RadarProjection::IsUnderground(-100.0));   // slope noise
    CHECK_FALSE(RadarProjection::IsUnderground(0.0 / 1.0 - 0.0 + 0.0));  // finite zero
}
