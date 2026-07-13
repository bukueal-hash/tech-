#pragma once

#include "../Core/Engine.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace WorldScan {

bool IsOldStyleInvalidXY(const Vector3& pos);

uint32_t ResolveItemClassId(uint32_t atPrimary, uint32_t atAlt);

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

} // namespace WorldScan
