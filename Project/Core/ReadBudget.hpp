#pragma once

// ── One honest ledger of DMA reads ──────────────────────────────────────────
// Why this exists: the overlay has a dozen subsystems that each read game
// memory on their own schedule, and nothing recorded who asked for what. The
// old readout summed a single stopwatch around one frame, so it printed
// 0.000 ms while the scan thread, the retain worker, the aim tick and the
// position refresh all competed for the same DMA ring. "Who is fighting whom"
// was unanswerable.
//
// This ledger answers it with two axes:
//
//   requester — which thread issued the read (engine workers name themselves
//               through SetCurrentRequester; anything else falls back to its
//               OS thread id, so no reader is invisible);
//   purpose   — what the read was FOR (position / health / broken / bones …),
//               set by a PurposeScope around the calling code so a read is
//               never attributed to the wrong consumer.
//
// It is measurement only: it never blocks, never allocates on the hot path and
// never changes what is read. Two clock calls per read are the price of an
// honest number — a pull read is microsecond-scale, so the ledger stays well
// under a percent of the read it measures.
//
// MarkPassBoundary() is called from the frame publish (the natural once-per-
// pass boundary) so the panel can report reads PER PASS instead of a
// meaningless lifetime total.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ReadBudget {

enum class Purpose : int {
    Untagged = 0,   // not yet migrated — a bug report on its own
    Position,
    Health,
    Broken,
    Bounds,
    Bones,
    Velocity,
    Name,
    World,
    Kit,
    Camera,
    // Pointer chases: pawn <-> PlayerState, root/mesh/capsule components, the
    // inventory component, the movement component. These are not "world" reads
    // (they never touch UWorld/level arrays) and they are the bulk of a per-actor
    // scan, so pooling them into World hid which field the scan really re-reads.
    Link,
    Count
};

inline constexpr int kPurposeCount = static_cast<int>(Purpose::Count);

inline const char* PurposeName(int p)
{
    switch (static_cast<Purpose>(p)) {
    case Purpose::Untagged: return "untagged";
    case Purpose::Position: return "position";
    case Purpose::Health:   return "health";
    case Purpose::Broken:   return "broken";
    case Purpose::Bounds:   return "bounds";
    case Purpose::Bones:    return "bones";
    case Purpose::Velocity: return "velocity";
    case Purpose::Name:     return "name";
    case Purpose::World:    return "world";
    case Purpose::Kit:      return "kit";
    case Purpose::Camera:   return "camera";
    case Purpose::Link:     return "link";
    default:                return "?";
    }
}

/** Pull = one synchronous read blocking the caller. Scatter = batched. */
enum class Kind : int { Pull = 0, Scatter = 1, KindCount = 2 };

inline constexpr size_t kMaxRequesters = 48;
inline constexpr size_t kMaxNameBytes = 24;
inline constexpr size_t kOtherSlot = kMaxRequesters - 1;

struct SlotCounters {
    std::atomic<uint64_t> reads[static_cast<int>(Kind::KindCount)][kPurposeCount]{};
    std::atomic<uint64_t> bytes[static_cast<int>(Kind::KindCount)][kPurposeCount]{};
    std::atomic<uint64_t> us[static_cast<int>(Kind::KindCount)][kPurposeCount]{};
};

inline SlotCounters g_slots[kMaxRequesters];
inline char g_slotNames[kMaxRequesters][kMaxNameBytes];
inline std::atomic<int> g_slotCount{ 0 };
// A slot must not be read before its name is written, or the panel shows an
// empty requester. The index is claimed first, then filled, then published.
inline std::atomic<bool> g_slotReady[kMaxRequesters]{};

// Cumulative totals across every requester, kept separately so the pass
// snapshot below never has to walk the whole slot table under contention.
inline std::atomic<uint64_t> g_totalReads{ 0 };
inline std::atomic<uint64_t> g_totalBytes{ 0 };
inline std::atomic<uint64_t> g_totalUs{ 0 };
inline std::atomic<uint64_t> g_totalPull{ 0 };
inline std::atomic<uint64_t> g_totalScatter{ 0 };

