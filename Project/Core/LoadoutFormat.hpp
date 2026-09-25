#pragma once
// Loadout readout formatting (ESP label extras for the loadout feature).
//
// Pure helpers only: the DMA side (Engine::ReadPlayerInventory) resolves the
// raw inventory slots (InventoryComponent / StowedWeaponLayout / WeaponActor)
// into names, tiers and counts, and these turn those into the compact label
// lines the ESP draws. A helper never invents data - an unknown value
// (-1 tier, 0 plates, 0 clip) leaves the line shorter, never wrong.

#include <string>
#include <vector>

namespace LoadoutFormat {

// Weapon/armor tier 1-4 -> I-IV roman (StowedWeaponInfo::WeaponQuality is
// 0-3 and the readers shift it to 1-4). Anything else returns "" so callers
// skip the segment entirely.
inline std::string RomanTier(int tier)
{
    switch (tier) {
    case 1: return "I";
    case 2: return "II";
    case 3: return "III";
    case 4: return "IV";
    default: return "";
    }
}

// "(24)" clip suffix for the weapon label. 0/negative = unknown = "".
inline std::string ClipText(int clip)
{
    if (clip <= 0)
        return "";
    return "(" + std::to_string(clip) + ")";
}

// Rarity name for ItemBase::QUALITY_LEVEL (the drop: 0=common,1=rare,2=epic,
// 3=legendary). Anything outside 0-3 is an unresolved read -> "".
inline std::string RarityName(int quality)
{
    switch (quality) {
    case 0: return "Common";
    case 1: return "Rare";
    case 2: return "Epic";
    case 3: return "Legendary";
    default: return "";
    }
}

// " (Rare)" suffix for item lines. Common and unknown stay silent so the
// label keeps its shape - rarity only decorates the interesting drops.
inline std::string RarityTag(int quality)
{
    if (quality < 1 || quality > 3)
        return "";
    return " (" + RarityName(quality) + ")";
}

// Full kit readout parts (phase 2 player kit). Every field is optional:
// unknown stays empty/-1 and its segment vanishes - the line never invents
// data. stowed0/1 are stowed-gun names (already tiered upstream by
// ReadPlayerInventory), tool is the stowed tool actor, pouch is the safe
// pouch item (ItemBase::QUALITY_LEVEL rarity), belt/pack are container slot
// capacities (UItemContainer::ItemLimit).
struct KitParts {
    std::string stowed0;
    std::string stowed1;
    std::string tool;
    std::string pouch;
    int pouchRarity = -1;
    int beltSlots = -1;
    int packSlots = -1;
};

// "Belt 3" - container slot capacity. Out-of-range reads are unresolved.
inline std::string SlotsText(const char* label, int slots)
{
    if (slots <= 0 || slots > 64)
        return "";
    return std::string(label) + " " + std::to_string(slots);
}

// "Kit: Rattler, Mule | Tool: Radar | Pouch: Fuel Cell (Rare) | Belt 3 |
// Pack 12" - segments joined with " | ", empty segments skipped, hard-capped
// at maxChars with '~' (same convention as CrateContents::JoinSummary).
inline std::string KitText(const KitParts& k, int maxChars)
{
    std::vector<std::string> segs;

    std::string guns;
    if (!k.stowed0.empty())
        guns = k.stowed0;
    if (!k.stowed1.empty()) {
        if (!guns.empty())
            guns += ", ";
        guns += k.stowed1;
    }
    if (!guns.empty())
        segs.push_back("Kit: " + guns);
    if (!k.tool.empty())
        segs.push_back("Tool: " + k.tool);
    if (!k.pouch.empty())
        segs.push_back("Pouch: " + k.pouch + RarityTag(k.pouchRarity));
    const std::string belt = SlotsText("Belt", k.beltSlots);
    if (!belt.empty())
        segs.push_back(belt);
    const std::string pack = SlotsText("Pack", k.packSlots);
    if (!pack.empty())
        segs.push_back(pack);

    std::string out;
    for (const std::string& seg : segs) {
        if (out.empty()) {
            out = seg;
            continue;
        }
        out += " | " + seg;
    }
    if (maxChars <= 0)
        return "";
    if (static_cast<int>(out.size()) <= maxChars)
        return out;
    return out.substr(0, static_cast<size_t>(maxChars - 1)) + "~";
}

// Armor line: "Armor TIII - 2 plates". Empty when neither the tier nor a
// plate count ever resolved.
inline std::string ArmorText(int tier, float plates)
{
    const std::string roman = RomanTier(tier);
    const int plateCount = plates > 0.f ? static_cast<int>(plates + 0.5f) : 0;
    if (roman.empty() && plateCount <= 0)
        return "";
    std::string out = "Armor";
    if (!roman.empty()) {
        out += " T";
        out += roman;
    }
    if (plateCount > 0) {
        out += " - ";
        out += std::to_string(plateCount);
        out += plateCount == 1 ? " plate" : " plates";
    }
    return out;
}

} // namespace LoadoutFormat
