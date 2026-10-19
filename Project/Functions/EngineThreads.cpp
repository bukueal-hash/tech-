#include "../Core/Engine.h"
#include "../Core/FeaturePolicy.hpp"
#include "../Core/RunReport.hpp"
#include "../Core/AgentLog.h"
#include "../Core/ActorType.h"
#include "../Core/EntityDiagnostics.hpp"
#include "../Core/ScanGatePolicy.hpp"
#include "../Core/ReadBudget.hpp"
#include "../Core/EmbreeVisCheck.hpp"
#include "../../DMA/Memory.h"
#include "../Interface/Utils/Variables/index.h"
#include "WorldScanCommon.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

extern bool showmenu;

namespace {

// #region agent log
struct PerfPassStats {
    int lastMs = 0;
    int maxMs = 0;
};

std::mutex g_perfMu;
std::unordered_map<std::string, PerfPassStats> g_perfStats;
std::chrono::steady_clock::time_point g_lastPerfConsole{};

void LogPerfSpike(const char* threadName, int ms)
{
    {
        std::lock_guard<std::mutex> lock(g_perfMu);
        auto& s = g_perfStats[threadName];
        s.lastMs = ms;
        if (ms > s.maxMs)
            s.maxMs = ms;
    }

    if (var::show_debug_overlay) {
        const auto now = std::chrono::steady_clock::now();
        bool printConsole = false;
        {
            std::lock_guard<std::mutex> lock(g_perfMu);
            if (g_lastPerfConsole.time_since_epoch().count() == 0
                || now - g_lastPerfConsole >= std::chrono::seconds(1)) {
                g_lastPerfConsole = now;
                printConsole = true;
            }
        }
        if (printConsole) {
            std::lock_guard<std::mutex> lock(g_perfMu);
            std::cout << "[debugPerf]";
            for (auto& [name, s] : g_perfStats) {
                std::cout << " " << name << "=" << s.lastMs
                    << "(max" << s.maxMs << ")";
                s.maxMs = 0;
            }
            std::cout << std::endl;
        }
    }

    // Throttle file IO: only log spikes, and at most ~2/s per thread name.
    if (ms < 40)
        return;
    thread_local std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_lastSpike;
    const auto now = std::chrono::steady_clock::now();
    auto& last = s_lastSpike[threadName];
    if (last.time_since_epoch().count() != 0
        && now - last < std::chrono::milliseconds(500))
        return;
    last = now;

    std::ostringstream f;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"baseline\",\"hypothesisId\":\"LAG1\","
      << "\"location\":\"EngineThreads.cpp\",\"message\":\"perf_spike\","
      << "\"data\":{\"thread\":\"" << threadName << "\",\"ms\":" << ms << "}"
      << ",\"timestamp\":" << ts << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}
// #endregion

// #region cadence instrumentation
// Lightweight per-thread cadence tracker: records inter-run intervals during
// active raids and dumps a 60s summary.  One instance per measured thread;
// each is used by exactly one thread so no locking is needed.
struct CadenceStats {
    const char* name = nullptr;
    int targetMs = 0;
    std::chrono::steady_clock::time_point last_run{};
    std::chrono::steady_clock::time_point window_start{};
    int64_t minInterval = 0;
    int64_t maxInterval = 0;
    int64_t totalInterval = 0;
    int count = 0;

    void init(const char* n, int target) { name = n; targetMs = target; }

    // Call at the top of each thread tick, BEFORE the IsEspRaidActive check.
    // Only accumulates once a raid is active; the60s window starts on the first
    // active tick so idle/menu time does not dilute the sample.
    void tick() {
        const auto now = std::chrono::steady_clock::now();
        if (!last_run.time_since_epoch().count()) {
            last_run = window_start = now;  // first tick — nothing to compare
            return;
        }
        const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_run).count();
        last_run = now;
        if (interval <= 0 || interval > 10000)
            return;  // ignore bogus gaps (startup / long stall / debugger pause)
        if (!count)
            minInterval = interval;
        if (interval < minInterval) minInterval = interval;
        if (interval > maxInterval) maxInterval = interval;
        totalInterval += interval;
        ++count;

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - window_start).count();
        if (elapsed >= 60 && count > 0) {
            dump();
            reset();
        }
    }

    void dump() const {
        if (!count) return;
        const double avg = static_cast<double>(totalInterval) / count;
        const double drift = avg - targetMs;
        std::cout << "[cadence] " << name
                  << " target=" << targetMs << "ms"
                  << "  count=" << count
                  << "  min=" << minInterval << "ms"
                  << "  max=" << maxInterval << "ms"
                  << "  avg=" << static_cast<int>(avg + 0.5) << "ms"
                  << "  drift=" << (drift >= 0 ? "+" : "") << static_cast<int>(drift + 0.5) << "ms"
                  << std::endl;
        // Also write to session log for post-hoc analysis.
        std::ostringstream f;
        const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        f << "{\"sessionId\":\"c190fb\",\"runId\":\"cadence\",\"hypothesisId\":\"TC\","
          << "\"location\":\"EngineThreads.cpp\",\"message\":\"cadence_stats\","
          << "\"data\":{\"thread\":\"" << name << "\""
          << ",\"targetMs\":" << targetMs
          << ",\"count\":" << count
          << ",\"minMs\":" << minInterval
          << ",\"maxMs\":" << maxInterval
          << ",\"avgMs\":" << static_cast<int>(avg + 0.5)
          << ",\"driftMs\":" << static_cast<int>(drift + 0.5)
          << "},\"timestamp\":" << ts << "}\n";
        EntityDiagnostics::LogRaw(f.str());
    }

    void reset() {
        minInterval = 0;
        maxInterval = 0;
        totalInterval = 0;
        count = 0;
        window_start = std::chrono::steady_clock::now();
    }
};

