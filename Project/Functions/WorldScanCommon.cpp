#include "WorldScanCommon.h"
#include "../Core/ActorType.h"
#include "../Core/AssetNames.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Core/WorldItemCategory.h"
#include "../Interface/Utils/Variables/index.h"

#include <chrono>
#include <cmath>
#include <cctype>
#include <mutex>
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

    const uintptr_t actorData = Memory::read<uintptr_t>(level + Offsets::AActors);
    const int32_t count = Memory::read<int32_t>(level + Offsets::ActorsCount);
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

    // (1) Inventory CurrentItemActors[0] — local + remotes when replicated.
    const uintptr_t inv =
        Memory::read<uintptr_t>(pawn + Offsets::InventoryComponent);
    if (inv && engine.IsValidPointer(inv)) {
        const uint64_t data =
            Memory::read<uint64_t>(inv + Offsets::LocalCurrentItemActors);
        const int32_t count =
            Memory::read<int32_t>(inv + Offsets::LocalCurrentItemActors + 0x8);
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

    // (1) Local InventoryComponent item array — accurate for the LOCAL player.
    const uintptr_t inv =
        Memory::read<uintptr_t>(pawn + Offsets::InventoryComponent);
    if (inv && engine.IsValidPointer(inv)) {
        // FTArray at LocalCurrentItemActors: { void* Data; int32 Count; int32 Max; }
        const uint64_t data =
            Memory::read<uint64_t>(inv + Offsets::LocalCurrentItemActors);
        const int32_t count =
            Memory::read<int32_t>(inv + Offsets::LocalCurrentItemActors + 0x8);
        if (data && count > 0 && count <= 64) {
            for (int32_t i = 0; i < count; ++i) {
                const uintptr_t item = Memory::read<uintptr_t>(
                    data + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
                if (!item || !engine.IsValidPointer(item))
                    continue;
                // Never blacklist floor/world harvest shells via inventory array
                // if DMA ever aliases them — held weapons/use-items still exclude.
                std::string cls = engine.GetActorClassFName(item);
                if (cls.empty())
                    cls = engine.GetActorFNameStringCached(item);
                if (!cls.empty() && FnameLooksLikeHarvestableActor(cls))
                    continue;
                // Console+NDJSON proof: occupied_held skipped masked=0xC0000 keys
                // (incl. CameraMgr) — LocalCurrentItemActors can alias floor loot /
                // garbage pointers. Only blacklist real held/use actors.
                if (ScoreHeldUseActor(item) < 0 && !IsHeldEquipmentActor(item))
                    continue;
                out.insert(item);
            }
        }
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

} // namespace WorldScan
