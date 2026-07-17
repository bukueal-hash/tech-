#include "../Core/Engine.h"
#include "../../DMA/Memory.h"
#include "CollisionVis.h"
#include "../Interface/Utils/Variables/index.h"

extern bool showmenu;

void Engine::StartWorkerThreads()
{
    if (m_workerThreadsStarted.exchange(true))
        return;

    // DMA throttle: slower periods cut PCIe/FPGA "packet loss" under load.
    // Frame velocity extrapolate still bridges gaps; aim 8 ms is plenty for kmbox.
    m_worldThread = std::make_unique<SyncedThread>([this] { Update(); }, 16);
    m_entityThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        EntityList();
    }, 16);
    m_robotEspThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        RobotList();
    }, 16);
    m_worldEspThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        ContainerList();
        ItemList();
    }, 16);
    m_positionThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        PositionRefreshPass();
    }, 16);
    m_frameBuilderThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        BuildEspRenderFrameWorker();
        CollisionVis::ApplyVisToEspCaches(*this);
    }, 12);
    m_visThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive() || !var::vis_enabled)
            return;
        CollisionVis::TickRebuild(*this);
    }, 80);
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
    m_positionThread.reset();
    m_robotEspThread.reset();
    m_worldEspThread.reset();
    m_entityThread.reset();
    m_worldThread.reset();
}
