#pragma once
// Part 6 - mission objective brief.
//
// A small, DMA-free model so the cached worker read (Engine::RefreshObjectives)
// and the paint-side Raid HUD panel agree on what is known, what is unknown and
// what must never be displayed. FText (Summary/Description) is deliberately NOT
// part of this model: resolving FText needs a resolved text table, so an
// unresolvable title stays absent rather than being read as raw bytes.
//
// Evidence: Angelscript.MainObjectiveComponent / MainObjectiveEntry /
// ObjectiveTaskEntry / ObjectiveProgress / ObjectiveTimer in sdk/sdk.txt, and
// the EMainObjectiveState / EObjectiveTimerType enums in sdk/SDK1.txt.

#include <cstdint>
#include <string>

namespace ObjectiveBrief {

// EMainObjectiveState (sdk/SDK1.txt:18244). MAX is a sentinel, not a state.
enum class State : int32_t {
    Unknown = -1,
    Primary = 0,
    Active = 1,
    Inactive = 2,
    Max = 3,
};

// EObjectiveTimerType (sdk/SDK1.txt:22979).
enum class TimerType : int32_t {
    Unknown = -1,
    Elapsed = 0,
    Remaining = 1,
    Max = 2,
};

inline bool StatePlausible(int32_t state)
{
    return state >= 0 && state <= 2;
}

inline bool TimerTypePlausible(int32_t type)
{
    return type >= 0 && type <= 1;
}

/** One objective's cached state. Absent fields stay at their unknown default;
 *  nothing here is a guess standing in for a failed DMA read. */
struct Brief {
    bool valid = false;
    int32_t objectiveId = -1;
    int32_t state = -1;              // EMainObjectiveState, -1 unknown
    int32_t progressCount = -1;      // ObjectiveProgress.Count, -1 unknown
    int32_t progressMax = -1;        // ObjectiveProgress.max, -1 unknown
    bool isPercent = false;          // ObjectiveProgress.bIsPercent
    double timerSeconds = -1.0;      // ObjectiveTimer.Seconds, -1 unknown
    int32_t timerType = -1;          // EObjectiveTimerType, -1 unknown
    int32_t taskCount = -1;          // number of task entries, -1 unknown
    uint64_t stampMs = 0;
    uint64_t worldGeneration = 0;
};

/** Counts are bounded so a bad read cannot produce an absurd fraction. */
inline bool ProgressPlausible(int32_t count, int32_t max)
{
    if (count < 0 || max < 0)
        return false;
    if (max > 1000000 || count > max)
        return false;
    return true;
}

inline std::string StateText(int32_t state)
{
    switch (state) {
    case 0: return "Primary";
    case 1: return "Active";
    case 2: return "Inactive";
    default: return "Unknown";
    }
}

inline std::string TimerTypeText(int32_t type)
{
    switch (type) {
    case 0: return "elapsed";
    case 1: return "remaining";
    default: return "unknown";
    }
}

/** "3/7" (or "45%" when the SDK marks the progress as a percentage).
 *  Returns "" when the pair is not plausible, so the caller shows nothing
 *  rather than "0/0". */
inline std::string ProgressText(int32_t count, int32_t max, bool isPercent)
{
    if (!ProgressPlausible(count, max) || max <= 0)
        return {};
    if (isPercent)
        return std::to_string(static_cast<int>((static_cast<double>(count) / max) * 100.0 + 0.5)) + "%";
    return std::to_string(count) + "/" + std::to_string(max);
}

/** "Objective 12 [Active] 3/7" - the Objective id is only shown when plausible
 *  (the SDK field is a plain int32, so -1 means the read failed). */
inline std::string SummaryLine(const Brief& b)
{
    if (!b.valid)
        return {};
    std::string out = "Objective";
    if (b.objectiveId >= 0)
        out += " " + std::to_string(b.objectiveId);
    const std::string state = StateText(b.state);
    if (state != "Unknown")
        out += " [" + state + "]";
    const std::string progress = ProgressText(b.progressCount, b.progressMax, b.isPercent);
    if (!progress.empty())
        out += " " + progress;
    return out;
}

} // namespace ObjectiveBrief