// Snapshot of the cumulative totals at the previous pass boundary.
struct PassMark {
    uint64_t reads = 0;
    uint64_t bytes = 0;
    uint64_t us = 0;
    uint64_t pull = 0;
    uint64_t scatter = 0;
};
inline PassMark g_passMark;
inline std::atomic<uint64_t> g_passes{ 0 };

inline uint64_t NowUs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline int AllocSlot(const char* name)
{
    for (;;) {
        // Non-const: compare_exchange_weak updates `expected` on failure.
        int used = g_slotCount.load(std::memory_order_acquire);
        if (used >= static_cast<int>(kOtherSlot)) {
            std::memcpy(g_slotNames[kOtherSlot], "other", 6);
            g_slotReady[kOtherSlot].store(true, std::memory_order_release);
            return static_cast<int>(kOtherSlot);
        }
        if (g_slotCount.compare_exchange_weak(used, used + 1,
                std::memory_order_acq_rel)) {
            char* dst = g_slotNames[used];
            std::memset(dst, 0, kMaxNameBytes);
            if (name && name[0]) {
                // memcpy over a zeroed buffer instead of strncpy: this project
                // treats C4996 (strncpy) as an error. The buffer is cleared
                // above, so truncated names stay null-terminated.
                std::memcpy(dst, name, (std::min)(std::strlen(name), kMaxNameBytes - 1));
            } else {
                // Unnamed reader: use its OS thread id so it is attributable
                // instead of pooled into one anonymous bucket.
                std::snprintf(dst, kMaxNameBytes, "tid:%lu",
                    static_cast<unsigned long>(::GetCurrentThreadId()));
            }
            g_slotReady[used].store(true, std::memory_order_release);
            return used;
        }
    }
}

inline thread_local int t_slot = -1;
inline thread_local Purpose t_purpose = Purpose::Untagged;

/** Slot for this thread, allocated on first use. */
inline int CurrentSlot()
{
    if (t_slot < 0)
        t_slot = AllocSlot(nullptr);
    return t_slot;
}

/**
 * Name this thread in the ledger. Engine workers call it once at start; a
 * thread that never calls it still gets a tid: slot, so coverage is total.
 */
inline void SetCurrentRequester(const char* name)
{
    // Idempotent: the caller may be a scan gate that runs this every pass, and
    // re-allocating each pass would burn slots until every reader collapsed
    // into the "other" bucket.
    if (t_slot >= 0)
        return;
    t_slot = AllocSlot(name);
}

/** Purpose for reads issued by this thread until the scope ends. */
class PurposeScope {
public:
    explicit PurposeScope(Purpose p) : m_prev(t_purpose) { t_purpose = p; }
    ~PurposeScope() { t_purpose = m_prev; }
    PurposeScope(const PurposeScope&) = delete;
    PurposeScope& operator=(const PurposeScope&) = delete;
private:
    Purpose m_prev;
};

/**
 * Tags reads ONLY when the caller has not already said what they are for.
 *
 * For shared low-level plumbing — the FName/decryption read helper, the actor
 * type-id accessor — where the physically-correct answer ("this read is part of
 * decrypting a name") is LESS specific than the caller's intent. A scan pass that
 * is already collecting held-equipment data should keep those reads counted as
 * kit, not have them re-labelled by the helper underneath it. Where nothing more
 * specific is active it fills the gap, which is what stops a shared helper from
 * leaving the scan's whole volume untagged.
 */
class DefaultPurposeScope {
public:
    explicit DefaultPurposeScope(Purpose p) : m_prev(t_purpose) {
        if (t_purpose == Purpose::Untagged)
            t_purpose = p;
    }
    ~DefaultPurposeScope() { t_purpose = m_prev; }
    DefaultPurposeScope(const DefaultPurposeScope&) = delete;
    DefaultPurposeScope& operator=(const DefaultPurposeScope&) = delete;
private:
    Purpose m_prev;
};

inline void Note(Kind kind, uint64_t bytes, uint64_t us)
{
    const int slot = CurrentSlot();
    const int k = static_cast<int>(kind);
    const int p = static_cast<int>(t_purpose);
    SlotCounters& s = g_slots[slot];
    s.reads[k][p].fetch_add(1, std::memory_order_relaxed);
    s.bytes[k][p].fetch_add(bytes, std::memory_order_relaxed);
    s.us[k][p].fetch_add(us, std::memory_order_relaxed);
    g_totalReads.fetch_add(1, std::memory_order_relaxed);
    g_totalBytes.fetch_add(bytes, std::memory_order_relaxed);
    g_totalUs.fetch_add(us, std::memory_order_relaxed);
    if (kind == Kind::Pull)
        g_totalPull.fetch_add(1, std::memory_order_relaxed);
    else
        g_totalScatter.fetch_add(1, std::memory_order_relaxed);
}

