// Raid clock suite - clock formatting + phase/count plausibility.
//
// The raid clock is a raw double over DMA; these tests pin that a bad read
// renders as "--:--" and that mm:ss never leaks garbage (no "61s", no "-1").

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/RaidClock.hpp"

TEST_CASE("ClockPlausible bounds the raid clock")
{
    CHECK(RaidClock::ClockPlausible(0.0));
    CHECK(RaidClock::ClockPlausible(1234.5));
    CHECK_FALSE(RaidClock::ClockPlausible(-0.5));      // unread/garbage
    CHECK_FALSE(RaidClock::ClockPlausible(999999.0));  // absurd
}

TEST_CASE("FormatClock renders mm:ss and h:mm:ss")
{
    CHECK(RaidClock::FormatClock(0.0) == "0:00");
    CHECK(RaidClock::FormatClock(74.6) == "1:15");
    CHECK(RaidClock::FormatClock(59.0) == "0:59");
    CHECK(RaidClock::FormatClock(3725.0) == "1:02:05");
    CHECK(RaidClock::FormatClock(-1.0) == "--:--");
}

TEST_CASE("PhaseText stays honest about unknown enums")
{
    CHECK(RaidClock::PhaseText(2) == "Phase 2");
    CHECK(RaidClock::PhaseText(0) == "Phase 0");
    CHECK(RaidClock::PhaseText(17) == "");
    CHECK(RaidClock::PhaseText(-1) == "");
}
