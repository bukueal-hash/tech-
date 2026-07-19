#include "../Core/Engine.h"
#include "../../DMA/Memory.h"
#include "CollisionVis.h"
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

    std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
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

// Scan gate: serialize the heavy scanner passes (Update, EntityList,
// RobotList, ContainerList/ItemList) so only one touches the DMA bus at a
// time. Latency-critical passes (PositionRefreshPass, FrameBuilder, Aim)
// stay ungated so positions/camera never wait behind a long scan.
std::mutex g_scanGateMu;
std::atomic<int> g_scanGateWaiters{0};
std::atomic<const char*> g_scanGateHolder{nullptr};

// #region agent log
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

    std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
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
    if (g_lastGateRelease.time_since_epoch().count() != 0) {
        const auto sinceRelease = std::chrono::steady_clock::now() - g_lastGateRelease;
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
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t0 - w0).count()),
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
    }, 16);
    // Phase 1.5: RobotList was the worst LAG1 offender (baseline ~622ms avg /
    // 1267ms max). Positions stay on PositionRefreshPass @16ms; lengthen this
    // admission/visual pass so FPGA bus contention drops.
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
    }, 48);
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
    }, 16);
    // CAM2 (Fix #3): camera shared m_positionThread with PositionRefreshPass,
    // whose 100-200ms scatter stalls delayed the next UpdateCamera by the same
    // amount (cam_refresh_gap 200-560ms every 3s window). A stale projection
    // POV makes every box/name jump-blink on rotation even though scanners are
    // steady — the flicker the user sees that flicker_score channels 0-2 miss.
    // Give the camera its own tiny ungated thread: one small read @8ms.
    m_cameraThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        UpdateCamera();
    }, 8);
    m_positionThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
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
        CollisionVis::ApplyVisToEspCaches(*this);
        const int totalMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        LogPerfSpike("FrameBuilder", totalMs);
    }, 12);
    m_visThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive() || !var::vis_enabled)
            return;
        CollisionVis::TickRebuild(*this);
    }, 250);
    m_aimThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive() || showmenu)
            return;
        AimAssistence();
    }, 4);
}

void Engine::StopWorkerThreads()
{
    if (!m_workerThreadsStarted.exchange(false))
        return;

    m_aimThread.reset();
    m_visThread.reset();
    m_frameBuilderThread.reset();
    m_cameraThread.reset();
    m_positionThread.reset();
    m_robotEspThread.reset();
    m_worldEspThread.reset();
    m_entityThread.reset();
    m_worldThread.reset();
}
