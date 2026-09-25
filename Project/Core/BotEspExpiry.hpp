#pragma once

// =============================================================================
// BotEspExpiry — when a bot's ESP must stop painting.
//
// Pins the fix for "when bots die, why does the ESP stay so long". Three
// causes, one policy:
//   1. ReadBotBrokenFlag compared a packed bitfield byte == 1 while the dump
//      types bIsDestroyed as "byte 0x1230 mask 0x1" — a destroyed bot whose
//      byte carried any sibling bit read "alive".
//   2. The retain loop kept Drawing=true on a dead bot and waited for a
//      10-miss eviction counter — 5-25s of frozen box after death.
//   3. ShouldDrawRobotEsp had no sample-age gate, so a despawned actor's
//      frozen position painted as a ghost box indefinitely.
//
// Split so the logic is testable without DMA (Tests/bot_esp_expiry_tests):
//   PosSampleFresh    — ghost kill at paint time
//   DeadDisposition   — flap debounce -> bounded corpse -> expire
// =============================================================================

#include <cstdint>

namespace BotEspExpiry {

// Normal bot position samples measured 0.4-2.1s old even under DMA load, so
// anything beyond 3s is a frozen sample from a dead/despawned actor.
constexpr uint64_t kMaxPosSampleAgeMs = 3000;
// A cache entry must have been seen in the current actor array recently as
// well as sampled through the live root. This is intentionally separate from
// position age: a stale root can return a plausible vector indefinitely.
constexpr uint64_t kMaxActorSeenAgeMs = 3000;

// Two retain passes (~0.5s cadence) must agree the bot is broken before the
// box drops: a single garbage DMA read must not blank a live box (flicker).
constexpr uint64_t kDeadConfirmMs = 400;

// show_dead_bots keeps a corpse visible (loot-husk locating), but bounded —
// a husk that lingers for minutes is the complaint this exists to kill.
constexpr uint64_t kCorpseHoldMs = 8000;

// True when the entry's newest position sample is recent enough to trust.
// Falls back to the admit stamp; entries with no clock yet stay governed by
// the other gates. Clock skew (now < stamp) never underflow-hides an entry.
inline bool PosSampleFresh(uint64_t sampleMs, uint64_t admittedMs, uint64_t nowMs)
{
    const uint64_t stamp = sampleMs ? sampleMs : admittedMs;
    if (stamp == 0 || nowMs < stamp)
        return true;
    return nowMs - stamp <= kMaxPosSampleAgeMs;
}

// Current-frame draw proof. Unlike PosSampleFresh, a zero stamp is not a pass:
// it means the corresponding worker has not produced evidence yet. Clock skew
// is treated conservatively as fresh so a timestamp-domain hiccup cannot hide
// every bot.
inline bool CurrentBotDrawProof(
    uint64_t actorSeenMs, uint64_t livePositionSampleMs, uint64_t nowMs)
{
    if (actorSeenMs == 0 || livePositionSampleMs == 0)
        return false;
    const uint64_t actorAge = nowMs < actorSeenMs
        ? 0 : nowMs - actorSeenMs;
    const uint64_t positionAge = nowMs < livePositionSampleMs
        ? 0 : nowMs - livePositionSampleMs;
    return actorAge <= kMaxActorSeenAgeMs
        && positionAge <= kMaxPosSampleAgeMs;
}

// A verified bot may temporarily have no display label while the worker
// retries name resolution. Its identity proof is stronger than an empty
// display string; retaining it lets paint use the safe "Bot" fallback instead
// of turning a label-read miss into a total disappearance.
inline bool ShouldRetainCachedBot(bool identityProven, bool allowType)
{
    return identityProven || allowType;
}

enum class DeadAction : uint8_t {
    Debounce = 0, // broken seen, not yet confirmed — hold previous state
    KeepCorpse,    // confirmed dead, bounded corpse hold running (show_dead_bots)
    Expire,        // drop Drawing and erase the entry now
};

// deadForMs = time since the FIRST broken observation for this actor.
inline DeadAction DeadDisposition(uint64_t deadForMs, bool showDeadBots)
{
    if (deadForMs < kDeadConfirmMs)
        return DeadAction::Debounce;
    if (showDeadBots && deadForMs < kCorpseHoldMs)
        return DeadAction::KeepCorpse;
    return DeadAction::Expire;
}

} // namespace BotEspExpiry
