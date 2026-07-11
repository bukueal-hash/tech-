#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/AssetNames.h"
#include "../Core/Offsets.h"
#include "../Core/WorldItemCategory.h"
#include "../Core/IntervalTimer.h"
#include "WorldScanCommon.h"
#include "../Interface/Utils/Variables/index.h"

#include <cctype>
#include <cmath>
#include <iostream>
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
        var::show_world_supply_station || var::show_world_locker ||
        var::show_world_safe || var::show_world_buried || var::show_world_deaddrop ||
        var::show_world_open_container);
}

std::string ToLowerCopy(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

float DistanceMeters(const Vector3& a, const Vector3& b)
{
    const double dx = static_cast<double>(a.x - b.x);
    const double dy = static_cast<double>(a.y - b.y);
    const double dz = static_cast<double>(a.z - b.z);
    return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0);
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
    }

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
static constexpr uint8_t kContainerPosMissEvict = 12;

static bool ContainerPosMissShouldEvict(uintptr_t key, bool posOk)
{
    if (posOk) {
        s_containerPosMisses.erase(key);
        return false;
    }
    const uint8_t misses = ++s_containerPosMisses[key];
    return misses >= kContainerPosMissEvict;
}

static void ClearContainerPosMiss(uintptr_t key)
{
    s_containerPosMisses.erase(key);
}

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

    static IntervalTimer admissionTimer(50);
    static IntervalTimer metadataTimer(250);
    const bool doAdmission = admissionTimer.fire() || localCache.empty();
    const bool doMetadata = metadataTimer.fire();

    if (doAdmission) {
    for (uint64_t actorU64 : ctx.currentActors) {
        const uintptr_t key = static_cast<uintptr_t>(actorU64);
        if (!key)
            continue;
        if (localPawn && key == localPawn)
            continue;
        if (occupiedCharacterKeys.contains(key))
            continue;
        if (WorldScan::ShouldExcludeFromWorldCaches(key, localPawn))
            continue;
        if (WorldScan::IsHeldEquipmentActor(key))
            continue;
        if (localCache.contains(key))
            continue;

        ++dbgScanned;

        const uint32_t maskedType =
            ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(key));
        if (ArcActorType::IsPlayerClassId(maskedType))
            continue;
        if (ArcActorType::IsBotClassId(maskedType))
            continue;

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
            if (FnameLooksLikeEngineSubobjectClass(classFname))
                continue;
            if (FnameExcludedFromContainerEsp(ToLowerCopy(classFname)))
                continue;
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
        if (!fnameIsPickup && !dataAssetFName.empty())
            fnameIsPickup = FnameLooksLikeDroppedPickup(dataAssetFName);

        if (classGroundLoot || fnameIsPickup)
            continue;
        if (!classFname.empty() && FnameLooksLikeDroppedPickup(classFname))
            continue;

        if (!fname.empty() && FnameExcludedFromContainerEsp(ToLowerCopy(fname)))
            continue;

        const uintptr_t containerLoot =
            Memory::read<uintptr_t>(key + Offsets::LootInteraction_Container);
        const bool hasContainerLoot =
            PointerIsLootInteractionComponent(containerLoot);

        const uint64_t itemDa =
            Memory::read<uint64_t>(key + static_cast<uint64_t>(Offsets::ItemDataAsset));
        if (itemDa != 0 && Memory::IsValidPtrFast2(itemDa)
            && !hasContainerLoot && !actorTypeChest && !classChest
            && TryReadItemGameAssetIdFromActor(key) != 0)
            continue;

        const uintptr_t lootComp =
            Memory::read<uintptr_t>(key + Offsets::LootInteractionComponent);
        const bool hasLootInteraction =
            PointerIsLootInteractionComponent(lootComp);

        if (!QuickContainerCandidate(
                maskedType, classChest, actorTypeChest, fname, classFname,
                hasContainerLoot, hasLootInteraction)) {
            ++dbgPreSkip;
            continue;
        }

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
                continue;
            }
            ++dbgAdmitGate;
        } else {
            ++dbgAdmitLooks;
        }

        const uintptr_t root = Memory::read<uintptr_t>(key + Offsets::RootComponent);
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
            displayName = GetEnglishItemName(key);
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
                && IsPlausibleEspLabel(memName) && !IsJunkWorldEspLabel(memName))
                displayName = memName;
        }

        const std::string fnameLower = ToLowerCopy(fname);
        const std::string displayLower = ToLowerCopy(displayName);

        const WorldItemCategory cat = ClassifyContainer(
            fnameLower, displayLower, hasLootInteraction, classChest, fnameIsContainer);

        if (cat == WorldItemCategory::Trash)
            continue;

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
            const std::string fallback =
                ContainerCategoryFallbackEspLabel(cat);
            if (!fallback.empty())
                displayName = fallback;
        }

        if (displayName.empty())
            continue;

        displayName = FormatEspDisplayLabel(displayName);
        if (displayName.empty())
            continue;

        const float distM = DistanceMeters(worldPos, ctx.camera.Location);
        WorldLootFilterView distView{
            static_cast<uint8_t>(cat),
            fname,
            displayName,
            0,
            0};
        if (distM > WorldLootPickupMaxDrawMeters(cat, &distView))
            continue;

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
    }
    }

    std::vector<decltype(localCache)::iterator> retainIters;
    std::vector<uintptr_t> retainRoots;
    retainIters.reserve(localCache.size());
    retainRoots.reserve(localCache.size());

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
            || IsGarbledEspLabel(it->second.ItemDisplayName)) {
            const auto cat = static_cast<WorldItemCategory>(it->second.worldCategory);
            if (const char* catLabel = WorldItemCategoryLabel(cat)) {
                const std::string s(catLabel);
                if (!s.empty() && !IsJunkWorldEspLabel(s)
                    && IsPlausibleEspLabel(s) && !IsGenericWorldEspLabel(s))
                    it->second.ItemDisplayName = s;
                else {
                    it = localCache.erase(it);
                    continue;
                }
            } else {
                it = localCache.erase(it);
                continue;
            }
        }

        retainIters.push_back(it);
        retainRoots.push_back(0);
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

    for (size_t i = 0; i < retainIters.size(); ++i) {
        const uintptr_t key = retainIters[i]->first;
        const uintptr_t root = retainRoots[i];
        const bool posOk = root && IsValidPointer(root)
            && IsPlausibleWorldPos(retainIters[i]->second.WorldPos);
        if (!posOk) {
            if (ContainerPosMissShouldEvict(key, false)) {
                ClearContainerPosMiss(key);
                localCache.erase(key);
            }
        } else {
            ContainerPosMissShouldEvict(key, true);
        }
    }

    if (doMetadata) {
        for (auto& [key, entry] : localCache) {
            entry.lootRarityTier = 0;
            entry.lootValue = 0;
        }
    }

    FinalizeWorldCacheMap(localCache, ctx.camera, dbgDrawing);

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

            char dedupeBuf[64]{};
            if (classPtr)
                snprintf(dedupeBuf, sizeof(dedupeBuf), "c:%llx",
                    static_cast<unsigned long long>(classPtr));
            else
                snprintf(dedupeBuf, sizeof(dedupeBuf), "a:%d", actorFnameId);

            if (s_seenContainerClasses.insert(dedupeBuf).second) {
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
            << " worldDist=" << static_cast<int>(var::world_distance)
            << std::endl;
    }
}
