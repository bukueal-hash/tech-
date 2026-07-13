#include "../Core/Engine.h"
#include "../Core/AssetNames.h"
#include "WorldScanCommon.h"
#include "../Core/Offsets.h"
#include "../Core/WorldItemCategory.h"
#include "../Interface/Utils/Variables/index.h"

#include <cmath>

bool Engine::GatherWorldScanContext(WorldScanContext& ctx)
{
    ctx = {};
    {
        std::shared_lock<std::shared_mutex> slock(m_stateMutex);
        ctx.gWorld = GWorld;
        ctx.persistentLevel = PersistentLevel;
        ctx.actors = Actors;
        ctx.acknowledgedPawn = AcknowledgedPawn;
    }

    if (!ctx.gWorld || !ctx.persistentLevel)
        return false;

    // Always union PersistentLevel + every UWorld::Levels entry. Persistent-only
    // missed streaming-level containers (lockers/crates in POI sublevels).
    WorldScan::CollectLevelActors(ctx.gWorld, ctx.persistentLevel, ctx.currentActors);

    if (ctx.currentActors.empty())
        return false;

    const int32_t actorCount = static_cast<int32_t>(ctx.currentActors.size());
    const int32_t actorMax = actorCount;

    if (!WorldScan::RefreshCachedActorPtrs(ctx.currentActors, actorCount, actorMax))
        return false;

    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        ctx.camera = g_Camera;
    }

    return true;
}

void Engine::FinalizeWorldCacheMap(
    std::unordered_map<uintptr_t, WorldCacheEntry>& cache,
    const CameraCache& cam,
    int& outDrawing)
{
    outDrawing = 0;
    for (auto it = cache.begin(); it != cache.end(); ) {
        auto& entry = it->second;

        const uintptr_t root = entry.rootComponent;
        if (root && IsValidPointer(root)) {
            const Vector3 freshPos = ReadSceneWorldPos(root);
            if (!WorldScan::IsOldStyleInvalidXY(freshPos))
                entry.WorldPos = freshPos;
        }

        Vector3 delta = entry.WorldPos - cam.Location;
        entry.Distance = static_cast<float>(std::sqrt(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) / 100.0);

        auto cat = static_cast<WorldItemCategory>(entry.worldCategory);

        if (WorldCategoryIsContainerProp(cat)
            && cat != WorldItemCategory::DroppedPickup
            && cat != WorldItemCategory::Harvestable
            && cat != WorldItemCategory::OpenedContainer
            && cat != WorldItemCategory::Trash
            && ContainerLootLooksOpened(it->first, entry.ActorName)) {
            if (!var::show_world_open_container) {
                entry.Drawing = false;
                ++it;
                continue;
            }
            entry.worldCategory = static_cast<uint8_t>(WorldItemCategory::OpenedContainer);
            cat = WorldItemCategory::OpenedContainer;
        }

        WorldLootFilterView filterView{
            entry.worldCategory,
            entry.ActorName,
            entry.ItemDisplayName,
            entry.lootValue,
            entry.lootRarityTier};
        const bool espVisible = WorldCategoryEnabled(entry.worldCategory);
        const bool radarVisible = WorldCategoryVisibleOnRadar(filterView);

        // Same tier rules as render: row SP checkbox → loot or SP slider.
        float maxDrawM = WorldLootPickupMaxDrawMeters(cat, &filterView);
        if (radarVisible) {
            const float radarRangeM =
                var::radar_range > 0.f ? var::radar_range : 100.f;
            maxDrawM = (std::max)(maxDrawM, radarRangeM);
        }

        if (entry.Distance > maxDrawM) {
            entry.Drawing = false;
            ++it;
            continue;
        }

        if (!espVisible && !radarVisible) {
            entry.Drawing = false;
            ++it;
            continue;
        }

        if (!PassesLootPickupFilters(filterView)) {
            entry.Drawing = false;
            ++it;
            continue;
        }

        entry.Drawing = true;
        ++outDrawing;
        entityStarted.store(true, std::memory_order_release);
        ++it;
    }
}
