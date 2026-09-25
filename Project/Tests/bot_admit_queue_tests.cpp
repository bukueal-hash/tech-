// BotAdmitQueue suite — pins the discovery-latency fix: "bot ESP takes a while
// before some bots show."
//
// The regression this locks down: admission used a one-shot new-actor diff, so
// an actor that missed its single priority window (64-row cap / budget clip /
// undecrypted name) lost "new" status forever and waited an up-to-19s ring
// sweep before being probed again. The queue must retain unclassified actors
// across passes until they reach a definitive state.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/BotAdmitQueue.hpp"

#include <vector>

TEST_CASE("BotAdmitQueue keeps an actor queued after a missed priority window")
{
    AdmitQueue::PendingQueue q;

    // 300 new actors stream in at once (arriving at a POI). The old one-shot
    // diff flagged them new for one pass only; everything past the cap lost
    // "new" status and fell to the ring lottery.
    for (uint64_t a = 1; a <= 300; ++a)
        q.NoteNew(a);
    CHECK(q.Size() == 300);

    // Backlog raises the cap, but it stays bounded (256).
    CHECK(q.Cap() == AdmitQueue::PendingQueue::kPrioHardMax);

    // This pass probes the first 256 (stable admit order); the rest were NOT
    // probed — and must still be queued next pass (the old code dropped them).
    std::vector<uint64_t> admitOrder;
    for (uint64_t a = 1; a <= 300; ++a)
        admitOrder.push_back(a);
    std::vector<uint64_t> prio;
    q.CollectPrio(admitOrder.begin(), admitOrder.end(), prio);
    REQUIRE(prio.size() == 256);

    // Settle what was probed; the untouched tail survives into the next pass.
    for (uint64_t a : prio)
        q.Settle(a);
    CHECK(q.Size() == 44);

    std::vector<uint64_t> prio2;
    q.CollectPrio(admitOrder.begin(), admitOrder.end(), prio2);
    REQUIRE(prio2.size() == 44);
    CHECK(prio2.front() == 257);
}

TEST_CASE("BotAdmitQueue holds the floor when the backlog is small")
{
    AdmitQueue::PendingQueue q;
    for (uint64_t a = 10; a <= 12; ++a)
        q.NoteNew(a);
    CHECK(q.Cap() == AdmitQueue::PendingQueue::kPrioFloor);
}

TEST_CASE("BotAdmitQueue retries transient failures and settles on the budget")
{
    AdmitQueue::PendingQueue q;
    q.NoteNew(42);

    // A spawn still streaming in (no root / no mesh / name not decrypted yet)
    // fails probes transiently. It must stay queued — the old memo hid it for
    // 10-17s after ONE failed probe.
    CHECK_FALSE(q.NoteTransient(42));
    CHECK(q.Contains(42));
    CHECK_FALSE(q.NoteTransient(42));
    CHECK(q.Contains(42));

    // Third transient failure exhausts the budget: settled here and now.
    CHECK(q.NoteTransient(42));
    CHECK_FALSE(q.Contains(42));
}

TEST_CASE("BotAdmitQueue Settle clears both queue and try state")
{
    AdmitQueue::PendingQueue q;
    q.NoteNew(7);
    CHECK_FALSE(q.NoteTransient(7));
    q.Settle(7);
    CHECK(q.Size() == 0);
    CHECK(q.TrySize() == 0);

    // Re-queued later (address reuse) gets a fresh try budget.
    q.NoteNew(7);
    CHECK_FALSE(q.NoteTransient(7));
    CHECK(q.Contains(7));
}

TEST_CASE("BotAdmitQueue prunes only despawned actors")
{
    AdmitQueue::PendingQueue q;
    for (uint64_t a = 1; a <= 5; ++a)
        q.NoteNew(a);
    CHECK_FALSE(q.NoteTransient(2)); // 2 carries try state

    q.Prune([](uint64_t a) { return a != 3 && a != 2; });

    CHECK(q.Size() == 3);
    CHECK_FALSE(q.Contains(2));
    CHECK_FALSE(q.Contains(3));
    CHECK(q.Contains(1));
    CHECK(q.TrySize() == 0);
}

TEST_CASE("BotAdmitQueue Clear wipes everything on world-generation change")
{
    AdmitQueue::PendingQueue q;
    for (uint64_t a = 1; a <= 100; ++a)
        q.NoteNew(a);
    CHECK_FALSE(q.NoteTransient(5));
    q.Clear();
    CHECK(q.Size() == 0);
    CHECK(q.TrySize() == 0);
    CHECK(q.Cap() == AdmitQueue::PendingQueue::kPrioFloor);
}
