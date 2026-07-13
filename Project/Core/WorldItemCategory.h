#pragma once

#include <cstdint>
#include <string>

struct WorldLootFilterView {
    uint8_t worldCategory = 0;
    std::string actorName;
    std::string itemDisplayName;
    int lootValue = 0;
    int lootRarityTier = 0;
};

enum class WorldItemCategory : uint8_t {
    Invalid = 0,
    DroppedPickup,
    Items,
    Ammo,
    ArcLoot,
    Backpack,
    Crate,
    Furniture,
    Grenade,
    Harvestable,
    Industrial,
    Medical,
    Other,
    Probe,
    RaiderCache,
    Vehicles,
    WeaponCase,
    FieldCrate,
    SupplyCallStation,
    Corpse,
    RaiderStock,
    ArcCargoship,
    Keys,
    Locker,
    Trash,
    Safe,
    Buried,
    DeadDrop,
    OpenedContainer,
    Count
};

class Engine;

/** Remap generic container labels and fname fragments to ST_WorldObjects-style names. */
std::string ResolveContainerDisplayLabel(
    const std::string& fname,
    const std::string& currentDisplay);

/**
 * HIGH-PRIORITY direct type-keyword pass for containers.
 * Scans the lowercased actor fname, class fname, and data-asset fname for basic
 * container type words (locker, safe, cabinet, ammo box, ...) and returns the
 * exact clean label on match. Runs BEFORE any category/CSV/world-object lookup so
 * a "locker"-named actor is always "Locker", never a category guess ("Industrial").
 * Returns empty when no known type keyword is present. All returned labels are
 * curated clean (pass IsCleanContainerName / IsPlausibleEspLabel).
 */
std::string DirectContainerKeywordLabel(
    const std::string& fname,
    const std::string& classFname,
    const std::string& dataAssetFname);

const char* WorldItemCategoryLabel(WorldItemCategory cat);
/** Guaranteed ESP label when a container has no specific resolved name. */
std::string ContainerCategoryFallbackEspLabel(WorldItemCategory cat);
/** Plants / consumable world nodes — never route through container/salvage path. */
bool FnameOrDisplayLooksLikeHarvestable(
    const std::string& fname,
    const std::string& displayLower);
WorldItemCategory ClassifyWorldActor(const std::string& fnameLower, const std::string& displayLower);
/** True when resolved hover/humanized label looks like a world container prop. */
bool DisplayLooksLikeWorldContainer(const std::string& displayLower);
/** Loot interaction / chest class prop that is not a dropped pickup item. */
bool IsWorldPropLootContainer(
    bool hasLootInteraction,
    bool classChest,
    const std::string& fname,
    const std::string& display);
/** Strict BP_SocketContainer_* / BP_SalvageContainer_* admission (fname prefix or mesh+ChosenMesh). */
bool IsRealSocketSalvageContainer(const std::string& fname, uintptr_t actor);
/** Parse socket/salvage class fname into a display label (Locker, Trash Can, etc.). */
std::string ResolveSocketContainerDisplayName(const std::string& fname);
/** Salvage/socket container by fname or ChosenMesh@0xCF0 in range 0..2. */
bool IsSalvageContainerActor(const std::string& fname, uintptr_t actor);
/** UObject pointer whose UClass fname is LootInteractionComponent (strict, no fallbacks). */
bool PointerIsLootInteractionComponent(uintptr_t obj);
/**
 * True when a component pointer at a loot-interaction slot is owned by `actor`
 * (UObject::OuterPrivate @ 0x20). Does not require class-FName decrypt — DMA
 * decrypt flakes were causing identical lockers/drawers to admit unevenly.
 */
bool LootInteractionOwnedByActor(uintptr_t component, uintptr_t actor);
/** Zipline anchors, corpses, pioneer characters — never container/open-container targets. */
bool FnameExcludedFromContainerEsp(const std::string& fnameLower);
/** True when container loot has already been searched/opened. */
bool ContainerLootLooksOpened(uintptr_t actor, const std::string& fnameHint = {});

enum class ContainerOpenSignal : uint8_t {
    None = 0,
    SalvageMesh = 1,
    ItemContainer = 2,
    LootSearched = 4,
};

/** First matching open detector (salvage mesh, item container open time, loot searched). */
ContainerOpenSignal ProbeContainerOpenSignals(uintptr_t actor, const std::string& fnameHint = {});

