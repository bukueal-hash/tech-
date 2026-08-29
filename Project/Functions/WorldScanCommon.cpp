#include "WorldScanCommon.h"
#include "../Core/ActorType.h"
#include "../Core/AssetNames.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Core/AgentLog.h"
#include "../Core/WorldItemCategory.h"
#include "../Interface/Utils/Variables/index.h"
#include "LrtsVisibility.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace WorldScan {

namespace {

struct TArrayU64 {
    uintptr_t Data = 0;
    int32_t Num = 0;
    int32_t Max = 0;
};

constexpr int32_t kMaxActorsPerLevel = 12000;
constexpr int32_t kMaxActorsTotal = 16384;

struct CachedActorPtr {
    uintptr_t addr = 0;
};

void AppendLevelActors(
    uintptr_t level,
    std::vector<uint64_t>& actors,
    std::unordered_set<uint64_t>& seen,
    int maxTotal)
{
    if (!level || actors.size() >= static_cast<size_t>(maxTotal))
        return;

    uintptr_t actorData = 0;
    int32_t count = 0;
    if (!ReadLevelActors(level, actorData, count))
        return;
    if (!actorData || count <= 0 || count > kMaxActorsPerLevel)
        return;

    std::vector<uint64_t> chunk(static_cast<size_t>(count));
    if (!Memory::ReadRaw(actorData, chunk.data(), static_cast<size_t>(count) * sizeof(uint64_t)))
        return;

    for (uint64_t a : chunk) {
        if (!a || seen.contains(a))
            continue;
        seen.insert(a);
        actors.push_back(a);
        if (actors.size() >= static_cast<size_t>(maxTotal))
            break;
    }
}

void CollectAllLevelActors(
    uintptr_t uworld,
    uintptr_t persistentLevel,
    std::vector<uint64_t>& actors)
{
    std::unordered_set<uint64_t> seen;
    actors.clear();
    AppendLevelActors(persistentLevel, actors, seen, kMaxActorsTotal);

    if (!uworld)
        return;

    const TArrayU64 levels = Memory::read<TArrayU64>(uworld + Offsets::Levels);
    if (!levels.Data || levels.Num <= 0 || levels.Num > 64)
        return;

    for (int32_t i = 0; i < levels.Num && actors.size() < static_cast<size_t>(kMaxActorsTotal); ++i) {
        const uintptr_t level = Memory::read<uintptr_t>(
            levels.Data + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
        AppendLevelActors(level, actors, seen, kMaxActorsTotal);
    }
}

std::vector<CachedActorPtr> g_cachedActorPtrs;
std::chrono::steady_clock::time_point g_actorListTimer{};
int32_t g_lastActorCount = -1;
int32_t g_lastActorMax = -1;
std::mutex g_actorCacheMutex;

bool RefreshActorPointerCache(
    const std::vector<uint64_t>& actorPtrs,
    int32_t actorCount,
    int32_t actorMax)
{
    const auto now = std::chrono::steady_clock::now();

    std::vector<CachedActorPtr> fresh;
    fresh.reserve(actorPtrs.size());
    for (uint64_t addr : actorPtrs) {
        if (!addr || !engine.IsValidPointer(addr))
            continue;
        fresh.push_back({ static_cast<uintptr_t>(addr) });
    }

    if (fresh.empty() && !g_cachedActorPtrs.empty())
        return true;

    g_cachedActorPtrs = std::move(fresh);
    g_lastActorCount = actorCount;
    g_lastActorMax = actorMax;
    g_actorListTimer = now;

    return !g_cachedActorPtrs.empty();
}

} // namespace

bool ReadLevelActors(uintptr_t level, uintptr_t& outData, int32_t& outCount)
{
    outData = 0;
    outCount = 0;
    if (!level || !Engine::IsPlausibleObjPtr(level))
        return false;

    // Primary path: Level → ActorCluster (LevelActorContainer*) → Actors
    const uintptr_t cluster = Memory::read<uintptr_t>(level + Offsets::ActorCluster);
    if (Engine::IsPlausibleObjPtr(cluster)) {
        const uintptr_t data = Memory::read<uintptr_t>(
            cluster + Offsets::LevelActorContainer_Actors);
        const int32_t count = Memory::read<int32_t>(
            cluster + Offsets::LevelActorContainer_ActorCount);
        if (Engine::IsPlausibleObjPtr(data) && count > 0 && count <= 12000) {
            outData = data;
            outCount = count;
            return true;
        }
    }

    // Fallback: Level → direct Actors TArray (forum live-pinned 0x108/0x110/0x114)
    // Non-UPROPERTY; not in SDK dump but confirmed by qwe900.
    {
        const uintptr_t data = Memory::read<uintptr_t>(level + Offsets::AActors);
        const int32_t count = Memory::read<int32_t>(level + Offsets::ActorsCount);
        if (Engine::IsPlausibleObjPtr(data) && count > 0 && count <= 12000) {
            outData = data;
            outCount = count;
            return true;
        }
    }

    return false;
}

void ClearCachedActorPtrs()
{
    std::lock_guard<std::mutex> lock(g_actorCacheMutex);
    g_cachedActorPtrs.clear();
    g_actorListTimer = {};
    g_lastActorCount = -1;
    g_lastActorMax = -1;
}

bool IsOldStyleInvalidXY(const Vector3& pos)
{
    return pos.x == 0.0 && pos.y == 0.0;
}

uint32_t ResolveItemClassId(uint32_t atPrimary, uint32_t atAlt)
{
    const uint32_t maskedPrimary = ArcActorType::MaskActorTypeId(atPrimary);
    if (ArcActorType::IsWorldItemClassIdMasked(maskedPrimary))
        return atPrimary;
    const uint32_t maskedAlt = ArcActorType::MaskActorTypeId(atAlt);
    if (ArcActorType::IsWorldItemClassIdMasked(maskedAlt))
        return atAlt;
    return 0;
}

void CollectLevelActors(
    uintptr_t uworld,
    uintptr_t persistentLevel,
    std::vector<uint64_t>& actors)
{
    CollectAllLevelActors(uworld, persistentLevel, actors);

    // Flaky-read hold: the level actor-array read is a ~12KB DMA transfer per
    // scanner pass. Under bus contention it can come back short or empty —
    // scanners treat the list as ground truth and ERASE every cache entry not
    // in it. That meant a player running up during a firefight evicted ALL
    // cached players/bots/loot ("player ran up, no ESP"), and the same flakes
    // are the residual random ESP flicker. If the fresh list is a small
    // fraction of the last good one, serve the last good list for up to 2s.
    static std::mutex s_holdMu;
    static std::vector<uint64_t> s_lastGood;
    static std::chrono::steady_clock::time_point s_lastGoodTp{};
    std::lock_guard<std::mutex> lock(s_holdMu);
    const auto now = std::chrono::steady_clock::now();
    if (!s_lastGood.empty()
        && now - s_lastGoodTp < std::chrono::seconds(2)
        && actors.size() * 2 < s_lastGood.size()) {
        actors = s_lastGood;
        return;
    }
    if (!actors.empty()) {
        s_lastGood = actors;
        s_lastGoodTp = now;
    }
}

bool RefreshCachedActorPtrs(
    const std::vector<uint64_t>& actorPtrs,
    int32_t actorCount,
    int32_t actorMax)
{
    std::lock_guard<std::mutex> lock(g_actorCacheMutex);
    return RefreshActorPointerCache(actorPtrs, actorCount, actorMax);
}

const std::vector<uintptr_t>& CachedActorPtrs()
{
    static thread_local std::vector<uintptr_t> out;
    std::lock_guard<std::mutex> lock(g_actorCacheMutex);
    out.clear();
    out.reserve(g_cachedActorPtrs.size());
    for (const CachedActorPtr& cached : g_cachedActorPtrs)
        out.push_back(cached.addr);
    return out;
}

void PruneStaleEntries(
    std::unordered_map<uintptr_t, Engine::WorldCacheEntry>& cache,
    const std::unordered_set<uint64_t>& currentSet)
{
    for (auto it = cache.begin(); it != cache.end(); ) {
        if (!currentSet.contains(it->first))
            it = cache.erase(it);
        else
            ++it;
    }
}

bool LooksLikePlayerPawn(uintptr_t actor, uintptr_t localPawn)
{
    if (!actor)
        return false;
    if (localPawn && actor == localPawn)
        return true;

    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsPlayerClassId(masked))
        return true;
    if (ArcActorType::IsBotClassId(masked))
        return false;
    // Floor loot class id can have garbage PS/mesh pointers — never treat as player
    // (console: ShouldExclude wiped masked=0xC0000 rack/floor items).
    if (ArcActorType::IsGroundLootClassId(masked))
        return false;

