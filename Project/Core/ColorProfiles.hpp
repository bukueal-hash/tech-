#pragma once
// Color profile document (D19). Pure and header-only, following the
// ItemMetaParser / PoiParser split: Project.Tests can include this without the
// Variables, ImGui, DMA or Engine dependencies of the code that applies it.
//
// File shape — an array of up to 8 slots:
//   [
//     {"name":"Warmup","colors":{"esp_color_visible":[1,0.4,0.4,1], ...}},
//     ...
//   ]
//
// Semantics:
//   • never throws — malformed JSON yields an empty document
//   • a slot without a usable name still loads (unnamed slot)
//   • a color that is not an array of 4 finite numbers is skipped, not fatal
//   • values are clamped to 0..1 so a hand-edited 5.0 cannot escape ImGui's
//     expected range

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace ColorProfiles {

inline constexpr int kSlotCount = 8;
inline constexpr int kRgba = 4;

using Rgba = std::array<float, kRgba>;

struct Slot {
    std::string name;
    std::map<std::string, Rgba> colors;  // config key -> rgba
};

using Doc = std::vector<Slot>;

namespace detail {

inline float Clamp01(double v)
{
    if (!(v > 0.0))   // also catches NaN
        return 0.0f;
    if (v > 1.0)
        return 1.0f;
    return static_cast<float>(v);
}

// A stored color must be an array of 4 finite numbers; anything else is
// treated as absent rather than poisoning the slot.
inline bool ToRgba(const nlohmann::json& v, Rgba& out)
{
    if (!v.is_array() || v.size() != kRgba)
        return false;
    for (int i = 0; i < kRgba; ++i) {
        if (!v[i].is_number())
            return false;
        const double d = v[i].get<double>();
        if (!std::isfinite(d))
            return false;
        out[static_cast<size_t>(i)] = Clamp01(d);
    }
    return true;
}

} // namespace detail

// Parse a full color_profiles document. Never throws: malformed JSON, a
// non-array document, over-long color lists or wrong-typed fields all degrade
// to whatever valid content survived.
inline Doc Parse(const std::string& text)
{
    Doc doc;
    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_array())
            return doc;

        for (const auto& slotJson : root) {
            // Array position is the slot identity, including invalid/empty rows.
            if (!slotJson.is_object()) {
                doc.emplace_back();
                if (doc.size() >= static_cast<size_t>(kSlotCount))
                    break;
                continue;
            }

            Slot slot;

            const auto nameIt = slotJson.find("name");
            if (nameIt != slotJson.end() && nameIt->is_string())
                slot.name = nameIt->get<std::string>();

            const auto colorsIt = slotJson.find("colors");
            if (colorsIt != slotJson.end() && colorsIt->is_object()) {
                for (auto it = colorsIt->begin(); it != colorsIt->end(); ++it) {
                    if (it.key().empty())
                        continue;
                    Rgba rgba{};
                    if (detail::ToRgba(it.value(), rgba))
                        slot.colors.emplace(it.key(), rgba);
                }
            }

            // Preserve empty placeholders so clearing Slot 2 cannot move Slot 3.
            doc.push_back(std::move(slot));
            if (doc.size() >= static_cast<size_t>(kSlotCount))
                break;
        }
    } catch (...) {
        // malformed JSON — no slots
    }
    return doc;
}

// Serialize back to pretty-printed JSON. Returns an empty string when nlohmann
// refuses (e.g. invalid UTF-8 in a slot name) so the caller can report a failed
// save instead of writing a truncated file.
inline std::string Serialize(const Doc& doc)
{
    try {
        nlohmann::json root = nlohmann::json::array();
        const size_t n = doc.size() < static_cast<size_t>(kSlotCount)
            ? doc.size() : static_cast<size_t>(kSlotCount);
        for (size_t i = 0; i < n; ++i) {
            nlohmann::json slot = nlohmann::json::object();
            slot["name"] = doc[i].name;
            nlohmann::json colors = nlohmann::json::object();
            for (const auto& kv : doc[i].colors) {
                nlohmann::json arr = nlohmann::json::array();
                for (int c = 0; c < kRgba; ++c)
                    arr.push_back(detail::Clamp01(kv.second[static_cast<size_t>(c)]));
                colors[kv.first] = std::move(arr);
            }
            slot["colors"] = std::move(colors);
            root.push_back(std::move(slot));
        }
        return root.dump(2);
    } catch (...) {
        return {};
    }
}

} // namespace ColorProfiles
