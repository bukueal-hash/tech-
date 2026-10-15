#pragma once

// =============================================================================
// BotPosSanity — is a new bot position sample consistent with the ones before it?
//
// The bug this exists for: one poisoned (wrong-but-plausible) read committed to
// the bot cache made a bot standing next to the player read as kilometres away.
// IsPlausibleWorldPos cannot catch it — a garbage vector is usually a perfectly
// ordinary pair of coordinates somewhere else in the level — and a distance gate
// cannot either, because the wrong answer is "far", which is exactly the state a
// far bot is supposed to be in. What separates the two is MOTION: the cache
// already holds this actor's previous samples, and a real bot cannot cross a
// kilometre between two samples 16 ms apart.
//
// Two anchors, because one is not enough. Comparing the candidate only against
// the newest sample would entrench a poisoned cache: if the poison got in first,
// every correct read afterwards looks like the jump and gets refused, and the box
// stays glued to the wrong place until the entry expires. So the older retained
// sample acts as a tiebreaker:
//
//   consistent with the newest                  -> accept
//   stale newest, or nothing to compare         -> accept (this gate has no say)
//   disagrees with the newest, but agrees with
//   the older sample while the newest itself
//   disagreed with that older sample            -> accept + re-anchor (the NEWEST
//                                                  was the outlier, not the candidate)
//   disagrees with both, reject count below cap -> refuse: hold the last position
//   disagrees with both, cap reached            -> accept + re-anchor (a sustained
//                                                  disagreement is the world
//                                                  changing, not a bad read)
//
// Refusing means "no new sample", never "write the wrong one": the caller keeps
// the previous position and does not advance the sample stamps, so the entry's
// freshness gates (Core/BotEspExpiry.hpp) decide what to do about a bot that has
// stopped sampling — hiding it after kMaxPosSampleAgeMs — instead of painting it
// at a place it is not.
//
// Pure: no clocks, no DMA, no globals. Times are passed in, so the decision is
// testable without a live target.

#include "Vector.hpp"

#include <cmath>
#include <cstdint>

namespace BotPosSanity {

// Fastest thing in the raid with a box (flying drones, the rolling fireball)
// measured well under this; the slack below absorbs the rest of the error term
// (a slightly wrong dt, an interpolated source, a dropped sample).
constexpr double kMaxTravelSpeedCmPerSec = 3000.0;  // 30 m/s
constexpr double kStepSlackCm = 500.0;              // 5 m of free headroom
// A gap longer than this cannot be judged from motion at all, so the allowance
// stops growing: the next 900 ms of travel does not make a 1 km step plausible.
constexpr uint64_t kMaxJudgeGapMs = 1000;
// An anchor older than this has already been condemned by the expiry policy, so
// it must not be used to reject a fresh read.
constexpr uint64_t kAnchorStaleMs = 3000;
// Sustained disagreement is the world changing (a respawn, a pooled actor reused
// for a different unit), not a bad read. Kept small enough to stay far below the
// 3 s sample-age kill this gate feeds into.
constexpr uint8_t kMaxRejectsBeforeReanchor = 8;

/** One retained position sample. valid is the caller's plausibility verdict. */
struct Sample
{
    Vector3 pos{};
    uint64_t stampMs = 0;
    bool valid = false;
};

struct Verdict
{
    bool accept = true;
    bool reanchored = false;   // accepted although it disagreed with the history
    const char* reason = "no_history";
    double stepCm = -1.0;      // distance from the newest anchor, -1 when unknown
    double limitCm = -1.0;     // the distance that was allowed for that step
};

inline double StepCm(const Vector3& a, const Vector3& b)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/** How far a bot may have moved between two stamps. */
inline double TravelLimitCm(uint64_t fromMs, uint64_t toMs)
{
    if (toMs <= fromMs)
        return kStepSlackCm;
    uint64_t gapMs = toMs - fromMs;
    if (gapMs > kMaxJudgeGapMs)
        gapMs = kMaxJudgeGapMs;
    return kMaxTravelSpeedCmPerSec * (static_cast<double>(gapMs) * 0.001) + kStepSlackCm;
}

/**
 * Judge one candidate against the entry's two retained samples.
 *
 * newest      — the sample the cache holds now (WorldPos / positionSampleMs)
 * older       — the sample before it (prevSampleWorldPos / prevSampleMs)
 * candidate   — the freshly read position, already plausibility-checked
 * nowMs       — steady-clock ms; must be the same domain as the sample stamps
 * rejects     — how many consecutive refusals this entry has already had
 */
inline Verdict Judge(const Sample& newest, const Sample& older,
    const Vector3& candidate, uint64_t nowMs, uint8_t rejects)
{
    Verdict v;
    if (!newest.valid || newest.stampMs == 0) {
        v.reason = "no_history";
        return v;
    }
    // Clock skew (now behind the stamp) is treated as fresh: a timestamp-domain
    // hiccup must not be able to refuse a good read.
    const uint64_t ageMs = nowMs > newest.stampMs ? nowMs - newest.stampMs : 0;
    if (ageMs > kAnchorStaleMs) {
        v.reason = "anchor_stale";
        return v;
    }

    v.stepCm = StepCm(candidate, newest.pos);
    v.limitCm = TravelLimitCm(newest.stampMs, nowMs);
    if (v.stepCm <= v.limitCm) {
        v.reason = "consistent";
        return v;
    }

    // The disagreement is real. Before refusing, ask whether the NEWEST sample is
    // the outlier: if the candidate agrees with the older sample while the newest
    // disagreed with it, the candidate is the coherent one and the newest is the
    // poison. Re-anchor to it now instead of rejecting correct reads for as long
    // as the poison stays stamped.
    if (older.valid && older.stampMs != 0 && newest.stampMs > older.stampMs
        && newest.stampMs - older.stampMs <= kAnchorStaleMs) {
        const double newestVsOlder = StepCm(newest.pos, older.pos);
        const double olderLimit = TravelLimitCm(older.stampMs, newest.stampMs);
        const double candidateVsOlder = StepCm(candidate, older.pos);
        if (candidateVsOlder <= TravelLimitCm(older.stampMs, nowMs)
            && newestVsOlder > olderLimit) {
            v.accept = true;
            v.reanchored = true;
            v.reason = "history_outlier_recovered";
            return v;
        }
    }

    if (static_cast<int>(rejects) + 1 >= static_cast<int>(kMaxRejectsBeforeReanchor)) {
        v.accept = true;
        v.reanchored = true;
        v.reason = "reanchor_after_rejects";
        return v;
    }

    v.accept = false;
    v.reason = "inconsistent_with_history";
    return v;
}

} // namespace BotPosSanity