    const uintptr_t playerState =
        Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (!playerState || !engine.IsValidPointer(playerState))
        return false;

    const uintptr_t mesh = engine.GetActorSkeletalMesh(actor);
    if (!mesh || !engine.IsValidPointer(mesh))
        return false;

    return true;
}

bool HasArcEnemyAssetPointer(uintptr_t actor)
{
    if (!actor)
        return false;

    auto validDa = [&](std::ptrdiff_t off) -> bool {
        const uint64_t da =
            Memory::read<uint64_t>(actor + static_cast<uint64_t>(off));
        return da != 0 && Memory::IsValidPtrFast2(da);
    };

    return validDa(Offsets::Constructable_EnemyTypeDataAsset)
        || validDa(Offsets::Constructable_AITemplateData);
}

// SHARED GATE — grep callers before edit
bool LooksLikeContainerActor(uintptr_t actor, const std::string& fname)
{
    if (!actor)
        return false;

    const std::string classFname = engine.GetActorClassFName(actor);
    if (!classFname.empty()) {
        if (FnameLooksLikeEngineSubobjectClass(classFname))
            return false;
        std::string classLower = classFname;
        for (char& c : classLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (FnameExcludedFromContainerEsp(classLower))
            return false;
        if (FnameLooksLikeWorldContainer(classFname)
            || IsSalvageContainerActor(classFname, actor)
            || IsRealSocketSalvageContainer(classFname, actor))
            return true;
    }

    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsPlayerClassId(masked))
        return false;
    if (LooksLikePlayerPawn(actor, 0))
        return false;
    if (ArcActorType::IsChestClassId(masked))
        return true;

    std::string probe = fname;
    if (probe.empty()) {
        probe = engine.GetActorFNameStringCached(actor);
        if (probe.empty())
            probe = engine.GetActorFNameString(actor);
    }

    auto fnameResolvesToBot = [&](const std::string& name) -> bool {
        if (name.empty())
            return false;
        if (!ResolveRobotTypeFromFName(engine, name).empty())
            return true;
        return !LookupEnemyBotByFName(name).empty();
    };

    auto hasReadableBotIdentity = [&]() -> bool {
        if (fnameResolvesToBot(probe))
            return true;

        static const std::ptrdiff_t kClassOffsets[] = { 0x10, 0x8 };
        for (const std::ptrdiff_t off : kClassOffsets) {
            const uint64_t uclass =
                Memory::read<uint64_t>(actor + static_cast<uint64_t>(off));
            if (!uclass || !Memory::IsValidPtrFast2(uclass))
                continue;
            std::string classFname = engine.GetActorFNameStringCached(uclass);
            if (classFname.empty())
                classFname = engine.GetActorFNameString(uclass);
            if (fnameResolvesToBot(classFname))
                return true;
        }

        if (const uintptr_t mesh = engine.GetActorSkeletalMesh(actor); mesh) {
            std::string meshFname = engine.GetActorFNameStringCached(mesh);
            if (meshFname.empty())
                meshFname = engine.GetActorFNameString(mesh);
            if (fnameResolvesToBot(meshFname))
                return true;
        }

        const uintptr_t embarkMesh =
            Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
        if (embarkMesh && engine.IsValidPointer(embarkMesh)) {
            std::string emFname = engine.GetActorFNameStringCached(embarkMesh);
            if (emFname.empty())
                emFname = engine.GetActorFNameString(embarkMesh);
            if (fnameResolvesToBot(emFname))
                return true;
        }

        if (HasArcEnemyAssetPointer(actor))
            return true;

        return false;
    };

    if (!probe.empty()) {
        std::string probeLower = probe;
        for (char& c : probeLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (FnameExcludedFromContainerEsp(probeLower))
            return false;
        if (FnameLooksLikeHarvestableActor(probe))
            return false;
        if (FnameLooksLikeDroppedPickup(probe))
            return false;
        if (FnameLooksLikeWorldContainer(probe))
            return true;
        if (IsSalvageContainerActor(probe, actor))
            return true;
    }

    const uintptr_t containerLoot =
        Memory::read<uintptr_t>(actor + Offsets::LootInteraction_Container);
    if (PointerIsLootInteractionComponent(containerLoot) && !probe.empty()) {
        if (FnameLooksLikeWorldContainer(probe)
            || IsSalvageContainerActor(probe, actor)
            || IsRealSocketSalvageContainer(probe, actor))
            return true;
    }

    const std::string itemDaFname = GetActorDataAssetFName(actor);
    if (!itemDaFname.empty() && FnameLooksLikeWorldContainer(itemDaFname))
        return true;

    // Loot interaction without enemy asset — only when the actor also looks like a
    // container by fname/data-asset, not any subobject that happens to reference
    // a loot-interaction component (player controller, status, etc.).
    const uintptr_t lootInteract =
        Memory::read<uintptr_t>(actor + Offsets::LootInteractionComponent);
    if (PointerIsLootInteractionComponent(lootInteract)
        && !HasArcEnemyAssetPointer(actor)
        && !hasReadableBotIdentity()) {
        if (!probe.empty()) {
            if (FnameLooksLikeWorldContainer(probe)
                || IsSalvageContainerActor(probe, actor))
                return true;
        }
        if (!itemDaFname.empty()
            && (FnameLooksLikeWorldContainer(itemDaFname)
                || IsSalvageContainerActor(itemDaFname, actor)))
            return true;
    }

    return false;
}

// SHARED GATE — grep callers before edit.
// Used only to EXCLUDE actors from item/container caches (not bot admission).
// tech- (bukueal-hash/tech-) early-outs floor loot via LootInteractionComponent.
// Our HasArcEnemyAssetPointer / fname path was false-positive on BP_ItemActor
// salvage (Ruined Parachute, Canister, …) and wiped majority floor coverage.
bool LooksLikeBotPawn(uintptr_t actor, uintptr_t localPawn)
{
    if (!actor || (localPawn && actor == localPawn))
        return false;

    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsPlayerClassId(masked))
        return false;

    // Ground-loot class id is authoritative floor salvage/pickup — never a bot.
    if (ArcActorType::IsGroundLootClassId(masked))
        return false;

    std::string probe = engine.GetActorClassFName(actor);
    if (probe.empty())
        probe = engine.GetActorFNameStringCached(actor);
    if (probe.empty())
        probe = engine.GetActorFNameString(actor);
    if (!probe.empty()) {
        if (FnameLooksLikeDroppedPickup(probe) || FnameLooksLikeHarvestableActor(probe))
            return false;
    }

    // tech- parity: anything with a loot interaction is world loot, not a bot.
    const uintptr_t lootComp =
        Memory::read<uintptr_t>(actor + Offsets::LootInteractionComponent);
    if (lootComp && engine.IsValidPointer(lootComp))
        return false;

    if (ArcActorType::IsBotClassId(masked))
        return true;

    if (ArcActorType::IsAnyBotActor(actor))
        return true;

    if (HasArcEnemyAssetPointer(actor))
        return true;

    const uintptr_t playerState =
        Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (playerState && engine.IsValidPointer(playerState))
        return false;

    auto fnameResolvesToBot = [](const std::string& name) -> bool {
        if (name.empty())
            return false;
        if (!ResolveRobotTypeFromFName(engine, name).empty())
            return true;
        return !LookupEnemyBotByFName(name).empty();
    };

    if (fnameResolvesToBot(probe))
        return true;

    const std::string fnameCached = engine.GetActorFNameStringCached(actor);
    if (fnameResolvesToBot(fnameCached))
        return true;

    return false;
}

