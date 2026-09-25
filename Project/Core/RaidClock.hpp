#pragma once
// Raid dashboard formatting (feature #9).
//
// Pure helpers between the DMA reads (FStageInfo::TimeLeft/GraceTime doubles
// and the PioneerGameState phase/counts) and the top-center HUD panel. A clock
// that fails the plausibility gate renders as "--:--", never as garbage.

#include <string>

namespace RaidClock {

// Plausible raid-clock seconds: 0 .. 4h. Anything else is a bad read.
inline bool ClockPlausible(double seconds)
{
    return seconds >= 0.0 && seconds < 14400.0;
}

// "12:34" mm:ss; hours spill to h:mm:ss; unknown renders "--:--".
inline std::string FormatClock(double seconds)
{
    if (!ClockPlausible(seconds))
        return "--:--";
    int total = static_cast<int>(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    std::string out;
    if (h > 0) {
        out = std::to_string(h);
        out += ":";
        if (m < 10)
            out += "0";
    }
    out += std::to_string(m);
    out += ":";
    if (s < 10)
        out += "0";
    out += std::to_string(s);
    return out;
}

// PioneerGameState::GamePhase is a numeric phase; its enum names are not in
// the offline sources, so the panel shows the raw phase number honestly
// instead of inventing labels. Empty for out-of-range reads.
inline std::string PhaseText(int phase)
{
    if (phase < 0 || phase > 16)
        return "";
    return "Phase " + std::to_string(phase);
}

} // namespace RaidClock