/** One synchronous read that blocked the caller. */
inline void NotePull(uint64_t bytes, uint64_t us) { Note(Kind::Pull, bytes, us); }

/** One batched entry submitted or resolved by a scatter execute. */
inline void NoteScatter(uint64_t bytes, uint64_t us) { Note(Kind::Scatter, bytes, us); }

/**
 * A whole scatter execute: one entry per prepared read, charged once so a
 * 200-entry batch does not cost 200 atomic round trips on the scan thread.
 */
inline void NoteScatterBatch(uint64_t entries, uint64_t bytes, uint64_t us)
{
    if (!entries)
        return;
    const int slot = CurrentSlot();
    const int p = static_cast<int>(t_purpose);
    SlotCounters& s = g_slots[slot];
    s.reads[1][p].fetch_add(entries, std::memory_order_relaxed);
    s.bytes[1][p].fetch_add(bytes, std::memory_order_relaxed);
    s.us[1][p].fetch_add(us, std::memory_order_relaxed);
    g_totalReads.fetch_add(entries, std::memory_order_relaxed);
    g_totalBytes.fetch_add(bytes, std::memory_order_relaxed);
    g_totalUs.fetch_add(us, std::memory_order_relaxed);
    g_totalScatter.fetch_add(entries, std::memory_order_relaxed);
}

/** Times a pull read around the caller's own DMA call. */
class PullTimer {
public:
    PullTimer() : m_start(NowUs()) {}
    ~PullTimer() { NotePull(m_bytes, NowUs() - m_start); }
    void setBytes(uint64_t bytes) { m_bytes = bytes; }
private:
    uint64_t m_start;
    uint64_t m_bytes = 0;
};

/**
 * Close the current measurement window. Called once per published frame, which
 * is the only pass boundary every consumer shares, so the panel reports
 * reads-per-pass rather than a lifetime total nobody can act on.
 */
inline void MarkPassBoundary()
{
    g_passMark.reads = g_totalReads.load(std::memory_order_relaxed);
    g_passMark.bytes = g_totalBytes.load(std::memory_order_relaxed);
    g_passMark.us = g_totalUs.load(std::memory_order_relaxed);
    g_passMark.pull = g_totalPull.load(std::memory_order_relaxed);
    g_passMark.scatter = g_totalScatter.load(std::memory_order_relaxed);
    g_passes.fetch_add(1, std::memory_order_relaxed);
}

struct Row {
    std::string name;
    uint64_t reads = 0;
    uint64_t pull = 0;
    uint64_t bytes = 0;
    uint64_t us = 0;
};

/**
 * This thread's cumulative reads, split by purpose. Read before and after a
 * scanner's turn to get that turn's cost — the ledger's per-pass totals are
 * shared by every thread, so they cannot answer "which pass spent the bus".
 */
struct ThreadTotals {
    uint64_t reads = 0;
    uint64_t pull = 0;
    uint64_t scatter = 0;
    uint64_t bytes = 0;
    uint64_t us = 0;
    uint64_t byPurpose[kPurposeCount]{};
};

inline ThreadTotals ThreadSnapshot()
{
    ThreadTotals out;
    const int slot = CurrentSlot();
    SlotCounters& s = g_slots[slot];
    for (int p = 0; p < kPurposeCount; ++p) {
        const uint64_t pull = s.reads[0][p].load(std::memory_order_relaxed);
        const uint64_t scatter = s.reads[1][p].load(std::memory_order_relaxed);
        out.pull += pull;
        out.scatter += scatter;
        out.byPurpose[p] = pull + scatter;
        out.reads += out.byPurpose[p];
        out.bytes += s.bytes[0][p].load(std::memory_order_relaxed);
        out.bytes += s.bytes[1][p].load(std::memory_order_relaxed);
        out.us += s.us[0][p].load(std::memory_order_relaxed);
        out.us += s.us[1][p].load(std::memory_order_relaxed);
    }
    return out;
}

