#include "../Core/Engine.h"
#include "../Core/AgentLog.h"
#include "../Core/ActorType.h"
#include "../Core/AssetNames.h"
#include "../Core/Offsets.h"
#include "../Core/WorldItemCategory.h"
#include "../Core/IntervalTimer.h"
#include "WorldScanCommon.h"
#include "../Interface/Utils/Variables/index.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

inline bool AnyContainerEspEnabled()
{
    return var::enable_world && (
        var::raiderStock || var::showArc || var::showDeadPlayers || var::showLoot ||
        var::show_world_crate || var::show_world_furniture || var::show_world_harvestable ||
        var::show_world_industrial || var::show_world_other || var::show_world_probe ||
        var::show_world_vehicles || var::show_world_weapon_case || var::show_world_field_crate ||
        var::show_world_supply_station || var::show_world_locker || var::show_world_trash ||
        var::show_world_safe || var::show_world_buried || var::show_world_deaddrop ||
        var::show_world_open_container);
}

// C7: container admission ring + negative memo (mirrors bot P6b / player P7).
// Containers are static world objects, so actors proven non-container are
// memoized with a long staggered TTL, and only one slice of the actor array
// gets DMA probes per pass. Newly-seen actors are prioritized so crates that
// stream in while moving still admit fast.
// debug-c190fb OV2: ContainerList held the scan gate 400-649ms per turn while
// sweeping 1/4 of all actors, saturating the DMA bus and spiking the ungated
// pos/cam/frame passes (~600ms overlay hitch every ~22s). 8 slices halves the
// per-turn hold; newly-seen crates still admit immediately via the prio path,
// so responsiveness is unchanged — only the background full-sweep is finer.
constexpr size_t kContAdmitSlices = 8;
constexpr size_t kContAdmitPrioNewMax = 64;
std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> s_containerScanNeg;
size_t s_contAdmitSliceCursor = 0;
uint64_t s_contAdmitRingGen = 0;
size_t s_contAdmitRingActorCount = 0;
uint64_t s_contAdmitCoveredMask = 0;
int s_contAdmitRingResets = 0;
int s_contAdmitLastCycleMs = 0;
std::chrono::steady_clock::time_point s_contAdmitCycleStart{};
std::unordered_set<uintptr_t> s_contAdmitPrevActors;

bool ContainerScanNegMemoHit(uintptr_t actor, int& outMemoSkip)
{
    const auto now = std::chrono::steady_clock::now();
    if (const auto it = s_containerScanNeg.find(actor); it != s_containerScanNeg.end()) {
        const auto ttl = std::chrono::seconds(20 + static_cast<int>((actor >> 4) & 15));
        if (now - it->second < ttl) {
            ++outMemoSkip;
            return true;
        }
        s_containerScanNeg.erase(it);
    }
    return false;
}

// C7b probe: remembers every actor that was ever negatively memoized so an
// eventual admit of that same actor can be logged as a memo lockout.
std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> s_containerMemoHistory;

void ContainerScanNegMemoize(uintptr_t actor)
{
    if (s_containerScanNeg.size() > 16384)
        s_containerScanNeg.clear();
    if (s_containerMemoHistory.size() > 16384)
        s_containerMemoHistory.clear();
    const auto now = std::chrono::steady_clock::now();
    s_containerScanNeg[actor] = now;
    s_containerMemoHistory.emplace(actor, now);
}

bool QuickContainerCandidate(
    uint32_t maskedType,
    bool classChest,
    bool actorTypeChest,
    const std::string& fname,
    const std::string& classFname,
    bool hasContainerLoot,
    bool hasLootInteraction)
{
    if (classChest || actorTypeChest
        || ArcActorType::IsChestClassId(maskedType))
        return true;

    if (hasContainerLoot || hasLootInteraction)
        return true;

    if (!fname.empty()) {
        const std::string lower = ToLowerCopy(fname);
        if (lower.find("raidercache") != std::string::npos
            || lower.find("raider_cache") != std::string::npos
            || lower.find("bp_raidercache") != std::string::npos
            || lower.find("cargoship") != std::string::npos
            || lower.find("arc_cargo") != std::string::npos
            || lower.find("arc_cargoship") != std::string::npos)
            return true;
        if (FnameLooksLikeWorldContainer(fname))
            return true;
    }

    if (!classFname.empty() && FnameLooksLikeWorldContainer(classFname))
        return true;

    return false;
}

bool AdmitContainerActor(
    uintptr_t key,
    uint32_t classId,
    bool classChest,
    bool actorTypeChest,
    bool classGroundLoot,
    const std::string& fname,
    const std::string& classFname,
    const std::string& dataAssetFName,
    bool fnameIsContainer,
    bool fnameIsPickup,
    bool hasLootInteraction,
    bool hasContainerLoot)
{
    if (WorldScan::LooksLikePlayerPawn(key, 0))
        return false;
    if (classId != 0
        && ArcActorType::IsPlayerClassId(ArcActorType::MaskActorTypeId(classId)))
        return false;

    if (!fname.empty() && FnameLooksLikeHarvestableActor(fname))
        return false;
    if (!dataAssetFName.empty() && FnameLooksLikeHarvestableActor(dataAssetFName))
        return false;
    if (!fname.empty() && FnameLooksLikeDroppedPickup(fname))
        return false;
    if (!dataAssetFName.empty() && FnameLooksLikeDroppedPickup(dataAssetFName))
        return false;

    if (classChest || actorTypeChest)
        return true;

    if (!classFname.empty()
        && (FnameLooksLikeWorldContainer(classFname)
            || IsSalvageContainerActor(classFname, key)
            || IsRealSocketSalvageContainer(classFname, key)))
        return true;

    if (!fname.empty()) {
        const std::string lower = ToLowerCopy(fname);
        if (lower.find("raidercache") != std::string::npos
            || lower.find("raider_cache") != std::string::npos
            || lower.find("bp_raidercache") != std::string::npos
            || lower.find("cargoship") != std::string::npos
            || lower.find("arc_cargo") != std::string::npos
            || lower.find("arc_cargoship") != std::string::npos)
            return true;
    }

    if (hasContainerLoot && !fnameIsPickup && !classGroundLoot) {
        std::string probe = fname;
        if (!fnameIsContainer && !FnameLooksLikeWorldContainer(probe) && !classFname.empty())
            probe = classFname;
        if (probe.empty() && !dataAssetFName.empty())
            probe = dataAssetFName;
        if (!probe.empty()
            && (fnameIsContainer || FnameLooksLikeWorldContainer(probe)
                || IsSalvageContainerActor(probe, key)
                || IsRealSocketSalvageContainer(probe, key)))
            return true;
    }

    if (!fname.empty()) {
        if (fnameIsContainer)
            return true;
        if (IsSalvageContainerActor(fname, key))
            return true;
        if (IsRealSocketSalvageContainer(fname, key))
            return true;
    }

    if (!dataAssetFName.empty() && !fnameIsPickup) {
        if (FnameLooksLikeWorldContainer(dataAssetFName))
            return true;
        if (IsSalvageContainerActor(dataAssetFName, key))
            return true;
    }

    if (!fname.empty() && WorldObjectAdmitsByFName(fname)) {
        if (fnameIsPickup)
            return false;
        if (fnameIsContainer || classChest || actorTypeChest)
            return true;
    }

    if (hasLootInteraction && !fnameIsPickup && !classGroundLoot) {
        if (IsRealSocketSalvageContainer(fname, key))
            return true;

        const std::string probe = !fname.empty() ? fname : dataAssetFName;
        if (!probe.empty()) {
            if (IsSalvageContainerActor(probe, key)
                || IsRealSocketSalvageContainer(probe, key))
                return true;
            const std::string lower = ToLowerCopy(probe);
            if (IsWorldPropLootContainer(
                    hasLootInteraction, classChest || actorTypeChest, lower, std::string{}))
                return true;
        }
        // OuterPrivate-validated loot interaction with no readable fname still counts.
        return true;
    }

    if (hasContainerLoot && !fnameIsPickup && !classGroundLoot)
        return true;

    if (!fname.empty() && FnameAdmitsWorldActor(fname)) {
        if (fnameIsPickup && !fnameIsContainer)
            return false;
        if (fnameIsContainer || classChest || actorTypeChest)
            return true;
        if (hasLootInteraction && !classGroundLoot) {
            const std::string lower = ToLowerCopy(fname);
            if (IsWorldPropLootContainer(
                    hasLootInteraction, classChest || actorTypeChest, lower, std::string{}))
                return true;
        }
    }

    (void)classId;
    (void)key;
    return false;
}

