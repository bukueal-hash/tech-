#include "../Core/Engine.h"
#include "../Core/AgentLog.h"
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
#include <vector>

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
    // Quest objective items (First Wave Tape, Celeste's Journal, …) get their
    // own category so the draw path can gold/star them and extend their range.
    // Matched on resolved display name first (name tables), fname tokens as a
    // fallback when the name hasn't decrypted yet.
    if (!displayLower.empty() && IsQuestItemDisplayName(displayLower))
        return WorldItemCategory::QuestItem;
    if (fnameLower.find("salvage_quest") != std::string::npos
        || fnameLower.find("questitem") != std::string::npos)
        return WorldItemCategory::QuestItem;

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

// #region agent log
static void AgentItemLog(const char* message, const char* hypothesisId, const std::string& dataJson)
{
    std::ofstream f(kArcDebugLogPath, std::ios::app);
    if (!f)
        return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"baseline\",\"hypothesisId\":\""
        << hypothesisId << "\",\"location\":\"ItemList.cpp\",\"message\":\""
        << message << "\",\"data\":" << dataJson
        << ",\"timestamp\":" << ms << "}\n";
}
// #endregion

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

    // LAG1: cap this pass's DMA so camera/position/frame-builder keeps cadence.
    WorldScan::ScanBudget scanBudget(std::chrono::milliseconds(90));

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
        std::string hoverValidated;
        if (displayName.empty()) {
            // CSV tables FIRST — deterministic and decrypt-independent. The
            // hover memory read can return FName-decrypt sludge that passes
            // the shape checks; it is vocabulary-validated inside
            // GetEnglishItemName and only consulted when the tables miss.
            if (const int64_t assetId = TryReadItemGameAssetIdFromActor(key);
                assetId != 0) {
                displayName = LookupDisplayByAssetId(assetId);
            }
            if (displayName.empty() && !dataAssetFName.empty())
                displayName = LookupByAssetName(dataAssetFName);
            if (displayName.empty()) {
                hoverValidated = GetEnglishItemName(key);
                displayName = hoverValidated;
            }
            bool slugSourced = false;
            if (displayName.empty() && !dataAssetFName.empty()) {
                displayName = HumanizeActorFName(dataAssetFName);
                slugSourced = !displayName.empty();
            }
            if (displayName.empty() && !labelFname.empty()) {
                displayName = HumanizeActorFName(labelFname);
                slugSourced = !displayName.empty();
            }
            // N1 telemetry: a label that came ONLY from fname humanization is
            // the sole remaining "random name" vector (CSV + hover are now
            // table/vocabulary-validated). Log distinct ones (bounded) so the
            // debug log shows exactly what slug labels ever draw.
            if (!displayName.empty() && slugSourced) {
                static std::unordered_set<std::string> s_slugSeen;
                if (s_slugSeen.size() < 64
                    && s_slugSeen.insert(displayName).second) {
                    // Bounded (≤64 distinct) verification tap → real log.
                    std::ofstream f(kArcVerifyPath, std::ios::app);
                    if (f) {
                        char lbl[96]{};
                        snprintf(lbl, sizeof(lbl), "%.80s", displayName.c_str());
                        const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        f << "{\"sessionId\":\"c190fb\",\"runId\":\"post-fix\","
                          << "\"hypothesisId\":\"N1\",\"location\":\"ItemList.cpp:ItemList\","
                          << "\"message\":\"item_slug_label\",\"data\":{\"label\":\"" << lbl
                          << "\"},\"timestamp\":" << ts << "}\n";
                    }
                }
            }
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
            if (displayName.empty())
                continue;
        }

        displayName = FormatEspDisplayLabel(displayName);
        if (displayName.empty() || IsJunkWorldEspLabel(displayName)
            || IsGarbledEspLabel(displayName)
            || IsGenericWorldEspLabel(displayName)
            || displayName == "Pickup" || displayName == "Item" || displayName == "Loot")
            continue;
        if (displayName.empty())
            continue;

        const std::string displayLower = ToLowerCopy(displayName);
        cat = ClassifyItem(
            fnameLower, displayLower, hasLootInteraction,
            classGroundLoot, classChest, fnameIsPickup, fnameIsContainer);

        int rarityTier = 0;
        int lootValue = 0;
        const bool metaHit =
            ResolveItemMetaForActor(*this, key, fname, displayName, rarityTier, lootValue);

        // Positive identification: a ground item must prove what it is from game
        // data — items_meta/asset tables (metaHit), the game's own hover name,
        // a harvestable node, or a class/fname that already passed AdmitItemActor
        // (which excludes VFX/engine actors).  The old gate dropped items that
        // had a valid display name from fname humanization or class lookup but no
        // items_meta entry — e.g. DA_Item_* shells with English hover names.
        const bool identityProven = metaHit
            || cat == WorldItemCategory::Harvestable
            || !hoverValidated.empty()
            || classGroundLoot
            || fnameIsPickup
            || (!displayName.empty()
                && !IsGenericWorldEspLabel(displayName)
                && !IsJunkWorldEspLabel(displayName)
                && !IsFurniturePropLabel(displayName)
                && IsPlausibleEspLabel(displayName));
        if (!identityProven) {
            // #region agent log
            {
                static std::unordered_set<std::string> s_posGateSeen;
                static int s_posGateDropped = 0;
                ++s_posGateDropped;
                if (s_posGateSeen.insert(displayName).second
                    && s_posGateSeen.size() <= 60) {
                    // Bounded (≤60 distinct) verification tap → real log.
                    std::ofstream f(kArcVerifyPath, std::ios::app);
                    if (f) {
                        char lbl[96]{};
                        snprintf(lbl, sizeof(lbl), "%.80s", displayName.c_str());
                        const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        f << "{\"sessionId\":\"c190fb\",\"runId\":\"post-fix\",\"hypothesisId\":\"P5\","
                          << "\"location\":\"ItemList.cpp:ItemList\",\"message\":\"item_posgate_drop\","
                          << "\"data\":{\"label\":\"" << lbl
                          << "\",\"cat\":" << static_cast<int>(cat)
                          << ",\"droppedTotal\":" << s_posGateDropped << "}"
                          << ",\"timestamp\":" << ts << "}\n";
                    }
                }
            }
            // #endregion
            continue;
        }

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
        if (scanBudget.expired())
            break;

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
            // #region agent log
            {
                char buf[192];
                snprintf(buf, sizeof(buf),
                    "{\"label\":\"%.48s\",\"reason\":\"sticky\",\"key\":%llu}",
                    it->second.ItemDisplayName.c_str(),
                    static_cast<unsigned long long>(key));
                AgentItemLog("item_evict", "G1", buf);
            }
            // #endregion
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
            // #region agent log
            {
                char buf[192];
                snprintf(buf, sizeof(buf),
                    "{\"label\":\"%.48s\",\"reason\":\"deplete\",\"key\":%llu}",
                    it->second.ItemDisplayName.c_str(),
                    static_cast<unsigned long long>(key));
                AgentItemLog("item_evict", "G1", buf);
            }
            // #endregion
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
            if (fixed.empty())
                continue;
            fixed = FormatEspDisplayLabel(fixed);
            if (fixed.empty() || fixed == "Pickup" || fixed == "Item" || fixed == "Loot"
                || IsGenericWorldEspLabel(fixed))
                continue;
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
                // #region agent log
                {
                    char buf[192];
                    snprintf(buf, sizeof(buf),
                        "{\"label\":\"%.48s\",\"reason\":\"pos_miss\",\"key\":%llu}",
                        retainIters[i]->second.ItemDisplayName.c_str(),
                        static_cast<unsigned long long>(key));
                    AgentItemLog("item_evict", "G1", buf);
                }
                // #endregion
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
    // Bukupex P3: batch the fixed-offset shell probes (one scatter for all
    // pickup retain entries) instead of serial ProbeGroundLootPickupSignals.
    {
        static std::unordered_map<uintptr_t, uint8_t> s_prevShellBits; // 1=hid 2=noCol
        static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> s_recentNearLive;
        constexpr auto kRecentNearTtl = std::chrono::seconds(45);
        const auto nowShell = std::chrono::steady_clock::now();

        struct ShellBatchRow {
            uintptr_t key = 0;
            decltype(localCache.begin()) it{};
            uintptr_t root = 0;
            uintptr_t collider = 0;
            uint8_t actorHidden = 0;
            uint8_t ddFlags = 0;
            uint8_t rootHid = 0;
            uint8_t colliderHid = 0;
            int hid = 0;
            int noCol = 0;
        };
        std::vector<ShellBatchRow> shellRows;
        shellRows.reserve(localCache.size());
        for (auto it = localCache.begin(); it != localCache.end(); ++it) {
            const auto cat = static_cast<WorldItemCategory>(it->second.worldCategory);
            if (!IsGroundPickupCategory(cat)
                && !FnameLooksLikeDroppedPickup(it->second.ActorName))
                continue;
            ShellBatchRow row{};
            row.key = it->first;
            row.it = it;
            shellRows.push_back(row);
        }

        int scatterExecs = 0;
        if (!shellRows.empty()) {
            {
                ScatterSession s1;
                if (s1.isValid()) {
                    bool ok = true;
                    for (auto& row : shellRows) {
                        ok = s1.prepare(row.key + Offsets::RootComponent, row.root) && ok;
                        ok = s1.prepare(
                                 row.key + Offsets::Pickup_RootCollider, row.collider)
                            && ok;
                        ok = s1.prepare(
                                 row.key + Offsets::Actor_bHiddenByte, row.actorHidden)
                            && ok;
                        ok = s1.prepare(row.key + Offsets::Actor_FlagsDd, row.ddFlags)
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
                    for (auto& row : shellRows) {
                        if (row.root && Memory::IsValidPtrFast2(row.root)) {
                            ok = s2.prepare(
                                     row.root + Offsets::Scene_bHiddenInGameByte,
                                     row.rootHid)
                                && ok;
                            ++prepared;
                        }
                        if (row.collider && Memory::IsValidPtrFast2(row.collider)) {
                            ok = s2.prepare(
                                     row.collider + Offsets::Scene_bHiddenInGameByte,
                                     row.colliderHid)
                                && ok;
                            ++prepared;
                        }
                    }
                    if (prepared > 0 && ok && s2.execute())
                        ++scatterExecs;
                }
            }
            for (auto& row : shellRows) {
                const bool actorHid =
                    (row.actorHidden & Offsets::Actor_bHiddenMask) != 0
                    || (row.ddFlags & Offsets::Actor_bActorIsBeingDestroyedMask) != 0;
                const bool sceneHid =
                    (row.root && Memory::IsValidPtrFast2(row.root)
                        && (row.rootHid & Offsets::Scene_bHiddenInGameMask) != 0)
                    || (row.collider && Memory::IsValidPtrFast2(row.collider)
                        && (row.colliderHid & Offsets::Scene_bHiddenInGameMask) != 0);
                row.hid = (actorHid || sceneHid) ? 1 : 0;
                row.noCol =
                    ((row.ddFlags & Offsets::Actor_bActorEnableCollisionMask) == 0)
                    ? 1
                    : 0;
            }
        }

        // #region agent log
        {
            static auto s_lastShellLog = std::chrono::steady_clock::time_point{};
            if (s_lastShellLog.time_since_epoch().count() == 0
                || nowShell - s_lastShellLog >= std::chrono::seconds(2)) {
                s_lastShellLog = nowShell;
                int pickedHidden = 0;
                for (const auto& row : shellRows)
                    pickedHidden += row.hid;
                std::ofstream f(kArcDebugLogPath, std::ios::app);
                if (f) {
                    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    f << "{\"sessionId\":\"c190fb\",\"runId\":\"batch\",\"hypothesisId\":\"P3\","
                      << "\"location\":\"ItemList.cpp:ItemList\",\"message\":\"item_shell_batch\","
                      << "\"data\":{\"n\":" << shellRows.size()
                      << ",\"scatterExecs\":" << scatterExecs
                      << ",\"pickedHidden\":" << pickedHidden << "}"
                      << ",\"timestamp\":" << ts << "}\n";
                }
            }
        }
        // #endregion

        std::unordered_set<uintptr_t> eraseKeys;
        for (const auto& row : shellRows) {
            auto& entry = row.it->second;
            const int hid = row.hid;
            const int noCol = row.noCol;

            if (entry.Drawing && entry.Distance >= 0.f && entry.Distance < 6.f
                && hid == 0 && noCol == 0) {
                s_recentNearLive[row.key] = nowShell;
            }

            const auto nearIt = s_recentNearLive.find(row.key);
            const bool recentNear = nearIt != s_recentNearLive.end()
                && (nowShell - nearIt->second) <= kRecentNearTtl;

            if (!s_prevShellBits.contains(row.key)) {
                s_prevShellBits[row.key] = static_cast<uint8_t>(
                    (hid ? 1u : 0u) | (noCol ? 2u : 0u));
                continue;
            }
            const uint8_t prevBits = s_prevShellBits[row.key];
            const int prevHid = (prevBits & 1u) ? 1 : 0;
            const int prevNoCol = (prevBits & 2u) ? 1 : 0;
            const bool hidRise = (hid == 1 && prevHid == 0);
            const bool noColRise = (noCol == 1 && prevNoCol == 0);
            const bool gate = recentNear && (hidRise || noColRise);
            s_prevShellBits[row.key] = static_cast<uint8_t>(
                (hid ? 1u : 0u) | (noCol ? 2u : 0u));

            if (!gate)
                continue;

            MarkGroundPickupGoneSticky(row.key);
            ClearItemPosMiss(row.key);
            // #region agent log
            {
                char buf[192];
                snprintf(buf, sizeof(buf),
                    "{\"label\":\"%.48s\",\"reason\":\"shell_gate\",\"key\":%llu}",
                    entry.ItemDisplayName.c_str(),
                    static_cast<unsigned long long>(row.key));
                AgentItemLog("item_evict", "G1", buf);
            }
            // #endregion
            s_recentNearLive.erase(row.key);
            s_prevShellBits.erase(row.key);
            eraseKeys.insert(row.key);
        }
        for (uintptr_t key : eraseKeys)
            localCache.erase(key);
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
    // Near-loot HUD runs on the paint thread every ~200ms. Never block on the
    // item scanner's exclusive DMA-backed cache write (same class as the radar
    // m_playerCacheMutex stall): try_lock + skip this HUD iteration if busy.
    std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex, std::defer_lock);
    if (!lock.try_lock())
        return;
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

// F7 "mark nearest picked" debug HUD: sticky-mark the ground pickup so the
// item scanner stops reporting it while it lingers. Called from Render.cpp.
// (Definition restored to fix a pre-existing LNK2001 unresolved external.)
void Engine::UserConfirmGroundItemPicked(uintptr_t key)
{
    MarkGroundPickupGoneSticky(key);
}

// Item scanner shares the same per-scanner static-clear contract as the
// container and robot scanners (they each clear their static maps on cache
// reset inside namespace WorldScan). Definition restored to fix a pre-existing
// LNK2001 unresolved external (declared in WorldScanCommon.h, called in
// Engine::ClearEspCaches but never defined).
namespace WorldScan {

void ClearItemScannerStaticState()
{
    ClearItemListStaticMaps();
}

} // namespace WorldScan