bool ShouldExcludeFromWorldCaches(uintptr_t actor, uintptr_t localPawn)
{
    if (!actor)
        return true;

    // Extraction hatches are bot-class (EACTOR_TARGET) but must load into the
    // world/container cache as WorldItemCategory::Hatch. LooksLikeBotPawn below
    // would otherwise exclude them before the container scan can admit them.
    {
        std::string hprobe = engine.GetActorClassFName(actor);
        if (hprobe.empty())
            hprobe = engine.GetActorFNameStringCached(actor);
        if (hprobe.empty())
            hprobe = engine.GetActorFNameString(actor);
        if (FnameLooksLikeExtractionHatch(hprobe))
            return false;
    }

    // Never strip authoritative floor-loot class actors (0xC0000). Console proof:
    // skip_ground_loot_class ShouldExclude wiped rack items next to the player.
    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsGroundLootClassId(masked))
        return false;

    if (LooksLikePlayerPawn(actor, localPawn) || LooksLikeBotPawn(actor, localPawn))
        return true;
    if (IsHeldEquipmentActor(actor))
        return true;

    std::string probe = engine.GetActorClassFName(actor);
    if (probe.empty())
        probe = engine.GetActorFNameStringCached(actor);
    if (probe.empty())
        probe = engine.GetActorFNameString(actor);
    if (!probe.empty()) {
        if (FnameLooksLikeDroppedPickup(probe) || FnameLooksLikeHarvestableActor(probe))
            return false;
        std::string probeLower = probe;
        for (char& c : probeLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (FnameExcludedFromContainerEsp(probeLower))
            return true;
    }

    return false;
}

bool IsHeldEquipmentActor(uintptr_t actor)
{
    if (!actor)
        return false;

    // Narrow gate for WORLD exclusion only. Broad consumable/BP_ItemActor
    // matching here hid Prickly Pear (consumable_*) and Canister (BP_ItemActor_*)
    // via ShouldExcludeFromWorldCaches. Held-item ESP uses ScoreHeldUseActor below.
    auto checkName = [](const std::string& name) -> bool {
        if (name.empty())
            return false;
        if (name.find("BP_WeaponActor_") != std::string::npos)
            return true;
        if (name.find("StowedWeapon") != std::string::npos)
            return true;
        std::string lower = name;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("stowedweapon") != std::string::npos)
            return true;
        if (lower.find("inventoryservice") != std::string::npos)
            return true;
        if (lower.find("fakeinventory") != std::string::npos)
            return true;
        return false;
    };

    std::string cls = engine.GetActorClassFName(actor);
    if (checkName(cls))
        return true;

    std::string fname = engine.GetActorFNameStringCached(actor);
    if (fname.empty())
        fname = engine.GetActorFNameString(actor);
    if (checkName(fname))
        return true;

    if (cls.empty())
        cls = fname;
    if (cls.empty())
        return false;

    // Ground loot / dropped pickups keep Instigator=dropper but are not equipment.
    if (FnameLooksLikeDroppedPickup(cls))
        return false;

    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsGroundLootClassId(masked))
        return false;

    return false;
}

namespace {

/** Name score for held-use ESP only — NEVER used for world-cache exclusion. */
int ScoreHeldUseActor(uintptr_t actor)
{
    if (!actor || !engine.IsValidPointer(actor))
        return -1;

    std::string cls = engine.GetActorClassFName(actor);
    std::string fname = engine.GetActorFNameStringCached(actor);
    if (fname.empty())
        fname = engine.GetActorFNameString(actor);
    std::string lower = cls.empty() ? fname : cls;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.empty())
        return -1;
    if (lower.find("inventoryservice") != std::string::npos
        || lower.find("fakeinventory") != std::string::npos)
        return -1;
    // Plant harvestables never count as held. Floor salvage shells (Canister)
    // must not win via bare bp_itemactor — only known use-item tokens.
    if (FnameLooksLikeHarvestableActor(lower))
        return -1;
    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsGroundLootClassId(masked)
        && lower.find("bp_weaponactor") == std::string::npos
        && lower.find("weaponactor") == std::string::npos
        && lower.find("healinghot") == std::string::npos
        && lower.find("adrenaline") == std::string::npos
        && lower.find("defibrillator") == std::string::npos
        && lower.find("shieldovertime") == std::string::npos
        && lower.find("armor_patcher") == std::string::npos
        && lower.find("grenade") == std::string::npos
        && lower.find("throwable") == std::string::npos
        && lower.find("jumpmine") == std::string::npos
        && lower.find("bandage") == std::string::npos)
        return -1;

    int score = -1;
    if (lower.find("stowedweapon") != std::string::npos)
        score = 2;
    else if (lower.find("bp_weaponactor") != std::string::npos
        || lower.find("weaponactor") != std::string::npos)
        score = 100;
    else if (lower.find("healinghot") != std::string::npos
        || lower.find("adrenaline") != std::string::npos
        || lower.find("defibrillator") != std::string::npos
        || lower.find("shieldovertime") != std::string::npos
        || lower.find("armor_patcher") != std::string::npos
        || lower.find("grenade") != std::string::npos
        || lower.find("throwable") != std::string::npos
        || lower.find("jumpmine") != std::string::npos
        || lower.find("bandage") != std::string::npos)
        score = 90;
    // Bare BP_ItemActor / DA shells are floor loot, not auto-held.
    return score;
}

} // namespace

