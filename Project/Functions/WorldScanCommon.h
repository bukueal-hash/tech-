#pragma once

#include "../Core/Engine.h"

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace WorldScan {

/** DMA time budget for scanner passes. Heavy scan loops check expired() and
 *  break early so latency-critical threads (camera / position / frame-builder
 *  bones) keep their cadence on the shared DMA link. A pass that hits the
 *  budget does NOT advance the admission ring band — it retries next pass. */
class ScanBudget {
public:
    explicit ScanBudget(std::chrono::milliseconds budget)
        : m_start(std::chrono::steady_clock::now()), m_budget(budget) {}

    bool expired() const {
        return std::chrono::steady_clock::now() - m_start >= m_budget;
    }

private:
    std::chrono::steady_clock::time_point m_start;
    std::chrono::milliseconds m_budget;
};

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

/**
 * One-shot read-only simple-collision probe. Walks every level actor's static
 * mesh to UBodySetup and reads the FKAggregateGeom TArray headers that live
 * INLINE at UBodySetup+0xB8. Diagnostic only — no ESP or visibility consumer.
 */
struct AggGeomProbeResult {
    bool ran = false;
    bool running = false;
    int actorsWalked = 0;
    int rootsValid = 0;
    int meshFromPrimary = 0;   // StaticMeshComponent + Offsets::StaticMesh
    int meshFromLegacy = 0;    // StaticMeshComponent + Offsets::StaticMeshLegacy
    int bodySetupsValid = 0;
    int bodySetupsUnique = 0;
    int bodySetupsNonEmpty = 0;
    /** BodySetups whose 0x70 block held a garbage header — misread, not "empty". */
    int headersRejected = 0;
    int sphereElems = 0;
    int boxElems = 0;
    int sphylElems = 0;
    int convexElems = 0;
    int taperedCapsuleElems = 0;
    int levelSetElems = 0;
    int skinnedLevelSetElems = 0;
    std::string note;
};

/** Kick the probe on a detached thread. No-op while one is already running. */
void StartAggGeomProbe();

/** Snapshot of the last (or in-flight) probe run. */
AggGeomProbeResult GetAggGeomProbeResult();

/**
 * One-shot read-only probe for UWorld's clock fields. This build reorders
 * UWorld (PersistentLevel 0x110, Levels 0x348), so the stock TimeSeconds
 * offset does not apply and the field is not a UPROPERTY, so the SDK dump
 * does not carry it. Samples the head of UWorld twice a second apart and
 * reports every float or double that advanced at wall-clock rate.
 * Diagnostic only.
 */
struct TimeSecondsProbeResult {
    bool ran = false;
    bool running = false;
    int hits = 0;              // fields advancing at ~1.0 per real second
    uint32_t firstOffset = 0;  // lowest matching offset (0 when none)
    double firstValue = 0.0;   // its value at the second sample
    bool firstIsFloat = false; // false = read as double, true = as float
    int bytesChanged = 0;      // 0 means the two samples were identical
    double elapsed = 0.0;      // measured wall seconds between the samples
    std::string note;
};

/** Kick the probe on a detached thread. Takes ~1s. No-op while running. */
void StartTimeSecondsProbe();

/** Snapshot of the last (or in-flight) probe run. */
TimeSecondsProbeResult GetTimeSecondsProbeResult();

/**
 * One-shot read-only probe that finds render-timestamp fields on a mesh
 * component WITHOUT decrypting anything. A render time ticks every frame while
 * the mesh is on screen and freezes when it is not, so the change rate alone
 * identifies the field — no XOR key and no hardcoded offset required.
 * Run it once with the target visible and once with it behind cover; the slot
 * that is busy in the first run and still in the second is the one we want.
 */
struct TickProbeResult {
    bool ran = false;
    bool running = false;
    uint64_t mesh = 0;
    int samples = 0;
    int slotsChanged = 0;   // dwords that moved at least once
    static constexpr int kTop = 6;
    uint32_t topOffset[kTop]{};
    int topCount[kTop]{};
    std::string note;
};

/** Kick the probe on a detached thread. Takes ~2s. No-op while running. */
void StartTickProbe();

/** Snapshot of the last (or in-flight) probe run. */
TickProbeResult GetTickProbeResult();

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

/** Paint-thread screen-jump probe: count boxes whose painted screen position
 *  leapt while the camera barely rotated between paints — a pure paint-space
 *  instability signature (shimmer the eye reads as flicker) that the drawn
 *  on/off flicker tracks cannot see. channel: 0 players, 1 bots. */
void NotePaintOsc(int channel);

// #endregion

} // namespace WorldScan
