#include "../Core/Engine.h"
#include "../Core/Offsets.h"
#include "../Interface/Utils/Variables/index.h"

#include <chrono>
#include <cstdio>
#include <fstream>
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

    const auto t0 = std::chrono::steady_clock::now();

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

    // #region agent log
    {
        static auto s_lastPosLog = std::chrono::steady_clock::time_point{};
        const auto nowLog = std::chrono::steady_clock::now();
        if (s_lastPosLog.time_since_epoch().count() == 0
            || nowLog - s_lastPosLog >= std::chrono::seconds(2)) {
            s_lastPosLog = nowLog;
            int nPlayer = 0, nBot = 0;
            for (const PosRefreshWork& w : work) {
                if (w.key.kind == PosRefreshKey::CacheKind::Player)
                    ++nPlayer;
                else if (w.key.kind == PosRefreshKey::CacheKind::Robot)
                    ++nBot;
            }
            std::ofstream f("F:/Test/ARCs/debug-5681af.log", std::ios::app);
            if (f) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"5681af\",\"runId\":\"post-fix\",\"hypothesisId\":\"H2\""
                  << ",\"location\":\"PositionRefreshPass.cpp\",\"message\":\"pos_work\""
                  << ",\"data\":{\"total\":" << work.size()
                  << ",\"players\":" << nPlayer
                  << ",\"bots\":" << nBot
                  << ",\"world\":0,\"cachedScatter\":1}"
                  << ",\"timestamp\":" << ms << "}\n";
            }
        }
    }
    // #endregion

    // Cached scatter (flags=0) — PositionRefresh only. Bones/world stay NOCACHE.
    ScatterSession scatter(/*cached=*/true);
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
                const double mag2 =
                    static_cast<double>(newVel.x) * newVel.x
                    + static_cast<double>(newVel.y) * newVel.y
                    + static_cast<double>(newVel.z) * newVel.z;
                if (mag2 < (3000.0 * 3000.0)) {
                    cachedVelocity.x = cachedVelocity.x * 0.5f + newVel.x * 0.5f;
                    cachedVelocity.y = cachedVelocity.y * 0.5f + newVel.y * 0.5f;
                    cachedVelocity.z = cachedVelocity.z * 0.5f + newVel.z * 0.5f;
                } else {
                    cachedVelocity = { 0, 0, 0 };
                }
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
            if (!IsPlausibleWorldPos(pos) && IsPlausibleWorldPos(item.relBuf))
                pos = item.relBuf;
            if (!IsPlausibleWorldPos(pos))
                continue;
            auto it = robotCache.find(item.key.key);
            if (it == robotCache.end())
                continue;
            applyVelocity(
                it->second.WorldPos,
                it->second.lastWorldPos,
                it->second.lastVelocityUpdate,
                it->second.cachedVelocity,
                pos);
        }
    }

    const auto posMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (posMs >= 50) {
        char hitchReason[48];
        snprintf(hitchReason, sizeof(hitchReason), "hitch_pos_%lldms",
            static_cast<long long>(posMs));
        NoteFlicker(hitchReason);
    }
}

void Engine::BuildEspRenderFrameWorker()
{
    if (!IsEspRaidActive())
        return;

    const auto t0 = std::chrono::steady_clock::now();
    EspRenderFrame frame{};
    if (!CollectEspRenderFrame(frame))
        return;

    std::unique_lock<std::shared_mutex> lock(m_espFrameMutex);
    m_lastEspFrame = std::move(frame);
    m_lastEspFrameValid.store(m_lastEspFrame.valid, std::memory_order_release);

    const auto frameMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (frameMs >= 50) {
        char hitchReason[48];
        snprintf(hitchReason, sizeof(hitchReason), "hitch_frame_%lldms",
            static_cast<long long>(frameMs));
        NoteFlicker(hitchReason);
    }
}
