#include "../Core/Engine.h"
#include "../Core/Offsets.h"
#include "../Interface/Utils/Variables/index.h"

#include <chrono>
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
    uintptr_t mesh = 0;
    Vector3 rootBuf{};
    Vector3 meshBuf{};
};

} // namespace

void Engine::PositionRefreshPass()
{
    if (!IsEspRaidActive())
        return;

    std::vector<PosRefreshWork> work;
    work.reserve(512);

    auto queueEntry = [&](PosRefreshKey::CacheKind kind, uintptr_t key,
                          uintptr_t root, uintptr_t mesh = 0) {
        if (!root || !IsValidPointer(root))
            return;
        work.push_back({ { kind, key }, root, mesh, {}, {} });
    };

    if (var::enableesp || var::show_radar) {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        for (const auto& [key, entry] : playerCache) {
            if (!entry.Drawing)
                continue;
            // Do not attach mesh for players — USkeletalMeshComponent@0x428 is CMC;
            // mesh WorldLocation scatter was overwriting with dead transforms.
            queueEntry(PosRefreshKey::CacheKind::Player, key, entry.rootComponent, 0);
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

    if (work.empty())
        return;

    ScatterSession scatter;
    if (!scatter.isValid())
        return;

    for (PosRefreshWork& item : work) {
        scatter.prepare(item.root + Offsets::RelativeLocation, item.rootBuf);
        if (item.mesh && IsValidPointer(item.mesh))
            scatter.prepare(item.mesh + Offsets::WorldLocation, item.meshBuf);
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

    for (PosRefreshWork& item : work) {
        Vector3 pos = item.rootBuf;
        const Vector3 rootScene = ReadSceneWorldPos(item.root);
        if (IsPlausibleWorldPos(rootScene))
            pos = rootScene;

        if (item.mesh && IsValidPointer(item.mesh)) {
            if (IsPlausibleWorldPos(item.meshBuf))
                pos = item.meshBuf;
            else {
                const Vector3 meshScene = ReadSceneWorldPos(item.mesh);
                if (IsPlausibleWorldPos(meshScene))
                    pos = meshScene;
            }
        }

        if (!IsPlausibleWorldPos(pos))
            continue;

        switch (item.key.kind) {
        case PosRefreshKey::CacheKind::Player: {
            std::unique_lock<std::shared_mutex> lock(m_playerCacheMutex);
            auto it = playerCache.find(item.key.key);
            if (it == playerCache.end())
                break;
            // Prefer live scene CompToWorld; RelativeLocation freezes on remotes.
            if (item.root && IsValidPointer(item.root)) {
                const Vector3 scene = ReadSceneWorldPos(item.root);
                if (IsPlausibleWorldPos(scene))
                    pos = scene;
            }
            if (!IsPlausibleWorldPos(pos)) {
                const Vector3 rel = Memory::read<Vector3>(
                    item.root + Offsets::RelativeLocation);
                if (IsPlausibleWorldPos(rel))
                    pos = rel;
            }
            if (!IsPlausibleWorldPos(pos)) {
                constexpr std::ptrdiff_t kStateInterpolator = 0x7c0;
                constexpr std::ptrdiff_t kReplicatedRootTransform = 0x1f8;
                constexpr std::ptrdiff_t kReplicatedMovement = 0x150;
                constexpr std::ptrdiff_t kRepMovLocation = 0x30;
                constexpr std::ptrdiff_t kACharacterMovement = 0x430;
                constexpr std::ptrdiff_t kCmcLastUpdateLocation = 0x3e0;
                const uintptr_t pawn = item.key.key;
                const uintptr_t interp = Memory::read<uintptr_t>(pawn + kStateInterpolator);
                if (interp && IsValidPointer(interp)) {
                    const Vector3 fromInterp =
                        ToVector3(Memory::read<FVector3d>(interp + kReplicatedRootTransform));
                    if (IsPlausibleWorldPos(fromInterp))
                        pos = fromInterp;
                }
                if (!IsPlausibleWorldPos(pos)) {
                    const Vector3 fromRep = ToVector3(Memory::read<FVector3d>(
                        pawn + kReplicatedMovement + kRepMovLocation));
                    if (IsPlausibleWorldPos(fromRep)) {
                        pos = fromRep;
                    } else {
                        uintptr_t cmc = Memory::read<uintptr_t>(pawn + Offsets::PioneerCharacterMovement);
                        if (!cmc || !IsValidPointer(cmc))
                            cmc = Memory::read<uintptr_t>(pawn + kACharacterMovement);
                        if (cmc && IsValidPointer(cmc)) {
                            const Vector3 fromCmc = ToVector3(
                                Memory::read<FVector3d>(cmc + kCmcLastUpdateLocation));
                            if (IsPlausibleWorldPos(fromCmc))
                                pos = fromCmc;
                        }
                    }
                }
            }
            applyVelocity(
                it->second.WorldPos,
                it->second.lastWorldPos,
                it->second.lastVelocityUpdate,
                it->second.cachedVelocity,
                pos);
            break;
        }
        case PosRefreshKey::CacheKind::Robot: {
            std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
            auto it = robotCache.find(item.key.key);
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
            auto it = containerCache.find(item.key.key);
            if (it != containerCache.end())
                it->second.WorldPos = pos;
            break;
        }
        case PosRefreshKey::CacheKind::Item: {
            std::unique_lock<std::shared_mutex> lock(m_itemCacheMutex);
            auto it = itemCache.find(item.key.key);
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