static CadenceStats g_camCadence;
static CadenceStats g_posCadence;
// #endregion

// Scan gate: serialize the heavy scanner passes (Update, EntityList,
// RobotList, ContainerList/ItemList) so only one touches the DMA bus at a
// time. Latency-critical passes (PositionRefreshPass, FrameBuilder, Aim)
// stay ungated so positions/camera never wait behind a long scan.
std::atomic<int> g_scanGateWaiters{0};
std::atomic<const char*> g_scanGateHolder{nullptr};

// #region agent log
// LogScanGate: emitted when a gated scan completes.
//   waitMs  — gate contention only (mutex-blocked time, NOT including the
//             enforced 12ms idle gap).  A waitMs < 5 means zero real contention;
//             the gap sleep itself is never counted in waitMs.
//   heldMs  — scan execution time (the fn() call after the gap).
//   waiters — current queue depth at log time (how many scanners were still
//             queued behind the gate when fn() finished).
//   blockedBy — the scan name that held the gate when this scanner first tried
//             to acquire it; null when no one was holding it.
void LogScanGate(const char* scanner, int waitMs, int heldMs, int waiters, const char* blockedBy)
{
    if (waitMs < 5)
        return;
    thread_local std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_lastGate;
    const auto now = std::chrono::steady_clock::now();
    auto& last = s_lastGate[scanner];
    if (last.time_since_epoch().count() != 0
        && now - last < std::chrono::milliseconds(500))
        return;
    last = now;

    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << "{\"sessionId\":\"c190fb\",\"runId\":\"scan-gate\",\"hypothesisId\":\"OV1\","
      << "\"location\":\"EngineThreads.cpp:gate\",\"message\":\"scan_gate\","
      << "\"data\":{\"scanner\":\"" << scanner << "\",\"waitMs\":" << waitMs
      << ",\"heldMs\":" << heldMs
      << ",\"waiters\":" << waiters
      << ",\"blockedBy\":\"" << (blockedBy ? blockedBy : "") << "\"}"
      << ",\"timestamp\":" << ts << "}\n";
    EntityDiagnostics::LogRaw(oss.str());
}
// #endregion

// N1: enforced idle window between scanner turns. The gate made scans
// take turns, but back-to-back turns still occupied the DMA bus ~100% of
// the time and starved the ungated latency passes (pos refresh spiked to
// 609ms). Each scanner must leave this much bus idle before starting.
//
// Semantics (important for reading LogScanGate):
//   waitMs  = time blocked on the gate mutex ONLY (real contention).
//   heldMs  = scan duration after the gap (fn() execution time).
//   gapMs   = enforced idle (0..12ms) between previous release and this scan.
//             Excluded from waitMs; it is not contention — it is the throttle.
//             A new scanner always sleeps (t_acquired → t0) up to kGateIdleGap
//             if the previous release was too recent, regardless of the holder.
//   A waitMs ≥ 5ms logged with blockedBy="" means the mutex was free but the
//             gap sleep fired — NOT a contention event (threshold filters it).
//   A waitMs ≫ 12ms with non-empty blockedBy = real gate queueing.
constexpr auto kGateIdleGap = std::chrono::milliseconds(12);

// #region agent log
// LogGateTurn (verify log): one line per scanner turn, throttled 250ms per
// scanner. sinceMs is the start-to-start gap — the number that answers "is
// RobotList actually running at its cadence".
void LogGateTurn(const char* scanner, int cadenceMs, int waitMs, int heldMs,
                 int gapMs, int64_t sinceMs)
{
    thread_local std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_lastTurn;
    const auto now = std::chrono::steady_clock::now();
    auto& last = s_lastTurn[scanner];
    if (last.time_since_epoch().count() != 0
        && now - last < std::chrono::milliseconds(250))
        return;
    last = now;

    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream oss;
    oss << "{\"sessionId\":\"c190fb\",\"runId\":\"gate\","
      << "\"location\":\"EngineThreads.cpp:gate\",\"message\":\"gate_turn\","
      << "\"data\":{\"scanner\":\"" << scanner << "\",\"cadenceMs\":" << cadenceMs
      << ",\"waitMs\":" << waitMs
      << ",\"heldMs\":" << heldMs
      << ",\"gapMs\":" << gapMs
      << ",\"sinceMs\":" << sinceMs << "}"
      << ",\"timestamp\":" << ts << "}\n";
    EntityDiagnostics::LogRaw(oss.str());
}
// #endregion