uintptr_t ResolvePreferredHeldItemActor(uintptr_t pawn)
{
    if (!pawn)
        return 0;

    // (1) Inventory CurrentItemActors[0] — replicated; LocalCurrentItemActors for self.
    const uintptr_t inv =
        Memory::read<uintptr_t>(pawn + Offsets::InventoryComponent);
    if (inv && engine.IsValidPointer(inv)) {
        uint64_t data =
            Memory::read<uint64_t>(inv + Offsets::CurrentItemActors);
        int32_t count =
            Memory::read<int32_t>(inv + Offsets::CurrentItemActors + 0x8);
        if (!data || count <= 0) {
            data = Memory::read<uint64_t>(inv + Offsets::LocalCurrentItemActors);
            count = Memory::read<int32_t>(inv + Offsets::LocalCurrentItemActors + 0x8);
        }
        if (data && count > 0 && count <= 64) {
            const uintptr_t item = Memory::read<uintptr_t>(data);
            // Inventory slot is authoritative for "in hand" — including BP_ItemActor
            // bandages/nades. Do not apply floor-shell reject scores here.
            if (item && engine.IsValidPointer(item)) {
                std::string cls = engine.GetActorClassFName(item);
                std::string lower = cls;
                for (char& c : lower)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower.find("inventoryservice") == std::string::npos
                    && lower.find("fakeinventory") == std::string::npos
                    && !FnameLooksLikeHarvestableActor(lower))
                    return item;
            }
        }
    }

    // (2) Remotes: scan level actors whose Instigator/Owner is this pawn.
    // Do NOT reuse IsHeldEquipmentActor here — that gate must stay weapon-only
    // so floor Canister / Prickly Pear stay in item ESP.
#pragma pack(push, 1)
    struct ActorOwnerInstigatorLocal {
        uintptr_t owner;
        uint8_t _pad[Offsets::ActorInstigator - Offsets::ActorOwner - sizeof(uintptr_t)];
        uintptr_t instigator;
    };
#pragma pack(pop)

    uintptr_t best = 0;
    int bestScore = -1;
    for (uintptr_t a : WorldScan::CachedActorPtrs()) {
        if (!a || a == pawn)
            continue;
        const ActorOwnerInstigatorLocal oi =
            Memory::read<ActorOwnerInstigatorLocal>(a + Offsets::ActorOwner);
        const bool linked =
            (oi.instigator == pawn && Memory::IsValidPtrFast2(oi.instigator))
            || (oi.owner == pawn && Memory::IsValidPtrFast2(oi.owner));
        if (!linked)
            continue;
        const int s = ScoreHeldUseActor(a);
        if (s > bestScore) {
            bestScore = s;
            best = a;
        }
    }
    return best;
}

namespace {

// One DMA read spanning AActor::Owner (0x1c0) and AActor::Instigator (0x210).
#pragma pack(push, 1)
struct ActorOwnerInstigator {
    uintptr_t owner;                                          // +0x1c0
    uint8_t   _pad[Offsets::ActorInstigator - Offsets::ActorOwner - sizeof(uintptr_t)];
    uintptr_t instigator;                                    // +0x210
};
#pragma pack(pop)

// Reverse index: holder pawn -> its held/equipped/stowed weapon & item actors.
// A remote player's held weapons live in separate replicated actors
// (BP_WeaponActor_* and StowedWeaponActor) whose Instigator/Owner is the pawn,
// NOT in the local-only InventoryComponent array. Rebuilt on a short TTL so the
// per-pawn calls from the container/item scans stay a cheap map lookup.
std::mutex g_heldIndexMutex;
std::unordered_map<uintptr_t, std::vector<uintptr_t>> g_heldByHolder;
std::chrono::steady_clock::time_point g_heldIndexBuilt{};

void RebuildHeldIndexLocked()
{
    g_heldByHolder.clear();
    for (uintptr_t a : WorldScan::CachedActorPtrs()) {
        if (!a || !IsHeldEquipmentActor(a))
            continue;
        const ActorOwnerInstigator oi =
            Memory::read<ActorOwnerInstigator>(a + Offsets::ActorOwner);
        if (oi.instigator && Memory::IsValidPtrFast2(oi.instigator))
            g_heldByHolder[oi.instigator].push_back(a);
        if (oi.owner && oi.owner != oi.instigator
            && Memory::IsValidPtrFast2(oi.owner))
            g_heldByHolder[oi.owner].push_back(a);
    }
    g_heldIndexBuilt = std::chrono::steady_clock::now();
}

} // namespace

