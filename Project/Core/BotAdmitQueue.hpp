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
    static constexpr uint8_t kMaxTransientTries = 3;
    // Priority rows per pass: the lane grows with the backlog (streaming burst
    // at a POI) but stays bounded so it cannot starve the ring band.
    static constexpr size_t kPrioFloor = 64;
    static constexpr size_t kPrioHardMax = 256;

    // First sight of an actor (the pass-to-pass diff). Idempotent.
    void NoteNew(uint64_t actor) { m_pending.insert(actor); }

    bool Contains(uint64_t actor) const { return m_pending.count(actor) != 0; }

    // Rows the priority lane may probe this pass. Rises with the backlog so a
    // burst drains in a pass or two instead of trickling at the floor.
    size_t Cap() const
    {
        const size_t backlog = m_pending.size();
        return (std::min)(kPrioHardMax, (std::max)(kPrioFloor, backlog));
    }

    // Collect queued actors in the caller's stable order (never unordered_set
    // order — probe order must be deterministic across passes).
    template <typename InputIt>
    void CollectPrio(InputIt begin, InputIt end,
                     std::vector<uint64_t>& out) const
    {
        const size_t cap = Cap();
        out.reserve(out.size() + cap);
        for (InputIt it = begin; it != end && out.size() < cap; ++it) {
            if (Contains(*it))
                out.push_back(*it);
        }
    }

    // Definitive outcome — the actor leaves the lane for good.
    void Settle(uint64_t actor)
    {
        m_pending.erase(actor);
        m_tries.erase(actor);
    }

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
    }

    // World generation change — every pointer is stale.
    void Clear()
    {
        m_pending.clear();
        m_tries.clear();
    }

    size_t Size() const { return m_pending.size(); }
    size_t TrySize() const { return m_tries.size(); }

private:
    std::unordered_set<uint64_t> m_pending;
    std::unordered_map<uint64_t, uint8_t> m_tries;
};

} // namespace AdmitQueue