// ── Turning "who waits" into "what the player sees" ────────────────────────
// The scan-time record (perf_spike) from a 1330-actor raid: Update p50 498 ms,
// EntityList 665 ms, RobotList 681 ms, ContainerList 254 ms, ItemList 401 ms —
// ~2.4 s of DMA work per gate cycle, so every consumer, ESP included, refreshed
// roughly once every 2.5 s. These constants are the split that gives the bus
// back to the passes whose staleness is visible on screen.
constexpr int64_t kDutyWindowMs = 2000;      // window the duty estimate covers
constexpr int kDeferLoadPermille = 850;      // bus above this = saturated
constexpr int kDeferCadenceFactor = 3;       // deferrable passes slow by this
constexpr int64_t kDeferMinIntervalMs = 1200; // ...and never more often than this
constexpr int kMaxEffectiveCadenceMs = 1000; // a pass still gets one turn/s
constexpr int kTurnLogMinHeldMs = 80;        // scan_turn: held-time threshold
constexpr int kTurnLogMinWaitMs = 150;       // scan_turn: wait-time threshold
constexpr int64_t kCycleReportMs = 5000;     // scan_cycle cadence

/** Rounded percentile of a sample vector (0.5 = median). */
inline int Percentile(std::vector<int>& samples, double p)
{
    if (samples.empty())
        return 0;
    std::sort(samples.begin(), samples.end());
    const size_t i = static_cast<size_t>(
        p * static_cast<double>(samples.size() - 1) + 0.5);
    return samples[i];
}

inline int64_t SteadyNowMs()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// #region agent log
// LogScanTurn: what ONE pass actually spent on the bus. scan_gate reports
// contention and gate_turn reports pacing, but neither could answer "which
// pass burned the bus", because the read ledger's per-pass totals are shared
// by every thread. This carries the reading thread's own before/after delta, so
// the cost of a turn is attributable to the pass that caused it.
void LogScanTurn(const char* scanner, int cadenceMs, int effCadenceMs,
    int waitMs, int heldMs, int64_t sinceMs,
    const ReadBudget::ThreadTotals& before, const ReadBudget::ThreadTotals& after)
{
    if (heldMs < kTurnLogMinHeldMs && waitMs < kTurnLogMinWaitMs)
        return;
    thread_local std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_lastTurn;
    const auto now = std::chrono::steady_clock::now();
    auto& last = s_lastTurn[scanner];
    if (last.time_since_epoch().count() != 0
        && now - last < std::chrono::milliseconds(500))
        return;
    last = now;

    const uint64_t reads = after.reads - before.reads;
    const uint64_t pull = after.pull - before.pull;
    const uint64_t scatter = after.scatter - before.scatter;
    const uint64_t bytes = after.bytes - before.bytes;
    const uint64_t us = after.us - before.us;
    const int top = ReadBudget::TopPurpose(before, after);

    // The split, not just the total. "reads": 36 per actor is only actionable
    // once it is broken down by field, so every purpose that moved is reported
    // busiest-first. untaggedReads is called out separately because it is the
    // count of reads whose site has no PurposeScope yet.
    ReadBudget::PurposeDelta deltas[ReadBudget::kPurposeCount]{};
    const size_t deltaCount = ReadBudget::PurposeDeltas(before, after, deltas);
    uint64_t untagged = 0;
    for (size_t i = 0; i < deltaCount; ++i) {
        if (static_cast<ReadBudget::Purpose>(deltas[i].purpose)
            == ReadBudget::Purpose::Untagged)
            untagged = deltas[i].reads;
    }

    std::ostringstream oss;
    oss << "{\"sessionId\":\"c190fb\",\"runId\":\"scan-turn\",\"hypothesisId\":\"LG2\","
      << "\"location\":\"EngineThreads.cpp:gate\",\"message\":\"scan_turn\","
      << "\"data\":{\"scanner\":\"" << scanner << "\""
      << ",\"cadenceMs\":" << cadenceMs
      << ",\"effCadenceMs\":" << effCadenceMs
      << ",\"waitMs\":" << waitMs
      << ",\"heldMs\":" << heldMs
      << ",\"sinceMs\":" << sinceMs
      << ",\"reads\":" << reads
      << ",\"pull\":" << pull
      << ",\"scatter\":" << scatter
      << ",\"bytes\":" << bytes
      << ",\"readUs\":" << us
      << ",\"topPurpose\":\"" << (top >= 0 ? ReadBudget::PurposeName(top) : "none") << "\""
      << ",\"topPurposeReads\":"
      << ((deltaCount && top >= 0) ? deltas[0].reads : 0)
      << ",\"untaggedReads\":" << untagged
      << ",\"purposeCount\":" << deltaCount
      << ",\"purposes\":{";
    for (size_t i = 0; i < deltaCount; ++i) {
        if (i)
            oss << ",";
        oss << "\"" << ReadBudget::PurposeName(deltas[i].purpose) << "\":"
            << deltas[i].reads;
    }
    oss << "}}"
      << ",\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count()
      << "}\n";
    EntityDiagnostics::LogRaw(oss.str());
}
// #endregion

