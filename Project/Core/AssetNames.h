#pragma once

#include <cstdint>
#include <string>

bool AssetNamesInit();

std::string LookupByAssetName(const std::string& assetName);
/** English display from numeric game asset id (asset_index.csv). */
std::string LookupDisplayByAssetId(int64_t assetId);
/** True when the string is a real display name from the game's own loc/asset
 *  tables — rejects FName-decrypt sludge that passes shape checks. */
bool IsKnownItemDisplayName(const std::string& displayName);
/** Read game asset id from world item actor (ItemDataAsset chain); 0 if unknown. */
int64_t TryReadItemGameAssetIdFromActor(uint64_t actor);
std::string LookupByLocKey(const std::string& locKey);
std::string LookupByInternalToken(const std::string& token);
std::string LookupWorldObjectByFName(const std::string& actorFName);
/** asset_index.csv world_category rows: fname substring -> display + category suffix. */
bool LookupAssetWorldPropByFName(
    const std::string& actorFName,
    std::string* outDisplay,
    std::string* outCategorySuffix);
/** ST_Enemy class token from en.json arc map (longest match). */
std::string LookupBotClassToken(const std::string& actorFName);
/** ST_Enemy token / fname substring -> robotsList name (Wasp, Bastion, ...). */
std::string LookupEnemyBotByFName(const std::string& actorFName);
class Engine;
/** ST_Enemy display label -> robotsList name (Light Drone -> Wasp). */
std::string LookupEnemyBotDisplayLabel(const std::string& displayLabel);
/** True when label is a known bot/enemy type — rejects loot names like Oil. */
bool IsAcceptedBotEspLabel(
    Engine& eng,
    const std::string& label,
    const std::string& fnameHint = {});
/** Resolve robotsList bot type from actor fname / asset tables only. */
std::string ResolveRobotTypeFromFName(Engine& eng, const std::string& fname);
/** Resolve robotsList bot type from actor fname, mesh, and enemy data asset. */
std::string ResolveRobotTypeForActor(
    Engine& eng,
    uintptr_t actor,
    const std::string& fnameHint);
bool LookupItemMeta(const std::string& displayName, int& outRarityTier, int& outValue);
bool LookupItemMetaByAssetName(const std::string& assetName, int& outRarityTier, int& outValue);
bool LookupItemMetaById(const std::string& metaId, int& outRarityTier, int& outValue);
/** DA_Item_* fname from actor hover data asset pointer (if any). */
std::string GetActorDataAssetFName(uint64_t actor);
/** True when a display name is a known quest objective item (gold/star ESP). */
bool IsQuestItemDisplayName(const std::string& display);
/** DA_EnemyType_* fname from constructable pawn EnemyTypeDataAsset (+0x11A0). */
std::string GetEnemyTypeDataAssetFName(uint64_t actor);
/** Resolved bot display from constructable enemy data asset pointers. */
std::string ResolveEnemyAssetBotLabel(uintptr_t actor);
/** Try display, hover name, fname, and asset tables for items_meta.json hit. */
bool ResolveItemMetaForActor(
    class Engine& eng,
    uintptr_t actor,
    const std::string& fnameHint,
    const std::string& displayHint,
    int& outRarityTier,
    int& outValue);

/** Longest-match lookup from asset_index.csv tokens against actor FName. */
std::string LookupDisplayByFNameAssetIndex(const std::string& actorFNameLower);

/** True when fname has a known world-loot prefix (da_item_, wid_, bp_pickupbase, etc.). */
bool IsStrictWorldLootFname(const std::string& actorFName);

/** Menu/inventory container FNames — not world-placed loot. */
bool IsInventoryWorldFnameExcluded(const std::string& actorFNameLower);

/** ST_WorldObjects / long token match in actor FName. */
bool WorldObjectAdmitsByFName(const std::string& actorFName);

/** True if FName looks like a mappable world actor (strict prefix / asset / world-object proof). */
bool FnameAdmitsWorldActor(const std::string& actorFName);

/** World-placed harvest nodes (consumables, loot sockets, forage props). */
bool FnameLooksLikeHarvestableActor(const std::string& actorFName);

/** World-placed loot containers (cabinets, trash cans, lockers, crates, etc.). */
bool FnameLooksLikeWorldContainer(const std::string& actorFName);

/** Dropped world pickups (guns, plants, salvage on ground). */
bool FnameLooksLikeDroppedPickup(const std::string& actorFName);

/** Lowercase ASCII copy (shared by AssetNames / ItemList / ContainerList). */
std::string ToLowerCopy(std::string s);

/** Clean UE FName into a readable label when CSV/loc lookups miss. */
std::string HumanizeActorFName(const std::string& actorFName);

/** Polish item/container labels: compound splits, spacing, title-case (ARC/SP preserved). */
std::string FormatEspDisplayLabel(const std::string& label);

/** Resolve display label: memory read first, then CSV/loc/FName fallbacks. */
std::string ResolveWorldLabel(uintptr_t actor, const std::string& actorFName);

/** Full world ESP label: hover, loc tables, fname humanize; never returns generic bucket names. */
std::string ResolveWorldDisplayLabel(uintptr_t actor, const std::string& fnameHint, int worldCategory = 0);