/** Index of the busiest purpose in a before/after pair, or -1 for none. */
inline int TopPurpose(const ThreadTotals& before, const ThreadTotals& after)
{
    int best = -1;
    uint64_t bestReads = 0;
    for (int p = 0; p < kPurposeCount; ++p) {
        const uint64_t d = after.byPurpose[p] - before.byPurpose[p];
        if (d > bestReads) {
            bestReads = d;
            best = p;
        }
    }
    return best;
}

struct PurposeDelta {
    int purpose = 0;
    uint64_t reads = 0;
};

/**
 * Every purpose that moved in a before/after pair, busiest first.
 *
 * TopPurpose() answers "which field dominates" but not "what is the rest" — and
 * a total of 36 reads per actor is only actionable once the split is visible
 * (e.g. position 18 / link 9 / name 6 / untagged 3 tells you which single field
 * to attack, in that order). Zeros are dropped; Untagged is NOT, because a
 * nonzero untagged count means a read site was never migrated and that is a bug
 * report rather than a measurement.
 */
inline size_t PurposeDeltas(const ThreadTotals& before, const ThreadTotals& after,
    PurposeDelta (&out)[kPurposeCount])
{
    size_t n = 0;
    for (int p = 0; p < kPurposeCount; ++p) {
        const uint64_t d = after.byPurpose[p] - before.byPurpose[p];
        if (d)
            out[n++] = PurposeDelta{ p, d };
    }
    // Insertion sort: <= 12 entries, and this runs once per logged turn.
    for (size_t i = 1; i < n; ++i) {
        const PurposeDelta key = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].reads < key.reads) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }
    return n;
}

// ── Executed proof that a turn's reads can be split by field ────────────────
// Run once from the shipping exe (Project.cpp) beside the other self-tests: the
// tests project cannot run here, so the invariant that scan_turn depends on —
// a scoped read lands in its OWN purpose row and the busiest row sorts first —
// is checked in-process. Hermetic: it only adds to this thread's own counters.
struct BreakdownSelfTest {
    bool passed = false;
    int cases = 0;
    int failed = 0;
    std::string detail;
};

inline BreakdownSelfTest RunBreakdownSelfTest()
{
    BreakdownSelfTest r;
    const ThreadTotals before = ThreadSnapshot();
    {
        PurposeScope p(Purpose::Bones);
        NotePull(64, 1);
        NotePull(64, 1);
    }
    {
        PurposeScope p(Purpose::Camera);
        NoteScatterBatch(3, 128, 2);   // 3 entries in one execute
    }
    const ThreadTotals after = ThreadSnapshot();

    PurposeDelta deltas[kPurposeCount]{};
    const size_t n = PurposeDeltas(before, after, deltas);
    uint64_t bones = 0, camera = 0, untagged = 0;
    for (size_t i = 0; i < n; ++i) {
        if (deltas[i].purpose == static_cast<int>(Purpose::Bones)) bones = deltas[i].reads;
        if (deltas[i].purpose == static_cast<int>(Purpose::Camera)) camera = deltas[i].reads;
        if (deltas[i].purpose == static_cast<int>(Purpose::Untagged)) untagged = deltas[i].reads;
    }

    const bool cases[4] = {
        // 2 pull + 3 batched entries, split 2 bones / 3 camera — not pooled.
        (after.reads - before.reads) == 5 && bones == 2 && camera == 3,
        // A scoped read is never charged to untagged; a nonzero untagged count
        // is the "not yet migrated" signal and must stay meaningful.
        untagged == 0,
        // Busiest first, so the top row of a scan_turn IS the answer to
        // "which field is read most per actor".
        n >= 2 && deltas[0].purpose == static_cast<int>(Purpose::Camera)
            && deltas[1].purpose == static_cast<int>(Purpose::Bones),
        std::string(PurposeName(static_cast<int>(Purpose::Link))) == "link"
            && std::string(PurposeName(static_cast<int>(Purpose::Untagged))) == "untagged",
    };
    for (const bool ok : cases) {
        ++r.cases;
        if (!ok)
            ++r.failed;
    }
    r.detail = std::string("split_totals=") + (cases[0] ? "pass " : "fail ")
        + "scoped_not_untagged=" + (cases[1] ? "pass " : "fail ")
        + "busiest_first=" + (cases[2] ? "pass " : "fail ")
        + "purpose_names=" + (cases[3] ? "pass " : "fail ");
    r.passed = (r.failed == 0);
    return r;
}

