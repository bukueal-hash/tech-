#pragma once
// Pure items_meta.json row parser, extracted from AssetNames.cpp
// (LoadItemsMetaJson) so Project.Tests can exercise it without the
// DMA/Engine dependency of AssetNames.cpp. Header-only.
//
// Semantics mirror LoadItemsMetaJson exactly (Pillar 1 extraction):
//   • row must be an object with a non-empty "name" and a known rarity
//   • "value" is optional; parsed only when it is a number
//   • never throws — malformed input yields an empty row list or nullopt

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

// NOTE: named ItemMetaParser (not ItemMeta) — AssetNames.cpp already has a
// `struct ItemMeta` in its anonymous namespace.
namespace ItemMetaParser {

struct Row {
    std::string name;    // "name" (display)
    std::string rarity;  // "rarity" (raw string; "" if absent)
    std::string id;      // "id"
    std::string asset;   // "asset"
    int value = 0;       // "value" (0 if absent / non-number / out of range)
    int tier = 0;        // RarityToTier(rarity); 0 when unknown
};

namespace detail {

inline std::string ToLowerCopy(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

inline std::string TrimCopy(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

} // namespace detail

// Rarity -> 1..5 (common..legendary), 0 for unknown. Same as AssetNames.cpp.
inline int RarityToTier(std::string rarityRaw)
{
    const std::string rarity = detail::ToLowerCopy(detail::TrimCopy(std::move(rarityRaw)));
    if (rarity == "common") return 1;
    if (rarity == "uncommon") return 2;
    if (rarity == "rare") return 3;
    if (rarity == "epic") return 4;
    if (rarity == "legendary") return 5;
    return 0;
}

// Parse one row object. Returns nullopt when the row is not a valid item row
// (non-object, empty name, unknown rarity). Never throws.
inline std::optional<Row> ParseRow(const nlohmann::json& row)
{
    if (!row.is_object())
        return std::nullopt;

    // NOTE: row.value(key, default) THROWS type_error 302 when the value is
    // present but not a string (it calls get<ValueType>()). The original
    // LoadItemsMetaJson let that escape — a malformed items_meta.json could
    // crash the app. Type-check explicitly instead; a non-string field is
    // treated as absent (string fields become empty).
    const auto str = [&row](const char* key) -> std::string {
        const auto it = row.find(key);
        if (it != row.end() && it->is_string())
            return it->get<std::string>();
        return {};
    };
    Row r;
    r.name = str("name");
    r.rarity = str("rarity");
    r.id = str("id");
    r.asset = str("asset");

    // "value" must be a number. nlohmann's get<int>() does not range-check:
    // 1e300 yields INT_MIN instead of throwing, and a huge int64 truncates —
    // clamp through a double so out-of-range values degrade to 0 (the
    // original could blow up on such input). In-range floats truncate as
    // before (12.5 -> 12).
    if (row.contains("value") && row["value"].is_number()) {
        const double d = row["value"].get<double>();
        if (d >= -2147483648.0 && d <= 2147483647.0)
            r.value = static_cast<int>(d);
    }

    if (r.name.empty())
        return std::nullopt;

    r.tier = RarityToTier(r.rarity);
    if (r.tier <= 0)
        return std::nullopt;

    return r;
}

// Parse a full items_meta.json document (array of rows). Never throws:
// malformed JSON, a non-array doc, or any parse error yields an empty list.
inline std::vector<Row> ParseItemsMetaDoc(const std::string& text)
{
    std::vector<Row> rows;
    try {
        const nlohmann::json doc = nlohmann::json::parse(text);
        if (!doc.is_array())
            return rows;
        rows.reserve(doc.size());
        for (const auto& row : doc) {
            if (auto r = ParseRow(row))
                rows.push_back(std::move(*r));
        }
    } catch (...) {
        // malformed JSON — no rows
    }
    return rows;
}

} // namespace ItemMetaParser