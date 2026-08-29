// items_meta.json parser suite — Pillar 1 (docs/aplus-plan.md)
// Exercises the REAL code from Core/ItemMetaParser.hpp, which AssetNames.cpp
// LoadItemsMetaJson now calls. Golden rows + malformed-input fuzz that must
// not crash.

#include "tests_main.hpp"
#include "Core/ItemMetaParser.hpp"

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

#include <random>
#include <string>
#include <vector>

namespace {

std::string goldenDoc()
{
    // Mirrors the shape of Project/Data/items_meta.json rows.
    return R"([
        {"name":"Iron Bar",   "rarity":"Common",   "value":12,  "id":"ITEM_IRON", "asset":"DA_Item_IronBar"},
        {"name":"Med Kit",    "rarity":"legendary","value":999, "id":"ITEM_MED",  "asset":"DA_Item_MedKit"},
        {"name":"  Bandage  ","rarity":" Rare ",   "value":3},
        {"name":"NoRarity",   "value":1},
        {"name":""},
        {"name":"NoValue",    "rarity":"Epic"},
        {"name":"Huge",       "rarity":"Common",   "value":1e300},
        {"name":"StrVal",     "rarity":"Common",   "value":"12"},
        {"name":"NegVal",     "rarity":"Uncommon", "value":-5},
        42,
        "string",
        null,
        {"rarity":"Common", "value":7},
        {"name":"Extra",      "rarity":"Common",   "value":7, "id":"", "asset":""}
    ])";
}

} // namespace

TEST_CASE("RarityToTier mapping")
{
    CHECK(ItemMetaParser::RarityToTier("common") == 1);
    CHECK(ItemMetaParser::RarityToTier("COMMON") == 1);
    CHECK(ItemMetaParser::RarityToTier(" Common ") == 1);
    CHECK(ItemMetaParser::RarityToTier("uncommon") == 2);
    CHECK(ItemMetaParser::RarityToTier("rare") == 3);
    CHECK(ItemMetaParser::RarityToTier("epic") == 4);
    CHECK(ItemMetaParser::RarityToTier("legendary") == 5);
    CHECK(ItemMetaParser::RarityToTier("") == 0);
    CHECK(ItemMetaParser::RarityToTier("mythic") == 0);
    CHECK(ItemMetaParser::RarityToTier("LEGENDARY!") == 0);
}

TEST_CASE("ParseItemsMetaDoc golden document")
{
    const auto rows = ItemMetaParser::ParseItemsMetaDoc(goldenDoc());
    REQUIRE(rows.size() == 8);

    // Iron Bar
    CHECK(rows[0].name == "Iron Bar");
    CHECK(rows[0].tier == 1);
    CHECK(rows[0].value == 12);
    CHECK(rows[0].id == "ITEM_IRON");
    CHECK(rows[0].asset == "DA_Item_IronBar");

    // Med Kit — rarity is case-insensitive, value parsed
    CHECK(rows[1].name == "Med Kit");
    CHECK(rows[1].tier == 5);
    CHECK(rows[1].value == 999);

    // Bandage — name is NOT trimmed (mirrors original), rarity IS trimmed
    CHECK(rows[2].name == "  Bandage  ");
    CHECK(rows[2].tier == 3);
    CHECK(rows[2].value == 3);

    // NoValue — missing "value" -> 0
    CHECK(rows[3].name == "NoValue");
    CHECK(rows[3].tier == 4);
    CHECK(rows[3].value == 0);

    // Huge — 1e300 as int would throw in the old code; now degrades to 0
    CHECK(rows[4].name == "Huge");
    CHECK(rows[4].tier == 1);
    CHECK(rows[4].value == 0);

    // StrVal — "value" is a string, not a number -> 0
    CHECK(rows[5].name == "StrVal");
    CHECK(rows[5].tier == 1);
    CHECK(rows[5].value == 0);

    // NegVal — negative ints are kept
    CHECK(rows[6].name == "NegVal");
    CHECK(rows[6].tier == 2);
    CHECK(rows[6].value == -5);

    // Extra — empty id/asset are fine
    CHECK(rows[7].name == "Extra");
    CHECK(rows[7].tier == 1);
    CHECK(rows[7].value == 7);
}

