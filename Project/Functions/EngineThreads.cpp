#include "../Core/Engine.h"
#include "../Core/AgentLog.h"
#include "../../DMA/Memory.h"
#include "../Interface/Utils/Variables/index.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

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
std::mutex g_scanGateMu;
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
std::chrono::steady_clock::time_point g_lastGateRelease{};

template <typename Fn>
void RunGatedScan(const char* scanner, Fn&& fn)
{
    const auto w0 = std::chrono::steady_clock::now();
    const char* blockedBy = g_scanGateHolder.load(std::memory_order_relaxed);
    g_scanGateWaiters.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(g_scanGateMu);
    g_scanGateWaiters.fetch_sub(1, std::memory_order_relaxed);
    const auto t_acquired = std::chrono::steady_clock::now();
    if (g_lastGateRelease.time_since_epoch().count() != 0) {
        const auto sinceRelease = t_acquired - g_lastGateRelease;
        if (sinceRelease < kGateIdleGap)
            std::this_thread::sleep_for(kGateIdleGap - sinceRelease);
    }
    const auto t0 = std::chrono::steady_clock::now();
    g_scanGateHolder.store(scanner, std::memory_order_relaxed);
    fn();
    g_lastGateRelease = std::chrono::steady_clock::now();
    g_scanGateHolder.store(nullptr, std::memory_order_relaxed);
    // #region agent log
    const auto t1 = std::chrono::steady_clock::now();
    LogScanGate(scanner,
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t_acquired - w0).count()),
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()),
        g_scanGateWaiters.load(std::memory_order_relaxed),
        blockedBy);
    // #endregion
}

} // namespace

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
        RunGatedScan("Update", [this] { Update(); });
        // #region agent log
        LogPerfSpike("Update", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()));
        // #endregion
    }, 16);
    m_entityThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        // #region agent log
        const auto t0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("EntityList", [this] { EntityList(); });
        // #region agent log
        LogPerfSpike("EntityList", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()));
        // #endregion
    }, 220);
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
        RunGatedScan("RobotList", [this] { RobotList(); });
        // #region agent log
        LogPerfSpike("RobotList", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()));
        // #endregion
    }, 200);
    m_worldEspThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        // #region agent log
        const auto t0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("ContainerList", [this] { ContainerList(); });
        // #region agent log
        LogPerfSpike("ContainerList", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()));
        const auto tItem0 = std::chrono::steady_clock::now();
        // #endregion
        RunGatedScan("ItemList", [this] { ItemList(); });
        // #region agent log
        LogPerfSpike("ItemList", static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tItem0).count()));
        // #endregion
    }, 250);
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
        const int totalMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        LogPerfSpike("FrameBuilder", totalMs);
    }, 12);
    m_aimThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive() || showmenu)
            return;
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

    m_aimThread.reset();
    m_frameBuilderThread.reset();
    m_cameraThread.reset();
    m_positionThread.reset();
    m_robotEspThread.reset();
    m_worldEspThread.reset();
    m_entityThread.reset();
    m_worldThread.reset();
}
