#include "../Core/Engine.h"
#include "../Core/AgentLog.h"
#include "../Core/EntityDiagnostics.hpp"
#include "../Core/ScanGatePolicy.hpp"
#include "../../DMA/Memory.h"
#include "../Interface/Utils/Variables/index.h"
#include "CollisionMirror.h"
#include "WorldScanCommon.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
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

    std::ofstream f(kArcDebugLogPath, std::ios::app);
    if (!f)
        return;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"baseline\",\"hypothesisId\":\"LAG1\","
      << "\"location\":\"EngineThreads.cpp\",\"message\":\"perf_spike\","
      << "\"data\":{\"thread\":\"" << threadName << "\",\"ms\":" << ms << "}"
      << ",\"timestamp\":" << ts << "}\n";
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
        std::ofstream f(kArcDebugLogPath, std::ios::app);
        if (f) {
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
        }
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
static CadenceStats g_aimCadence;
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

    std::ofstream f(kArcDebugLogPath, std::ios::app);
    if (!f)
        return;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"scan-gate\",\"hypothesisId\":\"OV1\","
      << "\"location\":\"EngineThreads.cpp:gate\",\"message\":\"scan_gate\","
      << "\"data\":{\"scanner\":\"" << scanner << "\",\"waitMs\":" << waitMs
      << ",\"heldMs\":" << heldMs
      << ",\"waiters\":" << waiters
      << ",\"blockedBy\":\"" << (blockedBy ? blockedBy : "") << "\"}"
      << ",\"timestamp\":" << ts << "}\n";
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

    std::ofstream f(kArcVerifyPath, std::ios::app);
    if (!f)
        return;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"gate\","
      << "\"location\":\"EngineThreads.cpp:gate\",\"message\":\"gate_turn\","
      << "\"data\":{\"scanner\":\"" << scanner << "\",\"cadenceMs\":" << cadenceMs
      << ",\"waitMs\":" << waitMs
      << ",\"heldMs\":" << heldMs
      << ",\"gapMs\":" << gapMs
      << ",\"sinceMs\":" << sinceMs << "}"
      << ",\"timestamp\":" << ts << "}\n";
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
    void Run(const char* scanner, int cadenceMs, Fn&& fn)
    {
        const auto w0 = std::chrono::steady_clock::now();
        const char* blockedBy = nullptr;
        int64_t sinceMs = -1;
        int gapMs = 0;
        std::chrono::steady_clock::time_point t_acquired;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            blockedBy = m_holder;
            const auto lastStart = LastStartLocked(scanner);
            if (lastStart.time_since_epoch().count() != 0)
                sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    w0 - lastStart).count();
            m_queue.push_back({ scanner, cadenceMs, w0, lastStart });
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
            m_busy = true;
            m_holder = scanner;
            m_lastStarts[scanner] = std::chrono::steady_clock::now();
        }

        const auto t0 = std::chrono::steady_clock::now();
        g_scanGateHolder.store(scanner, std::memory_order_relaxed);
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        g_scanGateHolder.store(nullptr, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_lastRelease = std::chrono::steady_clock::now();
            m_busy = false;
            m_holder = nullptr;
        }
        m_cv.notify_all();

        // #region agent log
        const int waitMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t_acquired - w0).count());
        const int heldMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        LogScanGate(scanner, waitMs, heldMs,
            g_scanGateWaiters.load(std::memory_order_relaxed), blockedBy);
        LogGateTurn(scanner, cadenceMs, waitMs, heldMs, gapMs, sinceMs);
        // #endregion
    }

private:
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

    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    bool m_busy = false;
    const char* m_holder = nullptr;
    std::vector<Waiter> m_queue;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastStarts;
    std::chrono::steady_clock::time_point m_lastRelease{};
};

static ScanGate g_scanGate;

template <typename Fn>
void RunGatedScan(const char* scanner, int cadenceMs, Fn&& fn)
{
    g_scanGate.Run(scanner, cadenceMs, fn);
}

// Scanner cadences: single source for the SyncedThread pacing AND the scan
// gate's fairness policy so the two cannot drift apart.
constexpr int kUpdateCadenceMs = 16;
constexpr int kEntityCadenceMs = 220;
constexpr int kRobotCadenceMs = 200;
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

    // DMA throttle: slower periods cut PCIe/FPGA "packet loss" under load.
    // Frame velocity extrapolate still bridges gaps; aim 8 ms is plenty for kmbox.
    m_worldThread = std::make_unique<SyncedThread>([this] {
        // #region agent log
        const auto t0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("Update", kUpdateCadenceMs, [this] { Update(); });
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
        RunGatedScan("EntityList", kEntityCadenceMs, [this] { EntityList(); });
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
        RunGatedScan("RobotList", kRobotCadenceMs, [this] { RobotList(); });
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
        RunGatedScan("ContainerList", kContainerCadenceMs, [this] { ContainerList(); });
        // #region agent log
        LogPerfSpike("ContainerList", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()));
        const auto tItem0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("ItemList", kItemCadenceMs, [this] { ItemList(); });
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
    m_aimThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive() || showmenu) {
            ReleaseAimOutputs();
            return;
        }
        g_aimCadence.tick();
        AimAssistence();
    }, 4);
    m_aimThread->set_priority(THREAD_PRIORITY_HIGHEST);

    g_camCadence.init("camera", 8);
    g_posCadence.init("position", 16);
    g_aimCadence.init("aim", 4);
}

void Engine::StopWorkerThreads()
{
    if (!m_workerThreadsStarted.exchange(false))
        return;

    // Flush any incomplete 60s cadence window before threads stop.
    g_camCadence.dump();
    g_posCadence.dump();
    g_aimCadence.dump();

    // Stop the scheduler that can submit background jobs before joining the
    // other workers. This prevents a world/ESP pass from racing shutdown.
    m_worldThread.reset();
    m_entityThread.reset();
    m_robotEspThread.reset();
    m_worldEspThread.reset();
    m_aimThread.reset();
    m_frameBuilderThread.reset();
    m_cameraThread.reset();
    m_positionThread.reset();
    ReleaseAimOutputs();

    // All engine workers are now stopped, so no new background job can be
    // submitted. Join diagnostic/rebuild jobs before destroying dependencies.
    CollisionMirror::StopBackgroundJobs();
    WorldScan::StopBackgroundJobs();
}
