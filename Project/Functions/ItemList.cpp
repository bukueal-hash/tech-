#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/AssetNames.h"
#include "../Core/Offsets.h"
#include "../Core/WorldItemCategory.h"
#include "../Core/IntervalTimer.h"
#include "WorldScanCommon.h"
#include "../Interface/Utils/Variables/index.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

inline bool AnyItemEspEnabled()
{
    return var::enable_world && (
        var::droppedItems || var::showLoot ||
        var::show_world_items || var::show_world_ammo || var::show_world_arc_loot ||
        var::show_world_backpack || var::show_world_grenade || var::show_world_medical ||
        var::show_world_harvestable || var::show_world_keys);
}

bool FnameLooksLikeNonWorldEspActor(const std::string& fname)
{
    if (fname.empty())
        return false;
    if (FnameLooksLikeDroppedPickup(fname))
        return false;
    const std::string lower = ToLowerCopy(fname);
    if (IsInventoryWorldFnameExcluded(lower))
        return true;
    if (FnameExcludedFromContainerEsp(lower))
        return true;
    if (lower.find("rootcollider") != std::string::npos
        || lower.find("root_collider") != std::string::npos
        || lower == "collider"
        || lower.find("boxcomponent") != std::string::npos
        || lower.find("capsulecomponent") != std::string::npos)
        return true;
    // Niagara / VFX / GameplayCue effects (e.g. NS_VisionCone_Peppermint, the
    // Queen's sight-cone effect) are visual FX actors, never floor loot.
    if (lower.rfind("ns_", 0) == 0
        || lower.find("niagara") != std::string::npos
        || lower.find("visioncone") != std::string::npos
        || lower.find("vision_cone") != std::string::npos
        || lower.rfind("gc_", 0) == 0
        || lower.find("gameplaycue") != std::string::npos
        || lower.find("_vfx") != std::string::npos
        || lower.find("vfx_") != std::string::npos)
        return true;
    // Log proof: DDGIVolume→"Double Property", IndoorsVolume→"Door",
    // WaterProcessingBuilding→item ESP. These are not floor loot.
    static const char* kJunkWorld[] = {
        "ddgivolume", "ddgi", "ambiencevolume", "indoorsvolume", "outdoorsvolume",
        "blockingvolume", "runtimevirtualtexture", "levelbounds",
        "waterprocessing", "processingbuilding", "embarkworldsettings",
        "pioneerswatersystem", "pioneerwatersystem", "defaultambience",
    };
    for (const char* token : kJunkWorld) {
        if (lower.find(token) != std::string::npos)
            return true;
    }
    if (lower.size() >= 6
        && lower.compare(lower.size() - 6, 6, "volume") == 0)
        return true;
    if (lower.size() >= 8
        && lower.compare(lower.size() - 8, 8, "building") == 0)
        return true;
    return false;
}

bool AdmitItemActor(
    uintptr_t key,
    uint32_t classId,
    bool classChest,
    bool classGroundLoot,
    const std::string& fname,
    const std::string& dataAssetFName,
    bool fnameIsContainer,
    bool fnameIsPickup,
    bool hasLootInteraction,
    bool hasContainerLoot,
    bool hasItemDataAsset)
{
    if (!fname.empty() && FnameLooksLikeHarvestableActor(fname))
        return true;
    if (!dataAssetFName.empty() && FnameLooksLikeHarvestableActor(dataAssetFName))
        return true;

    // Soft admit paths below used to swallow volumes/buildings when +0x488/
    // asset-id looked populated (Door@IndoorsVolume, WaterProcessingBuilding).
    if ((!fname.empty() && FnameLooksLikeNonWorldEspActor(fname))
        || (!dataAssetFName.empty() && FnameLooksLikeNonWorldEspActor(dataAssetFName)))
        return false;

    if (classChest || fnameIsContainer)
        return false;
    // Probe wrecks are containers (WorldItemCategory::Probe). Keep them out of
    // the ground-loot scanner so F6 / liveNear only tracks BP_PickupBase drops.
    if ((!fname.empty() && FnameLooksLikeWorldContainer(fname))
        || (!dataAssetFName.empty() && FnameLooksLikeWorldContainer(dataAssetFName)))
        return false;
    if (!fname.empty() && IsSalvageContainerActor(fname, key))
        return false;
    if (!fname.empty() && IsRealSocketSalvageContainer(fname, key))
        return false;

    if (classGroundLoot || fnameIsPickup)
        return true;

    // Only pickup/harvestable/class signals admit. Do not use loose ItemDataAsset
    // / LookupByAssetName / FnameAdmitsWorldActor — those admitted buildings.
    (void)classId;
    (void)hasLootInteraction;
    (void)hasContainerLoot;
    (void)hasItemDataAsset;
    (void)key;
    return false;
}