enum class GroundLootPickupSignal : uint8_t {
    None = 0,
    HiddenOrDestroyed = 1,
    LootSearched = 2,
    NoItemDa = 4,
    NoCollision = 8,
    SpawnItemsEmpty = 16,
    StillHasAssetId = 32,
};

inline GroundLootPickupSignal operator|(
    GroundLootPickupSignal a, GroundLootPickupSignal b)
{
    return static_cast<GroundLootPickupSignal>(
        static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline GroundLootPickupSignal operator&(
    GroundLootPickupSignal a, GroundLootPickupSignal b)
{
    return static_cast<GroundLootPickupSignal>(
        static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

/** True when any strong pickup-depletion signal is set (excludes StillHasAssetId). */
bool GroundLootPickupHasStrongSignal(GroundLootPickupSignal sig);
/** Bitmask probe for ground loot pickup / depletion signals. */
GroundLootPickupSignal ProbeGroundLootPickupSignals(
    uintptr_t actor, const std::string& fnameHint = {});
/** True when a ground pickup/harvestable was taken (searched flag or item asset gone). */
bool GroundLootLooksPickedUp(uintptr_t actor, const std::string& fnameHint = {});
/** True when cached world/item entry should be evicted (opened container or taken pickup). */
bool WorldLootCacheEntryDepleted(
    uintptr_t actor,
    const std::string& fnameHint,
    WorldItemCategory cat);
/** In-game style label (UIHover / loot component / ST_WorldObjects). */
std::string ResolveContainerEspDisplayName(uintptr_t actor, const std::string& fnameHint = {});
/** Classify loot actors; never returns Items for structural/container loot. */
WorldItemCategory InferLootWorldCategory(
    const std::string& fnameLower,
    const std::string& displayLower,
    const std::string& bucketType,
    bool hasLootInteraction,
    bool classGroundLoot = false,
    bool classChest = false);
bool IsGenericWorldEspLabel(const std::string& label);
/** Humanized engine/class labels (Player Controller, Dynamic Shelf, ...) — not loot props. */
bool IsJunkWorldEspLabel(const std::string& label);
bool IsGarbledEspLabel(const std::string& label);
/** UObject class/fname that is a component subobject, not a container blueprint. */
bool FnameLooksLikeEngineSubobjectClass(const std::string& fname);
/** Reject decrypt junk and non-readable ESP labels. */
bool IsPlausibleEspLabel(const std::string& label);
bool WorldCategoryIsContainerProp(WorldItemCategory cat);
bool WorldCategoryHasSpConfig(WorldItemCategory cat);
bool WorldCategoryUsesSpContainerRange(WorldItemCategory cat);
void SetContainerRangeSp(WorldItemCategory cat, bool useSp);
float WorldCategoryMaxDrawMeters(WorldItemCategory cat);
int LootMinRarityMenuToMinTier(int menuIndex);
float WorldLootPickupMaxDrawMeters(WorldItemCategory cat, const WorldLootFilterView* loot);
bool WorldLootEntryLooksLikeContainer(const WorldLootFilterView& loot);
bool LootItemLooksLikePickup(const WorldLootFilterView& loot);
bool PassesLootPickupFilters(const WorldLootFilterView& loot);
/** Final on-screen label from cache fields only (no DMA). */
std::string ResolveWorldDrawLabel(
    uint8_t worldCategory,
    const std::string& actorName,
    const std::string& itemDisplayName);
bool WorldCategoryEnabled(int category);
/** Radar-only visibility (independent of world ESP toggles). */
bool WorldCategoryVisibleOnRadar(const WorldLootFilterView& loot);
const char* WorldItemCategoryConfigSuffix(WorldItemCategory cat);
WorldItemCategory CategoryFromConfigSuffix(const char* suffix);
bool TrySetContainerRangeFromConfigSuffix(const char* suffix, bool useSp);

/** Menu color picker for each world ESP category (RGBA 0-255 packed ABGR). */
uint32_t WorldCategoryLabelColor(WorldItemCategory cat);
/** Rarity tier palette for dropped pickup labels. */
uint32_t RarityTierColor(int lootTier);
/** Pickup rarity/loot color vs per-category container color. */
uint32_t WorldLootLabelColor(WorldItemCategory cat, int lootTier, bool isPickup);
