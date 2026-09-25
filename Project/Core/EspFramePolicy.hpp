#pragma once

#include <cstdint>

namespace EspFramePolicy {

// A frame may be retained briefly through a worker/DMA hiccup, but never
// indefinitely. Frame collection can legitimately take 500-900ms while the
// DMA scatter and camera batch settle; dropping the whole overlay at 500ms
// turns one slow pass into visible all-ESP flicker. World-generation checks
// still prevent a retained frame from crossing a real map transition.
inline constexpr uint64_t kMaxFrameAgeMs = 1000;

struct SnapshotMeta {
    bool valid = false;
    uint64_t worldGeneration = 0;
    uint64_t collectStampMs = 0;
};

enum class Acceptance {
    Accepted,
    Invalid,
    WrongGeneration,
    MissingTimestamp,
    ClockSkew,
    Stale,
};

inline Acceptance Check(
    const SnapshotMeta& frame,
    uint64_t currentGeneration,
    uint64_t nowMs,
    uint64_t maxAgeMs = kMaxFrameAgeMs)
{
    if (!frame.valid)
        return Acceptance::Invalid;
    if (frame.worldGeneration != currentGeneration)
        return Acceptance::WrongGeneration;
    if (frame.collectStampMs == 0)
        return Acceptance::MissingTimestamp;
    if (nowMs < frame.collectStampMs)
        return Acceptance::ClockSkew;

    const uint64_t ageMs = nowMs - frame.collectStampMs;
    return ageMs <= maxAgeMs ? Acceptance::Accepted : Acceptance::Stale;
}

inline bool IsAcceptable(
    const SnapshotMeta& frame,
    uint64_t currentGeneration,
    uint64_t nowMs,
    uint64_t maxAgeMs = kMaxFrameAgeMs)
{
    return Check(frame, currentGeneration, nowMs, maxAgeMs)
        == Acceptance::Accepted;
}

} // namespace EspFramePolicy