WorldItemCategory ClassifyItem(
    const std::string& fnameLower,
    const std::string& displayLower,
    bool hasLootInteraction,
    bool classGroundLoot,
    bool classChest,
    bool fnameIsPickup,
    bool fnameIsContainer)
{
    // Plant/world harvestables stay Harvestable. Floor BP_ItemActor / DA shells
    // that also carry "consumable_*" (Prickly Pear fruit, etc.) must stay under
    // DroppedPickup so Items/Dropped toggles show them — not harvestable-only.
    if (FnameOrDisplayLooksLikeHarvestable(fnameLower, displayLower)
        && !(classGroundLoot || fnameIsPickup))
        return WorldItemCategory::Harvestable;

    WorldItemCategory cat = InferLootWorldCategory(
        fnameLower, displayLower, "",
        hasLootInteraction,
        classGroundLoot || fnameIsPickup,
        classChest || fnameIsContainer);
    if (cat == WorldItemCategory::Invalid)
        cat = ClassifyWorldActor(fnameLower, displayLower);
    if (cat == WorldItemCategory::Invalid) {
        if (classGroundLoot || fnameIsPickup
            || fnameLower.find("da_item") != std::string::npos
            || fnameLower.find("bp_pickup") != std::string::npos
            || fnameLower.find("bp_item") != std::string::npos)
            cat = WorldItemCategory::DroppedPickup;
        else
            cat = WorldItemCategory::Items;
    }
    if ((fnameIsPickup || classGroundLoot) && !fnameIsContainer && !classChest)
        return WorldItemCategory::DroppedPickup;

    // Item scanner feeds item/dropped rows only. Medical/Ammo/Grenade/Backpack/
    // ArcLoot/WeaponCase are container-type menu rows — never classify pickups
    // as those (e.g. a syringe must not inherit Medical SP distance).
    const auto remapContainerTyped = [&](WorldItemCategory c) -> WorldItemCategory {
        switch (c) {
        case WorldItemCategory::Medical:
        case WorldItemCategory::Ammo:
        case WorldItemCategory::Grenade:
        case WorldItemCategory::Backpack:
        case WorldItemCategory::ArcLoot:
        case WorldItemCategory::WeaponCase:
            return (classGroundLoot || fnameIsPickup)
                ? WorldItemCategory::DroppedPickup
                : WorldItemCategory::Items;
        default:
            return c;
        }
    };
    cat = remapContainerTyped(cat);

    // Remaining container props (crate/locker/…) → Items / DroppedPickup.
    if (WorldCategoryIsContainerProp(cat)
        && cat != WorldItemCategory::DroppedPickup
        && cat != WorldItemCategory::Harvestable
        && cat != WorldItemCategory::Items
        && cat != WorldItemCategory::Keys)
        return classGroundLoot || fnameIsPickup
            ? WorldItemCategory::DroppedPickup : WorldItemCategory::Items;
    if (cat != WorldItemCategory::DroppedPickup
        && cat != WorldItemCategory::Items
        && cat != WorldItemCategory::Keys
        && cat != WorldItemCategory::Harvestable) {
        if (classGroundLoot || fnameIsPickup)
            cat = WorldItemCategory::DroppedPickup;
        else
            cat = WorldItemCategory::Items;
    }
    return cat;
}