// Fair scan gate: one scan on the DMA bus at a time, but turn grants follow
// ScanGatePolicy instead of raw mutex order. The old std::mutex let the 16ms
// Update thread barge RobotList's 200ms admission pass into multi-second
// queues; the policy bounds every scanner's wait (~2x its cadence) and
// otherwise runs whoever is most due.
class ScanGate {
public:
    template <typename Fn>
    void Run(const char* scanner, int cadenceMs, bool deferrable, Fn&& fn)
    {
        // A deferrable pass (loot) stands down while the bus is saturated, so
        // the gate is left to the passes whose staleness the player actually
        // sees: the player/bot roster. Counted, never silent — scan_cycle
        // reports the deferrals per pass.
        if (deferrable && ShouldDefer(scanner))
            return;

        const auto w0 = std::chrono::steady_clock::now();
        const char* blockedBy = nullptr;
        int64_t sinceMs = -1;
        int gapMs = 0;
        int effCadenceMs = cadenceMs;
        std::chrono::steady_clock::time_point t_acquired;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            blockedBy = m_holder;
            effCadenceMs = EffectiveCadenceLocked(scanner, cadenceMs, deferrable);
            const auto lastStart = LastStartLocked(scanner);
            if (lastStart.time_since_epoch().count() != 0)
                sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    w0 - lastStart).count();
            m_queue.push_back({ scanner, effCadenceMs, w0, lastStart });
            g_scanGateWaiters.fetch_add(1, std::memory_order_relaxed);
            m_cv.wait(lock, [&] { return !m_busy && IsMyTurnLocked(scanner); });
            g_scanGateWaiters.fetch_sub(1, std::memory_order_relaxed);
            t_acquired = std::chrono::steady_clock::now();
            // N1 idle gap: the bus must rest between turns. Enforced under the
            // lock so no other scan fills the gap.
            if (m_lastRelease.time_since_epoch().count() != 0) {
                const auto sinceRelease = t_acquired - m_lastRelease;
                if (sinceRelease < kGateIdleGap) {
                    std::this_thread::sleep_for(kGateIdleGap - sinceRelease);
                    gapMs = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t_acquired).count());
                }
            }
            RemoveLocked(scanner);
            // The map update can allocate/throw; do it before publishing a
            // busy gate that would otherwise never reach the release guard.
            m_lastStarts[scanner] = std::chrono::steady_clock::now();
            m_busy = true;
            m_holder = scanner;
        }

        // Release on both normal completion and exception unwinding. The
        // worker's outer exception handler must never inherit a busy gate.
        struct TurnRelease {
            ScanGate& gate;
            bool active = true;
            void Release() {
                if (active) {
                    gate.ReleaseTurn();
                    active = false;
                }
            }
            ~TurnRelease() { Release(); }
        } release{ *this };
        const auto t0 = std::chrono::steady_clock::now();
        g_scanGateHolder.store(scanner, std::memory_order_relaxed);
        // This thread's own ledger counters, so the turn's reads are attributed
        // to this pass rather than to the engine-wide per-frame total.
        const ReadBudget::ThreadTotals readBefore = ReadBudget::ThreadSnapshot();
        fn();
        const ReadBudget::ThreadTotals readAfter = ReadBudget::ThreadSnapshot();
        const auto t1 = std::chrono::steady_clock::now();
        release.Release();

        // #region agent log
        const int waitMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t_acquired - w0).count());
        const int heldMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        LogScanGate(scanner, waitMs, heldMs,
            g_scanGateWaiters.load(std::memory_order_relaxed), blockedBy);
        LogGateTurn(scanner, effCadenceMs, waitMs, heldMs, gapMs, sinceMs);
        LogScanTurn(scanner, cadenceMs, effCadenceMs, waitMs, heldMs, sinceMs,
            readBefore, readAfter);
        // #endregion
        NoteTurn(scanner, effCadenceMs, heldMs);
    }