WorldItemCategory ClassifyContainer(
    const std::string& fnameLower,
    const std::string& displayLower,
    bool hasLootInteraction,
    bool classChest,
    bool fnameIsContainer)
{
    WorldItemCategory cat = ClassifyWorldActor(fnameLower, displayLower);
    if (cat == WorldItemCategory::Invalid) {
        cat = InferLootWorldCategory(
            fnameLower, displayLower, "",
            hasLootInteraction,
            false,
            classChest || fnameIsContainer);
    }
    if (cat == WorldItemCategory::Invalid) {
        if (classChest || fnameIsContainer)
            cat = WorldItemCategory::Crate;
        else
            cat = WorldItemCategory::Other;
    }
    if (cat == WorldItemCategory::DroppedPickup || cat == WorldItemCategory::Items)
        cat = WorldItemCategory::Other;
    return cat;
}

// Cached actor pointers can lag up to 10s when actor count is stable; admission
// must scan the fresh level list or containers randomly never appear.
static std::unordered_map<uintptr_t, uint8_t> s_containerPosMisses;
static constexpr uint8_t kContainerPosMissEvict = 4;

static bool ContainerPosMissShouldEvict(uintptr_t key, bool posOk)
{
    return WorldScan::MissCounterShouldEvict(
        s_containerPosMisses, key, posOk, kContainerPosMissEvict);
}

static void ClearContainerPosMiss(uintptr_t key)
{
    WorldScan::MissCounterClear(s_containerPosMisses, key);
}

static void ClearContainerListStaticMaps()
{
    s_containerPosMisses.clear();
}

// #region agent log
static void AgentCrateLog(
    const char* hypothesisId,
    const char* message,
    const std::string& dataJson)
{
    std::ofstream f(kArcDebugLogPath, std::ios::app);
    if (!f)
        return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"crates\",\"hypothesisId\":\""
        << hypothesisId << "\",\"location\":\"ContainerList.cpp\",\"message\":\""
        << message << "\",\"data\":" << dataJson
        << ",\"timestamp\":" << ms << "}\n";
}
// #endregion

} // namespace

