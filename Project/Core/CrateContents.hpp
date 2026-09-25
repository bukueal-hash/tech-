#pragma once
#include <string>

#include "LoadoutFormat.hpp"

// Crate contents preview (world ESP label extras).
//
// Pure formatting + sanity gates - no DMA - so Tests/crate_contents_tests.cpp
// can pin the behavior. The DMA side lives in Engine::ReadCrateContents
// (Functions/Utils.cpp): it resolves both pickup SpawnItems and the owned
// UItemContainerComponent used by world crates, then fills Stack arrays that
// ride along on Engine::WorldCacheEntry copies into the ESP frame and render as
// the "[...]" summary on a container's label.
namespace CrateContents {

/** Fixed capacity so per-frame WorldCacheEntry copies never heap-allocate. */
inline constexpr int kMaxStacks = 6;

/** Sanity ceilings for raw int32 reads off unverified slots. */
inline constexpr int kMaxAmount = 999;
inline constexpr int kMaxStackCap = 999;

struct Stack {
    std::string name;
    int amount = 0;
    int maxStack = 0;
    // ItemBase::QUALITY_LEVEL (0-3) through the pickup resolution chain;
    // -1 = unresolved, which renders no tag at all.
    int rarity = -1;
};

/**
 * Sanity gate for raw int32 reads off unverified slots (ItemUIHoverData::
 * AMOUNT / MAX_STACK and Pickup::VISIBLE_AMOUNT - the drop marks the latter
 * estimated). A wrong offset must mean "no stack shown", never garbage text.
 */
inline bool AmountPlausible(int amount, int maxStack)
{
    if (amount < 0 || kMaxAmount < amount)
        return false;
    if (maxStack < 0 || kMaxStackCap < maxStack)
        return false;
    if (0 < maxStack && maxStack < amount)
        return false;
    return true;
}

/** "Bandage x3 (Rare)" - amounts of 0/1 read as just the name; the rarity
 *  tag only appears when the game-side quality resolved. */
inline std::string FormatStack(const Stack& s)
{
    if (s.name.empty())
        return {};
    std::string out = s.name;
    if (1 < s.amount)
        out += " x" + std::to_string(s.amount);
    return out + LoadoutFormat::RarityTag(s.rarity);
}

/** Dispenser ports tag for container labels: " (2 ports)" - loot ejects from
 *  N dispenser drop points (LootInteractionComponent::DISPENSER_LOCATIONS). */
inline std::string PortsTag(int ports)
{
    if (ports <= 0 || ports > 32)
        return "";
    return " (" + std::to_string(ports)
        + (ports == 1 ? " port)" : " ports)");
}

/**
 * Comma-joined summary for the ESP label: "Bandage x2, Med Kit". Hard-capped
 * at maxChars so the label stays one line: stacks that do not fit collapse
 * into a trailing " +N" (N = stacks left unnamed), and a single oversized
 * name is truncated with '~' rather than silently dropped.
 */
inline std::string JoinSummary(const Stack* stacks, int count, int maxChars)
{
    if (!stacks || count <= 0 || maxChars <= 0)
        return {};

    std::string out;
    for (int i = 0; i < count; ++i) {
        const std::string piece = FormatStack(stacks[i]);
        if (piece.empty())
            continue;
        if (out.empty()) {
            if (static_cast<int>(piece.size()) <= maxChars) {
                out = piece;
                continue;
            }
            return piece.substr(0, static_cast<size_t>(maxChars - 1)) + "~";
        }
        const std::string candidate = out + ", " + piece;
        if (static_cast<int>(candidate.size()) <= maxChars) {
            out = candidate;
            continue;
        }
        int left = 0;
        for (int j = i; j < count; ++j) {
            if (!FormatStack(stacks[j]).empty())
                ++left;
        }
        const std::string suffix = " +" + std::to_string(left);
        const bool fits = static_cast<int>(out.size() + suffix.size()) <= maxChars;
        return fits ? out + suffix : out;
    }
    return out;
}

} // namespace CrateContents