private:
    void ReleaseTurn()
    {
        g_scanGateHolder.store(nullptr, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_lastRelease = std::chrono::steady_clock::now();
            m_busy = false;
            m_holder = nullptr;
        }
        m_cv.notify_all();
    }

    // Per-pass accounting for the cycle record. Bounded: the names are the
    // scanner literals, and the sample vectors are cleared every report.
    struct PassStats {
        const char* name = nullptr;
        int cadenceMs = 0;
        int lastHeldMs = 0;
        int64_t lastEndMs = 0;
        int turns = 0;
        int deferred = 0;
        std::vector<int> held;
        std::vector<int> intervals;
    };

    PassStats* FindStatsLocked(const char* scanner)
    {
        for (PassStats& s : m_stats) {
            if (std::strcmp(s.name, scanner) == 0)
                return &s;
        }
        return nullptr;
    }

    PassStats& StatsLocked(const char* scanner, int cadenceMs)
    {
        if (PassStats* found = FindStatsLocked(scanner))
            return *found;
        if (m_stats.size() >= kMaxPassStats)
            return m_stats.back();   // never expected; bounded regardless
        m_stats.push_back(PassStats{});
        PassStats& s = m_stats.back();
        s.name = scanner;
        s.cadenceMs = cadenceMs;
        return s;
    }

    /**
     * Fraction of recent wall time the bus spent holding a turn, in permille.
     * This is the number the priority split keys off: a pass cannot be given
     * the bus "more often" by policy alone — something else has to want it
     * less, and this is the measurement of how much contention there is.
     */
    int BusyPermilleLocked() const
    {
        if (m_recentTurns.size() < 4)
            return 0;
        const int64_t nowMs = SteadyNowMs();
        int64_t busyMs = 0;
        int64_t firstStartMs = nowMs;
        for (const std::pair<int64_t, int>& t : m_recentTurns) {
            if (t.first < nowMs - kDutyWindowMs)
                continue;
            busyMs += t.second;
            if (t.first - t.second < firstStartMs)
                firstStartMs = t.first - t.second;
        }
        const int64_t span = nowMs - firstStartMs;
        if (span <= 0)
            return 0;
        const int64_t permille = busyMs * 1000 / span;
        return static_cast<int>((std::min)(permille, int64_t(1000)));
    }

    /**
     * The cadence a pass is REQUESTED at. Two corrections, both measured:
     *  1. A pass cannot turn faster than it executes. Update asked for 16 ms
     *     while taking 498 ms, so it was permanently due and every turn it won
     *     was a turn the 665 ms EntityList pass did not get.
     *  2. Under saturation a deferrable pass asks less often, which is the only
     *     way to hand bus time to the ESP passes without starving community
     *     data outright.
     */
    int EffectiveCadenceLocked(const char* scanner, int cadenceMs, bool deferrable)
    {
        int base = cadenceMs;
        if (const PassStats* s = FindStatsLocked(scanner); s && s->lastHeldMs > base)
            base = s->lastHeldMs;
        if (deferrable && BusyPermilleLocked() >= kDeferLoadPermille)
            base *= kDeferCadenceFactor;
        return (std::min)(base, kMaxEffectiveCadenceMs);
    }

    bool ShouldDefer(const char* scanner)
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (BusyPermilleLocked() < kDeferLoadPermille)
            return false;
        PassStats* s = FindStatsLocked(scanner);
        const int64_t nowMs = SteadyNowMs();
        if (s && s->lastEndMs != 0 && nowMs - s->lastEndMs < kDeferMinIntervalMs) {
            ++s->deferred;
            return true;
        }
        return false;
    }

    void NoteTurn(const char* scanner, int effCadenceMs, int heldMs)
    {
        std::lock_guard<std::mutex> lock(m_mu);
        const int64_t nowMs = SteadyNowMs();
        PassStats& s = StatsLocked(scanner, effCadenceMs);
        if (s.lastEndMs != 0)
            s.intervals.push_back(static_cast<int>(nowMs - s.lastEndMs));
        s.lastHeldMs = heldMs;
        s.lastEndMs = nowMs;
        s.cadenceMs = effCadenceMs;
        ++s.turns;
        s.held.push_back(heldMs);
        m_recentTurns.push_back({ nowMs, heldMs });
        while (m_recentTurns.size() > kDutyWindowSamples)
            m_recentTurns.pop_front();

        // #region agent log
        // LogScanCycle: one line every kCycleReportMs that answers, for the pass
        // the raid was actually slow in: how often each pass turned, how long it
        // held, how stale it got (interval), how much it was told to stand down,
        // and how much the type memo absorbed. Built under the lock on purpose —
        // it runs at 0.2 Hz, and copying the sample vectors out to format them
        // unlocked would cost more than the string does.
        if (m_cycleStartMs == 0)
            m_cycleStartMs = nowMs;
        if (nowMs - m_cycleStartMs < kCycleReportMs) {
            return;
        }
        uint64_t memoHits = 0;
        uint64_t memoMisses = 0;
        ArcActorType::TypeMemoStats(memoHits, memoMisses);
        const int64_t sysNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ostringstream oss;
        oss << "{\"sessionId\":\"c190fb\",\"runId\":\"scan-cycle\","
            << "\"hypothesisId\":\"LG2\","
            << "\"location\":\"EngineThreads.cpp:gate\",\"message\":\"scan_cycle\","
            << "\"data\":{\"windowMs\":" << (nowMs - m_cycleStartMs)
            << ",\"busyPermille\":" << BusyPermilleLocked()
            << ",\"typeMemo\":{\"hits\":" << memoHits
            << ",\"misses\":" << memoMisses << "}"
            << ",\"passes\":[";
        bool first = true;
        for (PassStats& p : m_stats) {
            if (!p.turns && !p.deferred)
                continue;
            if (!first)
                oss << ',';
            first = false;
            oss << "{\"name\":\"" << p.name << "\""
                << ",\"cadenceMs\":" << p.cadenceMs
                << ",\"turns\":" << p.turns
                << ",\"heldP50Ms\":" << Percentile(p.held, 0.5)
                << ",\"heldMaxMs\":" << Percentile(p.held, 1.0)
                << ",\"intervalP50Ms\":" << Percentile(p.intervals, 0.5)
                << ",\"intervalMaxMs\":" << Percentile(p.intervals, 1.0)
                << ",\"deferred\":" << p.deferred << '}';
        }
        // Closes the passes array, then data, then the record — and carries the
        // wall-clock stamp the other records use, so this line lands in the same
        // time-ordered view as gate_turn and perf_spike instead of sorting to 0.
        oss << "]},\"timestamp\":" << sysNowMs << "}\n";
        EntityDiagnostics::LogRaw(oss.str());
        for (PassStats& p : m_stats) {
            p.held.clear();
            p.intervals.clear();
            p.turns = 0;
            p.deferred = 0;
        }
        m_cycleStartMs = nowMs;
        // #endregion
    }

    struct Waiter {
        const char* name;
        int cadenceMs;
        std::chrono::steady_clock::time_point enqueue;
        std::chrono::steady_clock::time_point lastStart;
    };

    std::chrono::steady_clock::time_point LastStartLocked(const char* scanner) const
    {
        const auto it = m_lastStarts.find(scanner);
        return it == m_lastStarts.end()
            ? std::chrono::steady_clock::time_point{}
            : it->second;
    }

    bool IsMyTurnLocked(const char* scanner) const
    {
        if (m_queue.empty())
            return false;
        std::vector<ScanGatePolicy::Candidate> cands;
        cands.reserve(m_queue.size());
        for (const Waiter& w : m_queue) {
            ScanGatePolicy::Candidate c;
            c.cadenceMs = w.cadenceMs;
            c.lastStartMs = (w.lastStart.time_since_epoch().count() == 0)
                ? ScanGatePolicy::Candidate::kNeverRan()
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      w.lastStart.time_since_epoch()).count();
            c.enqueueMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                w.enqueue.time_since_epoch()).count();
            cands.push_back(c);
        }
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const int pick = ScanGatePolicy::PickNext(cands, nowMs);
        return pick >= 0
            && std::strcmp(m_queue[static_cast<size_t>(pick)].name, scanner) == 0;
    }

    void RemoveLocked(const char* scanner)
    {
        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (std::strcmp(it->name, scanner) == 0) {
                m_queue.erase(it);
                return;
            }
        }
    }

    static constexpr size_t kMaxPassStats = 8;
    static constexpr size_t kDutyWindowSamples = 64;

    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_busy = false;
    const char* m_holder = nullptr;
    std::vector<Waiter> m_queue;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastStarts;
    std::chrono::steady_clock::time_point m_lastRelease{};
    std::vector<PassStats> m_stats;
    std::deque<std::pair<int64_t, int>> m_recentTurns;   // (endMs, heldMs)
    int64_t m_cycleStartMs = 0;
};

