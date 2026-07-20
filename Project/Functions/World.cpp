#include "../Core/Engine.h"
#include "../Core/AssetNames.h"
#include "WorldScanCommon.h"
#include "../Core/Offsets.h"
#include "../Core/WorldItemCategory.h"
#include "../Interface/Utils/Variables/index.h"

#include <cmath>
#include <unordered_map>

namespace {

std::unordered_map<uintptr_t, uint8_t> s_worldDistMisses;
constexpr uint8_t kWorldDistMissClearDrawing = 3;
int g_worldFinalizeDistHystHold = 0;
int g_worldFinalizePosReRead = 0;

} // namespace

int Engine::LastWorldFinalizeDistHystHold() const
{
    return g_worldFinalizeDistHystHold;
}

int Engine::LastWorldFinalizePosReRead() const
{
    return g_worldFinalizePosReRead;
}

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
    g_worldFinalizeDistHystHold = 0;
    g_worldFinalizePosReRead = 0;
    for (auto it = cache.begin(); it != cache.end(); ) {
        auto& entry = it->second;

        const uintptr_t root = entry.rootComponent;
        if (root && IsValidPointer(root)) {
            if (!IsPlausibleWorldPos(entry.WorldPos)) {
                const Vector3 freshPos = ReadSceneWorldPos(root);
                ++g_worldFinalizePosReRead;
                if (!WorldScan::IsOldStyleInvalidXY(freshPos))
                    entry.WorldPos = freshPos;
            }
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
        // Do not widen maxDrawM for radar — ESP draw cap stays category/SP distance.
        const float maxDrawM = WorldLootPickupMaxDrawMeters(cat, &filterView);

        if (entry.Distance > maxDrawM) {
            if (WorldScan::MissCounterShouldEvict(
                    s_worldDistMisses, it->first, false, kWorldDistMissClearDrawing)) {
                entry.Drawing = false;
            } else {
                entry.Drawing = true;
                ++g_worldFinalizeDistHystHold;
                ++outDrawing;
            }
            ++it;
            continue;
        }
        WorldScan::MissCounterClear(s_worldDistMisses, it->first);

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

    // #region agent log
    {
        // Drawing-only transitions. Do NOT track key disappearance across
        // FinalizeWorldCacheMap calls — ItemList and ContainerList alternate
        // through this function with different caches, which falsely counted
        // every handoff as world evict/re-admit (flicker_score world=106,
        // all evictReadmit, while bots/players stayed 0).
        for (const auto& [key, entry] : cache) {
            WorldScan::FlickerCause cause = WorldScan::FlickerCause::Other;
            if (!entry.Drawing) {
                if (!IsPlausibleWorldPos(entry.WorldPos))
                    cause = WorldScan::FlickerCause::PosFail;
                else
                    cause = WorldScan::FlickerCause::DistEdge;
            }
            WorldScan::NoteFlickerDrawing(
                WorldScan::FlickerChannel::World, key, entry.Drawing, cause);
        }
        WorldScan::MaybeFlushFlickerScore();
    }
    // #endregion
}
