#include "../Core/Engine.h"
#include "../Core/TaskManager.h"
#include "../Core/TaskIntervals.h"
#include "../../DMA/Memory.h"
#include "CollisionVis.h"
#include "../Interface/Utils/Variables/index.h"

#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

extern bool showmenu;

namespace {
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
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"taskmanager\",\"hypothesisId\":\"TM1\","
      << "\"location\":\"EngineThreads.cpp\",\"message\":\"perf_spike\","
      << "\"data\":{\"thread\":\"" << threadName << "\",\"ms\":" << ms << "}"
      << ",\"timestamp\":" << ts << "}\n";
}

void TimedCall(const char* name, const std::function<void()>& fn)
{
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    LogPerfSpike(name, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count()));
}
} // namespace

void Engine::GetScanGateSnapshot(int& waiters, const char*& holder) const
{
    // Legacy API kept for paint_gap telemetry; TaskManager replaced the mutex gate.
    waiters = 0;
    holder = "TaskManager";
}

void Engine::StartWorkerThreads()
{
    if (m_workerThreadsStarted.exchange(true))
        return;

    m_taskWorkersStop.store(false, std::memory_order_release);

    // Hot lane: camera / pos / frame / aim — never heavy, never behind loot.
    m_hotWorker = std::thread([this] {
        TaskManager tm;
        tm.setCameraTaskName("UpdateCamera");
        tm.addTask("UpdateCamera", [this] {
            if (!IsEspRaidActive())
                return;
            TimedCall("UpdateCamera", [this] { UpdateCamera(); });
        }, &TaskIntervals::cameraMs);
        tm.addTask("PositionRefresh", [this] {
            if (!IsEspRaidActive())
                return;
            TimedCall("PositionRefreshPass", [this] { PositionRefreshPass(); });
        }, &TaskIntervals::positionMs);
        tm.addTask("FrameBuilder", [this] {
            if (!IsEspRaidActive())
                return;
            TimedCall("FrameBuilder", [this] {
                BuildEspRenderFrameWorker();
                CollisionVis::ApplyVisToEspCaches(*this);
            });
        }, &TaskIntervals::frameMs);
        tm.addTask("AimAssistence", [this] {
            if (!IsEspRaidActive() || showmenu)
                return;
            TimedCall("AimAssistence", [this] { AimAssistence(); });
        }, &TaskIntervals::aimMs);
        tm.run(m_taskWorkersStop);
    });

    // Main lane: world/PC resolve + players + bots (RobotList is heavy).
    m_mainWorker = std::thread([this] {
        TaskManager tm;
        tm.setCameraTaskName(""); // no camera on this lane
        tm.addTask("Update", [this] {
            TimedCall("Update", [this] { Update(); });
        }, &TaskIntervals::updateMs);
        tm.addTask("EntityList", [this] {
            if (!IsEspRaidActive())
                return;
            TimedCall("EntityList", [this] { EntityList(); });
        }, &TaskIntervals::entityMs);
        tm.addTask("RobotList", [this] {
            if (!IsEspRaidActive())
                return;
            TimedCall("RobotList", [this] { RobotList(); });
        }, &TaskIntervals::robotMs, /*phaseOffsetMs=*/0.0);
        tm.run(m_taskWorkersStop);
    });

    // Features lane: cold heavies (container/item) + mild vis; one-heavy-per-tick.
    m_featuresWorker = std::thread([this] {
        TaskManager tm;
        tm.setCameraTaskName("");
        tm.addTask("ContainerList", [this] {
            if (!IsEspRaidActive())
                return;
            TimedCall("ContainerList", [this] { ContainerList(); });
        }, &TaskIntervals::containerMs, /*phaseOffsetMs=*/50.0);
        tm.addTask("ItemList", [this] {
            if (!IsEspRaidActive())
                return;
            TimedCall("ItemList", [this] { ItemList(); });
        }, &TaskIntervals::itemMs, /*phaseOffsetMs=*/100.0);
        tm.addTask("VisRebuild", [this] {
            if (!IsEspRaidActive() || !var::vis_enabled)
                return;
            TimedCall("VisRebuild", [this] { CollisionVis::TickRebuild(*this); });
        }, &TaskIntervals::visMs, /*phaseOffsetMs=*/25.0);
        tm.run(m_taskWorkersStop);
    });

    std::cout << "[TaskManager] 3 lanes started (hot/main/features)" << std::endl;
}

void Engine::StopWorkerThreads()
{
    if (!m_workerThreadsStarted.exchange(false))
        return;

    m_taskWorkersStop.store(true, std::memory_order_release);
    if (m_hotWorker.joinable())
        m_hotWorker.join();
    if (m_mainWorker.joinable())
        m_mainWorker.join();
    if (m_featuresWorker.joinable())
        m_featuresWorker.join();
}
