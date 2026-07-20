#include "../Core/Engine.h"
#include "../Core/Offsets.h"
#include "../Interface/Utils/Variables/index.h"
#include "WorldScanCommon.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace {

struct PosRefreshKey {
    enum class CacheKind : uint8_t { Container, Item, Robot, Player };

    CacheKind kind = CacheKind::Robot;
    uintptr_t key = 0;
};

struct PosRefreshWork {
    PosRefreshKey key{};
    uintptr_t root = 0;
    Engine::FVector3d worldBuf{};
    Vector3 relBuf{};
};

} // namespace

void Engine::PositionRefreshPass()
{
    if (!IsEspRaidActive())
        return;

    std::vector<PosRefreshWork> work;
    work.reserve(256);

    auto queueEntry = [&](PosRefreshKey::CacheKind kind, uintptr_t key, uintptr_t root) {
        if (!root || !IsValidPointer(root))
            return;
        work.push_back({ { kind, key }, root, {}, {} });
    };

    // Players + bots only. Containers/items are stationary — WorldPos set at admit.
    if (var::enableesp || var::show_radar) {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        for (const auto& [key, entry] : playerCache) {
            if (!entry.Drawing)
                continue;
            queueEntry(PosRefreshKey::CacheKind::Player, key, entry.rootComponent);
        }
    }

    if (var::showRobots || var::robotAimEnabled || var::show_radar) {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        for (const auto& [key, entry] : robotCache) {
            if (!entry.Drawing)
                continue;
            queueEntry(PosRefreshKey::CacheKind::Robot, key, entry.rootComponent);
        }
    }

    if (work.empty())
        return;


    // NOCACHE — cached VMM pages froze bot WorldPos (boxes stuck at spawn footprint).
    // Same class of bug EntityList fixed for players with read_nocache.
    ScatterSession scatter(/*cached=*/false);
    if (!scatter.isValid())
        return;

    for (PosRefreshWork& item : work) {
        scatter.prepare(item.root + Offsets::WorldLocation, item.worldBuf);
        scatter.prepare(item.root + Offsets::RelativeLocation, item.relBuf);
    }
    if (!scatter.execute())
        return;

    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    auto applyVelocity = [&](
        Vector3& worldPos,
        Vector3& lastWorldPos,
        float& lastVelocityUpdate,
        Vector3& cachedVelocity,
        const Vector3& freshPos) {
        worldPos = freshPos;
        if (lastVelocityUpdate > 0.f) {
            const float dtSec =
                (nowMs - static_cast<uint64_t>(lastVelocityUpdate)) * 0.001f;
            if (dtSec > 0.001f && dtSec < 0.5f) {
                const Vector3 delta = freshPos - lastWorldPos;
                Vector3 newVel{
                    delta.x / dtSec,
                    delta.y / dtSec,
                    delta.z / dtSec};
                WorldScan::BlendCachedVelocity(cachedVelocity, newVel);
            }
        }
        lastWorldPos = freshPos;
        lastVelocityUpdate = static_cast<float>(nowMs);
    };

    // One unique_lock per cache kind — not per entity.
    {
        std::unique_lock<std::shared_mutex> lock(m_playerCacheMutex);
        for (PosRefreshWork& item : work) {
            if (item.key.kind != PosRefreshKey::CacheKind::Player)
                continue;
            Vector3 pos = ToVector3(item.worldBuf);
            if (!IsPlausibleWorldPos(pos) && IsPlausibleWorldPos(item.relBuf))
                pos = item.relBuf;
            if (!IsPlausibleWorldPos(pos))
                continue;
            auto it = playerCache.find(item.key.key);
            if (it == playerCache.end())
                continue;
            applyVelocity(
                it->second.WorldPos,
                it->second.lastWorldPos,
                it->second.lastVelocityUpdate,
                it->second.cachedVelocity,
                pos);
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
        for (PosRefreshWork& item : work) {
            if (item.key.kind != PosRefreshKey::CacheKind::Robot)
                continue;
            Vector3 pos = ToVector3(item.worldBuf);
            // Bots: never use RelativeLocation as live WorldPos (freezes remotes).
            if (!IsPlausibleWorldPos(pos))
                continue;
            auto it = robotCache.find(item.key.key);
            if (it == robotCache.end())
                continue;
            const Vector3 oldPos = it->second.WorldPos;
            applyVelocity(
                it->second.WorldPos,
                it->second.lastWorldPos,
                it->second.lastVelocityUpdate,
                it->second.cachedVelocity,
                pos);
            // Keep part/aim helpers glued to the live root so boxes cannot stay
            // planted on stale mesh CTW after WorldPos moves.
            const Vector3 d{
                it->second.WorldPos.x - oldPos.x,
                it->second.WorldPos.y - oldPos.y,
                it->second.WorldPos.z - oldPos.z};
            if (d.x != 0.0 || d.y != 0.0 || d.z != 0.0) {
                if (IsPlausibleWorldPos(it->second.CenterWorldPos)) {
                    it->second.CenterWorldPos.x += d.x;
                    it->second.CenterWorldPos.y += d.y;
                    it->second.CenterWorldPos.z += d.z;
                }
                if (it->second.hasBotHeadWorldPos
                    && IsPlausibleWorldPos(it->second.BotHeadWorldPos)) {
                    it->second.BotHeadWorldPos.x += d.x;
                    it->second.BotHeadWorldPos.y += d.y;
                    it->second.BotHeadWorldPos.z += d.z;
                }
                for (int i = 0; i < it->second.BotPartCount; ++i) {
                    if (!IsPlausibleWorldPos(it->second.BotPartPos[i]))
                        continue;
                    it->second.BotPartPos[i].x += d.x;
                    it->second.BotPartPos[i].y += d.y;
                    it->second.BotPartPos[i].z += d.z;
                }
            }
        }
    }
}

void Engine::BuildEspRenderFrameWorker()
{
    if (!IsEspRaidActive())
        return;

    const auto t0 = std::chrono::steady_clock::now();
    const int writeIdx = 1 - m_espFramePublished.load(std::memory_order_acquire);
    if (!CollectEspRenderFrame(m_espFrameBuffers[writeIdx]))
        return;

    m_espFramePublished.store(writeIdx, std::memory_order_release);
    m_lastEspFrame = m_espFrameBuffers[writeIdx];
    m_lastEspFrameValid.store(m_espFrameBuffers[writeIdx].valid, std::memory_order_release);
    const auto publishMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    // #region agent log
    if (publishMs > 5) {
        static auto s_lastPubLog = std::chrono::steady_clock::time_point{};
        const auto nowPub = std::chrono::steady_clock::now();
        if (s_lastPubLog.time_since_epoch().count() == 0
            || nowPub - s_lastPubLog >= std::chrono::milliseconds(500)) {
            s_lastPubLog = nowPub;
            std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
            if (f) {
                const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"c190fb\",\"runId\":\"flicker-debug\",\"hypothesisId\":\"H2\","
                  << "\"location\":\"PositionRefreshPass.cpp:BuildEspRenderFrameWorker\","
                  << "\"message\":\"frame_publish\","
                  << "\"data\":{\"publishMs\":" << publishMs << ",\"writeIdx\":" << writeIdx << "}"
                  << ",\"timestamp\":" << ts << "}\n";
            }
        }
    }
    if (var::show_debug_overlay && publishMs > 0) {
        static auto s_lastDbg = std::chrono::steady_clock::time_point{};
        const auto nowDbg = std::chrono::steady_clock::now();
        if (s_lastDbg.time_since_epoch().count() == 0
            || nowDbg - s_lastDbg >= std::chrono::seconds(1)) {
            s_lastDbg = nowDbg;
            std::cout << "[debugFrame] publishMs=" << publishMs << std::endl;
        }
    }
    // #endregion
}