TEST_CASE("ParseItemsMetaDoc rejects malformed docs without crashing")
{
    // Non-array JSON docs -> empty.
    CHECK(ItemMetaParser::ParseItemsMetaDoc("{}").empty());
    CHECK(ItemMetaParser::ParseItemsMetaDoc("\"hello\"").empty());
    CHECK(ItemMetaParser::ParseItemsMetaDoc("42").empty());
    CHECK(ItemMetaParser::ParseItemsMetaDoc("null").empty());

    // Malformed JSON -> empty, no throw.
    CHECK(ItemMetaParser::ParseItemsMetaDoc("{").empty());
    CHECK(ItemMetaParser::ParseItemsMetaDoc("").empty());
    CHECK(ItemMetaParser::ParseItemsMetaDoc("[1,2,").empty());
    CHECK(ItemMetaParser::ParseItemsMetaDoc("{{{{").empty());
    CHECK(ItemMetaParser::ParseItemsMetaDoc("not json at all").empty());
}

TEST_CASE("ParseRow direct")
{
    // nullopt for non-objects.
    CHECK_FALSE(ItemMetaParser::ParseRow(nlohmann::json(nullptr)).has_value());
    CHECK_FALSE(ItemMetaParser::ParseRow(nlohmann::json(42)).has_value());
    CHECK_FALSE(ItemMetaParser::ParseRow(nlohmann::json::array()).has_value());
    CHECK_FALSE(ItemMetaParser::ParseRow(nlohmann::json("x")).has_value());

    // Empty object / missing name.
    CHECK_FALSE(ItemMetaParser::ParseRow(nlohmann::json::object()).has_value());
    CHECK_FALSE(ItemMetaParser::ParseRow(
        nlohmann::json{ { "rarity", "Common" } }).has_value());

    // Unknown rarity.
    CHECK_FALSE(ItemMetaParser::ParseRow(
        nlohmann::json{ { "name", "X" }, { "rarity", "mythic" } }).has_value());

    // "name" as a non-string -> treated as absent -> skipped (no throw;
    // row.value() itself would throw type_error 302 here).
    CHECK_FALSE(ItemMetaParser::ParseRow(
        nlohmann::json{ { "name", 123 } }).has_value());
}

TEST_CASE("Fuzz: random byte garbage must not crash")
{
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> lenDist(0, 512);
    std::uniform_int_distribution<int> byteDist(0, 255);

    for (int iter = 0; iter < 400; ++iter) {
        const int n = lenDist(rng);
        std::string blob;
        blob.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            blob.push_back(static_cast<char>(byteDist(rng)));
        // Must not crash or throw; result size is unconstrained.
        (void)ItemMetaParser::ParseItemsMetaDoc(blob);
    }
}

TEST_CASE("Fuzz: structured random JSON must not crash")
{
    std::mt19937 rng(54321);

    std::function<nlohmann::json(int)> randomJson = [&](int depth) -> nlohmann::json {
        std::uniform_int_distribution<int> kind(0, 6);
        switch (kind(rng)) {
        case 0: return nullptr;
        case 1: return rng() % 2 == 0;
        case 2: return static_cast<int>(rng() % 2000) - 1000;
        case 3: return static_cast<double>(rng() % 10000) / 7.0;
        case 4: {
            std::string s;
            const int n = static_cast<int>(rng() % 16);
            for (int i = 0; i < n; ++i)
                s.push_back(static_cast<char>(33 + rng() % 90));
            return s;
        }
        case 5: {
            if (depth <= 0)
                return nullptr;
            nlohmann::json arr = nlohmann::json::array();
            const int n = static_cast<int>(rng() % 6);
            for (int i = 0; i < n; ++i)
                arr.push_back(randomJson(depth - 1));
            return arr;
        }
        default: {
            if (depth <= 0)
                return nullptr;
            nlohmann::json obj = nlohmann::json::object();
            const int n = static_cast<int>(rng() % 6);
            for (int i = 0; i < n; ++i)
                obj["k" + std::to_string(i)] = randomJson(depth - 1);
            return obj;
        }
        }
    };

    for (int iter = 0; iter < 300; ++iter) {
        const nlohmann::json doc = randomJson(4);
        const std::string text = doc.dump();
        const auto rows = ItemMetaParser::ParseItemsMetaDoc(text);
        // Any parsed row must be internally consistent.
        for (const auto& r : rows) {
            CHECK_FALSE(r.name.empty());
            CHECK(r.tier >= 1);
            CHECK(r.tier <= 5);
        }
    }
}