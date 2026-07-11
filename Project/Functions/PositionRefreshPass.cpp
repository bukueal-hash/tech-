#include "../Core/Engine.h"
#include "../Core/Offsets.h"
#include "../Interface/Utils/Variables/index.h"

#include <chrono>
#include <unordered_map>
#include <vector>

namespace {

struct PosRefreshKey {
    enum class CacheKind : uint8_t { Container, Item, Robot };

    CacheKind kind = CacheKind::Robot;
    uintptr_t key = 0;
};

} // namespace

void Engine::PositionRefreshPass()
{
    if (!IsEspRaidActive())
        return;

    std::vector<PosRefreshKey> keys;
    std::vector<uintptr_t> roots;
    std::vector<Vector3> readBufs;
    keys.reserve(512);
    roots.reserve(512);
    readBufs.reserve(512);

    auto queueEntry = [&](PosRefreshKey::CacheKind kind, uintptr_t key, uintptr_t root) {
        if (!root || !IsValidPointer(root))
            return;
        keys.push_back({ kind, key });
        roots.push_back(root);
        readBufs.emplace_back();
    };

    if (var::showRobots || var::robotAimEnabled || var::show_radar) {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        for (const auto& [key, entry] : robotCache) {
            if (!entry.Drawing)
                continue;
            queueEntry(PosRefreshKey::CacheKind::Robot, key, entry.rootComponent);
        }
    }

    if (AnyWorldEspEnabled() || var::show_radar) {
        {
            std::shared_lock<std::shared_mutex> lock(m_containerCacheMutex);
            for (const auto& [key, entry] : containerCache) {
                if (!entry.Drawing)
                    continue;
                queueEntry(PosRefreshKey::CacheKind::Container, key, entry.rootComponent);
            }
        }
        {
            std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
            for (const auto& [key, entry] : itemCache) {
                if (!entry.Drawing)
                    continue;
                queueEntry(PosRefreshKey::CacheKind::Item, key, entry.rootComponent);
            }
        }
    }

    if (keys.empty())
        return;

    ScatterSession scatter;
    if (!scatter.isValid())
        return;

    for (size_t i = 0; i < roots.size(); ++i)
        scatter.prepare(roots[i] + Offsets::RelativeLocation, readBufs[i]);
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

    for (size_t i = 0; i < keys.size(); ++i) {
        Vector3 pos = readBufs[i];
        const Vector3 scene = ReadSceneWorldPos(roots[i]);
        if (IsPlausibleWorldPos(scene))
            pos = scene;
        if (!IsPlausibleWorldPos(pos))
            continue;

        switch (keys[i].kind) {
        case PosRefreshKey::CacheKind::Robot: {
            std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
            auto it = robotCache.find(keys[i].key);
            if (it == robotCache.end())
                break;
            applyVelocity(
                it->second.WorldPos,
                it->second.lastWorldPos,
                it->second.lastVelocityUpdate,
                it->second.cachedVelocity,
                pos);
            break;
        }
        case PosRefreshKey::CacheKind::Container: {
            std::unique_lock<std::shared_mutex> lock(m_containerCacheMutex);
            auto it = containerCache.find(keys[i].key);
            if (it != containerCache.end())
                it->second.WorldPos = pos;
            break;
        }
        case PosRefreshKey::CacheKind::Item: {
            std::unique_lock<std::shared_mutex> lock(m_itemCacheMutex);
            auto it = itemCache.find(keys[i].key);
            if (it != itemCache.end())
                it->second.WorldPos = pos;
            break;
        }
        }
    }
}

void Engine::BuildEspRenderFrameWorker()
{
    if (!IsEspRaidActive())
        return;

    EspRenderFrame frame{};
    if (!CollectEspRenderFrame(frame))
        return;

    std::unique_lock<std::shared_mutex> lock(m_espFrameMutex);
    m_lastEspFrame = std::move(frame);
}