static ScanGate g_scanGate;

// deferrable: this pass may stand down while the bus is saturated. True only
// for the passes whose staleness the player does not see on screen (loot).
// Skipping an ESP pass would make the symptom worse, so those never defer.
template <typename Fn>
void RunGatedScan(const char* scanner, int cadenceMs, bool deferrable, Fn&& fn)
{
    // Name this worker in the DMA read ledger. Every gated scan thread funnels
    // through here, so requester attribution covers the whole engine without a
    // per-thread edit, and the name is the same one the scan gate reports.
    ReadBudget::SetCurrentRequester(scanner);
    g_scanGate.Run(scanner, cadenceMs, deferrable, fn);
}

// Scanner cadences: single source for the SyncedThread pacing AND the scan
// gate's fairness policy so the two cannot drift apart.
constexpr int kUpdateCadenceMs = 16;
constexpr int kEntityCadenceMs = 220;
constexpr int kRobotCadenceMs = 120;
constexpr int kContainerCadenceMs = 250;
constexpr int kItemCadenceMs = 250;

} // namespace

void Engine::LogEntityDiagnostics()
{
    using namespace EntityDiagnostics;

    const auto now = std::chrono::steady_clock::now();
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
    size_t cursors[4]{};
    CategorySummary players;
    CategorySummary bots;
    CategorySummary containers;
    CategorySummary items;
    players.kind = "player";
    bots.kind = "bot";
    containers.kind = "container";
    items.kind = "item";

    const auto emitSamples = [&](const char* kind, auto& cache,
        size_t& cursor, CategorySummary& summary) {
        using Entry = typename std::decay_t<decltype(cache)>::mapped_type;
        constexpr bool isPlayer = std::is_same_v<Entry, PlayerCacheEntry>;
        std::shared_lock<std::shared_mutex> lock(
            isPlayer ? m_playerCacheMutex
                : std::is_same_v<Entry, WorldCacheEntry> && kind[0] == 'b'
                    ? m_robotCacheMutex
                    : kind[0] == 'c' ? m_containerCacheMutex : m_itemCacheMutex);
        summary.total = cache.size();
        if (cache.empty())
            return;

        constexpr size_t kMaxSamplesPerKind = 32;
        const size_t size = cache.size();
        const size_t start = cursor % size;
        size_t emitted = 0;
        for (size_t step = 0; step < size && emitted < kMaxSamplesPerKind; ++step) {
            auto it = cache.begin();
            std::advance(it, static_cast<std::ptrdiff_t>((start + step) % size));
            const uintptr_t key = it->first;
            const Entry& entry = it->second;
            if (entry.Drawing)
                ++summary.drawing;

            const bool positionValid = IsPlausiblePosition(
                static_cast<float>(entry.WorldPos.x),
                static_cast<float>(entry.WorldPos.y),
                static_cast<float>(entry.WorldPos.z));
            const uint64_t sampledAt = entry.positionSampleMs;
            const uint64_t age = sampledAt && nowMs > sampledAt ? nowMs - sampledAt : 0;
            const bool initialized = [&]() {
                if constexpr (isPlayer)
                    return entry.positionInitialized;
                else
                    return sampledAt != 0 || positionValid;
            }();
            const uint32_t issues = BuildIssueFlags(
                !entry.ActorName.empty(), positionValid, initialized,
                entry.Drawing, entry.health, entry.maxhealth, age);
            if (issues)
                ++summary.issues;

            if (emitted >= kMaxSamplesPerKind)
                break;
            EntitySample sample;
            sample.kind = kind;
            sample.actorKey = key;
            sample.name = entry.ActorName;
            if constexpr (isPlayer) {
                sample.detail = entry.weaponName;
                sample.category = entry.statusFlags;
            } else {
                sample.detail = entry.ItemType;
                if (sample.detail.empty())
                    sample.detail = entry.weaponName;
                sample.category = entry.worldCategory;
            }
            sample.distance = entry.Distance;
            sample.health = entry.health;
            sample.maxHealth = entry.maxhealth;
            sample.x = static_cast<float>(entry.WorldPos.x);
            sample.y = static_cast<float>(entry.WorldPos.y);
            sample.z = static_cast<float>(entry.WorldPos.z);
            sample.positionAgeMs = age;
            sample.issueFlags = issues;
            sample.drawing = entry.Drawing;
            sample.visible = entry.isVisible;
            sample.positionInitialized = initialized;
            LogEntity(sample);
            ++emitted;
        }
        cursor = start + emitted;
    };

    emitSamples("player", playerCache, cursors[0], players);
    emitSamples("bot", robotCache, cursors[1], bots);
    emitSamples("container", containerCache, cursors[2], containers);
    emitSamples("item", itemCache, cursors[3], items);

    std::ostringstream features;
    features << "world=" << (var::enable_world && var::showLoot)
        << ",playerEsp=" << var::enableesp
        << ",botEsp=" << var::showRobots
        << ",radar=" << var::show_radar
        << ",loot=" << (var::raiderStock || var::droppedItems
            || var::show_world_items || var::show_world_crate);
    LogSummary("live", features.str(), players, bots, containers, items,
        m_worldGeneration.load(std::memory_order_acquire));
}

