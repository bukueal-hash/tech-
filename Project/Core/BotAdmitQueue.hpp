#pragma once

// =============================================================================
// BotAdmitQueue — persistent priority lane for newly-seen actors.
//
// The bug this exists for: "bot ESP takes a while before some bots show."
// Admission used a one-shot diff — an actor was NEW for exactly one pass, and
// if it missed that single priority window (fixed 64-row cap, a budget-clipped
// pass, or a name that had not decrypted yet) it lost "new" status forever and
// waited for the 8-slice admission ring to reach it: up to ~19s of invisibility
// depending on array position. A single transient verify failure (spawn still
// streaming in — no root/mesh yet) additionally memoized the actor as a reject
// for 10-17s.
//
// The queue carries the memory instead. An actor joins when first seen and
// leaves ONLY on a definitive outcome:
//   - admitted to the cache, or
//   - proven non-bot with live evidence (player / world item / decoded name
//     that is not a bot / verify failure with root+mesh present), or
//   - transient failure budget spent (kMaxTransientTries), or
//   - left the world.
// Transient failures stay queued and re-probe on the next pass, so a spawn
// that arrives unready is retried until it settles instead of being punished
// for arriving early.
//
// Pure bookkeeping — no DMA — so Tests/bot_admit_queue_tests.cpp pins the
// semantics (the missed-window retry is the regression case).
//
// ── First-probe latency ─────────────────────────────────────────────────────
//
// Staying queued is only half the fix: an actor that is queued but not probed
// is just as invisible as one that was dropped. The probe that decides what a
// new actor IS comes from this lane, because the ring band skips queued actors
// (the band must not double-probe what the lane owns), so the LANE'S ORDERING
// decides the latency — a full ring sweep is a worst case the lane can create
// on its own. Two orderings did exactly that:
//
//   1) Retry-first. A spawn that fails transiently keeps its try state for the
//      whole streaming window (45s in the bot lane) and was re-collected ahead
//      of every never-probed actor on every pass. Once the retry set reached
//      one pass budget, NO never-probed actor was collected at all and a new
//      bot could not be verified until the retries settled.
//   2) Caller's index order. Rows were collected in the actor array's order, so
//      when the never-probed set exceeded the per-pass budget the same prefix
//      was collected every pass and the tail waited a full rotation for its
//      first probe.
//
// The lane now orders by AGE, not by class or by array position:
//
//   never-probed and older than kFirstProbeBoundMs  -> collected first, oldest
//                                                     first, ahead of every
//                                                     retry (a retry has
//                                                     already had its probe;
//                                                     this actor has not)
//   never-probed, inside the bound                  -> a reserved share of the
//                                                     pass, which a retry storm
//                                                     cannot take
//   retries                                         -> the rest of the budget
//   any budget left over                           -> back to never-probed work,
//                                                     so the lane still drains
//
// kFirstProbeBoundMs is a preemption threshold, not a typical wait: the reserved
// share normally gives a new actor its probe on the next pass, and the bound
// decides who is served first when a burst (a POI arrival, a ring reset) exceeds
// the lane's per-second capacity. One pass of slack on top of the bound is
// deliberate — probes only run at pass rate — and ProbeStats() reports the real
// wait of every first probe so the promise is measured, not assumed.
// =============================================================================

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AdmitQueue {

class PendingQueue {
public:
    // A spawn still streaming in may fail a probe a few times before its root,
    // mesh or name exist. After this many transient failures it is treated as
    // a definitive reject (the caller's TTL memo takes over).
    static constexpr uint8_t kMaxTransientTries = 8;
    // Priority rows per pass: the lane grows with the backlog (streaming burst
    // at a POI) but stays bounded so it cannot starve the ring band.
    static constexpr size_t kPrioFloor = 64;
    static constexpr size_t kPrioHardMax = 256;

    // How long a never-probed actor may wait before it preempts everything else
    // in the lane. 750ms + one pass (the scanner's own cadence, 200ms in the bot
    // lane) + that pass's process slice lands the first probe inside a second,
    // which is what "a new actor is verified within a second" needs.
    static constexpr uint64_t kFirstProbeBoundMs = 750;
    // Rows of every pass reserved for never-probed actors, so a retry storm
    // cannot consume the pass before a new actor is looked at.
    static constexpr size_t kFirstSightRowsPerPass = 32;

    // Which lane a queued actor belongs to this pass.
    enum class Lane
    {
        OverdueFirst, // never probed, past kFirstProbeBoundMs — goes first
        FirstSight,   // never probed — gets the reserved share
        Retry,        // already probed at least once — takes what is left
    };

    // One pass's lane budget. maxRows is the caller's hard row budget for the
    // pass (its DMA budget divided into rows); the reserve is a floor for
    // never-probed work, never a ceiling on it.
    struct PassPlan
    {
        size_t maxRows = 0;
        size_t minFirstSightRows = kFirstSightRowsPerPass;
    };

    // The last kProbeWindowSamples first probes. A cumulative mean since Clear()
    // cannot answer "is a new actor verified within a second": a cold load's
    // thousand first probes dominate that mean for the rest of the raid and hide
    // the steady state. The window is what the lane is doing NOW; n/overBound/
    // sumMs/maxMs stay cumulative beside it, because the cold load is a real cost
    // worth reporting too.
    static constexpr size_t kProbeWindowSamples = 128;
    struct FirstProbeStats
    {
        uint64_t n = 0;
        uint64_t overBound = 0;
        uint64_t sumMs = 0;
        uint64_t maxMs = 0;
        // Last kProbeWindowSamples probes.
        uint64_t winN = 0;
        uint64_t winOverBound = 0;
        uint64_t winSumMs = 0;
        uint64_t winMaxMs = 0;
    };

    // First sight of an actor (the pass-to-pass diff) with the clock of the
    // sighting. Idempotent: a re-note (ring reset, N jump) must not restart the
    // latency clock, or a burst would keep pushing its own bound back.
    void NoteNew(uint64_t actor, uint64_t nowMs)
    {
        m_pending.insert(actor);
        m_firstSeen.try_emplace(actor, nowMs);
    }

    bool Contains(uint64_t actor) const { return m_pending.count(actor) != 0; }

    // Rows the priority lane may probe this pass. Rises with the backlog so a
    // burst drains in a pass or two instead of trickling at the floor.
    size_t Cap() const
    {
        const size_t backlog = m_pending.size();
        return (std::min)(kPrioHardMax, (std::max)(kPrioFloor, backlog));
    }

    // Classify one queued actor for this pass.
    Lane Classify(uint64_t actor, uint64_t nowMs) const
    {
        if (m_probed.count(actor) != 0)
            return Lane::Retry;
        return FirstProbeWaitMs(actor, nowMs) >= kFirstProbeBoundMs
            ? Lane::OverdueFirst
            : Lane::FirstSight;
    }

    // How long this actor has been waiting for its first probe (0 when it has
    // already had one, or when it was never stamped).
    uint64_t FirstProbeWaitMs(uint64_t actor, uint64_t nowMs) const
    {
        if (m_probed.count(actor) != 0)
            return 0;
        const auto it = m_firstSeen.find(actor);
        if (it == m_firstSeen.end())
            return 0;
        return nowMs > it->second ? nowMs - it->second : 0;
    }

    bool Probed(uint64_t actor) const { return m_probed.count(actor) != 0; }

    // The actor's first probe happened on the caller's clock. Records the wait
    // once (a second probe is not a first probe) — this is the measurement the
    // bound exists for, so it is taken where the probe actually happens.
    void NoteProbed(uint64_t actor, uint64_t nowMs)
    {
        if (Probed(actor))
            return;
        const uint64_t waitMs = FirstProbeWaitMs(actor, nowMs);
        m_probed.insert(actor);
        ++m_probeStats.n;
        m_probeStats.sumMs += waitMs;
        if (waitMs > m_probeStats.maxMs)
            m_probeStats.maxMs = waitMs;
        if (waitMs >= kFirstProbeBoundMs)
            ++m_probeStats.overBound;
        PushProbeWindow(waitMs);
    }

    // Record one first probe in the rolling window and recompute the window's
    // own totals. The rescan is 128 entries per probe and only runs once per
    // actor, so the retired sample cannot stay in the max or the over-bound
    // count — which is the whole point of reporting a window.
    void PushProbeWindow(uint64_t waitMs)
    {
        m_winWait[m_winCursor] = waitMs;
        m_winCursor = (m_winCursor + 1) % kProbeWindowSamples;
        if (m_winCount < kProbeWindowSamples)
            ++m_winCount;

        m_probeStats.winN = 0;
        m_probeStats.winOverBound = 0;
        m_probeStats.winSumMs = 0;
        m_probeStats.winMaxMs = 0;
        for (size_t i = 0; i < m_winCount; ++i) {
            const uint64_t w = m_winWait[i];
            ++m_probeStats.winN;
            m_probeStats.winSumMs += w;
            if (w > m_probeStats.winMaxMs)
                m_probeStats.winMaxMs = w;
            if (w >= kFirstProbeBoundMs)
                ++m_probeStats.winOverBound;
        }
    }

    FirstProbeStats ProbeStats() const { return m_probeStats; }

    // Collect this pass's rows from the caller's list, highest priority first.
    //
    // The caller's order is used only to break ties (probe order must stay
    // deterministic across passes); the lane's priority is age and probe state,
    // never array position. Pass the caller's full candidate list — filtering by
    // class is this function's job.
    template <typename InputIt>
    void CollectPrio(InputIt begin, InputIt end, uint64_t nowMs,
                     const PassPlan& plan, std::vector<uint64_t>& out) const
    {
        if (plan.maxRows == 0)
            return;
        const size_t before = out.size();
        const auto added = [&out, before]() { return out.size() - before; };
        // Each class is taken through a cursor: the last sweep revisits the
        // never-probed rows, and a row must never be collected twice (a
        // duplicate row would probe the same actor twice in one pass).
        const auto take = [&out, &plan, before](const std::vector<uint64_t>& rows,
                                                size_t& cursor, size_t limit) {
            for (; cursor < rows.size(); ++cursor) {
                if (out.size() - before >= limit || out.size() - before >= plan.maxRows)
                    break;
                out.push_back(rows[cursor]);
            }
        };

        std::vector<uint64_t> overdue, firstSight, retry;
        for (InputIt it = begin; it != end; ++it) {
            if (!Contains(*it))
                continue;
            switch (Classify(*it, nowMs)) {
            case Lane::OverdueFirst: overdue.push_back(*it); break;
            case Lane::FirstSight: firstSight.push_back(*it); break;
            case Lane::Retry: retry.push_back(*it); break;
            }
        }

        // 1. Overdue rows first, oldest first. A set larger than the budget must
        //    drain in arrival order — re-serving the same prefix every pass is
        //    the array-order bug this replaces.
        std::stable_sort(overdue.begin(), overdue.end(),
            [this](uint64_t a, uint64_t b) {
                return m_firstSeen.at(a) < m_firstSeen.at(b);
            });
        size_t overCursor = 0;
        size_t sightCursor = 0;
        size_t retryCursor = 0;
        take(overdue, overCursor, plan.maxRows);

        // 2. Never-probed work keeps a reserved share of the pass.
        take(firstSight, sightCursor, added() + plan.minFirstSightRows);

        // 3. Retries take the rest of the budget.
        take(retry, retryCursor, plan.maxRows);

        // 4. Headroom left (few retries, many new actors) goes back to the
        //    never-probed rows, so a burst still drains at lane rate.
        take(firstSight, sightCursor, plan.maxRows);
    }

    // Definitive outcome — the actor leaves the lane for good.
    void Settle(uint64_t actor)
    {
        m_pending.erase(actor);
        m_tries.erase(actor);
        m_firstSeen.erase(actor);
        m_probed.erase(actor);
    }

    // True when the actor has already burned at least one transient try and
    // is still queued. Callers probe these first: a retry is one probe from
    // settling, while a never-probed actor can always wait one more pass.
    bool HasTries(uint64_t actor) const
    {
        auto it = m_tries.find(actor);
        return it != m_tries.end() && it->second > 0;
    }

    // Count a transient try WITHOUT settling — the caller enforces its own
    // time window (streaming spawns can need far more probes than the old
    // 8-try budget allowed; settling them hid live bots for a minute+).
    void NoteTry(uint64_t actor) { ++m_tries[actor]; }

    // Transient failure (still streaming / not yet decrypted). Returns true
    // when the try budget is exhausted: the caller should Settle() (done here)
    // and memoize the reject. Returns false while the actor stays queued.
    bool NoteTransient(uint64_t actor)
    {
        uint8_t& tries = m_tries[actor];
        if (static_cast<uint8_t>(tries + 1) >= kMaxTransientTries) {
            Settle(actor);
            return true;
        }
        ++tries;
        return false;
    }

    // Drop actors that left the world (address reuse would otherwise queue a
    // stale entry forever). aliveFn(actor) -> bool.
    template <typename AliveFn>
    void Prune(AliveFn&& aliveFn)
    {
        for (auto it = m_pending.begin(); it != m_pending.end(); ) {
            if (!aliveFn(*it))
                it = m_pending.erase(it);
            else
                ++it;
        }
        for (auto it = m_tries.begin(); it != m_tries.end(); ) {
            if (!aliveFn(it->first))
                it = m_tries.erase(it);
            else
                ++it;
        }
        for (auto it = m_firstSeen.begin(); it != m_firstSeen.end(); ) {
            if (!aliveFn(it->first))
                it = m_firstSeen.erase(it);
            else
                ++it;
        }
        for (auto it = m_probed.begin(); it != m_probed.end(); ) {
            if (!aliveFn(*it))
                it = m_probed.erase(it);
            else
                ++it;
        }
    }

    // World generation change — every pointer is stale.
    void Clear()
    {
        m_pending.clear();
        m_tries.clear();
        m_firstSeen.clear();
        m_probed.clear();
        m_probeStats = FirstProbeStats{};
        for (uint64_t& w : m_winWait)
            w = 0;
        m_winCount = 0;
        m_winCursor = 0;
    }

    size_t Size() const { return m_pending.size(); }
    size_t TrySize() const { return m_tries.size(); }

private:
    std::unordered_set<uint64_t> m_pending;
    std::unordered_map<uint64_t, uint8_t> m_tries;
    std::unordered_map<uint64_t, uint64_t> m_firstSeen;
    std::unordered_set<uint64_t> m_probed;
    FirstProbeStats m_probeStats;
    uint64_t m_winWait[kProbeWindowSamples]{};
    size_t m_winCount = 0;
    size_t m_winCursor = 0;
};

} // namespace AdmitQueue
