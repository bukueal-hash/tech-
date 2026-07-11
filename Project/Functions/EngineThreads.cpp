#include "../Core/Engine.h"
#include "../../DMA/Memory.h"

extern bool showmenu;

void Engine::StartWorkerThreads()
{
    if (m_workerThreadsStarted.exchange(true))
        return;

    m_worldThread = std::make_unique<SyncedThread>([this] { Update(); }, 16);
    // Hot player path: health/team/bones every 10 ms; admission gated inside EntityList.
    m_entityThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        EntityList();
    }, 10);
    m_robotEspThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        RobotList();
    }, 10);
    m_containerEspThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        ContainerList();
    }, 50);
    m_worldEspThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        ItemList();
    }, 50);
    m_positionThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        PositionRefreshPass();
    }, 10);
    m_frameBuilderThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive())
            return;
        BuildEspRenderFrameWorker();
    }, 8);
    m_aimThread = std::make_unique<SyncedThread>([this] {
        if (!IsEspRaidActive() || showmenu)
            return;
        AimAssistence();
    }, 1);
}

void Engine::StopWorkerThreads()
{
    if (!m_workerThreadsStarted.exchange(false))
        return;

    m_aimThread.reset();
    m_frameBuilderThread.reset();
    m_positionThread.reset();
    m_robotEspThread.reset();
    m_containerEspThread.reset();
    m_worldEspThread.reset();
    m_entityThread.reset();
    m_worldThread.reset();
}
