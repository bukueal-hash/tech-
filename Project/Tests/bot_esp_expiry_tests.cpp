// BotEspExpiry suite — pins "when bots die, why does the ESP stay so long".
//
// The regressions locked down here:
//   - a dead bot's box used to outlive it by a 10-miss eviction climb
//     (5-25s of frozen ESP) and a packed-bitfield byte==1 comparison that
//     missed every destroyed byte with sibling bits set;
//   - a despawned actor's frozen position sample painted as a ghost box
//     forever because no gate checked sample age.

#include "doctest/doctest.h"

#include "Core/BotEspExpiry.hpp"

TEST_CASE("PosSampleFresh kills frozen samples and survives clock skew")
{
    using namespace BotEspExpiry;
    CHECK(PosSampleFresh(1000, 0, 3999));                          // normal age
    CHECK(PosSampleFresh(1000, 0, 1000 + kMaxPosSampleAgeMs));     // exactly at limit
    CHECK_FALSE(PosSampleFresh(1000, 0, 1000 + kMaxPosSampleAgeMs + 1)); // ghost
    CHECK(PosSampleFresh(0, 500, 500 + 2999));                     // admit fallback
    CHECK_FALSE(PosSampleFresh(0, 500, 500 + 3001));               // stale admit
    CHECK(PosSampleFresh(0, 0, 999999));                           // no clock — other gates
    CHECK(PosSampleFresh(5000, 0, 4000));                          // now < stamp: never hide
}

TEST_CASE("CurrentBotDrawProof rejects stale actors and unproven positions")
{
    using namespace BotEspExpiry;
    const uint64_t now = 10000;
    CHECK(CurrentBotDrawProof(9900, 9950, now));
    CHECK_FALSE(CurrentBotDrawProof(0, 9950, now));
    CHECK_FALSE(CurrentBotDrawProof(9900, 0, now));
    CHECK_FALSE(CurrentBotDrawProof(1000, 9950, now));
    CHECK_FALSE(CurrentBotDrawProof(9900, 1000, now));
    CHECK(CurrentBotDrawProof(10001, 10002, now)); // clock skew
}

TEST_CASE("Verified bots survive a temporary display-label miss")
{
    using namespace BotEspExpiry;
    CHECK(ShouldRetainCachedBot(true, false));
    CHECK(ShouldRetainCachedBot(false, true));
    CHECK_FALSE(ShouldRetainCachedBot(false, false));
}

TEST_CASE("DeadDisposition holds through DMA flaps then expires, bounded")
{
    using namespace BotEspExpiry;
    // Flap debounce: a single broken read (or one bad pass) holds state.
    CHECK(DeadDisposition(0, false) == DeadAction::Debounce);
    CHECK(DeadDisposition(kDeadConfirmMs - 1, true) == DeadAction::Debounce);
    // Hidden mode: the box is erased at confirmation — no miss-climb wait.
    CHECK(DeadDisposition(kDeadConfirmMs, false) == DeadAction::Expire);
    // Corpse mode: bounded hold, then expire even with show_dead_bots on.
    CHECK(DeadDisposition(kDeadConfirmMs, true) == DeadAction::KeepCorpse);
    CHECK(DeadDisposition(kCorpseHoldMs - 1, true) == DeadAction::KeepCorpse);
    CHECK(DeadDisposition(kCorpseHoldMs, true) == DeadAction::Expire);
    // A long-dead entry always expires.
    CHECK(DeadDisposition(kCorpseHoldMs * 10, false) == DeadAction::Expire);
}