void Engine::ContainerList()
{
    if (!var::enable_world)
        return;
    if (!AnyContainerEspEnabled() && !var::showLoot
        && !(var::show_radar && var::show_radar_special))
        return;

    InitConsts();

    WorldScanContext ctx{};
    if (!GatherWorldScanContext(ctx))
        return;

    const uint64_t genAtStart =
        m_worldGeneration.load(std::memory_order_acquire);

    std::unordered_map<uintptr_t, WorldCacheEntry> localCache;
    {
        std::shared_lock<std::shared_mutex> lock(m_containerCacheMutex);
        localCache = containerCache;
    }

    const std::unordered_set<uint64_t> currentSet(
        ctx.currentActors.begin(),
        ctx.currentActors.end());
    WorldScan::PruneStaleEntries(localCache, currentSet);

    std::unordered_set<uintptr_t> occupiedCharacterKeys;
    {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        for (const auto& [key, _] : playerCache) {
            occupiedCharacterKeys.insert(key);
            // A player's equipped/stowed weapons are separate actors; keep them
            // out of the container scan so they never render as loose loot.
            WorldScan::CollectHeldItemActors(key, occupiedCharacterKeys);
        }
    }
    {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        for (const auto& [key, _] : robotCache)
            occupiedCharacterKeys.insert(key);
    }

    const uintptr_t localPawn = ctx.acknowledgedPawn;
    WorldScan::CollectHeldItemActors(localPawn, occupiedCharacterKeys);

    {
        std::shared_lock<std::shared_mutex> slock(m_stateMutex);
        if (PlayerController)
            occupiedCharacterKeys.insert(PlayerController);
        if (PioneerPlayerController)
            occupiedCharacterKeys.insert(PioneerPlayerController);
        if (PlayerCameraManager)
            occupiedCharacterKeys.insert(PlayerCameraManager);
    }

    // Drop any already-cached entry that turned out to be a held weapon/item or
    // a tracked character, so stale weapon-as-container labels disappear.
    for (uintptr_t occupied : occupiedCharacterKeys)
        localCache.erase(occupied);

    int dbgScanned = 0;
    int dbgAdmitted = 0;
    int dbgAdmitSkip = 0;
    int dbgPreSkip = 0;
    int dbgStructHit = 0;
    int dbgAdmitLooks = 0;
    int dbgAdmitGate = 0;
    int dbgRootSkip = 0;
    int dbgPosSkip = 0;
    int dbgDrawing = 0;

    static IntervalTimer metadataTimer(250);
    // Always admit — gating on a 50ms timer while the worker is already 50ms
    // left new crates waiting on flaky open/distance until the next lucky pass
    // (user saw ~30s blank then labels appear).
    const bool doAdmission = true;
    const bool doMetadata = metadataTimer.fire();

    int dbgMemoSkip = 0;
    int dbgRingChecked = 0;
    int dbgRingPrioNew = 0;
    int dbgRingSliceActors = 0;
    size_t dbgRingSlice = 0;
    size_t contN = 0;

    if (doAdmission) {
    // C7: prune memo entries for actors that left the world.
    for (auto it = s_containerScanNeg.begin(); it != s_containerScanNeg.end(); ) {
        if (!currentSet.contains(static_cast<uint64_t>(it->first)))
            it = s_containerScanNeg.erase(it);
        else
            ++it;
    }
    for (auto it = s_containerMemoHistory.begin(); it != s_containerMemoHistory.end(); ) {
        if (!currentSet.contains(static_cast<uint64_t>(it->first)))
            it = s_containerMemoHistory.erase(it);
        else
            ++it;
    }

    // Cheap CPU index of candidates; DMA probes only hit the current slice
    // plus a bounded set of newly-seen actors.
    std::vector<uintptr_t> admitIndex;
    admitIndex.reserve(ctx.currentActors.size());
    for (uint64_t actorU64 : ctx.currentActors) {
        const uintptr_t key = static_cast<uintptr_t>(actorU64);
        if (!key)
            continue;
        if (localPawn && key == localPawn)
            continue;
        if (occupiedCharacterKeys.contains(key))
            continue;
        // Cached containers are filtered in the probe loop, not here — dropping
        // them from the index would shrink contN as the cache fills and falsely
        // trip the ring-reset delta heuristic every pass.
        admitIndex.push_back(key);
    }

    contN = admitIndex.size();
    bool contRingReset = false;
    if (s_contAdmitRingGen != genAtStart) {
        contRingReset = true;
        // World generation changed — actor pointers are stale, clear everything.
        s_containerScanNeg.clear();
        s_containerMemoHistory.clear();
    } else if (s_contAdmitRingActorCount != 0) {
        const size_t delta = (contN > s_contAdmitRingActorCount)
            ? (contN - s_contAdmitRingActorCount)
            : (s_contAdmitRingActorCount - contN);
        // Streaming levels cause ±50-100 actor fluctuations every few seconds.
        // The old threshold (max(64, contN/8)) reset the ring ~every pass,
        // clearing the negative memo and losing all scan progress.  Use a
        // wider 25% threshold with a 128-actor floor so streaming jitter
        // does not thrash the ring.
        const size_t threshold = (std::max)(static_cast<size_t>(128), contN / 4);
        if (delta > threshold)
            contRingReset = true;
    }
    if (contRingReset) {
        s_contAdmitSliceCursor = 0;
        s_contAdmitCoveredMask = 0;
        s_contAdmitPrevActors.clear();
        s_contAdmitCycleStart = std::chrono::steady_clock::now();
        s_contAdmitLastCycleMs = 0;
        ++s_contAdmitRingResets;
        // Do NOT clear the negative memo — it remembers actors that were
        // probed and proven non-container, which saves hundreds of DMA
        // reads per ring cycle.  Only clear on world generation change
        // (handled above via s_contAdmitRingGen != genAtStart).
    }
    s_contAdmitRingGen = genAtStart;
    s_contAdmitRingActorCount = contN;
    if (s_contAdmitCycleStart.time_since_epoch().count() == 0)
        s_contAdmitCycleStart = std::chrono::steady_clock::now();

    const size_t contSlice = s_contAdmitSliceCursor % kContAdmitSlices;
    const size_t contBase = (contN * contSlice) / kContAdmitSlices;
    const size_t contSliceEnd = (contN * (contSlice + 1)) / kContAdmitSlices;
    dbgRingSlice = contSlice;

    std::unordered_set<uintptr_t> probeSet;
    probeSet.reserve((contSliceEnd - contBase) + kContAdmitPrioNewMax + 8);
    for (size_t i = contBase; i < contSliceEnd; ++i)
        probeSet.insert(admitIndex[i]);
    dbgRingSliceActors = static_cast<int>(probeSet.size());

    size_t prioAdded = 0;
    for (uintptr_t newKey : admitIndex) {
        if (prioAdded >= kContAdmitPrioNewMax)
            break;
        if (s_contAdmitPrevActors.contains(newKey))
            continue;
        if (probeSet.insert(newKey).second)
            ++prioAdded;
    }
    dbgRingPrioNew = static_cast<int>(prioAdded);
    s_contAdmitPrevActors.clear();
    s_contAdmitPrevActors.insert(admitIndex.begin(), admitIndex.end());

    for (uintptr_t key : probeSet) {
        if (localCache.contains(key))
            continue;
        if (ContainerScanNegMemoHit(key, dbgMemoSkip))
            continue;
        if (WorldScan::ShouldExcludeFromWorldCaches(key, localPawn))
            continue;
        if (WorldScan::IsHeldEquipmentActor(key))
            continue;

        ++dbgScanned;
        ++dbgRingChecked;

        const uint32_t maskedType =
            ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(key));
        if (ArcActorType::IsPlayerClassId(maskedType)) {
            ContainerScanNegMemoize(key);
            continue;
        }
        if (ArcActorType::IsBotClassId(maskedType)) {
            ContainerScanNegMemoize(key);
            continue;
        }

        const uint32_t classAt70 =
            Memory::read<uint32_t>(key + Offsets::ClassDefaultObject);
        const uint32_t classAt78 =
            Memory::read<uint32_t>(key + Offsets::ClassDefaultObjectAlt);
        const uint32_t classId = WorldScan::ResolveItemClassId(classAt70, classAt78);
        const bool classGroundLoot = classId != 0 &&
            ArcActorType::IsGroundLootClassId(ArcActorType::MaskActorTypeId(classId));
        const bool classChest = classId != 0 &&
            ArcActorType::IsChestClassId(ArcActorType::MaskActorTypeId(classId));
        const bool actorTypeChest =
            ArcActorType::IsChestClassId(maskedType);

        const std::string classFname = GetActorClassFName(key);
        if (!classFname.empty()) {
            if (FnameLooksLikeEngineSubobjectClass(classFname)) {
                ContainerScanNegMemoize(key);
                continue;
            }
            if (FnameExcludedFromContainerEsp(ToLowerCopy(classFname))) {
                ContainerScanNegMemoize(key);
                continue;
            }
        }

        if (WorldScan::LooksLikePlayerPawn(key, localPawn))
            continue;

        std::string fname = GetActorFNameStringCached(key);
        if (fname.empty())
            fname = GetActorFNameString(key);
        if (fname.empty() && !classFname.empty()
            && !FnameLooksLikeEngineSubobjectClass(classFname))
            fname = classFname;

        if (WorldScan::LooksLikeBotPawn(key, localPawn))
            continue;

        std::string dataAssetFName;
        bool fnameIsContainer = false;
        bool fnameIsPickup = false;
        if (!fname.empty()) {
            fnameIsContainer = FnameLooksLikeWorldContainer(fname);
            fnameIsPickup = FnameLooksLikeDroppedPickup(fname);
        }
        if (!fnameIsContainer && !classFname.empty())
            fnameIsContainer = FnameLooksLikeWorldContainer(classFname);

        dataAssetFName = GetActorDataAssetFName(key);

        const uintptr_t containerLoot =
            Memory::read<uintptr_t>(key + Offsets::LootInteraction_Container);
        // Soft path: valid pointer at SDK offset is enough. Outer/class-FName
        // decrypt flakes and was dropping locker banks (2 of 5). Player/bot
        // excludes already ran above.
        const bool hasContainerLoot =
            (containerLoot != 0 && Memory::IsValidPtrFast2(containerLoot))
            || LootInteractionOwnedByActor(containerLoot, key)
            || PointerIsLootInteractionComponent(containerLoot);

        const uintptr_t lootComp =
            Memory::read<uintptr_t>(key + Offsets::LootInteractionComponent);
        const bool hasLootInteraction =
            (lootComp != 0 && Memory::IsValidPtrFast2(lootComp))
            || LootInteractionOwnedByActor(lootComp, key)
            || PointerIsLootInteractionComponent(lootComp);

        // Data-asset DA_Item_* must NOT force a pickup skip when the actor is a
        // chest/container by fname/class OR has a real loot-interaction pointer.
        // That was dropping whole container banks (lockers, crates, etc.) whenever
        // ItemDataAsset decrypted, while DA-flake siblings still showed.
        if (!fnameIsPickup && !fnameIsContainer && !classChest && !actorTypeChest
            && !hasContainerLoot && !hasLootInteraction
            && !dataAssetFName.empty())
            fnameIsPickup = FnameLooksLikeDroppedPickup(dataAssetFName);

        // Ground loot never belongs in the container cache — even if a loot
        // interaction pointer is present (caused Oil ESP doubled as Crate).
        // C7b: no memo on these — the chest/container guards read flaky DMA
        // pointers, so a real crate can land here on a bad pass and must be
        // retried next slice instead of being locked out for the memo TTL.
        if ((classGroundLoot || fnameIsPickup)
            && !fnameIsContainer && !classChest && !actorTypeChest)
            continue;
        if (!classFname.empty() && FnameLooksLikeDroppedPickup(classFname)
            && !fnameIsContainer && !classChest && !actorTypeChest)
            continue;
        if (!dataAssetFName.empty() && FnameLooksLikeDroppedPickup(dataAssetFName)
            && !fnameIsContainer && !classChest && !actorTypeChest)
            continue;

        if (!fname.empty() && FnameExcludedFromContainerEsp(ToLowerCopy(fname))) {
            ContainerScanNegMemoize(key);
            continue;
        }

        // Only drop clear ground-item actors. Do NOT skip when fname already looks
        // like a container, or when either loot pointer is valid.
        const uint64_t itemDa =
            Memory::read<uint64_t>(key + static_cast<uint64_t>(Offsets::ItemDataAsset));
        if (itemDa != 0 && Memory::IsValidPtrFast2(itemDa)
            && !hasContainerLoot && !hasLootInteraction
            && !actorTypeChest && !classChest && !fnameIsContainer
            && TryReadItemGameAssetIdFromActor(key) != 0) {
            ContainerScanNegMemoize(key);
            continue;
        }

        if (!QuickContainerCandidate(
                maskedType, classChest, actorTypeChest, fname, classFname,
                hasContainerLoot, hasLootInteraction)) {
            // Chest class / actor-type still qualifies even when fname decrypt
            // and loot pointers flake (blank ESP while standing on crates).
            if (!classChest && !actorTypeChest && !fnameIsContainer)
            {
                ++dbgPreSkip;
                // C7b: no memo — QuickContainerCandidate depends on flaky loot
                // pointers; a locker flaking here was invisible for the TTL.
                continue;
            }
        }

        if (ContainerLootLooksOpened(key, fname.empty() ? classFname : fname)
            && !var::show_world_open_container)
            continue;

        // Phase 3E: bare hasLootInteraction/hasContainerLoot alone admitted
        // volumes/StaticMeshActor as "Crate" (debug-c190fb). Require class/fname/
        // structural proof; AdmitContainerActor still covers flaky-decrypt lockers.
        const bool looksLikeContainer =
            classChest || actorTypeChest || fnameIsContainer
            || (!classFname.empty()
                && (FnameLooksLikeWorldContainer(classFname)
                    || IsSalvageContainerActor(classFname, key)
                    || IsRealSocketSalvageContainer(classFname, key)))
            || WorldScan::LooksLikeContainerActor(key, fname)
            || (hasContainerLoot && fnameIsContainer);

        if (!looksLikeContainer) {
            if (!AdmitContainerActor(
                    key, classId, classChest, actorTypeChest, classGroundLoot, fname,
                    classFname, dataAssetFName, fnameIsContainer, fnameIsPickup,
                    hasLootInteraction, hasContainerLoot)) {
                ++dbgAdmitSkip;
                ++dbgStructHit;
                // C7b: no memo — AdmitContainerActor also reads flaky structs.
                continue;
            }
            ++dbgAdmitGate;
        } else {
            ++dbgAdmitLooks;
        }

        const uintptr_t root = Engine::ResolveActorRoot(key);
        if (!root || !IsValidPointer(root)) {
            ++dbgRootSkip;
            continue;
        }

        const Vector3 worldPos = ReadSceneWorldPos(root);
        if (WorldScan::IsOldStyleInvalidXY(worldPos)) {
            ++dbgPosSkip;
            continue;
        }

        if (fname.empty() && !dataAssetFName.empty())
            fname = dataAssetFName;

        std::string displayName;
        // HIGH-PRIORITY: the container's own type keyword (locker/safe/cabinet/...)
        // in the actor, class, or data-asset fname wins over every category/CSV
        // lookup below, so a "locker"-named actor is always "Locker".
        displayName = DirectContainerKeywordLabel(fname, classFname, dataAssetFName);
        if (displayName.empty()
            && !fname.empty() && !FnameLooksLikeEngineSubobjectClass(fname)) {
            displayName = ResolveContainerDisplayLabel(fname, {});
            if (displayName.empty() || IsGenericWorldEspLabel(displayName))
                displayName = LookupWorldObjectByFName(fname);
        }
        if ((displayName.empty() || IsGenericWorldEspLabel(displayName)
                || !IsPlausibleEspLabel(displayName))
            && !classFname.empty() && !FnameLooksLikeEngineSubobjectClass(classFname)) {
            displayName = ResolveContainerDisplayLabel(classFname, {});
            if (displayName.empty() || IsGenericWorldEspLabel(displayName))
                displayName = LookupWorldObjectByFName(classFname);
        }
        if (displayName.empty() || IsGenericWorldEspLabel(displayName)
            || !IsPlausibleEspLabel(displayName)) {
            const std::string memName = GetEnglishItemName(key);
            if (!memName.empty() && !IsGenericWorldEspLabel(memName)
                && IsPlausibleEspLabel(memName) && !IsJunkWorldEspLabel(memName)
                && !IsGarbledEspLabel(memName)
                && memName.find('_') == std::string::npos
                && memName.size() <= 28)
                displayName = memName;
        }
        if (displayName.empty() || IsGenericWorldEspLabel(displayName)
            || !IsPlausibleEspLabel(displayName)) {
            const std::string hint = !fname.empty() ? fname : dataAssetFName;
            displayName = ResolveContainerEspDisplayName(key, hint);
        }
        if (displayName.empty() || IsGenericWorldEspLabel(displayName)
            || !IsPlausibleEspLabel(displayName))
            displayName = ResolveWorldDisplayLabel(key, fname, 0);
        if (IsJunkWorldEspLabel(displayName) || IsGarbledEspLabel(displayName)
            || !IsPlausibleEspLabel(displayName))
            displayName.clear();

        if (IsJunkWorldEspLabel(displayName) || IsGarbledEspLabel(displayName)) {
            const std::string memName = GetEnglishItemName(key);
            if (!memName.empty() && !IsGenericWorldEspLabel(memName)
                && IsPlausibleEspLabel(memName) && !IsJunkWorldEspLabel(memName)
                && !IsGarbledEspLabel(memName)
                && memName.find('_') == std::string::npos
                && memName.size() <= 28)
                displayName = memName;
        }

        const std::string fnameLower = ToLowerCopy(fname);
        const std::string displayLower = ToLowerCopy(displayName);

        WorldItemCategory cat = ClassifyContainer(
            fnameLower, displayLower, hasLootInteraction, classChest, fnameIsContainer);

        // Owned loot-interaction actors with flaky FName often land in Other and
        // disappear when only Locker/Furniture toggles are on. Re-home them.
        if ((cat == WorldItemCategory::Other || cat == WorldItemCategory::Invalid
                || cat == WorldItemCategory::Trash)
            && (hasLootInteraction || hasContainerLoot || classChest || actorTypeChest)) {
            WorldItemCategory fromDisplay = ClassifyWorldActor(std::string{}, displayLower);
            if (fromDisplay != WorldItemCategory::Invalid
                && fromDisplay != WorldItemCategory::Other
                && fromDisplay != WorldItemCategory::Trash)
                cat = fromDisplay;
            else if (displayLower.find("locker") != std::string::npos)
                cat = WorldItemCategory::Locker;
            else if (displayLower.find("drawer") != std::string::npos)
                cat = WorldItemCategory::Furniture;
            else if (displayLower.find("safe") != std::string::npos)
                cat = WorldItemCategory::Safe;
            else
                cat = WorldItemCategory::Crate;
        }

        if (cat == WorldItemCategory::Trash) {
            ContainerScanNegMemoize(key);
            continue;
        }

        if (displayName.empty() || IsGenericWorldEspLabel(displayName)
            || !IsPlausibleEspLabel(displayName) || IsJunkWorldEspLabel(displayName)) {
            if (const char* catLabel = WorldItemCategoryLabel(cat)) {
                const std::string s(catLabel);
                if (!s.empty() && s != "Unknown" && s != "Other" && s != "Items"
                    && IsPlausibleEspLabel(s) && !IsGenericWorldEspLabel(s)
                    && !IsJunkWorldEspLabel(s))
                    displayName = s;
            }
        }

        if (IsJunkWorldEspLabel(displayName) || IsGarbledEspLabel(displayName)
            || displayName.empty() || !IsPlausibleEspLabel(displayName)) {
            if (cat == WorldItemCategory::Invalid
                || cat == WorldItemCategory::Other
                || cat == WorldItemCategory::Trash)
                continue;
            const std::string fallback =
                ContainerCategoryFallbackEspLabel(cat);
            if (!fallback.empty())
                displayName = fallback;
        }

        if (displayName.empty()) {
            const char* catFallback = WorldItemCategoryLabel(cat);
            if (catFallback && catFallback[0] && std::string(catFallback) != "Unknown"
                && cat != WorldItemCategory::Invalid
                && cat != WorldItemCategory::Other
                && cat != WorldItemCategory::Trash)
                displayName = std::string(catFallback);
        }

        displayName = FormatEspDisplayLabel(displayName);
        if (displayName.empty())
            continue;

        // Distance/Drawing owned by FinalizeWorldCacheMap (item parity) — do not
        // cull at admit or crates beyond loot_distance never enter cache.
        auto& entry = localCache[key];
        entry.rootComponent = root;
        entry.APawn = key;
        entry.ActorName = fname;
        entry.ItemDisplayName = displayName;
        entry.ItemType = displayName;
        entry.worldCategory = static_cast<uint8_t>(cat);
        entry.lootRarityTier = 0;
        entry.lootValue = 0;
        entry.WorldPos = worldPos;
        ++dbgAdmitted;

        // #region agent log
        if (const auto histIt = s_containerMemoHistory.find(key);
            histIt != s_containerMemoHistory.end()) {
            const int lockedSec = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - histIt->second).count());
            char lockBuf[256];
            snprintf(lockBuf, sizeof(lockBuf),
                "{\"label\":\"%.48s\",\"lockedSec\":%d,\"key\":%llu}",
                displayName.c_str(), lockedSec,
                static_cast<unsigned long long>(key));
            AgentCrateLog("C7b", "container_memo_lockout", lockBuf);
            s_containerMemoHistory.erase(histIt);
        }
        // #endregion

        // #region agent log
        {
            const Vector3 d = worldPos - ctx.camera.Location;
            const int admitDistM = static_cast<int>(
                std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) / 100.0);
            char buf[256];
            snprintf(buf, sizeof(buf),
                "{\"label\":\"%.48s\",\"cat\":%d,\"distM\":%d,\"key\":%llu}",
                displayName.c_str(), static_cast<int>(cat), admitDistM,
                static_cast<unsigned long long>(key));
            AgentCrateLog("CA", "container_admit", buf);
        }
        // #endregion


    }

    // C7: advance the ring only after the slice fully ran.
    s_contAdmitCoveredMask |= (1ull << (dbgRingSlice % kContAdmitSlices));
    s_contAdmitSliceCursor = (dbgRingSlice + 1) % kContAdmitSlices;
    if (s_contAdmitSliceCursor == 0) {
        const auto nowCycle = std::chrono::steady_clock::now();
        if (s_contAdmitCycleStart.time_since_epoch().count() != 0)
            s_contAdmitLastCycleMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowCycle - s_contAdmitCycleStart).count());
        s_contAdmitCycleStart = nowCycle;
        s_contAdmitCoveredMask = 0;
    }

    // #region agent log
    {
        static std::chrono::steady_clock::time_point s_lastRingLog{};
        const auto nowRl = std::chrono::steady_clock::now();
        if (s_lastRingLog.time_since_epoch().count() == 0
            || nowRl - s_lastRingLog >= std::chrono::seconds(2)) {
            s_lastRingLog = nowRl;
            char buf[320];
            snprintf(buf, sizeof(buf),
                "{\"actors\":%zu,\"slice\":%zu,\"sliceActors\":%d,\"prioNew\":%d,"
                "\"checked\":%d,\"memoSkip\":%d,\"memoSize\":%zu,\"coverMask\":%llu,"
                "\"cycleMs\":%d,\"ringResets\":%d}",
                contN, dbgRingSlice, dbgRingSliceActors, dbgRingPrioNew,
                dbgRingChecked, dbgMemoSkip, s_containerScanNeg.size(),
                static_cast<unsigned long long>(s_contAdmitCoveredMask),
                s_contAdmitLastCycleMs, s_contAdmitRingResets);
            AgentCrateLog("C7", "container_admit_ring", buf);
        }
    }
    // #endregion
    }

    WorldScan::DedupeWorldCacheByRoot(localCache);

    std::vector<decltype(localCache)::iterator> retainIters;
    std::vector<uintptr_t> retainRoots;
    std::vector<std::string> retainClassFnames;
    retainIters.reserve(localCache.size());
    retainRoots.reserve(localCache.size());
    retainClassFnames.reserve(localCache.size());

    for (auto it = localCache.begin(); it != localCache.end(); ) {
        const uintptr_t key = it->first;

        if (occupiedCharacterKeys.contains(key)) {
            it = localCache.erase(it);
            continue;
        }
        if (WorldScan::ShouldExcludeFromWorldCaches(key, localPawn)) {
            it = localCache.erase(it);
            continue;
        }
        if (WorldScan::IsHeldEquipmentActor(key)) {
            it = localCache.erase(it);
            continue;
        }

        std::string fname = it->second.ActorName;
        if (fname.empty())
            fname = GetActorFNameStringCached(key);

        const auto retainCat = static_cast<WorldItemCategory>(it->second.worldCategory);
        if (WorldLootCacheEntryDepleted(key, fname, retainCat)) {
            it = localCache.erase(it);
            continue;
        }

        const std::string classFname = GetActorClassFName(key);
        if (!fname.empty() && FnameExcludedFromContainerEsp(ToLowerCopy(fname))) {
            it = localCache.erase(it);
            continue;
        }
        if (!classFname.empty()
            && (FnameExcludedFromContainerEsp(ToLowerCopy(classFname))
                || FnameLooksLikeEngineSubobjectClass(classFname))) {
            it = localCache.erase(it);
            continue;
        }
        if (IsJunkWorldEspLabel(it->second.ItemDisplayName)
            || IsGarbledEspLabel(it->second.ItemDisplayName)
            || it->second.ItemDisplayName.empty()) {
            const auto cat = static_cast<WorldItemCategory>(it->second.worldCategory);
            if (cat == WorldItemCategory::Invalid
                || cat == WorldItemCategory::Other
                || cat == WorldItemCategory::Trash) {
                it = localCache.erase(it);
                continue;
            }
            std::string fixed = ContainerCategoryFallbackEspLabel(cat);
            if (!fixed.empty()) {
                it->second.ItemDisplayName = fixed;
                it->second.ItemType = fixed;
            }
        }

        // Phase 3E retain re-verify REMOVED (debug-c190fb): predicate could not
        // see classChest/actorTypeChest/AdmitContainerActor proof from admit, so
        // it evicted real containers every scan (180,588 evicts vs 181,814
        // admits — Car/Locker/Crate churned each frame). Admission already
        // verified these entries; junk/excluded checks above handle stale junk.
        // #region agent log
        {
            const bool verifyLooks =
                WorldScan::LooksLikeContainerActor(key, fname)
                || (!classFname.empty()
                    && (FnameLooksLikeWorldContainer(classFname)
                        || IsSalvageContainerActor(classFname, key)
                        || IsRealSocketSalvageContainer(classFname, key)))
                || FnameLooksLikeWorldContainer(fname);
            if (!verifyLooks) {
                static std::unordered_set<uintptr_t> s_verifySoftMissSeen;
                if (s_verifySoftMissSeen.insert(key).second) {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "{\"label\":\"%.48s\",\"fname\":\"%.48s\",\"key\":%llu}",
                        it->second.ItemDisplayName.c_str(), fname.c_str(),
                        static_cast<unsigned long long>(key));
                    AgentCrateLog("G2", "container_verify_softmiss", buf);
                }
            }
        }
        // #endregion

        retainIters.push_back(it);
        retainRoots.push_back(0);
        retainClassFnames.push_back(classFname);
        ++it;
    }

    if (!retainIters.empty()) {
        std::vector<WorldScan::CacheRootScatterRow> scatterRows;
        scatterRows.reserve(retainIters.size());
        for (size_t i = 0; i < retainIters.size(); ++i) {
            WorldScan::CacheRootScatterRow row{};
            row.actorKey = retainIters[i]->first;
            scatterRows.push_back(row);
        }
        if (WorldScan::ScatterReadActorRootPositions(scatterRows)) {
            for (size_t i = 0; i < retainIters.size(); ++i) {
                const WorldScan::CacheRootScatterRow& row = scatterRows[i];
                if (!row.rootValid)
                    continue;
                retainRoots[i] = row.root;
                auto& entry = retainIters[i]->second;
                entry.rootComponent = row.root;
                if (row.posValid)
                    entry.WorldPos = row.worldPos;
            }
        }
    }

    struct OpenProbeCand {
        uintptr_t key = 0;
        size_t retainIdx = 0;
        float openDistM = 0.f;
    };
    std::vector<OpenProbeCand> openProbeCands;
    openProbeCands.reserve(4);

    for (size_t i = 0; i < retainIters.size(); ++i) {
        const uintptr_t key = retainIters[i]->first;
        const uintptr_t root = retainRoots[i];
        const bool posOk = root && IsValidPointer(root)
            && IsPlausibleWorldPos(retainIters[i]->second.WorldPos);
        if (!posOk) {
            ++dbgPosSkip;
            if (ContainerPosMissShouldEvict(key, false)) {
                ClearContainerPosMiss(key);
                // #region agent log
                {
                    char buf[192];
                    snprintf(buf, sizeof(buf),
                        "{\"label\":\"%.48s\",\"key\":%llu}",
                        localCache[key].ItemDisplayName.c_str(),
                        static_cast<unsigned long long>(key));
                    AgentCrateLog("CB", "container_pos_evict", buf);
                }
                // #endregion
                localCache.erase(key);
            }
        } else {
            ContainerPosMissShouldEvict(key, true);

            const std::string& retainFname = retainIters[i]->second.ActorName;
            const std::string& retainClassFname = retainClassFnames[i];
            const std::string retainFnameLower = ToLowerCopy(
                retainFname.empty() ? retainClassFname : retainFname);
            const bool classChest =
                !retainClassFname.empty()
                && FnameLooksLikeWorldContainer(retainClassFname);
            const bool fnameIsContainer =
                !retainFnameLower.empty()
                && FnameLooksLikeWorldContainer(retainFnameLower);
            // CC retain re-verify eviction REMOVED (debug-c190fb post-fix run):
            // same defect as the G2 one — the retain predicate cannot see the
            // chest-class / actor-type / loot-pointer proof admission had, so
            // real Crates/Buttons whose fname flaked at retain were evicted and
            // re-admitted every ~270ms. Admission stays authoritative.
            const bool stillContainer =
                classChest || fnameIsContainer
                || WorldScan::LooksLikeContainerActor(key, retainFname)
                || (!retainClassFname.empty()
                    && (IsSalvageContainerActor(retainClassFname, key)
                        || IsRealSocketSalvageContainer(retainClassFname, key)));
            if (!stillContainer) {
                // #region agent log
                {
                    static std::unordered_set<uintptr_t> s_ccSoftMissSeen;
                    if (s_ccSoftMissSeen.insert(key).second) {
                        char buf[192];
                        snprintf(buf, sizeof(buf),
                            "{\"label\":\"%.48s\",\"key\":%llu}",
                            retainIters[i]->second.ItemDisplayName.c_str(),
                            static_cast<unsigned long long>(key));
                        AgentCrateLog("CC", "container_verify_softmiss", buf);
                    }
                }
                // #endregion
            }

            // O1: open state was only probed at ADMISSION, so a locker opened
            // mid-session kept drawing as unopened until app restart (fresh
            // admission re-probed it). Re-probe cached entries near the player.
            // O2 regression fix (debug-c190fb: frame_build spiked 46ms -> 282ms
            // and boxes slid off moving players): probing EVERY container within
            // 60m on EVERY 16ms pass flooded the DMA bus and starved the frame
            // builder. Bound it: per-container cooldown + per-pass cap keeps the
            // flip under ~2s at a fraction of the reads.
            {
                static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
                    s_lastOpenProbe;
                static auto s_lastProbePass = std::chrono::steady_clock::time_point{};
                constexpr auto kOpenProbeCooldown = std::chrono::milliseconds(1500);
                constexpr auto kOpenProbePassGap = std::chrono::milliseconds(250);
                constexpr int kOpenProbeMaxPerPass = 4;
                static int s_openProbesThisPass = 0;
                static bool s_probePassAllowed = false;
                if (i == 0) {
                    s_openProbesThisPass = 0;
                    const auto nowPass = std::chrono::steady_clock::now();
                    s_probePassAllowed =
                        s_lastProbePass.time_since_epoch().count() == 0
                        || nowPass - s_lastProbePass >= kOpenProbePassGap;
                    if (s_probePassAllowed)
                        s_lastProbePass = nowPass;
                }

                const Vector3 od = retainIters[i]->second.WorldPos - ctx.camera.Location;
                const float openDistM = static_cast<float>(std::sqrt(
                    od.x * od.x + od.y * od.y + od.z * od.z)) / 100.0f;
                const auto nowProbe = std::chrono::steady_clock::now();
                bool mayProbe = s_probePassAllowed
                    && openDistM <= 60.0f
                    && s_openProbesThisPass < kOpenProbeMaxPerPass;
                if (mayProbe) {
                    const auto lastIt = s_lastOpenProbe.find(key);
                    if (lastIt != s_lastOpenProbe.end()
                        && nowProbe - lastIt->second < kOpenProbeCooldown)
                        mayProbe = false;
                }
                if (mayProbe) {
                    ++s_openProbesThisPass;
                    s_lastOpenProbe[key] = nowProbe;
                    if (s_lastOpenProbe.size() > 4096)
                        s_lastOpenProbe.clear();
                }
                if (mayProbe) {
                    OpenProbeCand cand{};
                    cand.key = key;
                    cand.retainIdx = i;
                    cand.openDistM = openDistM;
                    openProbeCands.push_back(cand);
                }
            }
        }
    }

    // P4: scatter LI pointers + searched bytes for the bounded probe set.
    if (!openProbeCands.empty()) {
        struct OpenProbeRow {
            uintptr_t key = 0;
            size_t retainIdx = 0;
            float openDistM = 0.f;
            uintptr_t liComp = 0;
            uintptr_t liCont = 0;
            uintptr_t liSimple = 0;
            uint8_t searched0 = 0;
            uint8_t searched1 = 0;
            uint8_t searched2 = 0;
        };
        std::vector<OpenProbeRow> rows;
        rows.reserve(openProbeCands.size());
        for (const auto& c : openProbeCands) {
            OpenProbeRow row{};
            row.key = c.key;
            row.retainIdx = c.retainIdx;
            row.openDistM = c.openDistM;
            rows.push_back(row);
        }
        int scatterExecs = 0;
        {
            ScatterSession s1;
            if (s1.isValid()) {
                bool ok = true;
                for (auto& row : rows) {
                    ok = s1.prepare(
                             row.key + Offsets::LootInteractionComponent, row.liComp)
                        && ok;
                    ok = s1.prepare(
                             row.key + Offsets::LootInteraction_Container, row.liCont)
                        && ok;
                    ok = s1.prepare(
                             row.key + Offsets::SimpleLootActivity_LootInteraction,
                             row.liSimple)
                        && ok;
                }
                if (ok && s1.execute())
                    ++scatterExecs;
            }
        }
        {
            ScatterSession s2;
            if (s2.isValid()) {
                bool ok = true;
                int prepared = 0;
                for (auto& row : rows) {
                    if (row.liComp && Memory::IsValidPtrFast2(row.liComp)) {
                        ok = s2.prepare(
                                 row.liComp + Offsets::LootInteraction_Searched,
                                 row.searched0)
                            && ok;
                        ++prepared;
                    }
                    if (row.liCont && Memory::IsValidPtrFast2(row.liCont)) {
                        ok = s2.prepare(
                                 row.liCont + Offsets::LootInteraction_Searched,
                                 row.searched1)
                            && ok;
                        ++prepared;
                    }
                    if (row.liSimple && Memory::IsValidPtrFast2(row.liSimple)) {
                        ok = s2.prepare(
                                 row.liSimple + Offsets::LootInteraction_Searched,
                                 row.searched2)
                            && ok;
                        ++prepared;
                    }
                }
                if (prepared > 0 && ok && s2.execute())
                    ++scatterExecs;
            }
        }
        std::unordered_set<uintptr_t> eraseOpened;
        for (auto& row : rows) {
            const bool opened =
                ((row.liComp && Memory::IsValidPtrFast2(row.liComp)
                     && (row.searched0 & 0x1) != 0)
                    || (row.liCont && Memory::IsValidPtrFast2(row.liCont)
                        && (row.searched1 & 0x1) != 0)
                    || (row.liSimple && Memory::IsValidPtrFast2(row.liSimple)
                        && (row.searched2 & 0x1) != 0));
            if (!opened)
                continue;
            // #region agent log
            {
                static std::unordered_set<uintptr_t> s_openFlipSeen;
                if (s_openFlipSeen.insert(row.key).second) {
                    char buf[224];
                    const auto& entry = retainIters[row.retainIdx]->second;
                    snprintf(buf, sizeof(buf),
                        "{\"label\":\"%.48s\",\"distM\":%d,\"hideOpened\":%d,\"key\":%llu}",
                        entry.ItemDisplayName.c_str(),
                        static_cast<int>(row.openDistM),
                        var::show_world_open_container ? 0 : 1,
                        static_cast<unsigned long long>(row.key));
                    AgentCrateLog("O1", "container_open_flip", buf);
                }
            }
            // #endregion
            if (!var::show_world_open_container)
                eraseOpened.insert(row.key);
        }
        for (uintptr_t key : eraseOpened)
            localCache.erase(key);
        // #region agent log
        {
            static auto s_lastOpenBatch = std::chrono::steady_clock::time_point{};
            const auto nowB = std::chrono::steady_clock::now();
            if (s_lastOpenBatch.time_since_epoch().count() == 0
                || nowB - s_lastOpenBatch >= std::chrono::seconds(2)) {
                s_lastOpenBatch = nowB;
                std::ofstream f(kArcDebugLogPath, std::ios::app);
                if (f) {
                    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    f << "{\"sessionId\":\"c190fb\",\"runId\":\"batch\",\"hypothesisId\":\"P4\","
                      << "\"location\":\"ContainerList.cpp:ContainerList\",\"message\":\"container_open_batch\","
                      << "\"data\":{\"probed\":" << rows.size()
                      << ",\"scatterExecs\":" << scatterExecs << "}"
                      << ",\"timestamp\":" << ts << "}\n";
                }
            }
        }
        // #endregion
    }

    if (doMetadata) {
        for (auto& [key, entry] : localCache) {
            entry.lootRarityTier = 0;
            entry.lootValue = 0;
        }
    }

    FinalizeWorldCacheMap(localCache, ctx.camera, dbgDrawing);



    if (m_worldGeneration.load(std::memory_order_acquire) != genAtStart)
        return;

    {
        std::unique_lock<std::shared_mutex> lock(m_containerCacheMutex);
        containerCache = std::move(localCache);
    }

    if (var::show_debug_overlay) {
        std::shared_lock<std::shared_mutex> lock(m_containerCacheMutex);
        int dbgOpenedDrawing = 0;
        int dbgUnopenedDrawing = 0;
        int dbgOpenSalvage = 0;
        int dbgOpenItemContainer = 0;
        int dbgOpenSearched = 0;
        static std::unordered_set<std::string> s_seenContainerClasses;
        for (const auto& [actorKey, entry] : containerCache) {
            if (!entry.Drawing)
                continue;
            if (ContainerLootLooksOpened(actorKey, entry.ActorName))
                ++dbgOpenedDrawing;
            else
                ++dbgUnopenedDrawing;

            switch (ProbeContainerOpenSignals(actorKey, entry.ActorName)) {
            case ContainerOpenSignal::SalvageMesh:
                ++dbgOpenSalvage;
                break;
            case ContainerOpenSignal::ItemContainer:
                ++dbgOpenItemContainer;
                break;
            case ContainerOpenSignal::LootSearched:
                ++dbgOpenSearched;
                break;
            default:
                break;
            }

            // Name discovery: print decrypted class name per distinct container type.
            const uintptr_t classPtr = steam_decrypt::GetActorClassPtr(actorKey);
            const std::string classFname = GetActorClassFName(actorKey);
            const int32_t classFnameId = classPtr ? GetActorFNameId(classPtr) : 0;

            const int32_t actorFnameId = GetActorFNameId(actorKey);
            const std::string actorFnameLive = GetActorFNameString(actorKey);

            char dedupeBuf[160]{};
            if (classPtr)
                snprintf(dedupeBuf, sizeof(dedupeBuf), "c:%llx|%s",
                    static_cast<unsigned long long>(classPtr),
                    entry.ItemDisplayName.c_str());
            else
                snprintf(dedupeBuf, sizeof(dedupeBuf), "a:%d|%s",
                    actorFnameId, entry.ItemDisplayName.c_str());

            if (!s_seenContainerClasses.insert(dedupeBuf).second)
                continue;

            std::cout << "[containerName]"
                << " actor=" << std::hex << actorKey << std::dec
                << " classPtr=" << std::hex << classPtr << std::dec
                << " classId=" << classFnameId
                << " class=\"" << classFname << "\""
                << " actorId=" << actorFnameId
                << " actorFname=\"" << actorFnameLive << "\""
                << " cachedFname=\"" << entry.ActorName << "\""
                << " dataAsset=\"" << GetActorDataAssetFName(actorKey) << "\""
                << " label=\"" << entry.ItemDisplayName << "\""
                << std::endl;

        }
        std::cout << "[debugContainer] scanned=" << dbgScanned
            << " admitted=" << dbgAdmitted
            << " admitSkip=" << dbgAdmitSkip
            << " preSkip=" << dbgPreSkip
            << " structHit=" << dbgStructHit
            << " admitLooks=" << dbgAdmitLooks
            << " admitGate=" << dbgAdmitGate
            << " rootSkip=" << dbgRootSkip
            << " posSkip=" << dbgPosSkip
            << " drawing=" << dbgDrawing
            << " opened=" << dbgOpenedDrawing
            << " unopened=" << dbgUnopenedDrawing
            << " openSalvage=" << dbgOpenSalvage
            << " openItemContainer=" << dbgOpenItemContainer
            << " openSearched=" << dbgOpenSearched
            << " cache=" << containerCache.size()
            << " lootDist=" << static_cast<int>(var::loot_distance)
            << " spDist=" << static_cast<int>(var::container_distance_sp)
            << std::endl;

    }
}

namespace WorldScan {

void ClearContainerScannerStaticState()
{
    ClearContainerListStaticMaps();
}

} // namespace WorldScan
