#include "../Core/Engine.h"
#include "../Core/AgentLog.h"
#include "../Core/AssetNames.h"
#include "WorldScanCommon.h"
#include "../Core/Offsets.h"
#include "../Core/WorldItemCategory.h"
#include "../Interface/Utils/Variables/index.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

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
    // #region agent log
    int distSkip = 0;
    float farDist = 0.f;
    float farMax = 0.f;
    float farBase = 0.f;
    int farCat = -1;
    int farRadar = 0;
    int farSp = 0;
    std::string farLabel;
    int skipSample = 0;
    float skipDist = 0.f;
    float skipMax = 0.f;
    int skipCat = -1;
    std::string skipLabel;
    // #endregion
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
            && cat != WorldItemCategory::Trash) {
            // Use cached opened-state to avoid per-container per-pass DMA
            // reads.  Adaptive TTL:
            //   - Never probed (cachedOpened < 0): probe immediately.
            //   - State just changed (<5 s): 500 ms — catch open/close flips.
            //   - Stable opened (>5 s): 500 ms — catch closure quickly.
            //   - Stable closed (>5 s): 5 s — closed rarely re-opens mid-raid.
            const auto nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            uint64_t ttlMs = 2000;  // fallback
            if (entry.cachedOpened >= 0 && entry.openedProbeMs > 0) {
                if (entry.cachedOpened == 0) {
                    // Closed: check stability.
                    const uint64_t heldMs = (entry.openedStateMs > 0)
                        ? (nowMs - entry.openedStateMs) : 0;
                    ttlMs = (heldMs > 5000) ? 5000 : 500;
                } else {
                    // Opened: always 500 ms to detect closure quickly.
                    ttlMs = 500;
                }
            }
            const bool needProbe = entry.cachedOpened < 0
                || (nowMs - entry.openedProbeMs) > ttlMs;
            if (needProbe) {
                const int8_t prevOpened = entry.cachedOpened;
                entry.cachedOpened = ContainerLootLooksOpened(
                    it->first, entry.ActorName) ? 1 : 0;
                entry.openedProbeMs = nowMs;
                if (entry.cachedOpened != prevOpened)
                    entry.openedStateMs = nowMs;
            }
            if (entry.cachedOpened > 0) {
                if (!var::show_world_open_container) {
                    entry.Drawing = false;
                    ++it;
                    continue;
                }
                entry.worldCategory = static_cast<uint8_t>(WorldItemCategory::OpenedContainer);
                cat = WorldItemCategory::OpenedContainer;
            }
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
        const float baseMaxM = maxDrawM;
        // Distance hysteresis (mirrors player/robot ESP): an entry that is
        // already Drawing stays drawn until it drifts 5% beyond the cap,
        // instead of popping on/off at the hard edge. This was the biggest
        // flicker source in debug-c190fb — distEdge 949 of 13038 rows.
        // Admission still uses the strict cap, so new items appear at the
        // correct range; only the drop edge is widened.
        const float dropM = entry.Drawing ? maxDrawM * 1.05f : maxDrawM;

        if (entry.Distance > dropM) {
            entry.Drawing = false;
            // #region agent log
            ++distSkip;
            if (skipSample == 0 || entry.Distance < skipDist) {
                ++skipSample;
                skipDist = entry.Distance;
                skipMax = dropM;
                skipCat = static_cast<int>(cat);
                skipLabel = entry.ItemDisplayName;
            }
            // #endregion
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
        // #region agent log
        if (entry.Distance > farDist) {
            farDist = entry.Distance;
            farCat = static_cast<int>(cat);
            farMax = maxDrawM;
            farBase = baseMaxM;
            farRadar = 0;
            farSp = WorldCategoryUsesSpContainerRange(cat) ? 1 : 0;
            farLabel = entry.ItemDisplayName;
        }
        // #endregion
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

    // #region agent log
    {
        thread_local auto s_last = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (s_last.time_since_epoch().count() == 0
            || now - s_last >= std::chrono::seconds(2)) {
            s_last = now;
            std::ofstream f(kArcDebugLogPath, std::ios::app);
            if (f) {
                const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                char farLabelEsc[64]{};
                snprintf(farLabelEsc, sizeof(farLabelEsc), "%.48s", farLabel.c_str());
                char skipLabelEsc[64]{};
                snprintf(skipLabelEsc, sizeof(skipLabelEsc), "%.48s", skipLabel.c_str());
                f << "{\"sessionId\":\"c190fb\",\"runId\":\"baseline\",\"hypothesisId\":\"H1\","
                  << "\"location\":\"World.cpp:FinalizeWorldCacheMap\",\"message\":\"world_draw_caps\","
                  << "\"data\":{\"cache\":" << cache.size()
                  << ",\"drawing\":" << outDrawing
                  << ",\"distSkip\":" << distSkip
                  << ",\"lootDist\":" << static_cast<int>(var::loot_distance)
                  << ",\"spDist\":" << static_cast<int>(var::container_distance_sp)
                  << ",\"radar\":" << static_cast<int>(var::radar_range)
                  << ",\"farDist\":" << static_cast<int>(farDist)
                  << ",\"farCat\":" << farCat
                  << ",\"farMax\":" << static_cast<int>(farMax)
                  << ",\"farBase\":" << static_cast<int>(farBase)
                  << ",\"farSp\":" << farSp
                  << ",\"farRadar\":" << farRadar
                  << ",\"farLabel\":\"" << farLabelEsc << "\""
                  << ",\"nearestHiddenDist\":" << static_cast<int>(skipDist)
                  << ",\"nearestHiddenCap\":" << static_cast<int>(skipMax)
                  << ",\"nearestHiddenCat\":" << skipCat
                  << ",\"nearestHiddenLabel\":\"" << skipLabelEsc << "\"}"
                  << ",\"timestamp\":" << ts << "}\n";
            }
        }
    }
    // #endregion
}
