#pragma once
#include "WorldItemCategory.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <ostream>
#include <sstream>
#include <string>

namespace EntityFilters {
struct Type { const char* name; const char* needle; const char* alias; };
inline constexpr Type kArcs[] = {
    {"Wasp", "wasp", "light drone"}, {"Hornet", "hornet", "elite"},
    {"Tick", "tick", ""}, {"Turret", "turret", ""}, {"Bastion", "bastion", ""},
    {"Rocketeer", "rocketeer", "heavy drone"}, {"Fireball", "fireball", ""},
    {"Pop", "pop", ""}, {"Leaper", "leaper", ""}, {"Spotter", "spotter", ""},
    {"Bombardier", "bombardier", ""}, {"Surveyor", "surveyor", "rollbot"},
    {"Snitch", "snitch", ""}, {"Shredder", "shredder", ""},
    {"Queen", "queen", ""}, {"Sentinel", "sentinel", ""}, {"Matriarch", "matriarch", ""},
    {"Firefly", "firefly", "blaze hornet"}, {"Comet", "comet", ""},
    {"Vaporizer", "vaporizer", ""}, {"Assessor", "assessor", ""},
    {"Turbine", "turbine", ""}, {"Harvester", "harvester", ""}, {"Monolith", "monolith", ""},
    {"Husk", "husk", ""}, {"Swarmer", "swarmer", ""}, {"Other / Unknown", "", ""}
};
inline constexpr Type kResources[] = {
    {"Agave", "agave", ""}, {"Great Mullein", "mullein", ""}, {"Lemon", "lemon", ""},
    {"Olive", "olive", ""}, {"Apricot", "apricot", ""}, {"Mushroom", "mushroom", ""},
    {"Moss Rock", "moss", ""}, {"Fertilizer", "fertilizer", "fertiliser"},
    {"Other / Unknown", "", ""}
};
inline constexpr Type kGrenades[] = {
    {"Smoke", "smoke", ""}, {"Gas", "gas", ""}, {"EMP", "emp", ""},
    {"Shock", "shock", ""}, {"Seeker", "seeker", ""}, {"Light Impact", "lightimpact", "light impact"},
    {"Heavy Fuze", "heavyfuze", "heavy fuze"}, {"Lure", "lure", ""},
    {"Trigger", "trigger", ""}, {"Snap Blast", "snapblast", "snap blast"}, {"Other / Unknown", "", ""}
};
inline constexpr Type kMines[] = {
    {"Explosive", "explosive", ""}, {"Shock", "shock", ""}, {"Gas", "gas", ""},
    {"Other / Unknown", "", ""}
};
inline constexpr Type kTraps[] = {
    {"Barricade", "barricade", ""}, {"Snitch Scanner", "snitchscanner", "snitch scanner"},
    {"Door Blocker", "doorblocker", "door blocker"}, {"Other / Unknown", "", ""}
};
inline constexpr Type kExtractions[] = {
    {"Elevator", "elevator", ""}, {"Fan", "fan", ""}, {"Gaspipe", "gaspipe", "gas pipe"},
    {"Hatch", "hatch", ""}, {"Major", "major", ""}, {"Metro", "metro", ""},
    {"Minor", "minor", ""}, {"Other / Unknown", "", ""}
};
inline uint64_t arcMask = UINT64_MAX, resourceMask = UINT64_MAX, grenadeMask = UINT64_MAX;
inline uint64_t mineMask = UINT64_MAX, trapMask = UINT64_MAX, extractionMask = UINT64_MAX;
inline std::string Lower(std::string text)
{
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
inline bool Contains(const std::string& lower, const char* needle)
{
    return needle[0] && lower.find(needle) != std::string::npos;
}
template<size_t N> inline bool Selected(uint64_t mask, const Type (&types)[N], const std::string& name)
{
    static_assert(N <= 64);
    const std::string lower = Lower(name);
    // Prefer the most specific identity: "Blaze Hornet" is Firefly, not Hornet.
    size_t selected = N - 1;
    size_t specificity = 0;
    for (size_t i = 0; i + 1 < N; ++i) {
        for (const char* needle : {types[i].needle, types[i].alias}) {
            const size_t length = std::char_traits<char>::length(needle);
            if (length > specificity && Contains(lower, needle)) {
                selected = i;
                specificity = length;
            }
        }
    }
    return (mask & (uint64_t{1} << selected)) != 0;
}
inline bool Arc(const std::string& label) { return Selected(arcMask, kArcs, label); }
inline bool World(uint8_t category, const std::string& actor, const std::string& display)
{
    const auto cat = static_cast<WorldItemCategory>(category);
    const std::string text = actor + " " + display;
    if (cat == WorldItemCategory::Harvestable) return Selected(resourceMask, kResources, text);
    if (cat == WorldItemCategory::Hatch) return Selected(extractionMask, kExtractions, text);
    if (cat == WorldItemCategory::Grenade) {
        const std::string lower = Lower(text);
        if (Contains(lower, "mine")) return Selected(mineMask, kMines, text);
        if (Contains(lower, "trap") || Contains(lower, "barricade") || Contains(lower, "doorblocker") || Contains(lower, "snitchscanner") || Contains(lower, "door blocker") || Contains(lower, "snitch scanner"))
            return Selected(trapMask, kTraps, text);
        return Selected(grenadeMask, kGrenades, text);
    }
    return true;
}
struct MaskKey { const char* key; uint64_t* value; };
inline MaskKey kKeys[] = {
    {"filter_arc_types", &arcMask}, {"filter_resource_types", &resourceMask},
    {"filter_grenade_types", &grenadeMask}, {"filter_mine_types", &mineMask},
    {"filter_trap_types", &trapMask}, {"filter_extraction_types", &extractionMask}
};
inline void Save(std::ostream& out)
{
    for (const auto& k : kKeys) out << k.key << '=' << *k.value << '\n';
}
inline std::string Capture() { std::ostringstream out; Save(out); return out.str(); }
inline bool Load(const std::string& key, const std::string& value)
{
    for (const auto& k : kKeys) if (key == k.key) {
        if (value.empty() || value[0] == '-') return true;
        char* end = nullptr;
        errno = 0;
        const unsigned long long mask = std::strtoull(value.c_str(), &end, 10);
        if (errno != ERANGE && end != value.c_str() && *end == '\0')
            *k.value = static_cast<uint64_t>(mask);
        return true;
    }
    return false;
}
} // namespace EntityFilters