struct Snapshot {
    uint64_t passReads = 0;
    uint64_t passPull = 0;
    uint64_t passScatter = 0;
    uint64_t passBytes = 0;
    uint64_t passUs = 0;
    uint64_t totalReads = 0;
    uint64_t totalUs = 0;
    uint64_t passes = 0;
    std::vector<Row> requesters;   // busiest first
    std::vector<Row> purposes;     // busiest first
};

inline bool RowBusier(const Row& a, const Row& b) { return a.reads > b.reads; }

inline Snapshot Get(size_t maxRequesters = 6, size_t maxPurposes = 6)
{
    Snapshot out;
    const PassMark mark = g_passMark;
    const uint64_t nowReads = g_totalReads.load(std::memory_order_relaxed);
    const uint64_t nowBytes = g_totalBytes.load(std::memory_order_relaxed);
    const uint64_t nowUs = g_totalUs.load(std::memory_order_relaxed);
    const uint64_t nowPull = g_totalPull.load(std::memory_order_relaxed);
    const uint64_t nowScatter = g_totalScatter.load(std::memory_order_relaxed);

    out.passReads = nowReads - mark.reads;
    out.passBytes = nowBytes - mark.bytes;
    out.passUs = nowUs - mark.us;
    out.passPull = nowPull - mark.pull;
    out.passScatter = nowScatter - mark.scatter;
    out.totalReads = nowReads;
    out.totalUs = nowUs;
    out.passes = g_passes.load(std::memory_order_relaxed);

    const int slots = g_slotCount.load(std::memory_order_acquire);
    std::vector<Row> reqs;
    reqs.reserve(static_cast<size_t>(slots) + 1);
    for (int i = 0; i < slots; ++i) {
        if (!g_slotReady[i].load(std::memory_order_acquire))
            continue;
        Row r;
        r.name = g_slotNames[i];
        for (int p = 0; p < kPurposeCount; ++p) {
            r.reads += g_slots[i].reads[0][p].load(std::memory_order_relaxed);
            r.pull += g_slots[i].reads[0][p].load(std::memory_order_relaxed);
            r.reads += g_slots[i].reads[1][p].load(std::memory_order_relaxed);
            r.bytes += g_slots[i].bytes[0][p].load(std::memory_order_relaxed);
            r.bytes += g_slots[i].bytes[1][p].load(std::memory_order_relaxed);
            r.us += g_slots[i].us[0][p].load(std::memory_order_relaxed);
            r.us += g_slots[i].us[1][p].load(std::memory_order_relaxed);
        }
        if (r.reads)
            reqs.push_back(std::move(r));
    }
    std::sort(reqs.begin(), reqs.end(), RowBusier);
    if (reqs.size() > maxRequesters)
        reqs.resize(maxRequesters);
    out.requesters = std::move(reqs);

    std::vector<Row> purps;
    for (int p = 0; p < kPurposeCount; ++p) {
        Row r;
        r.name = PurposeName(p);
        for (int i = 0; i < slots; ++i) {
            if (!g_slotReady[i].load(std::memory_order_acquire))
                continue;
            r.reads += g_slots[i].reads[0][p].load(std::memory_order_relaxed);
            r.pull += g_slots[i].reads[0][p].load(std::memory_order_relaxed);
            r.reads += g_slots[i].reads[1][p].load(std::memory_order_relaxed);
            r.bytes += g_slots[i].bytes[0][p].load(std::memory_order_relaxed);
            r.bytes += g_slots[i].bytes[1][p].load(std::memory_order_relaxed);
            r.us += g_slots[i].us[0][p].load(std::memory_order_relaxed);
            r.us += g_slots[i].us[1][p].load(std::memory_order_relaxed);
        }
        if (r.reads)
            purps.push_back(std::move(r));
    }
    std::sort(purps.begin(), purps.end(), RowBusier);
    if (purps.size() > maxPurposes)
        purps.resize(maxPurposes);
    out.purposes = std::move(purps);

    return out;
}

} // namespace ReadBudget
