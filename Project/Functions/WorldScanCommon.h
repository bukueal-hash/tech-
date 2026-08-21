#pragma once

#include "../Core/Engine.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace WorldScan {

bool IsOldStyleInvalidXY(const Vector3& pos);

uint32_t ResolveItemClassId(uint32_t atPrimary, uint32_t atAlt);

/** Level → ActorCluster → LevelActorContainer::Actors TArray. */
bool ReadLevelActors(uintptr_t level, uintptr_t& outData, int32_t& outCount);

void CollectLevelActors(
    uintptr_t uworld,
    uintptr_t persistentLevel,
    std::vector<uint64_t>& actors);

bool RefreshCachedActorPtrs(
    const std::vector<uint64_t>& actorPtrs,
    int32_t actorCount,
    int32_t actorMax);

const std::vector<uintptr_t>& CachedActorPtrs();

void ClearCachedActorPtrs();

void ClearItemScannerStaticState();
void ClearContainerScannerStaticState();
void ClearRobotScannerStaticState();

void PruneStaleEntries(
    std::unordered_map<uintptr_t, Engine::WorldCacheEntry>& cache,
    const std::unordered_set<uint64_t>& currentSet);

/** Player pawn, local pawn, or anything already tracked as a player. */
bool LooksLikePlayerPawn(uintptr_t actor, uintptr_t localPawn);

/** Bot / ARC constructable — not a world container or ground item. */
bool LooksLikeBotPawn(uintptr_t actor, uintptr_t localPawn);

/** Non-null enemy type data asset pointer @0x11A0 / 0x1190 (fname may be encrypted). */
bool HasArcEnemyAssetPointer(uintptr_t actor);

/** World crate / socket / salvage container — not an ARC bot. */
bool LooksLikeContainerActor(uintptr_t actor, const std::string& fname = {});

/** Exclude from containerCache and itemCache (players, bots, local pawn). */
bool ShouldExcludeFromWorldCaches(uintptr_t actor, uintptr_t localPawn);

/** Held/equipped/stowed weapon or inventory-service actors — not ground loot. */
bool IsHeldEquipmentActor(uintptr_t actor);

/**
 * Collect a pawn's equipped/stowed weapon & item actors (from its
 * InventoryComponent) into `out`, so world/container/item scans never draw a
 * player's held weapon as a loose loot item or container.
 */
void CollectHeldItemActors(uintptr_t pawn, std::unordered_set<uintptr_t>& out);

/**
 * Current item in hand for ESP: Inventory CurrentItemActors[0], else best
 * Instigator/Owner held actor (gun, bandage, nade, defib, etc.).
 */
uintptr_t ResolvePreferredHeldItemActor(uintptr_t pawn);

struct CacheRootScatterRow {
    uintptr_t actorKey = 0;
    uintptr_t root = 0;
    Vector3 worldPos{};
    bool rootValid = false;
    bool posValid = false;
};

/** Scatter-read RootComponent + world position for cached world/item actors. */
bool ScatterReadActorRootPositions(std::vector<CacheRootScatterRow>& rows);

struct WorldCacheRetainRow {
    uintptr_t actorKey = 0;
    Engine::WorldCacheEntry* entry = nullptr;
    uintptr_t* rootOut = nullptr;
};

/** Scatter-refresh root + world position for item/container cache retain rows. */
void RefreshWorldCacheRetainPositions(std::vector<WorldCacheRetainRow>& rows);

/**
 * Keep one cache entry per rootComponent. Prefer higher lootValue, then
 * non-empty display name, then the later-scanned actor key.
 */
void DedupeWorldCacheByRoot(std::unordered_map<uintptr_t, Engine::WorldCacheEntry>& cache);

/** EMA blend for aim/ESP velocity; zeros if |newVel| >= 3000 cm/s. */
inline void BlendCachedVelocity(Vector3& cachedVelocity, const Vector3& newVel)
{
    const double mag2 = static_cast<double>(newVel.x) * newVel.x
        + static_cast<double>(newVel.y) * newVel.y
        + static_cast<double>(newVel.z) * newVel.z;
    if (mag2 < (3000.0 * 3000.0)) {
        cachedVelocity.x = cachedVelocity.x * 0.5f + newVel.x * 0.5f;
        cachedVelocity.y = cachedVelocity.y * 0.5f + newVel.y * 0.5f;
        cachedVelocity.z = cachedVelocity.z * 0.5f + newVel.z * 0.5f;
    } else {
        cachedVelocity = {};
    }
}

/** Shared miss-streak eviction for item/container/bot scanners. */
inline bool MissCounterShouldEvict(
    std::unordered_map<uintptr_t, uint8_t>& misses,
    uintptr_t key,
    bool ok,
    uint8_t limit)
{
    if (ok) {
        misses.erase(key);
        return false;
    }
    const uint8_t n = ++misses[key];
    return n >= limit;
}

inline void MissCounterClear(
    std::unordered_map<uintptr_t, uint8_t>& misses,
    uintptr_t key)
{
    misses.erase(key);
}

// #region agent log
/** Autonomous flicker-fix loop: measure on->off->on draw transitions.
 *  Channels 0-2 sample the scanner Drawing flag (1-2s cadence).
 *  Channels 3-5 sample the PAINT path (240fps) — what the user actually sees.
 *  Paint channels only count blinks whose off-gap is >= 48ms (visible). */
enum class FlickerChannel : uint8_t {
    Bot = 0,
    Player = 1,
    World = 2,
    PaintBot = 3,
    PaintPlayer = 4,
    PaintWorld = 5
};
enum class FlickerCause : uint8_t {
    PosFail = 0,
    VisMiss = 1,
    EvictReadmit = 2,
    DistEdge = 3,
    Other = 4,
    ProjFail = 5,
    LabelMiss = 6
};

/** Record final Drawing for this pass. Off->on within 2s of an off counts as a flicker. */
void NoteFlickerDrawing(
    FlickerChannel channel,
    uintptr_t key,
    bool drawing,
    FlickerCause cause = FlickerCause::Other);

/** Entity left the cache (evict). Treated as Drawing=false with EvictReadmit. */
void NoteFlickerGone(FlickerChannel channel, uintptr_t key);

/** Emit flicker_score NDJSON at most once per 10s window when counts moved. */
void MaybeFlushFlickerScore();

// #endregion

} // namespace WorldScan
