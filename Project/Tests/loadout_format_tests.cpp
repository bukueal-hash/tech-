// Loadout format suite - pure label formatting for the loadout readout.
//
// The ESP loadout line is assembled from DMA-resolved values that can be
// partially unknown (-1 tier, 0 plates, 0 clip). These tests pin that a miss
// shortens the line instead of printing garbage like "Armor T" or "(0)".

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/LoadoutFormat.hpp"

TEST_CASE("RomanTier maps 1-4 and rejects the rest")
{
    CHECK(LoadoutFormat::RomanTier(1) == "I");
    CHECK(LoadoutFormat::RomanTier(2) == "II");
    CHECK(LoadoutFormat::RomanTier(3) == "III");
    CHECK(LoadoutFormat::RomanTier(4) == "IV");
    CHECK(LoadoutFormat::RomanTier(0) == "");
    CHECK(LoadoutFormat::RomanTier(-1) == "");
    CHECK(LoadoutFormat::RomanTier(5) == "");
}

TEST_CASE("ClipText only formats real clips")
{
    CHECK(LoadoutFormat::ClipText(24) == "(24)");
    CHECK(LoadoutFormat::ClipText(1) == "(1)");
    CHECK(LoadoutFormat::ClipText(0) == "");
    CHECK(LoadoutFormat::ClipText(-3) == "");
}

TEST_CASE("ArmorText stays empty on full unknowns")
{
    CHECK(LoadoutFormat::ArmorText(-1, 0.f) == "");
    CHECK(LoadoutFormat::ArmorText(0, 0.f) == "");
}

TEST_CASE("ArmorText composes the segments it has")
{
    CHECK(LoadoutFormat::ArmorText(3, 0.f) == "Armor TIII");
    CHECK(LoadoutFormat::ArmorText(3, 2.f) == "Armor TIII - 2 plates");
    CHECK(LoadoutFormat::ArmorText(-1, 1.f) == "Armor - 1 plate");
    // rounding: partial plate values collapse to a whole count
    CHECK(LoadoutFormat::ArmorText(2, 2.4f) == "Armor TII - 2 plates");
}

TEST_CASE("RarityName/RarityTag cover the drop's EItemRarity values")
{
    CHECK(LoadoutFormat::RarityName(0) == "Common");
    CHECK(LoadoutFormat::RarityName(1) == "Rare");
    CHECK(LoadoutFormat::RarityName(2) == "Epic");
    CHECK(LoadoutFormat::RarityName(3) == "Legendary");
    CHECK(LoadoutFormat::RarityName(-1) == "");
    CHECK(LoadoutFormat::RarityName(4) == "");
    // common and unknown stay silent so labels keep their shape
    CHECK(LoadoutFormat::RarityTag(0) == "");
    CHECK(LoadoutFormat::RarityTag(1) == " (Rare)");
    CHECK(LoadoutFormat::RarityTag(3) == " (Legendary)");
    CHECK(LoadoutFormat::RarityTag(-1) == "");
}

TEST_CASE("SlotsText formats slot capacities and rejects bad reads")
{
    CHECK(LoadoutFormat::SlotsText("Belt", 3) == "Belt 3");
    CHECK(LoadoutFormat::SlotsText("Pack", 12) == "Pack 12");
    CHECK(LoadoutFormat::SlotsText("Belt", 0) == "");
    CHECK(LoadoutFormat::SlotsText("Belt", -2) == "");
    CHECK(LoadoutFormat::SlotsText("Pack", 65) == "");
}

TEST_CASE("KitText composes the segments it has and skips the rest")
{
    LoadoutFormat::KitParts k;
    CHECK(LoadoutFormat::KitText(k, 90).empty());

    k.tool = "Radar";
    CHECK(LoadoutFormat::KitText(k, 90) == "Tool: Radar");

    k.pouch = "Fuel Cell";
    k.pouchRarity = 1;
    k.beltSlots = 3;
    k.packSlots = 12;
    CHECK(LoadoutFormat::KitText(k, 90)
          == "Tool: Radar | Pouch: Fuel Cell (Rare) | Belt 3 | Pack 12");

    // stowed guns lead the line when present
    k.stowed0 = "Rattler";
    k.stowed1 = "Mule";
    CHECK(LoadoutFormat::KitText(k, 90)
          == "Kit: Rattler, Mule | Tool: Radar | Pouch: Fuel Cell (Rare) | Belt 3 | Pack 12");
}

TEST_CASE("KitText truncates long lines with '~'")
{
    LoadoutFormat::KitParts k;
    k.pouch = "Very Long Item Name That Keeps Going";
    const std::string line = LoadoutFormat::KitText(k, 16);
    CHECK(line.size() == 16);
    CHECK(line.back() == '~');
}
