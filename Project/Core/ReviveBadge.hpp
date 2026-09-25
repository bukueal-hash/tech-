#pragma once
// DBNO / revive countdown formatting (feature: DBNO & revive system).
//
// Pure helpers between the DMA reads (FPlayerHealthInfo::bIsDBNO and the
// interaction component's defib/bleedout timer float pairs) and the ESP badge
// + countdown ring. A timer pair that fails the plausibility gate collapses to
// "no ring" - never a garbage countdown.

#include <string>

namespace ReviveBadge {

// Remainder in seconds from an (elapsed, total) timer pair. Returns -1 when
// the pair is not a plausible timer (flaky read, zeroed slot, absurd total).
inline float RemainSeconds(float elapsed, float total)
{
    if (!(total > 0.f) || total > 300.f)
        return -1.f;
    if (elapsed < -0.5f || elapsed > total + 0.5f)
        return -1.f;
    const float remain = total - elapsed;
    return remain >= 0.f ? remain : -1.f;
}

// Ring fill fraction 0..1 (share of the timer still remaining).
inline float RingFraction(float remain, float total)
{
    if (!(total > 0.f) || remain <= 0.f)
        return 0.f;
    const float frac = remain / total;
    return frac > 1.f ? 1.f : frac;
}

// "0:07" mm:ss countdown text; empty for unknown remainders.
inline std::string FormatCountdown(float remainS)
{
    if (remainS < 0.f)
        return "";
    int total = static_cast<int>(remainS + 0.5f);
    if (total > 300)
        total = 300;
    std::string out = std::to_string(total / 60);
    out += ":";
    const int ss = total % 60;
    if (ss < 10)
        out += "0";
    out += std::to_string(ss);
    return out;
}

} // namespace ReviveBadge