void CollectHeldItemActors(uintptr_t pawn, std::unordered_set<uintptr_t>& out)
{
    if (!pawn)
        return;

    // (1) CurrentItemActors (replicated) then LocalCurrentItemActors.
    const uintptr_t inv =
        Memory::read<uintptr_t>(pawn + Offsets::InventoryComponent);
    if (inv && engine.IsValidPointer(inv)) {
        auto collectFromArr = [&](std::ptrdiff_t arrOff) {
            const uint64_t data = Memory::read<uint64_t>(inv + arrOff);
            const int32_t count = Memory::read<int32_t>(inv + arrOff + 0x8);
            if (!data || count <= 0 || count > 64)
                return;
            for (int32_t i = 0; i < count; ++i) {
                const uintptr_t item = Memory::read<uintptr_t>(
                    data + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
                if (!item || !engine.IsValidPointer(item))
                    continue;
                std::string cls = engine.GetActorClassFName(item);
                if (cls.empty())
                    cls = engine.GetActorFNameStringCached(item);
                if (!cls.empty() && FnameLooksLikeHarvestableActor(cls))
                    continue;
                if (ScoreHeldUseActor(item) < 0 && !IsHeldEquipmentActor(item))
                    continue;
                out.insert(item);
            }
        };
        collectFromArr(Offsets::CurrentItemActors);
        collectFromArr(Offsets::LocalCurrentItemActors);
    }

    // (2) Reverse Instigator/Owner index — covers REMOTE players' equipped and
    // stowed weapon actors (and other held item actors) so they never render as
    // a loose container/loot label sitting on the player.
    {
        std::lock_guard<std::mutex> lock(g_heldIndexMutex);
        const auto now = std::chrono::steady_clock::now();
        if (g_heldByHolder.empty()
            || now - g_heldIndexBuilt > std::chrono::milliseconds(200))
            RebuildHeldIndexLocked();
        if (auto it = g_heldByHolder.find(pawn); it != g_heldByHolder.end()) {
            for (uintptr_t held : it->second)
                out.insert(held);
        }
    }
}

bool ScatterReadActorRootPositions(std::vector<CacheRootScatterRow>& rows)
{
    if (rows.empty())
        return false;

    ScatterSession scatter;
    if (!scatter.isValid())
        return false;

    bool ok = true;
    for (CacheRootScatterRow& row : rows) {
        row.root = 0;
        row.posValid = false;
        row.rootValid = false;
        if (!row.actorKey)
            continue;
        ok = ok && scatter.prepare(row.actorKey + Offsets::RootComponent, row.root);
    }
    if (!ok || !scatter.execute())
        return false;

    for (CacheRootScatterRow& row : rows) {
        uintptr_t root = row.root;
        if (!root || !engine.IsValidPointer(root))
            root = Engine::ResolveLootActorRoot(row.actorKey);
        else {
            const Vector3 probe = Engine::ReadSceneWorldPos(root);
            if (!IsPlausibleWorldPos(probe))
                root = Engine::ResolveLootActorRoot(row.actorKey);
        }
        if (!root || !engine.IsValidPointer(root))
            continue;
        row.root = root;
        row.rootValid = true;
        const Vector3 worldPos = Engine::ReadSceneWorldPos(root);
        if (WorldScan::IsOldStyleInvalidXY(worldPos))
            continue;
        row.worldPos = worldPos;
        row.posValid = IsPlausibleWorldPos(worldPos);
    }

    return true;
}

void RefreshWorldCacheRetainPositions(std::vector<WorldCacheRetainRow>& rows)
{
    if (rows.empty())
        return;

    std::vector<CacheRootScatterRow> scatterRows;
    scatterRows.reserve(rows.size());
    for (const WorldCacheRetainRow& row : rows) {
        CacheRootScatterRow scatter{};
        scatter.actorKey = row.actorKey;
        scatterRows.push_back(scatter);
    }
    if (!ScatterReadActorRootPositions(scatterRows))
        return;

    for (size_t i = 0; i < rows.size(); ++i) {
        const CacheRootScatterRow& scatter = scatterRows[i];
        if (!scatter.rootValid)
            continue;
        if (rows[i].rootOut)
            *rows[i].rootOut = scatter.root;
        if (rows[i].entry) {
            rows[i].entry->rootComponent = scatter.root;
            if (scatter.posValid)
                rows[i].entry->WorldPos = scatter.worldPos;
        }
    }
}

void DedupeWorldCacheByRoot(std::unordered_map<uintptr_t, Engine::WorldCacheEntry>& cache)
{
    if (cache.size() < 2)
        return;

    std::unordered_map<uintptr_t, uintptr_t> rootWinner;
    std::unordered_set<uintptr_t> losers;

    auto prefer = [&](uintptr_t aKey, uintptr_t bKey) -> uintptr_t {
        const auto aIt = cache.find(aKey);
        const auto bIt = cache.find(bKey);
        if (aIt == cache.end())
            return bKey;
        if (bIt == cache.end())
            return aKey;
        const auto& a = aIt->second;
        const auto& b = bIt->second;
        if (a.lootValue != b.lootValue)
            return (a.lootValue > b.lootValue) ? aKey : bKey;
        const bool aName = !a.ItemDisplayName.empty();
        const bool bName = !b.ItemDisplayName.empty();
        if (aName != bName)
            return aName ? aKey : bKey;
        return aKey;
    };

    // Only collapse ground-loot twins that share a root. Real crates/lockers/safes
    // often resolve to the same RootComponent via DMA — full dedupe erased ~half.
    auto isRealContainer = [](const Engine::WorldCacheEntry& entry) -> bool {
        const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
        return WorldCategoryIsContainerProp(cat)
            && cat != WorldItemCategory::DroppedPickup
            && cat != WorldItemCategory::Items
            && cat != WorldItemCategory::Harvestable
            && cat != WorldItemCategory::Keys;
    };

    for (const auto& [key, entry] : cache) {
        if (isRealContainer(entry))
            continue;
        const uintptr_t root = entry.rootComponent;
        if (!root)
            continue;
        const auto it = rootWinner.find(root);
        if (it == rootWinner.end()) {
            rootWinner.emplace(root, key);
            continue;
        }
        const uintptr_t keep = prefer(key, it->second);
        const uintptr_t drop = (keep == key) ? it->second : key;
        it->second = keep;
        losers.insert(drop);
    }

    for (uintptr_t key : losers)
        cache.erase(key);
}

// #region agent log
namespace {

constexpr int kFlickerFixIteration = 14;
constexpr int kFlickerChannels = 6;
constexpr int kFlickerCauses = 7;

struct FlickerTrack {
    bool has = false;
    bool drawing = false;
    FlickerCause lastOffCause = FlickerCause::Other;
    std::chrono::steady_clock::time_point lastOff{};
};

struct FlickerBucket {
    std::unordered_map<uintptr_t, FlickerTrack> tracks;
    int causeCounts[kFlickerCauses]{};
};

FlickerBucket g_flickerBuckets[kFlickerChannels];
std::mutex g_flickerMu;
std::chrono::steady_clock::time_point g_flickerWindowStart{};
int g_flickerWindowTotals[kFlickerChannels]{};

// FREEZE1 (Fix #5): this used to open + write the log file while holding
// g_flickerMu. The 240fps paint thread takes the same mutex in
// NoteFlickerDrawing, so every 10s flush parked the render thread behind
// file I/O — visible as "ESP freezes, then picks back up". Now: snapshot
// counters under the lock, write the file with the lock RELEASED.
struct FlickerScoreSnapshot {
    int totals[kFlickerChannels]{};
    int byCause[kFlickerCauses]{};
};

FlickerScoreSnapshot TakeFlickerSnapshotLocked(
    const std::chrono::steady_clock::time_point& now)
{
    FlickerScoreSnapshot snap{};
    for (int ch = 0; ch < kFlickerChannels; ++ch) {
        snap.totals[ch] = g_flickerWindowTotals[ch];
        for (int c = 0; c < kFlickerCauses; ++c)
            snap.byCause[c] += g_flickerBuckets[ch].causeCounts[c];
        for (int c = 0; c < kFlickerCauses; ++c)
            g_flickerBuckets[ch].causeCounts[c] = 0;
        g_flickerWindowTotals[ch] = 0;
        if (g_flickerBuckets[ch].tracks.size() > 8192)
            g_flickerBuckets[ch].tracks.clear();
    }
    g_flickerWindowStart = now;
    return snap;
}

void WriteFlickerScoreUnlocked(const FlickerScoreSnapshot& snap)
{
    // Verify tap: flicker stats are the diagnosis for user-visible blink, so
    // they go to the real log (10 s cadence, counts only — negligible I/O).
    std::ofstream f(kArcVerifyPath, std::ios::app);
    if (!f)
        return;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"flicker-loop\",\"hypothesisId\":\"F0\","
      << "\"location\":\"WorldScanCommon.cpp\",\"message\":\"flicker_score\","
      << "\"data\":{\"bots\":" << snap.totals[0]
      << ",\"players\":" << snap.totals[1]
      << ",\"world\":" << snap.totals[2]
      << ",\"posFail\":" << snap.byCause[0]
      << ",\"visMiss\":" << snap.byCause[1]
      << ",\"evictReadmit\":" << snap.byCause[2]
      << ",\"distEdge\":" << snap.byCause[3]
      << ",\"other\":" << snap.byCause[4]
      << ",\"projFail\":" << snap.byCause[5]
      << ",\"labelMiss\":" << snap.byCause[6]
      << ",\"paintBots\":" << snap.totals[3]
      << ",\"paintPlayers\":" << snap.totals[4]
      << ",\"paintWorld\":" << snap.totals[5]
      << ",\"fixN\":" << kFlickerFixIteration
      << "},\"timestamp\":" << ts << "}\n";
}

} // namespace

void NoteFlickerDrawing(
    FlickerChannel channel,
    uintptr_t key,
    bool drawing,
    FlickerCause cause)
{
    if (!key)
        return;
    const int ch = static_cast<int>(channel);
    if (ch < 0 || ch >= kFlickerChannels)
        return;

    const auto now = std::chrono::steady_clock::now();
    // FREEZE1: the paint thread (channels 3-5) must NEVER block on this
    // mutex — a missed sample is harmless, a parked render thread is a
    // visible freeze. Scanner channels may still wait (they're off-frame).
    std::unique_lock<std::mutex> lock(g_flickerMu, std::defer_lock);
    if (ch >= 3) {
        if (!lock.try_lock())
            return;
    } else {
        lock.lock();
    }
    if (g_flickerWindowStart.time_since_epoch().count() == 0)
        g_flickerWindowStart = now;

    FlickerTrack& t = g_flickerBuckets[ch].tracks[key];
    if (!t.has) {
        t.has = true;
        t.drawing = drawing;
        return;
    }
    if (t.drawing == drawing)
        return;

    if (t.drawing && !drawing) {
        t.lastOff = now;
        t.lastOffCause = cause;
        t.drawing = false;
        return;
    }

    // false -> true
    t.drawing = true;
    if (t.lastOff.time_since_epoch().count() != 0
        && now - t.lastOff <= std::chrono::seconds(2)) {
        // Paint channels run at ~240fps; a 1-2 frame gap (<48ms) is invisible
        // to the eye and would flood the score with try_lock frame swaps.
        const bool paintChannel = ch >= 3;
        if (paintChannel && now - t.lastOff < std::chrono::milliseconds(48))
            return;
        const int causeIdx = static_cast<int>(t.lastOffCause);
        if (causeIdx >= 0 && causeIdx < kFlickerCauses)
            ++g_flickerBuckets[ch].causeCounts[causeIdx];
        ++g_flickerWindowTotals[ch];
    }
}

void NoteFlickerGone(FlickerChannel channel, uintptr_t key)
{
    NoteFlickerDrawing(channel, key, false, FlickerCause::EvictReadmit);
}

void MaybeFlushFlickerScore()
{
    const auto now = std::chrono::steady_clock::now();
    FlickerScoreSnapshot snap{};
    bool shouldWrite = false;
    {
        std::lock_guard<std::mutex> lock(g_flickerMu);
        if (g_flickerWindowStart.time_since_epoch().count() == 0) {
            g_flickerWindowStart = now;
            return;
        }
        if (now - g_flickerWindowStart < std::chrono::seconds(10))
            return;
        snap = TakeFlickerSnapshotLocked(now);
        shouldWrite = true;
    }
    // File I/O strictly outside the mutex (FREEZE1).
    if (shouldWrite)
        WriteFlickerScoreUnlocked(snap);
}

// #endregion

// ── AggGeom probe (diagnostic only, read-only) ─────────────────────────────
// Offsets taken from this repo's own SDK dump, not from a forum paste:
//   UStaticMesh::BodySetup          SDK/Class.cpp:26511   0x1F0
//   UBodySetup::AggGeom             SDK/Class.cpp:25263   0xB8, size 0x78,
//                                   INLINE struct — never dereference it
//   FKAggregateGeom TArray order    SDK/Struct.cpp:12248-12254
namespace {

constexpr uintptr_t kUStaticMesh_BodySetup = 0x1F0;
constexpr uintptr_t kUBodySetup_AggGeom = 0xB8;
constexpr int32_t kAggGeomMaxElems = 4096;
constexpr int kAggGeomArrays = 7;
constexpr int kAggGeomNameSamples = 64;

/** FName text is normally [A-Za-z0-9_], but never trust it into raw JSON. */
std::string JsonSafeName(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\')
            continue;
        out.push_back(static_cast<char>(c));
        if (out.size() >= 96)
            break;
    }
    return out;
}

std::mutex g_aggProbeMu;
AggGeomProbeResult g_aggProbe;
std::atomic<bool> g_aggProbeRunning{ false };

/**
 * Read the 7 inline TArray headers.
 *
 * All 7 come out of one 0x70 struct read, so a single garbage header means the
 * block is not an FKAggregateGeom at all. Reject the whole BodySetup instead of
 * trusting whichever slots happened to look sane — otherwise a junk header that
 * lands inside the sanity window (e.g. Num == Max == 0x1000) gets counted as
 * real collision.
 */
bool ReadAggGeomHeaders(
    uintptr_t bodySetup,
    int32_t outCounts[kAggGeomArrays],
    uint8_t outRaw[0x70])
{
    if (!Memory::ReadRaw(bodySetup + kUBodySetup_AggGeom, outRaw, 0x70))
        return false;

    for (int i = 0; i < kAggGeomArrays; ++i) {
        uintptr_t data = 0;
        int32_t num = 0;
        int32_t max = 0;
        memcpy(&data, outRaw + i * 0x10, sizeof(data));
        memcpy(&num, outRaw + i * 0x10 + 0x8, sizeof(num));
        memcpy(&max, outRaw + i * 0x10 + 0xC, sizeof(max));

        outCounts[i] = 0;
        if (!data && !num && !max)
            continue;  // genuinely empty slot

        if (num < 0 || max < 0 || num > max || max >= kAggGeomMaxElems
            || !Memory::IsValidPtrFast2(data))
            return false;

        outCounts[i] = num;
    }
    return true;
}

void RunAggGeomProbeJob()
{
    AggGeomProbeResult r;
    r.ran = true;

    const Engine::EngineStateSnapshot snap = engine.GetStateSnapshot();
    if (!snap.gWorld || !snap.persistentLevel) {
        r.note = "no gWorld/persistentLevel — not in raid";
        std::lock_guard<std::mutex> lock(g_aggProbeMu);
        g_aggProbe = r;
        return;
    }

    std::vector<uint64_t> actors;
    CollectLevelActors(snap.gWorld, snap.persistentLevel, actors);
    r.actorsWalked = static_cast<int>(actors.size());

    std::unordered_set<uintptr_t> seenBodySetups;
    int nameSamplesLogged = 0;

    for (uint64_t a : actors) {
        const uintptr_t actor = static_cast<uintptr_t>(a);
        if (!actor)
            continue;

        const uintptr_t root = Memory::read<uintptr_t>(actor + Offsets::RootComponent);
        if (!Engine::IsPlausibleObjPtr(root))
            continue;
        ++r.rootsValid;

        // No component-class filter here: neither engine.GetActorClassFName nor
        // steam_decrypt::GetActorClassFName resolves a name for root components
        // on this build, so filtering on it zeroed out the entire walk. The
        // whole-block validation in ReadAggGeomHeaders is what rejects chains
        // followed off a non-StaticMeshComponent root.
        uintptr_t staticMesh = Memory::read<uintptr_t>(root + Offsets::StaticMesh);
        bool legacy = false;
        if (!Engine::IsPlausibleObjPtr(staticMesh)) {
            staticMesh = Memory::read<uintptr_t>(root + Offsets::StaticMeshLegacy);
            legacy = true;
        }
        if (!Engine::IsPlausibleObjPtr(staticMesh))
            continue;
        if (legacy)
            ++r.meshFromLegacy;
        else
            ++r.meshFromPrimary;

        const uintptr_t bodySetup =
            Memory::read<uintptr_t>(staticMesh + kUStaticMesh_BodySetup);
        if (!Engine::IsPlausibleObjPtr(bodySetup))
            continue;
        ++r.bodySetupsValid;

        // Many actors share one UStaticMesh, so one UBodySetup. Read each once.
        if (!seenBodySetups.insert(bodySetup).second)
            continue;
        ++r.bodySetupsUnique;

        int32_t counts[kAggGeomArrays] = {};
        uint8_t raw[0x70] = {};
        if (!ReadAggGeomHeaders(bodySetup, counts, raw)) {
            ++r.headersRejected;
            continue;
        }

        r.sphereElems += counts[0];
        r.boxElems += counts[1];
        r.sphylElems += counts[2];
        r.convexElems += counts[3];
        r.taperedCapsuleElems += counts[4];
        r.levelSetElems += counts[5];
        r.skinnedLevelSetElems += counts[6];

        int total = 0;
        for (int i = 0; i < kAggGeomArrays; ++i)
            total += counts[i];
        if (total <= 0)
            continue;
        ++r.bodySetupsNonEmpty;

        // Name what the geometry actually belongs to. The UStaticMesh asset
        // FName is the decisive one: SM_Wall_* / SM_Floor_* means this covers
        // world structure, prop-only names mean AggGeom can't back a vis check.
        if (nameSamplesLogged < kAggGeomNameSamples) {
            ++nameSamplesLogged;

            std::string meshName = engine.GetActorFNameStringCached(staticMesh);
            if (meshName.empty())
                meshName = engine.GetActorFNameString(staticMesh);
            std::string actorName = engine.GetActorFNameStringCached(actor);
            if (actorName.empty())
                actorName = engine.GetActorFNameString(actor);

            std::ofstream f(kArcVerifyPath, std::ios::app);
            if (f) {
                const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\"location\":\"WorldScanCommon.cpp\","
                  << "\"message\":\"agggeom_sample\","
                  << "\"timestamp\":" << ts
                  << ",\"mesh\":\"" << JsonSafeName(meshName) << "\""
                  << ",\"actor\":\"" << JsonSafeName(actorName) << "\""
                  << ",\"bodySetup\":\"0x" << std::hex << bodySetup << std::dec << "\""
                  << ",\"sphere\":" << counts[0]
                  << ",\"box\":" << counts[1]
                  << ",\"sphyl\":" << counts[2]
                  << ",\"convex\":" << counts[3]
                  << ",\"tapered\":" << counts[4]
                  << ",\"levelSet\":" << counts[5]
                  << ",\"skinnedLevelSet\":" << counts[6]
                  << "}\n";
            }
        }
    }

    {
        std::ofstream f(kArcVerifyPath, std::ios::app);
        if (f) {
            const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            f << "{\"location\":\"WorldScanCommon.cpp\","
              << "\"message\":\"agggeom_probe\","
              << "\"timestamp\":" << ts
              << ",\"actorsWalked\":" << r.actorsWalked
              << ",\"rootsValid\":" << r.rootsValid
              << ",\"meshPrimary\":" << r.meshFromPrimary
              << ",\"meshLegacy\":" << r.meshFromLegacy
              << ",\"bodySetupsValid\":" << r.bodySetupsValid
              << ",\"bodySetupsUnique\":" << r.bodySetupsUnique
              << ",\"bodySetupsNonEmpty\":" << r.bodySetupsNonEmpty
              << ",\"headersRejected\":" << r.headersRejected
              << ",\"sphere\":" << r.sphereElems
              << ",\"box\":" << r.boxElems
              << ",\"sphyl\":" << r.sphylElems
              << ",\"convex\":" << r.convexElems
              << ",\"tapered\":" << r.taperedCapsuleElems
              << ",\"levelSet\":" << r.levelSetElems
              << ",\"skinnedLevelSet\":" << r.skinnedLevelSetElems
              << "}\n";
        }
    }

    std::lock_guard<std::mutex> lock(g_aggProbeMu);
    g_aggProbe = r;
}

// ── UWorld clock-field probe ────────────────────────────────────────────────

constexpr uint32_t kClockScanSize = 0x2000;
constexpr int kClockSampleMs = 1000;

std::mutex g_clockProbeMu;
TimeSecondsProbeResult g_clockProbe;
std::atomic<bool> g_clockProbeRunning{ false };

void RunTimeSecondsProbeJob()
{
    TimeSecondsProbeResult r;
    r.ran = true;

    const Engine::EngineStateSnapshot snap = engine.GetStateSnapshot();
    if (!snap.gWorld) {
        r.note = "no gWorld - not in raid";
        std::lock_guard<std::mutex> lock(g_clockProbeMu);
        g_clockProbe = r;
        return;
    }

    // Must bypass the VMM data cache. A cached read serves both samples from
    // the same page copy, every delta comes out zero, and a working clock
    // looks identical to a missing one.
    std::vector<uint8_t> a(kClockScanSize), b(kClockScanSize);
    const auto t0 = std::chrono::steady_clock::now();
    if (!PCIMemory::ReadVirtualMemoryNoCache(snap.gWorld, a.data(), kClockScanSize)) {
        r.note = "first sample read failed";
        std::lock_guard<std::mutex> lock(g_clockProbeMu);
        g_clockProbe = r;
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kClockSampleMs));

    const auto t1 = std::chrono::steady_clock::now();
    if (!PCIMemory::ReadVirtualMemoryNoCache(snap.gWorld, b.data(), kClockScanSize)) {
        r.note = "second sample read failed";
        std::lock_guard<std::mutex> lock(g_clockProbeMu);
        g_clockProbe = r;
        return;
    }

    r.elapsed = std::chrono::duration<double>(t1 - t0).count();

    r.bytesChanged = 0;
    for (uint32_t i = 0; i < kClockScanSize; ++i)
        if (a[i] != b[i])
            ++r.bytesChanged;

    // A real clock advances by exactly the wall time that passed. Pointers and
    // flags reinterpreted as doubles land in denormal or absurd ranges, so the
    // magnitude window plus the growth-rate window together leave very little
    // room for a false positive.
    const double lo = r.elapsed * 0.80;
    const double hi = r.elapsed * 1.20;

    std::ofstream f(kArcVerifyPath, std::ios::app);
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // UE5 widened TimeSeconds to double, but this build is reordered enough
    // that the type cannot be assumed either. Sweep both: doubles on 8-byte
    // alignment, floats on 4-byte.
    auto consider = [&](uint32_t off, bool isFloat, double v1, double v2) {
        if (!std::isfinite(v1) || !std::isfinite(v2))
            return;
        if (v1 < 1.0 || v1 > 1.0e7)
            return;

        const double delta = v2 - v1;
        if (delta < lo || delta > hi)
            return;

        if (r.hits == 0) {
            r.firstOffset = off;
            r.firstValue = v2;
            r.firstIsFloat = isFloat;
        }
        ++r.hits;

        if (f) {
            f << "{\"location\":\"WorldScanCommon.cpp\","
              << "\"message\":\"worldclock_candidate\","
              << "\"timestamp\":" << ts
              << ",\"offset\":\"0x" << std::hex << off << std::dec << "\""
              << ",\"type\":\"" << (isFloat ? "float" : "double") << "\""
              << ",\"first\":" << v1
              << ",\"second\":" << v2
              << ",\"delta\":" << delta
              << ",\"elapsed\":" << r.elapsed
              << "}\n";
        }
    };

    for (uint32_t off = 0; off + 8 <= kClockScanSize; off += 8) {
        double v1, v2;
        memcpy(&v1, a.data() + off, sizeof(v1));
        memcpy(&v2, b.data() + off, sizeof(v2));
        consider(off, false, v1, v2);
    }

    for (uint32_t off = 0; off + 4 <= kClockScanSize; off += 4) {
        float v1, v2;
        memcpy(&v1, a.data() + off, sizeof(v1));
        memcpy(&v2, b.data() + off, sizeof(v2));
        consider(off, true, static_cast<double>(v1), static_cast<double>(v2));
    }

    if (r.hits == 0) {
        r.note = r.bytesChanged == 0
            ? "both samples identical - reads still cached or world frozen"
            : "no advancing float or double in UWorld+0x000..0x2000";
    }

    if (f) {
        f << "{\"location\":\"WorldScanCommon.cpp\","
          << "\"message\":\"worldclock_summary\","
          << "\"timestamp\":" << ts
          << ",\"gWorld\":\"0x" << std::hex << snap.gWorld << std::dec << "\""
          << ",\"hits\":" << r.hits
          << ",\"firstOffset\":\"0x" << std::hex << r.firstOffset << std::dec << "\""
          << ",\"bytesChanged\":" << r.bytesChanged
          << ",\"elapsed\":" << r.elapsed
          << "}\n";
    }

    std::lock_guard<std::mutex> lock(g_clockProbeMu);
    g_clockProbe = r;
}

