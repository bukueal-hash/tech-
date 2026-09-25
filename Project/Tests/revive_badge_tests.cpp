// Revive badge suite - DBNO countdown formatting + timer plausibility gates.
//
// The defib/bleedout timers arrive as raw float pairs over DMA. These tests
// pin that a bad pair produces "no countdown" instead of a negative or absurd
// ring, and that the mm:ss text never leaks a garbage value.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/ReviveBadge.hpp"

TEST_CASE("RemainSeconds accepts a sane (elapsed, total) pair")
{
    CHECK(ReviveBadge::RemainSeconds(2.f, 8.f) == doctest::Approx(6.f));
    CHECK(ReviveBadge::RemainSeconds(0.f, 30.f) == doctest::Approx(30.f));
    CHECK(ReviveBadge::RemainSeconds(7.5f, 8.f) == doctest::Approx(0.5f));
}

TEST_CASE("RemainSeconds rejects implausible pairs")
{
    CHECK(ReviveBadge::RemainSeconds(0.f, 0.f) == -1.f);      // zeroed slot
    CHECK(ReviveBadge::RemainSeconds(1.f, -4.f) == -1.f);     // negative total
    CHECK(ReviveBadge::RemainSeconds(0.f, 9999.f) == -1.f);   // absurd duration
    CHECK(ReviveBadge::RemainSeconds(9.f, 2.f) == -1.f);      // elapsed past total
    CHECK(ReviveBadge::RemainSeconds(-3.f, 8.f) == -1.f);     // negative elapsed
}

TEST_CASE("RingFraction clamps to 0..1")
{
    CHECK(ReviveBadge::RingFraction(4.f, 8.f) == doctest::Approx(0.5f));
    CHECK(ReviveBadge::RingFraction(0.f, 8.f) == doctest::Approx(0.f));
    CHECK(ReviveBadge::RingFraction(9.f, 8.f) == doctest::Approx(1.f));
    CHECK(ReviveBadge::RingFraction(4.f, 0.f) == doctest::Approx(0.f));
}

TEST_CASE("FormatCountdown renders mm:ss and hides unknowns")
{
    CHECK(ReviveBadge::FormatCountdown(7.2f) == "0:07");
    CHECK(ReviveBadge::FormatCountdown(65.f) == "1:05");
    CHECK(ReviveBadge::FormatCountdown(0.f) == "0:00");
    CHECK(ReviveBadge::FormatCountdown(-1.f) == "");
}