static std::unordered_map<uintptr_t, uint8_t> s_itemPosMisses;
static constexpr uint8_t kItemPosMissEvict = 12;
static constexpr uint8_t kPickupPosMissEvict = 3;

static bool ItemPosMissShouldEvict(uintptr_t key, bool posOk, bool pickupLike)
{
    const uint8_t lim = pickupLike ? kPickupPosMissEvict : kItemPosMissEvict;
    return WorldScan::MissCounterShouldEvict(s_itemPosMisses, key, posOk, lim);
}

static void ClearItemPosMiss(uintptr_t key)
{
    WorldScan::MissCounterClear(s_itemPosMisses, key);
}

static void ClearItemListStaticMaps()
{
    s_itemPosMisses.clear();
    ClearGroundLootPickupStickyState();
}

static bool IsGroundPickupCategory(WorldItemCategory cat)
{
    return cat == WorldItemCategory::DroppedPickup
        || cat == WorldItemCategory::Items
        || cat == WorldItemCategory::Harvestable;
}

} // namespace

void Engine::ItemList()
{
    if (!var::enable_world)
        return;
    if (!AnyItemEspEnabled() && !var::showLoot && !var::show_radar)
        return;

    WorldScanContext ctx{};
    if (!GatherWorldScanContext(ctx))
        return;

    const uint64_t genAtStart =
        m_worldGeneration.load(std::memory_order_acquire);

    std::unordered_map<uintptr_t, WorldCacheEntry> localCache;
    {
        std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
        localCache = itemCache;
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
            // Exclude each player's equipped/stowed weapon & item actors so a
            // held weapon never appears as a loose ground item with a value.
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

    for (uintptr_t occupied : occupiedCharacterKeys) {
        if (const auto it = localCache.find(occupied); it != localCache.end()) {
            if (IsGroundPickupCategory(static_cast<WorldItemCategory>(it->second.worldCategory)))
                MarkGroundPickupGoneSticky(occupied);
        }
        localCache.erase(occupied);
    }

    int dbgScanned = 0;
    int dbgAdmitted = 0;
    int dbgPosSkip = 0;
    int dbgDepleted = 0;
    int dbgDrawing = 0;

    static IntervalTimer metadataTimer(250);
    const bool doMetadata = metadataTimer.fire();

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

        if (IsGroundPickupGoneSticky(key))
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

        std::string fname = GetActorFNameStringCached(key);
        if (fname.empty())
            fname = GetActorFNameString(key);

        const std::string classFname = GetActorClassFName(key);
        if (!classFname.empty() && FnameLooksLikeEngineSubobjectClass(classFname))
            continue;

        const bool classFnameIsPickup =
            !classFname.empty() && FnameLooksLikeDroppedPickup(classFname);
        const bool scanPickupLike = classGroundLoot || classFnameIsPickup
            || FnameLooksLikeDroppedPickup(fname);

        if (!scanPickupLike && FnameLooksLikeNonWorldEspActor(fname))
            continue;
        if (!scanPickupLike && FnameLooksLikeNonWorldEspActor(classFname))
            continue;

        if (!scanPickupLike && WorldScan::LooksLikeBotPawn(key, localPawn))
            continue;

        std::string dataAssetFName;
        bool fnameIsContainer = false;
        bool fnameIsPickup = false;
        if (!fname.empty()) {
            fnameIsContainer = FnameLooksLikeWorldContainer(fname);
            fnameIsPickup = FnameLooksLikeDroppedPickup(fname);
        }
        if (!fnameIsPickup && !classFname.empty())
            fnameIsPickup = FnameLooksLikeDroppedPickup(classFname);
        if (!fnameIsContainer && !classFname.empty())
            fnameIsContainer = FnameLooksLikeWorldContainer(classFname);

        dataAssetFName = GetActorDataAssetFName(key);
        if (!fnameIsPickup && !dataAssetFName.empty())
            fnameIsPickup = FnameLooksLikeDroppedPickup(dataAssetFName);

        const bool pickupLike = classGroundLoot || fnameIsPickup
            || TryReadItemGameAssetIdFromActor(key) != 0;

        if (!pickupLike && WorldScan::LooksLikeContainerActor(key, fname))
            continue;

        const uintptr_t lootComp =
            Memory::read<uintptr_t>(key + Offsets::LootInteractionComponent);
        const bool hasLootInteraction =
            lootComp != 0 && IsValidPointer(lootComp);

        const uintptr_t containerLoot =
            Memory::read<uintptr_t>(key + Offsets::LootInteraction_Container);
        const bool hasContainerLoot =
            PointerIsLootInteractionComponent(containerLoot);

        const uint64_t itemDa =
            Memory::read<uint64_t>(key + static_cast<uint64_t>(Offsets::ItemDataAsset));
        const uint64_t pickupDa =
            Memory::read<uint64_t>(key + static_cast<uint64_t>(Offsets::Pickup_DefaultPickupDataAsset));
        const bool hasItemDataAsset =
            (itemDa != 0 && Memory::IsValidPtrFast2(itemDa))
            || (pickupDa != 0 && Memory::IsValidPtrFast2(pickupDa));

        if (!AdmitItemActor(
                key, classId, classChest, classGroundLoot, fname, dataAssetFName,
                fnameIsContainer, fnameIsPickup, hasLootInteraction,
                hasContainerLoot, hasItemDataAsset))
            continue;

        // tech- parity: floor pickups use AActor::RootComponent only. Preferring
        // Pickup_RootCollider assigned the wrong transform → Distance 400–500m →
        // Drawing=false while standing 14m away (see debug-5681af Ruined Parachute).
        const uintptr_t root = pickupLike
            ? Memory::read<uintptr_t>(key + Offsets::RootComponent)
            : Engine::ResolveLootActorRoot(key, false);
        if (!root || !IsValidPointer(root))
            continue;

        const Vector3 worldPos = ReadSceneWorldPos(root);
        if (WorldScan::IsOldStyleInvalidXY(worldPos)) {
            ++dbgPosSkip;
            continue;
        }

        std::string labelFname = fname;
        if (labelFname.empty() && !dataAssetFName.empty())
            labelFname = dataAssetFName;
        if (fname.empty() && !dataAssetFName.empty())
            fname = dataAssetFName;

        const std::string fnameLower = ToLowerCopy(fname);
        // Classify before label resolve so harvestable/category fallbacks fire (#10).
        WorldItemCategory cat = ClassifyItem(
            fnameLower, std::string{}, hasLootInteraction,
            classGroundLoot, classChest, fnameIsPickup, fnameIsContainer);

        std::string displayName = ResolveWorldDisplayLabel(
            key, labelFname, static_cast<int>(cat));
        if (displayName.empty() || IsJunkWorldEspLabel(displayName)
            || IsGarbledEspLabel(displayName) || !IsPlausibleEspLabel(displayName))
            displayName.clear();
        if (displayName.empty()) {
            displayName = GetEnglishItemName(key);
            if (displayName.empty()) {
                if (const int64_t assetId = TryReadItemGameAssetIdFromActor(key);
                    assetId != 0) {
                    displayName = LookupDisplayByAssetId(assetId);
                }
            }
            if (displayName.empty() && !dataAssetFName.empty()) {
                displayName = LookupByAssetName(dataAssetFName);
                if (displayName.empty())
                    displayName = HumanizeActorFName(dataAssetFName);
            }
            if (displayName.empty() && !labelFname.empty())
                displayName = HumanizeActorFName(labelFname);
            // Final fallback: try the class FName — it is a different memory read
            // and often still decryptable when the actor FName read fails.
            if ((displayName.empty() || IsGenericWorldEspLabel(displayName))
                && !classFname.empty()
                && !FnameLooksLikeEngineSubobjectClass(classFname)) {
                const std::string classHuman = HumanizeActorFName(classFname);
                if (!classHuman.empty() && !IsGenericWorldEspLabel(classHuman)
                    && !IsJunkWorldEspLabel(classHuman) && IsPlausibleEspLabel(classHuman))
                    displayName = classHuman;
            }
            if (displayName.empty()) {
                if (fnameIsPickup || classGroundLoot) displayName = "Pickup";
                else displayName = "Item";
            }
        }

        displayName = FormatEspDisplayLabel(displayName);
        if (displayName.empty() || IsJunkWorldEspLabel(displayName)
            || IsGarbledEspLabel(displayName)
            || IsGenericWorldEspLabel(displayName)) {
            // "Pickup" is intentionally not in IsGenericWorldEspLabel so the render
            // path won't loop-replace it; use it rather than "Dropped Pickup".
            if (fnameIsPickup || classGroundLoot)
                displayName = "Pickup";
            else
                displayName = "Item";
        }
        if (displayName.empty())
            continue;

        const std::string displayLower = ToLowerCopy(displayName);
        cat = ClassifyItem(
            fnameLower, displayLower, hasLootInteraction,
            classGroundLoot, classChest, fnameIsPickup, fnameIsContainer);

        int rarityTier = 0;
        int lootValue = 0;
        ResolveItemMetaForActor(*this, key, fname, displayName, rarityTier, lootValue);

        auto& entry = localCache[key];
        entry.rootComponent = root;
        entry.APawn = key;
        entry.ActorName = fname;
        entry.ItemDisplayName = displayName;
        entry.ItemType = displayName;
        entry.worldCategory = static_cast<uint8_t>(cat);
        entry.lootRarityTier = rarityTier;
        entry.lootValue = lootValue;
        entry.WorldPos = worldPos;
        ++dbgAdmitted;
    }

    WorldScan::DedupeWorldCacheByRoot(localCache);

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

        // F6 / deplete sticky: ItemList snapshots itemCache at start. UserConfirm
        // can erase the live map mid-scan; without this, localCache writeback
        // restores the ghost (log: liveNear same key after userMark).
        if (IsGroundPickupGoneSticky(key)) {
            ClearItemPosMiss(key);
            it = localCache.erase(it);
            continue;
        }

        std::string retainFname = it->second.ActorName;
        if (retainFname.empty())
            retainFname = GetActorFNameStringCached(key);
        const std::string retainClass = GetActorClassFName(key);
        if (FnameLooksLikeNonWorldEspActor(retainFname)
            || FnameLooksLikeNonWorldEspActor(retainClass)) {
            it = localCache.erase(it);
            continue;
        }
        // Evict misrouted containers (e.g. Probe Crashed) stuck from older admits.
        if (FnameLooksLikeWorldContainer(retainFname)
            || FnameLooksLikeWorldContainer(retainClass)) {
            ClearItemPosMiss(key);
            it = localCache.erase(it);
            continue;
        }

        const auto retainCat = static_cast<WorldItemCategory>(it->second.worldCategory);
        if (WorldLootCacheEntryDepleted(key, retainFname, retainCat)) {
            ++dbgDepleted;
            if (IsGroundPickupCategory(retainCat)
                || FnameLooksLikeDroppedPickup(retainFname))
                MarkGroundPickupGoneSticky(key);
            ClearItemPosMiss(key);
            it = localCache.erase(it);
            continue;
        }

        // Re-home stale Medical/Ammo/… categories so old cache entries cannot
        // keep Medical SP distance after ClassifyItem remapping.
        {
            const bool fnameIsPickup = FnameLooksLikeDroppedPickup(retainFname)
                || FnameLooksLikeDroppedPickup(retainClass);
            const WorldItemCategory fixed = ClassifyItem(
                ToLowerCopy(retainFname),
                ToLowerCopy(it->second.ItemDisplayName),
                false,
                ArcActorType::IsGroundLootClassId(
                    ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(key))),
                false,
                fnameIsPickup,
                FnameLooksLikeWorldContainer(retainFname));
            it->second.worldCategory = static_cast<uint8_t>(fixed);
        }

        if (IsJunkWorldEspLabel(it->second.ItemDisplayName)
            || IsGarbledEspLabel(it->second.ItemDisplayName)
            || it->second.ItemDisplayName.empty()) {
            const auto cat = static_cast<WorldItemCategory>(it->second.worldCategory);
            std::string fixed = ResolveWorldDisplayLabel(
                key, retainFname, static_cast<int>(cat));
            if (fixed.empty() || IsJunkWorldEspLabel(fixed)
                || IsGarbledEspLabel(fixed) || !IsPlausibleEspLabel(fixed))
                fixed.clear();
            if (fixed.empty()) {
                const bool fnameIsPickup = FnameLooksLikeDroppedPickup(retainFname)
                    || FnameLooksLikeDroppedPickup(retainClass);
                fixed = fnameIsPickup ? "Pickup" : "Item";
            }
            fixed = FormatEspDisplayLabel(fixed);
            if (!fixed.empty()) {
                it->second.ItemDisplayName = fixed;
                it->second.ItemType = fixed;
            }
        }

        retainIters.push_back(it);
        retainRoots.push_back(0);
        ++it;
    }

    if (!retainIters.empty()) {
        std::vector<WorldScan::WorldCacheRetainRow> refreshRows;
        refreshRows.reserve(retainIters.size());
        for (size_t i = 0; i < retainIters.size(); ++i) {
            WorldScan::WorldCacheRetainRow row{};
            row.actorKey = retainIters[i]->first;
            row.entry = &retainIters[i]->second;
            row.rootOut = &retainRoots[i];
            refreshRows.push_back(row);
        }
        WorldScan::RefreshWorldCacheRetainPositions(refreshRows);
    }

    for (size_t i = 0; i < retainIters.size(); ++i) {
        const uintptr_t key = retainIters[i]->first;
        const uintptr_t root = retainRoots[i];
        const bool posOk = root && IsValidPointer(root)
            && IsPlausibleWorldPos(retainIters[i]->second.WorldPos);
        const bool pickupLike = IsGroundPickupCategory(
            static_cast<WorldItemCategory>(retainIters[i]->second.worldCategory));
        if (!posOk) {
            ++dbgPosSkip;
            if (ItemPosMissShouldEvict(key, false, pickupLike)) {
                if (pickupLike)
                    MarkGroundPickupGoneSticky(key);
                ClearItemPosMiss(key);
                localCache.erase(key);
            }
        } else {
            ItemPosMissShouldEvict(key, true, pickupLike);
        }
    }

    if (doMetadata) {
        for (auto& [key, entry] : localCache) {
            std::string fname = entry.ActorName;
            if (fname.empty())
                fname = GetActorFNameStringCached(key);
            int rarityTier = entry.lootRarityTier;
            int lootValue = entry.lootValue;
            ResolveItemMetaForActor(
                *this, key, fname, entry.ItemDisplayName, rarityTier, lootValue);
            entry.lootRarityTier = rarityTier;
            entry.lootValue = lootValue;
        }
    }

    FinalizeWorldCacheMap(localCache, ctx.camera, dbgDrawing);

    // H7: ground-pickup shells that were liveNear <6m (hid=0/noCol=0 proven on
    // live BP_PickupBase) — clear on 0→1 hid/noCol / HiddenOrDestroyed without
    // waiting for ~15m actorGone (Canister L2258 / Battery L2259: strong=1).
    // Absolute noCol alone false-positives live shells; require prior live sample.
    {
        using S = GroundLootPickupSignal;
        static std::unordered_map<uintptr_t, uint8_t> s_prevShellBits; // 1=hid 2=noCol
        static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> s_recentNearLive;
        constexpr auto kRecentNearTtl = std::chrono::seconds(45);
        const auto nowShell = std::chrono::steady_clock::now();

        for (auto it = localCache.begin(); it != localCache.end(); ) {
            const uintptr_t key = it->first;
            auto& entry = it->second;
            const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
            if (!IsGroundPickupCategory(cat)
                && !FnameLooksLikeDroppedPickup(entry.ActorName)) {
                ++it;
                continue;
            }

            const GroundLootPickupSignal sig =
                ProbeGroundLootPickupSignals(key, entry.ActorName);
            const int hid = ((sig & S::HiddenOrDestroyed) != S::None) ? 1 : 0;
            const int noCol = ((sig & S::NoCollision) != S::None) ? 1 : 0;
            const int strong = GroundLootPickupHasStrongSignal(sig) ? 1 : 0;

            // Stamp only when proven live under 6m (both flags clear).
            if (entry.Drawing && entry.Distance >= 0.f && entry.Distance < 6.f
                && hid == 0 && noCol == 0) {
                s_recentNearLive[key] = nowShell;
            }

            const auto nearIt = s_recentNearLive.find(key);
            const bool recentNear = nearIt != s_recentNearLive.end()
                && (nowShell - nearIt->second) <= kRecentNearTtl;

            // First sample: seed prev without clearing (need a live baseline).
            if (!s_prevShellBits.contains(key)) {
                s_prevShellBits[key] = static_cast<uint8_t>(
                    (hid ? 1u : 0u) | (noCol ? 2u : 0u));
                ++it;
                continue;
            }
            const uint8_t prevBits = s_prevShellBits[key];
            const int prevHid = (prevBits & 1u) ? 1 : 0;
            const int prevNoCol = (prevBits & 2u) ? 1 : 0;
            const bool hidRise = (hid == 1 && prevHid == 0);
            const bool noColRise = (noCol == 1 && prevNoCol == 0);
            // recentNear stamped only while hid=0&&noCol=0 under 6m → live baseline.
            const bool gate = recentNear && (hidRise || noColRise);
            s_prevShellBits[key] = static_cast<uint8_t>(
                (hid ? 1u : 0u) | (noCol ? 2u : 0u));

            if (!gate) {
                ++it;
                continue;
            }

            MarkGroundPickupGoneSticky(key);
            ClearItemPosMiss(key);
            s_recentNearLive.erase(key);
            s_prevShellBits.erase(key);
            it = localCache.erase(it);
        }
    }



    if (m_worldGeneration.load(std::memory_order_acquire) != genAtStart)
        return;

    {
        std::unique_lock<std::shared_mutex> lock(m_itemCacheMutex);
        itemCache = std::move(localCache);
    }

    if (var::show_debug_overlay) {
        std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
        int dbgPickup = 0;
        int dbgPickedHidden = 0;
        int dbgPickedSearched = 0;
        int dbgPickedNoDa = 0;
        int dbgPickedNoCol = 0;
        int dbgPickedSpawnEmpty = 0;
        int dbgPickedStillId = 0;
        for (const auto& [actorKey, entry] : itemCache) {
            if (!entry.Drawing)
                continue;
            const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
            if (cat == WorldItemCategory::DroppedPickup
                || cat == WorldItemCategory::Items
                || cat == WorldItemCategory::Harvestable)
                ++dbgPickup;
            // Do NOT ProbeGroundLootPickupSignals here. DMA under this shared_lock
            // saturated the FPGA bus and (via writer-preference) stalled paint
            // Present 500-700ms (ghost flicker: espMs/overlayMs == paint_gap).
            // picked* counters come from the admit/evict path when available.
            (void)actorKey;
        }

        // Name-discovery DMA (GetActorClassFName / GetActorDataAssetFName) under
        // this shared_lock was removed — it stalled Present alongside the pickup
        // probes (ghost flicker). Use admit-time labels only.

        std::cout << "[debugItem] scanned=" << dbgScanned
            << " admitted=" << dbgAdmitted
            << " depleted=" << dbgDepleted
            << " posSkip=" << dbgPosSkip
            << " drawing=" << dbgDrawing
            << " cache=" << itemCache.size()
            << " pickups=" << dbgPickup
            << " pickedHidden=" << dbgPickedHidden
            << " pickedSearched=" << dbgPickedSearched
            << " pickedNoDa=" << dbgPickedNoDa
            << " pickedNoCol=" << dbgPickedNoCol
            << " pickedSpawnEmpty=" << dbgPickedSpawnEmpty
            << " pickedStillId=" << dbgPickedStillId
            << " lootDist=" << static_cast<int>(var::loot_distance)
            << std::endl;
    }

    // Always refresh nearest live pickup for the on-screen item-pick panel
    // (does not require Show offset validation).
    {
        static auto s_lastNear = std::chrono::steady_clock::time_point{};
        const auto nowNear = std::chrono::steady_clock::now();
        if (s_lastNear.time_since_epoch().count() == 0
            || nowNear - s_lastNear >= std::chrono::seconds(1)) {
            s_lastNear = nowNear;
            float bestDist = 1.0e9f;
            uintptr_t bestKey = 0;
            std::string bestLab;
            std::string bestFname;
            WorldItemCategory bestCat = WorldItemCategory::Items;
            bool found = false;
            {
                std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
                for (const auto& [actorKey, entry] : itemCache) {
                    if (!entry.Drawing)
                        continue;
                    const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
                    if (!IsGroundPickupCategory(cat))
                        continue;
                    if (entry.Distance < bestDist) {
                        bestDist = entry.Distance;
                        bestKey = actorKey;
                        bestLab = entry.ItemDisplayName.empty()
                            ? entry.ActorName : entry.ItemDisplayName;
                        bestFname = entry.ActorName;
                        bestCat = cat;
                        found = true;
                    }
                }
            }
        }
    }
}