void Engine::StartWorkerThreads()
{
    if (m_workerThreadsStarted.exchange(true))
        return;

    // Initialize callback-owned telemetry before any constructor launches a worker.
    g_camCadence.init("camera", 8);
    g_posCadence.init("position", 16);

    // DMA throttle: slower periods cut PCIe/FPGA "packet loss" under load.
    // Frame velocity extrapolate still bridges gaps; aim 8 ms is plenty for kmbox.
    m_worldThread = std::make_unique<SyncedThread>([this] {
        // #region agent log
        const auto t0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("Update", kUpdateCadenceMs, false, [this] { Update(); });
        // #region agent log
        LogPerfSpike("Update", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()));
        // #endregion
    }, kUpdateCadenceMs);
    m_entityThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        // #region agent log
        const auto t0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("EntityList", kEntityCadenceMs, false, [this] { EntityList(); });
        // #region agent log
        LogPerfSpike("EntityList", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()));
        // #endregion
    }, kEntityCadenceMs);
    // Phase 1.5: RobotList was the worst LAG1 offender (baseline ~622ms avg /
    // 1267ms max). Positions stay on PositionRefreshPass @16ms; lengthen this
    // admission/visual pass so FPGA bus contention drops. 200ms cadence + the
    // 90ms ScanBudget inside RobotList() leaves the DMA link to camera/position/
    // frame-builder between bursts.
    m_robotEspThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        // #region agent log
        const auto t0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("RobotList", kRobotCadenceMs, false, [this] { RobotList(); });
        // #region agent log
        LogPerfSpike("RobotList", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()));
        // #endregion
    }, kRobotCadenceMs);
    m_worldEspThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        // #region agent log
        const auto t0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("ContainerList", kContainerCadenceMs, true, [this] { ContainerList(); });
        // #region agent log
        LogPerfSpike("ContainerList", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()));
        const auto tItem0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("ItemList", kItemCadenceMs, true, [this] { ItemList(); });
        // #region agent log
        LogPerfSpike("ItemList", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tItem0).count()));
        // #endregion
    }, kContainerCadenceMs);
    // CAM2 (Fix #3): camera shared m_positionThread with PositionRefreshPass,
    // whose 100-200ms scatter stalls delayed the next UpdateCamera by the same
    // amount (cam_refresh_gap 200-560ms every 3s window). A stale projection
    // POV makes every box/name jump-blink on rotation even though scanners are
    // steady — the flicker the user sees that flicker_score channels 0-2 miss.
    // Give the camera its own tiny ungated thread: one small read @8ms.
    m_cameraThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        g_camCadence.tick();
        UpdateCamera();
    }, 8);
    m_cameraThread->set_priority(THREAD_PRIORITY_HIGHEST);
    m_positionThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        g_posCadence.tick();
        const auto t0 = std::chrono::steady_clock::now();
        PositionRefreshPass();
        const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        LogPerfSpike("PositionRefreshPass", ms);
    }, 16);
    m_frameBuilderThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        const auto t0 = std::chrono::steady_clock::now();
        BuildEspRenderFrameWorker();
        static std::chrono::steady_clock::time_point s_lastEntityDiagnostics{};
        if (s_lastEntityDiagnostics.time_since_epoch().count() == 0
            || std::chrono::steady_clock::now() - s_lastEntityDiagnostics
                >= std::chrono::seconds(2)) {
            s_lastEntityDiagnostics = std::chrono::steady_clock::now();
            LogEntityDiagnostics();
        }
        const int totalMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        LogPerfSpike("FrameBuilder", totalMs);
    }, 12);
    m_visThread = std::make_unique<SyncedThread>([this] {
        // Build stages: 0 = near-player scene (fast first ready), 1 = full map, 2 = done.
        static uint64_t s_lastWorldGen = 0;
        static int s_buildStage = 0;
        static auto s_nextBuildAt = std::chrono::steady_clock::time_point{};
        const uint64_t curGen = m_worldGeneration.load(std::memory_order_acquire);
        if (curGen != s_lastWorldGen) {
            s_lastWorldGen = curGen;
            s_buildStage = 0;
            s_nextBuildAt = {};
            EmbreeVis::ResetVisibilityWorld();
        }
        if (!IsEspRaidActive() || !UpgradeOptions::visCheckPerBone)
            return;
        ReadBudget::SetCurrentRequester("Visibility");
        uintptr_t gw = 0;
        {
            std::shared_lock<std::shared_mutex> lk(m_stateMutex);
            gw = GWorld;
        }
        if (!gw)
            return;
        CameraCache cam{};
        {
            std::shared_lock<std::shared_mutex> lk(m_cameraMutex);
            cam = g_Camera;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool ready = EmbreeVis::g_visibility_meshes_ready.load(std::memory_order_acquire);
        if (s_buildStage == 2 && !ready) s_buildStage = 0; /* a rebuild request clears ready: start over */
        if (s_buildStage != 2 && (!ready || s_buildStage == 1) && now >= s_nextBuildAt) {
            const uint32_t myGen = EmbreeVis::g_vis_generation.load(std::memory_order_acquire);
            const bool nearPass = (s_buildStage == 0);
            // Near pass: only actors within 150 m (Unreal units are cm).
            const float range = nearPass ? 15000.0f : 0.0f;
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = EmbreeVis::CollectVisibilityMeshesFromWorld(gw, cam.Location, range, myGen);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (ok) {
                std::printf("[VISIBILITY] %s build took %lld ms\n", nearPass ? "near" : "full", (long long)ms);
                EmbreeVis::g_visibility_meshes_ready.store(true, std::memory_order_release);
                s_buildStage = nearPass ? 1 : 2;
            } else if (nearPass) {
                // No meshes within range: fall back to the full map right away.
                s_buildStage = 1;
            } else {
                // Full map not usable yet (still loading). Wait instead of rescanning every tick.
                s_nextBuildAt = now + std::chrono::milliseconds(1500);
            }
        }
        if (UpgradeOptions::visibilityObstructionSpheres || UpgradeOptions::visDebugObstructions) {
            EmbreeVis::CacheObstructions(gw);
        }
    }, 50);
}

void Engine::StopWorkerThreads()
{
    if (!m_workerThreadsStarted.exchange(false))
        return;

    // Stop the scheduler that can submit background jobs before joining the
    // other workers. This prevents a world/ESP pass from racing shutdown.
    m_visThread.reset();
    m_worldThread.reset();
    m_entityThread.reset();
    m_robotEspThread.reset();
    m_worldEspThread.reset();
    m_frameBuilderThread.reset();
    m_cameraThread.reset();
    m_positionThread.reset();
    
    // All tracker writers are joined; aggregation cannot race tick().
    g_camCadence.dump();
    g_posCadence.dump();

    // All engine workers are now stopped, so no new background job can be
    // submitted. Join diagnostic/rebuild jobs before destroying dependencies.
    WorldScan::StopBackgroundJobs();
}
