// Part damage suite - PART_HP_ARRAY fraction classification + summaries.
//
// An unreadable hp slot must never read as damage ("which bot leg is blown off"
// has to mean a real leg), so -1 stays its own class all the way to the draw.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/PartDamage.hpp"

TEST_CASE("Classify buckets hp fractions")
{
    CHECK(PartDamage::Classify(-1.f) == PartDamage::Pip::Unread);
    CHECK(PartDamage::Classify(0.f) == PartDamage::Pip::Dead);
    CHECK(PartDamage::Classify(0.2f) == PartDamage::Pip::Critical);
    CHECK(PartDamage::Classify(0.5f) == PartDamage::Pip::Hurt);
    CHECK(PartDamage::Classify(0.95f) == PartDamage::Pip::Ok);
    CHECK(PartDamage::Classify(1.f) == PartDamage::Pip::Ok);
}

TEST_CASE("CountBelow skips unreadable slots")
{
    const float hp[] = { 1.f, 0.4f, -1.f, 0.f, 0.94f };
    CHECK(PartDamage::CountBelow(hp, 5, 0.95f) == 3);   // 0.4, 0.0, 0.94
    CHECK(PartDamage::CountBelow(hp, 5, 0.4f) == 1);    // 0.0 only
    CHECK(PartDamage::CountBelow(nullptr, 5, 0.95f) == 0);
    CHECK(PartDamage::CountBelow(hp, 0, 0.95f) == 0);
}

TEST_CASE("SummaryText hides when nothing was read")
{
    CHECK(PartDamage::SummaryText(2, 8) == "parts 2/8 down");
    CHECK(PartDamage::SummaryText(0, 3) == "parts 0/3 down");
    CHECK(PartDamage::SummaryText(0, 0) == "");
    CHECK(PartDamage::SummaryText(4, -1) == "");
}