void Engine::CollectDrawingGroundPickups(std::vector<GroundPickupHudRow>& out) const
{
    out.clear();
    std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
    out.reserve(itemCache.size());
    for (const auto& [key, entry] : itemCache) {
        if (!entry.Drawing)
            continue;
        const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
        if (cat != WorldItemCategory::DroppedPickup
            && cat != WorldItemCategory::Items
            && cat != WorldItemCategory::Harvestable)
            continue;
        GroundPickupHudRow row{};
        row.key = key;
        row.name = entry.ItemDisplayName.empty() ? entry.ActorName : entry.ItemDisplayName;
        row.distM = entry.Distance;
        row.worldCategory = entry.worldCategory;
        row.fname = entry.ActorName;
        out.push_back(std::move(row));
    }
    std::sort(
        out.begin(),
        out.end(),
        [](const GroundPickupHudRow& a, const GroundPickupHudRow& b) {
            return a.distM < b.distM;
        });
    if (out.size() > 24)
        out.resize(24);
}

void Engine::UserConfirmGroundItemPicked(uintptr_t key)
{
    if (!key)
        return;

    {
        std::unique_lock<std::shared_mutex> lock(m_itemCacheMutex);
        itemCache.erase(key);
    }

    MarkGroundPickupGoneSticky(key);
}

namespace WorldScan {

void ClearItemScannerStaticState()
{
    ClearItemListStaticMaps();
}

} // namespace WorldScan
