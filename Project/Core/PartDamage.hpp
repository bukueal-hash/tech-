#pragma once
// Per-part damage (feature #8).
//
// Pure classification between the DMA reads (HealthService::PART_HP_ARRAY
// fractions + ConstructableStaticMeshStyle::IS_DESTROYED/DESTROYED_REASON) and
// the per-part pips drawn over the bot. A part whose hp slot fails to read is
// "unreadable" and simply gets no pip - never a false "destroyed leg".

#include <string>

namespace PartDamage {

enum class Pip {
    Unread = 0,  // hp slot failed to read (-1 sentinel)
    Ok,          // ratio >= 0.95
    Hurt,        // 0.35 <= ratio < 0.95
    Critical,    // 0 < ratio < 0.35
    Dead,        // ratio <= 0
};

// Classify a PART_HP_ARRAY fraction. Ratios are plausibility-gated at read
// time into [-1, 4]; here -1 is unreadable and anything below 0 counts dead.
inline Pip Classify(float hpRatio)
{
    if (hpRatio < 0.f)
        return Pip::Unread;
    if (hpRatio == 0.f)
        return Pip::Dead;
    if (hpRatio < 0.35f)
        return Pip::Critical;
    if (hpRatio < 0.95f)
        return Pip::Hurt;
    return Pip::Ok;
}

// Parts below the threshold (unreadable slots excluded).
inline int CountBelow(const float* hp, int count, float threshold)
{
    if (!hp || count <= 0)
        return 0;
    int n = 0;
    for (int i = 0; i < count; ++i) {
        if (hp[i] >= 0.f && hp[i] < threshold)
            ++n;
    }
    return n;
}

// "parts 2/8 down" summary line; empty when nothing was read.
inline std::string SummaryText(int damaged, int total)
{
    if (total <= 0)
        return "";
    return "parts " + std::to_string(damaged) + "/" + std::to_string(total)
        + " down";
}

} // namespace PartDamage
