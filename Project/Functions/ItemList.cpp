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

inline bool AnyItemEspEnabled()
{
    return var::enable_world && (
        var::droppedItems || var::showLoot ||
        var::show_world_items || var::show_world_ammo || var::show_world_arc_loot ||
        var::show_world_backpack || var::show_world_grenade || var::show_world_medical ||
        var::show_world_harvestable || var::show_world_keys);
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

    if (classChest || fnameIsContainer)
        return false;
    if (!fname.empty() && IsSalvageContainerActor(fname, key))
        return false;
    if (!fname.empty() && IsRealSocketSalvageContainer(fname, key))
        return false;
    if (!fname.empty() && FnameLooksLikeWorldContainer(fname))
        return false;

    if (classGroundLoot || fnameIsPickup)
        return true;

    if (TryReadItemGameAssetIdFromActor(key) != 0)
        return true;

    if (!dataAssetFName.empty()) {
        if (FnameLooksLikeWorldContainer(dataAssetFName))
            return false;
        if (FnameLooksLikeDroppedPickup(dataAssetFName))
            return true;
        if (!LookupByAssetName(dataAssetFName).empty())
            return true;
    }

    if (hasItemDataAsset && !fnameIsContainer && !classChest)
        return true;

    // Container-route actors use LootInteraction_Container (+0xBB8), not ground loot.
    if (hasContainerLoot && !fnameIsPickup && !classGroundLoot)
        return false;

    if (!fname.empty() && FnameAdmitsWorldActor(fname)) {
        if (IsStrictWorldLootFname(fname) && !fnameIsContainer)
            return true;
        if (FnameLooksLikeHarvestableActor(fname))
            return true;
    }

    if (!fname.empty() && !LookupByAssetName(fname).empty()
        && !FnameLooksLikeWorldContainer(fname))
        return true;

    (void)classId;
    (void)hasLootInteraction;
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

static bool ItemPosMissShouldEvict(uintptr_t key, bool posOk)
{
    if (posOk) {
        s_itemPosMisses.erase(key);
        return false;
    }
    const uint8_t misses = ++s_itemPosMisses[key];
    return misses >= kItemPosMissEvict;
}

static void ClearItemPosMiss(uintptr_t key)
{
    s_itemPosMisses.erase(key);
}

static void ClearItemListStaticMaps()
{
    s_itemPosMisses.clear();
    ClearGroundLootPickupStickyState();
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

    for (uintptr_t occupied : occupiedCharacterKeys)
        localCache.erase(occupied);

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

        std::string retainFname = it->second.ActorName;
        if (retainFname.empty())
            retainFname = GetActorFNameStringCached(key);
        const std::string retainClass = GetActorClassFName(key);
        if (FnameLooksLikeNonWorldEspActor(retainFname)
            || FnameLooksLikeNonWorldEspActor(retainClass)) {
            it = localCache.erase(it);
            continue;
        }

        const auto retainCat = static_cast<WorldItemCategory>(it->second.worldCategory);
        if (WorldLootCacheEntryDepleted(key, retainFname, retainCat)) {
            ++dbgDepleted;
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
        if (!posOk) {
            ++dbgPosSkip;
            if (ItemPosMissShouldEvict(key, false)) {
                ClearItemPosMiss(key);
                localCache.erase(key);
            }
        } else {
            ItemPosMissShouldEvict(key, true);
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

            const GroundLootPickupSignal sig =
                ProbeGroundLootPickupSignals(actorKey, entry.ActorName);
            using S = GroundLootPickupSignal;
            if ((sig & S::HiddenOrDestroyed) != S::None)
                ++dbgPickedHidden;
            if ((sig & S::LootSearched) != S::None)
                ++dbgPickedSearched;
            if ((sig & S::NoItemDa) != S::None)
                ++dbgPickedNoDa;
            if ((sig & S::NoCollision) != S::None)
                ++dbgPickedNoCol;
            if ((sig & S::SpawnItemsEmpty) != S::None)
                ++dbgPickedSpawnEmpty;
            if ((sig & S::StillHasAssetId) != S::None)
                ++dbgPickedStillId;
        }
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
}

namespace WorldScan {

void ClearItemScannerStaticState()
{
    ClearItemListStaticMaps();
}

} // namespace WorldScan