// ── Mesh render-tick probe ──────────────────────────────────────────────────

constexpr uint32_t kTickScanSize = 0x1000;
constexpr int kTickSamples = 200;
constexpr int kTickIntervalMs = 10;

std::mutex g_tickProbeMu;
TickProbeResult g_tickProbe;
std::atomic<bool> g_tickProbeRunning{ false };

void RunTickProbeJob()
{
    TickProbeResult r;
    r.ran = true;

    uint64_t mesh = 0;
    {
        std::lock_guard<std::mutex> lock(LrtsVis::g_session.mu);
        mesh = LrtsVis::g_session.lastMesh;
    }
    if (!mesh) {
        r.note = "no mesh seen yet - enable LRTS in a raid first";
        std::lock_guard<std::mutex> lock(g_tickProbeMu);
        g_tickProbe = r;
        return;
    }
    r.mesh = mesh;

    const uint32_t slots = kTickScanSize / 4;
    std::vector<uint32_t> prev(slots), cur(slots);
    std::vector<int> changes(slots, 0);

    if (!PCIMemory::ReadVirtualMemoryNoCache(mesh, prev.data(), kTickScanSize)) {
        r.note = "initial mesh read failed";
        std::lock_guard<std::mutex> lock(g_tickProbeMu);
        g_tickProbe = r;
        return;
    }

    for (int s = 0; s < kTickSamples; ++s) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickIntervalMs));
        if (!PCIMemory::ReadVirtualMemoryNoCache(mesh, cur.data(), kTickScanSize))
            continue;
        ++r.samples;
        for (uint32_t i = 0; i < slots; ++i) {
            if (cur[i] != prev[i]) {
                ++changes[i];
                prev[i] = cur[i];
            }
        }
    }

    for (uint32_t i = 0; i < slots; ++i) {
        if (changes[i] <= 0)
            continue;
        ++r.slotsChanged;
        // Keep the busiest slots. A per-frame timestamp changes far more often
        // than positions or animation state, so it rises to the top.
        for (int k = 0; k < TickProbeResult::kTop; ++k) {
            if (changes[i] > r.topCount[k]) {
                for (int j = TickProbeResult::kTop - 1; j > k; --j) {
                    r.topCount[j] = r.topCount[j - 1];
                    r.topOffset[j] = r.topOffset[j - 1];
                }
                r.topCount[k] = changes[i];
                r.topOffset[k] = i * 4;
                break;
            }
        }
    }

    if (r.slotsChanged == 0)
        r.note = "nothing changed - mesh may be dead or reads failing";

    {
        std::ofstream f(kArcVerifyPath, std::ios::app);
        if (f) {
            const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            for (uint32_t i = 0; i < slots; ++i) {
                if (changes[i] <= 0)
                    continue;
                f << "{\"location\":\"WorldScanCommon.cpp\","
                  << "\"message\":\"tick_slot\","
                  << "\"timestamp\":" << ts
                  << ",\"mesh\":\"0x" << std::hex << mesh << std::dec << "\""
                  << ",\"offset\":\"0x" << std::hex << (i * 4) << std::dec << "\""
                  << ",\"changes\":" << changes[i]
                  << ",\"samples\":" << r.samples
                  << "}\n";
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_tickProbeMu);
    g_tickProbe = r;
}

} // namespace

