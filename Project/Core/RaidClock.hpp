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

// SDK1.txt:12482 EGamePhase. MAX is a sentinel, not a valid phase.
inline std::string PhaseText(int phase)
{
    switch (phase) {
    case 0: return "Pre-game";
    case 1: return "Early game";
    case 2: return "Mid-game";
    case 3: return "Late game";
    case 4: return "Not initialized";
    default: return "";
    }
}

// SDK1.txt:26136 EPerformanceHealthStatus; this is the replicated client
// status only, not a measured CPU/GPU value or complete server telemetry.
inline const char* ServerHealthText(int status)
{
    switch (status) {
    case 0: return "OK";
    case 1: return "Warning";
    case 2: return "Bad";
    default: return "Unknown";
    }
}

} // namespace RaidClock
