// Crate contents preview suite (world ESP label extras).
//
// The ESP label carries a compact "[...]" summary of what is inside a
// container. These tests pin the pure formatting and the sanity gates that
// stand between raw DMA int32s (ItemUIHoverData::AMOUNT / MAX_STACK, and the
// drop-estimated Pickup::VISIBLE_AMOUNT) and the label: a bad read must
// render as "no summary", never as garbage amounts.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/CrateContents.hpp"

TEST_CASE("AmountPlausible gates raw stack reads")
{
    CHECK(CrateContents::AmountPlausible(1, 1));
    CHECK(CrateContents::AmountPlausible(3, 5));
    CHECK(CrateContents::AmountPlausible(0, 0));    // unknown max still passes
    CHECK_FALSE(CrateContents::AmountPlausible(-1, 5));
    CHECK_FALSE(CrateContents::AmountPlausible(6, 5));      // over the max stack
    CHECK_FALSE(CrateContents::AmountPlausible(1000, 0));   // implausible amount
    CHECK_FALSE(CrateContents::AmountPlausible(1, -1));
}

TEST_CASE("FormatStack renders the name with an xN suffix")
{
    CrateContents::Stack s;
    s.name = "Bandage";
    CHECK(CrateContents::FormatStack(s) == "Bandage");
    s.amount = 1;
    CHECK(CrateContents::FormatStack(s) == "Bandage");
    s.amount = 3;
    CHECK(CrateContents::FormatStack(s) == "Bandage x3");
    s.name.clear();
    CHECK(CrateContents::FormatStack(s).empty());
}

TEST_CASE("FormatStack tags resolved rarities only")
{
    CrateContents::Stack s;
    s.name = "Bandage";
    s.amount = 2;
    CHECK(CrateContents::FormatStack(s) == "Bandage x2");   // rarity -1 = silent
    s.rarity = 1;
    CHECK(CrateContents::FormatStack(s) == "Bandage x2 (Rare)");
    s.rarity = 0;
    CHECK(CrateContents::FormatStack(s) == "Bandage x2");   // common = silent
    s.rarity = 9;
    CHECK(CrateContents::FormatStack(s) == "Bandage x2");   // junk = silent
}

TEST_CASE("PortsTag covers real port counts only")
{
    CHECK(CrateContents::PortsTag(1) == " (1 port)");
    CHECK(CrateContents::PortsTag(2) == " (2 ports)");
    CHECK(CrateContents::PortsTag(0) == "");
    CHECK(CrateContents::PortsTag(-1) == "");
    CHECK(CrateContents::PortsTag(33) == "");
}

TEST_CASE("JoinSummary lists stacks and caps the label")
{
    CrateContents::Stack stacks[3];
    stacks[0].name = "Bandage"; stacks[0].amount = 2;
    stacks[1].name = "Med Kit"; stacks[1].amount = 1;
    stacks[2].name = "Ammo";    stacks[2].amount = 4;

    CHECK(CrateContents::JoinSummary(nullptr, 3, 56).empty());
    CHECK(CrateContents::JoinSummary(stacks, 0, 56).empty());
    CHECK(CrateContents::JoinSummary(stacks, 3, 56)
        == "Bandage x2, Med Kit, Ammo x4");

    // Past the cap: named stacks plus the count left unnamed.
    CHECK(CrateContents::JoinSummary(stacks, 3, 23)
        == "Bandage x2, Med Kit +1");

    // One oversized name is truncated, never wrapped or dropped silently.
    CrateContents::Stack huge;
    huge.name = "Some Extremely Long Item Name That Never Fits";
    CHECK(CrateContents::JoinSummary(&huge, 1, 12) == "Some Extrem~");
}