void StartTickProbe()
{
    bool expected = false;
    if (!g_tickProbeRunning.compare_exchange_strong(expected, true))
        return;

    {
        std::lock_guard<std::mutex> lock(g_tickProbeMu);
        g_tickProbe = TickProbeResult{};
        g_tickProbe.running = true;
    }

    std::thread([]() {
        try {
            RunTickProbeJob();
        } catch (...) {
            std::lock_guard<std::mutex> lock(g_tickProbeMu);
            g_tickProbe.ran = true;
            g_tickProbe.note = "probe threw";
        }
        {
            std::lock_guard<std::mutex> lock(g_tickProbeMu);
            g_tickProbe.running = false;
        }
        g_tickProbeRunning.store(false);
    }).detach();
}

TickProbeResult GetTickProbeResult()
{
    std::lock_guard<std::mutex> lock(g_tickProbeMu);
    return g_tickProbe;
}

void StartTimeSecondsProbe()
{
    bool expected = false;
    if (!g_clockProbeRunning.compare_exchange_strong(expected, true))
        return;

    {
        std::lock_guard<std::mutex> lock(g_clockProbeMu);
        g_clockProbe = TimeSecondsProbeResult{};
        g_clockProbe.running = true;
    }

    std::thread([]() {
        try {
            RunTimeSecondsProbeJob();
        } catch (...) {
            std::lock_guard<std::mutex> lock(g_clockProbeMu);
            g_clockProbe.ran = true;
            g_clockProbe.note = "probe threw";
        }
        {
            std::lock_guard<std::mutex> lock(g_clockProbeMu);
            g_clockProbe.running = false;
        }
        g_clockProbeRunning.store(false);
    }).detach();
}

TimeSecondsProbeResult GetTimeSecondsProbeResult()
{
    std::lock_guard<std::mutex> lock(g_clockProbeMu);
    return g_clockProbe;
}

void StartAggGeomProbe()
{
    bool expected = false;
    if (!g_aggProbeRunning.compare_exchange_strong(expected, true))
        return;

    {
        std::lock_guard<std::mutex> lock(g_aggProbeMu);
        g_aggProbe = AggGeomProbeResult{};
        g_aggProbe.running = true;
    }

    std::thread([]() {
        try {
            RunAggGeomProbeJob();
        } catch (...) {
            std::lock_guard<std::mutex> lock(g_aggProbeMu);
            g_aggProbe.ran = true;
            g_aggProbe.note = "probe threw";
        }
        {
            std::lock_guard<std::mutex> lock(g_aggProbeMu);
            g_aggProbe.running = false;
        }
        g_aggProbeRunning.store(false);
    }).detach();
}

AggGeomProbeResult GetAggGeomProbeResult()
{
    std::lock_guard<std::mutex> lock(g_aggProbeMu);
    return g_aggProbe;
}

} // namespace WorldScan
