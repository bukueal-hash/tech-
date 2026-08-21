#include "WorldItemCategory.h"
#include "Memory.h"
#include "ActorType.h"
#include "AssetNames.h"
#include "Engine.h"
#include "Offsets.h"
#include "../Interface/Utils/Variables/index.h"
#include "../Functions/EspDraw.h"
#include "../ThirdParty/ImGui/imgui.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern Engine engine;

namespace {

bool Contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

struct WorldPropTokenEntry {
    const char* token;
    WorldItemCategory category;
    const char* label;
};

// Longest-token-wins matching in MatchWorldPropToken; list specific tokens before generic.
static const WorldPropTokenEntry kWorldPropTokens[] = {
    { "greatmullein", WorldItemCategory::Harvestable, "Great Mullein" },
    { "consumablemullein", WorldItemCategory::Harvestable, "Great Mullein" },
    { "torchginger", WorldItemCategory::Harvestable, "Great Mullein" },
    { "consumablemoss", WorldItemCategory::Harvestable, "Moss" },
    { "consumablemushroom", WorldItemCategory::Harvestable, "Mushroom" },
    { "candleberrybush", WorldItemCategory::Harvestable, "Candleberry Bush" },
    { "pioneerlootsocket", WorldItemCategory::Harvestable, "Harvestable" },
    { "bpactorconsumable", WorldItemCategory::Harvestable, "Harvestable" },
    { "environmentalloot", WorldItemCategory::Harvestable, "Harvestable" },
    { "consumableagave", WorldItemCategory::Harvestable, "Agave" },
    { "consumable_agave", WorldItemCategory::Harvestable, "Agave" },
    { "consumable_prickly", WorldItemCategory::Harvestable, "Prickly Pear" },
    { "pricklypear", WorldItemCategory::Harvestable, "Prickly Pear" },
    { "consumablepricklypear", WorldItemCategory::Harvestable, "Prickly Pear" },
    { "arcdeforesterhusk", WorldItemCategory::ArcCargoship, "Arc Husk" },
    { "arcdeforester", WorldItemCategory::ArcCargoship, "ARC Deforester" },
    { "arcentry", WorldItemCategory::ArcCargoship, "ARC Entry" },
    { "arc_entry", WorldItemCategory::ArcCargoship, "ARC Entry" },
    { "arcassessor", WorldItemCategory::ArcCargoship, "ARC Assessor" },
    { "suspiciousbackpack", WorldItemCategory::Backpack, "Suspicious Backpack" },
    { "wornbackpack", WorldItemCategory::Backpack, "Worn Backpack" },
    { "raiderbackpack", WorldItemCategory::Backpack, "Worn Backpack" },
    { "grenadecontainer", WorldItemCategory::Grenade, "Grenade Tube" },
    { "grenadetube", WorldItemCategory::Grenade, "Grenade Tube" },
    { "container_grenade", WorldItemCategory::Grenade, "Grenade Tube" },
    { "medicalbag", WorldItemCategory::Medical, "Medical Bag" },
    { "medical_bag", WorldItemCategory::Medical, "Medical Bag" },
    { "container_medical", WorldItemCategory::Medical, "Medical Bag" },
    { "weaponcase", WorldItemCategory::WeaponCase, "Weapon Case" },
    { "weapon_case", WorldItemCategory::WeaponCase, "Weapon Case" },
    { "container_weapon", WorldItemCategory::WeaponCase, "Weapon Case" },
    { "ammobox", WorldItemCategory::Ammo, "Ammo Box" },
    { "ammo_box", WorldItemCategory::Ammo, "Ammo Box" },
    { "container_ammo", WorldItemCategory::Ammo, "Ammo Box" },
    { "shippingcontainer", WorldItemCategory::Crate, "Shipping Container" },
    { "fieldcrate", WorldItemCategory::FieldCrate, "Field Crate" },
    { "field_crate", WorldItemCategory::FieldCrate, "Field Crate" },
    { "supplycallstation", WorldItemCategory::SupplyCallStation, "Supply Call Station" },
    { "supplycall", WorldItemCategory::SupplyCallStation, "Supply Call Station" },
    { "supply_call", WorldItemCategory::SupplyCallStation, "Supply Call Station" },
    { "firstwaveraidercache", WorldItemCategory::RaiderCache, "Raider Cache" },
    { "raidercache", WorldItemCategory::RaiderCache, "Raider Cache" },
    { "raider_cache", WorldItemCategory::RaiderCache, "Raider Cache" },
    { "bp_raidercache", WorldItemCategory::RaiderCache, "Raider Cache" },
    { "electricalcabinet", WorldItemCategory::Industrial, "Electrical Cabinet" },
    { "trailercompressor", WorldItemCategory::Industrial, "Trailer Compressor" },
    { "ventmachine", WorldItemCategory::Industrial, "Vent Machine" },
    { "filingcabinet", WorldItemCategory::Furniture, "Filing Cabinet" },
    { "weaponsrack", WorldItemCategory::Furniture, "Weapons Rack" },
    { "weaponrack", WorldItemCategory::Furniture, "Weapon Rack" },
    { "securitycamera", WorldItemCategory::Industrial, "Security Camera" },
    { "armoredpatrolcar", WorldItemCategory::Vehicles, "Armored Patrol Car" },
    { "patrolcar", WorldItemCategory::Vehicles, "Patrol Car" },
    { "patrol_car", WorldItemCategory::Vehicles, "Patrol Car" },
    { "truckutility", WorldItemCategory::Vehicles, "Truck Utility" },
    { "crashedarcprobe", WorldItemCategory::Probe, "Crashed ARC Probe" },
    { "arcprobe", WorldItemCategory::Probe, "ARC Probe" },
    { "burieddetectable", WorldItemCategory::Buried, "Buried Loot" },
    { "buried_detectable", WorldItemCategory::Buried, "Buried Loot" },
    { "storagelocker", WorldItemCategory::Locker, "Locker" },
    { "footlocker", WorldItemCategory::Locker, "Footlocker" },
    { "wastebasket", WorldItemCategory::Trash, "Waste Basket" },
    { "wastebin", WorldItemCategory::Trash, "Waste Bin" },
    { "trashbin", WorldItemCategory::Trash, "Trash Can" },
    { "trashcan", WorldItemCategory::Trash, "Trash Can" },
    { "bintrash", WorldItemCategory::Trash, "Trash Can" },
    { "dumpster", WorldItemCategory::Trash, "Dumpster" },
    { "cartcrate", WorldItemCategory::Crate, "Cart Crate" },
    { "cart_crate", WorldItemCategory::Crate, "Cart Crate" },
    { "objectcrate", WorldItemCategory::Crate, "Object Crate" },
    { "cargoship", WorldItemCategory::ArcCargoship, "Arc Cargo" },
    { "arc_cargo", WorldItemCategory::ArcCargoship, "Arc Cargo" },
    { "arc_cargoship", WorldItemCategory::ArcCargoship, "Arc Cargo" },
    { "archusk", WorldItemCategory::ArcCargoship, "Arc Husk" },
    { "arc_husk", WorldItemCategory::ArcCargoship, "Arc Husk" },
    { "arc_loot", WorldItemCategory::ArcLoot, "ARC Loot" },
    { "arcloot", WorldItemCategory::ArcLoot, "ARC Loot" },
    { "harvester", WorldItemCategory::ArcCargoship, "ARC Harvester" },
    { "extractor", WorldItemCategory::ArcCargoship, "ARC Assessor" },
    { "deadplayer", WorldItemCategory::Corpse, "Corpse" },
    { "deaddrop", WorldItemCategory::DeadDrop, "Dead Drop" },
    { "dead_drop", WorldItemCategory::DeadDrop, "Dead Drop" },
    { "fruitbasket", WorldItemCategory::Harvestable, "Basket of Fruit" },
    { "candleberry", WorldItemCategory::Harvestable, "Candleberry Bush" },
    { "birdnest", WorldItemCategory::Harvestable, "Bee Nest" },
    { "beenest", WorldItemCategory::Harvestable, "Bee Nest" },
    { "lootsocket", WorldItemCategory::Harvestable, "Harvestable" },
    { "consumable", WorldItemCategory::Harvestable, "Harvestable" },
    { "mullein", WorldItemCategory::Harvestable, "Great Mullein" },
    { "mushroom", WorldItemCategory::Harvestable, "Mushroom" },
    { "moss", WorldItemCategory::Harvestable, "Moss" },
    { "agave", WorldItemCategory::Harvestable, "Agave" },
    { "apricot", WorldItemCategory::Harvestable, "Apricot" },
    { "lemon", WorldItemCategory::Harvestable, "Lemon" },
    { "workbench", WorldItemCategory::Industrial, "Workbench" },
    { "generator", WorldItemCategory::Industrial, "Generator" },
    { "toolbox", WorldItemCategory::Industrial, "Toolbox" },
    { "compressor", WorldItemCategory::Industrial, "Compressor" },
    { "lockers", WorldItemCategory::Locker, "Locker" },
    { "locker", WorldItemCategory::Locker, "Locker" },
    { "filing", WorldItemCategory::Furniture, "Filing Cabinet" },
    { "cabinet", WorldItemCategory::Furniture, "Cabinet" },
    { "cupboard", WorldItemCategory::Furniture, "Cupboard" },
    { "drawer", WorldItemCategory::Furniture, "Drawer" },
    { "desk", WorldItemCategory::Furniture, "Desk" },
    { "closet", WorldItemCategory::Furniture, "Closet" },
    { "cooler", WorldItemCategory::Crate, "Cooler" },
    { "icebox", WorldItemCategory::Crate, "Cooler" },
    { "backpack", WorldItemCategory::Backpack, "Backpack" },
    { "coffeemachine", WorldItemCategory::Other, "Coffee Machine" },
    { "computer", WorldItemCategory::Other, "Computer" },
    { "servers", WorldItemCategory::Other, "Servers" },
    { "vault", WorldItemCategory::Safe, "Vault" },
    { "fertilizer", WorldItemCategory::DroppedPickup, "Fertilizer" },
    { "itemsalvagefertilizer", WorldItemCategory::DroppedPickup, "Fertilizer" },
    { "corpse", WorldItemCategory::Corpse, "Corpse" },
};

static bool IsGenericSocketPrefixToken(const char* token)
{
    return std::strcmp(token, "bpsocketcontainerraider") == 0
        || std::strcmp(token, "bpsocketcontainermedical") == 0
        || std::strcmp(token, "bpsocketcontainerrsra") == 0
        || std::strcmp(token, "bpsocketcontainergvt") == 0
        || std::strcmp(token, "bpsocketcontainerresidential") == 0
        || std::strcmp(token, "bpsocketcontainerindustrial") == 0
        || std::strcmp(token, "bpsocketcontainer") == 0
        || std::strcmp(token, "bpsalvagecontainer") == 0;
}

static std::string NormalizeTokenHaystack(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c))
            out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

WorldItemCategory MatchWorldPropToken(const std::string& haystack, std::string* outLabel)
{
    const std::string norm = NormalizeTokenHaystack(haystack);
    if (norm.empty())
        return WorldItemCategory::Invalid;

    const WorldPropTokenEntry* best = nullptr;
    size_t bestLen = 0;
    for (const WorldPropTokenEntry& entry : kWorldPropTokens) {
        if (!entry.token || !*entry.token)
            continue;
        if (norm.find(entry.token) == std::string::npos)
            continue;
        const size_t len = std::strlen(entry.token);
        if (len > bestLen) {
            bestLen = len;
            best = &entry;
        }
    }
    if (!best)
        return WorldItemCategory::Invalid;
    if (outLabel && best->label)
        *outLabel = best->label;
    return best->category;
}

std::string ToLowerLocal(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string NormalizeSocketContainerFName(std::string fname)
{
    std::string lower = ToLowerLocal(std::move(fname));
    if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "_c") == 0)
        lower.resize(lower.size() - 2);
    if (lower.size() > 3 && lower.compare(0, 3, "bp_") == 0)
        lower = lower.substr(3);
    return lower;
}

bool FnameHasSocketSalvagePrefix(const std::string& fname)
{
    if (fname.empty())
        return false;

    const std::string lower = ToLowerLocal(fname);
    // Ground item pickups (salvage fertilizer, fabric, etc.) are not world containers.
    if (lower.find("da_item") != std::string::npos
        || lower.find("bp_item") != std::string::npos
        || lower.find("bp_pickup") != std::string::npos
        || lower.find("pickupbase") != std::string::npos
        || lower.find("wid_") != std::string::npos)
        return false;
    if (FnameLooksLikeDroppedPickup(fname) || FnameLooksLikeHarvestableActor(fname))
        return false;

    const std::string norm = NormalizeSocketContainerFName(fname);
    // Prefix-only: never match bare "salvage" inside item_salvage_fertilizer.
    return norm.rfind("socketcontainer_", 0) == 0
        || norm.rfind("salvagecontainer_", 0) == 0
        || norm.rfind("lootcontainersingle_", 0) == 0
        || norm.rfind("lootcontainer_", 0) == 0
        || norm == "socketcontainer" || norm == "bpsocketcontainer"
        || norm == "salvagecontainer" || norm == "bpsalvagecontainer";
}

WorldItemCategory ClassifySocketSalvageContainerFromFname(const std::string& fnameLower)
{
    if (!FnameHasSocketSalvagePrefix(fnameLower))
        return WorldItemCategory::Invalid;

    const std::string norm = NormalizeSocketContainerFName(fnameLower);
    size_t prefixLen = 0;
    if (norm.rfind("socketcontainer_", 0) == 0)
        prefixLen = 16;
    else if (norm.rfind("salvagecontainer_", 0) == 0)
        prefixLen = 17;
    else if (norm.rfind("lootcontainersingle_", 0) == 0)
        prefixLen = 20;
    else if (norm.rfind("lootcontainer_", 0) == 0)
        prefixLen = 14;
    else if (norm == "socketcontainer" || norm == "bpsocketcontainer")
        return WorldItemCategory::Crate;
    else if (norm == "salvagecontainer" || norm == "bpsalvagecontainer")
        return WorldItemCategory::Crate;
    else
        return WorldItemCategory::Invalid;

    const std::string rest = prefixLen > 0 && norm.size() > prefixLen
        ? norm.substr(prefixLen) : norm;
    if (const WorldItemCategory cat = MatchWorldPropToken(rest, nullptr);
        cat != WorldItemCategory::Invalid)
        return cat;

    return WorldItemCategory::Crate;
}

static bool IsSpokenNumberWord(const std::string& wordLower)
{
    static const char* kWords[] = {
        "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
        "eleven", "twelve", "oh", "o",
    };
    for (const char* w : kWords) {
        if (wordLower == w)
            return true;
    }
    return false;
}

bool IsBareNumericOrSpokenNumberLabel(const std::string& label)
{
    if (label.empty())
        return false;

    int digits = 0;
    int letters = 0;
    for (unsigned char c : label) {
        if (std::isdigit(c))
            ++digits;
        else if (std::isalpha(c))
            ++letters;
    }
    if (digits >= 1 && letters == 0)
        return true;

    const std::string lower = ToLowerLocal(label);
    if (IsSpokenNumberWord(lower))
        return true;

    std::vector<std::string> words;
    words.reserve(4);
    std::string word;
    for (size_t i = 0; i <= lower.size(); ++i) {
        const char c = (i < lower.size()) ? lower[i] : ' ';
        if (c == ' ') {
            if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
        } else {
            word.push_back(c);
        }
    }
    if (!words.empty()) {
        bool allSpoken = true;
        for (const std::string& w : words) {
            if (!IsSpokenNumberWord(w)) {
                allSpoken = false;
                break;
            }
        }
        if (allSpoken)
            return true;
    }
    return false;
}

static bool IsContainerFnameNoiseSegment(const std::string& segment)
{
    if (segment.empty())
        return true;

    const std::string lower = ToLowerLocal(segment);
    if (lower.size() == 1)
        return true;

    bool hasAlpha = false;
    bool allDigitOrAlpha = true;
    for (unsigned char c : lower) {
        if (std::isalpha(c))
            hasAlpha = true;
        else if (!std::isdigit(c)) {
            allDigitOrAlpha = false;
            break;
        }
    }
    if (allDigitOrAlpha && !hasAlpha)
        return true;

    static const char* kNoise[] = {
        "res", "rsr", "cmr", "gvt", "coms", "wrh", "raider", "residential",
        "lb", "wa", "db", "bc",
        "lid", "dynamic", "clean", "blue", "green", "yellow", "red",
        "school", "bc", "blockout", "socket", "salvage", "loot",
        "container", "single", "drawer", "normal", "high", "tier",
    };
    for (const char* n : kNoise) {
        if (lower == n)
            return true;
    }
    return false;
}

std::string HumanizePropSegment(const std::string& segment)
{
    if (segment.empty() || IsContainerFnameNoiseSegment(segment))
        return {};
    if (const std::string human = HumanizeActorFName("BP_" + segment); !human.empty()
        && !IsBareNumericOrSpokenNumberLabel(human))
        return human;
    return {};
}

} // namespace

std::string ResolveSocketContainerDisplayName(const std::string& fname)
{
    if (fname.empty())
        return {};

    const std::string norm = NormalizeSocketContainerFName(fname);
    size_t prefixLen = 0;
    if (norm.rfind("socketcontainer_", 0) == 0)
        prefixLen = 16;
    else if (norm.rfind("salvagecontainer_", 0) == 0)
        prefixLen = 17;
    else if (norm.rfind("lootcontainersingle_", 0) == 0)
        prefixLen = 20;
    else if (norm.rfind("lootcontainer_", 0) == 0)
        prefixLen = 14;
    else if (norm == "socketcontainer" || norm == "bpsocketcontainer")
        return "Socket Container";
    else if (norm == "salvagecontainer" || norm == "bpsalvagecontainer")
        return "Container";
    else
        return {};

    const std::string rest = norm.substr(prefixLen);
    if (rest.empty())
        return {};

    static const struct {
        const char* token;
        const char* label;
    } kPropLabels[] = {
        { "lockers", "Locker" },
        { "locker", "Locker" },
        { "bintrash", "Trash Can" },
        { "trashbin", "Trash Can" },
        { "ammobox", "Ammo Box" },
        { "weaponcase", "Weapon Case" },
        { "medicalbag", "Medical Bag" },
        { "grenadecontainer", "Grenade Tube" },
        { "grenadetube", "Grenade Tube" },
        { "filingcabinet", "Filing Cabinet" },
        { "filing", "Filing Cabinet" },
        { "cabinet", "Filing Cabinet" },
        { "drawer", "Drawer" },
        { "safe", "Safe" },
        { "shippingcontainer", "Shipping Container" },
        { "raidercache", "Raider Cache" },
        { "burieddetectable", "Buried Loot" },
        { "buried_detectable", "Buried Loot" },
        { "deaddrop", "Dead Drop" },
        { "footlocker", "Footlocker" },
        { "dumpster", "Dumpster" },
        { "toolbox", "Toolbox" },
        { "vault", "Vault" },
        { "coffeemachine", "Coffee Machine" },
        { "benchkitchen", "Kitchen Bench" },
        { "cooler", "Cooler" },
        { "fridge", "Fridge" },
        { "refrigerator", "Refrigerator" },
        { "freezer", "Freezer" },
        { "icebox", "Cooler" },
        { "cartcrate", "Cart Crate" },
        { "fieldcrate", "Field Crate" },
        { "closet", "Closet" },
        { "cupboard", "Cupboard" },
        { "cupboardcafe", "Cupboard" },
        { "computer", "Computer" },
        { "servers", "Servers" },
        { "informationterminal", "Information Terminal" },
        { "rocketparts", "Rocket Parts" },
        { "trashbin", "Trash Can" },
        { "trailercompressor", "Trailer Compressor" },
        { "electricalcabinet", "Electrical Cabinet" },
        { "ventmachine", "Vent Machine" },
        { "solarpanel", "Solar Panel" },
        { "seedconsole", "Seed Console" },
        { "highwayaddon", "Highway Addon" },
        { "roadbarrier", "Road Barrier" },
        { "weaponsrack", "Weapons Rack" },
        { "weaponrack", "Weapon Rack" },
        { "securitycamera", "Security Camera" },
        { "barricade", "Barricade" },
        { "archusk", "Arc Husk" },
        { "truckutility", "Truck Utility" },
        { "machinem", "Machine" },
        { "doorright", "Machine" },
    };

    for (const auto& entry : kPropLabels) {
        if (rest.find(entry.token) != std::string::npos)
            return entry.label;
    }

    if (std::string fromTable;
        MatchWorldPropToken(rest, &fromTable) != WorldItemCategory::Invalid
        && !fromTable.empty())
        return fromTable;

    if (std::string fromCsv; LookupAssetWorldPropByFName(fname, &fromCsv, nullptr)
        && !fromCsv.empty() && !IsGenericWorldEspLabel(fromCsv))
        return fromCsv;

    std::vector<std::string> parts;
    parts.reserve(8);
    {
        std::string piece;
        for (char c : rest) {
            if (c == '_') {
                if (!piece.empty()) {
                    parts.push_back(piece);
                    piece.clear();
                }
            } else {
                piece.push_back(c);
            }
        }
        if (!piece.empty())
            parts.push_back(piece);
    }

    std::string best;
    size_t bestLen = 0;
    for (const std::string& part : parts) {
        if (IsContainerFnameNoiseSegment(part))
            continue;
        const std::string human = HumanizePropSegment(part);
        if (human.empty() || IsGenericWorldEspLabel(human) || IsJunkWorldEspLabel(human)
            || !IsPlausibleEspLabel(human) || IsGarbledEspLabel(human))
            continue;
        if (human.size() > bestLen) {
            best = human;
            bestLen = human.size();
        }
    }
    if (!best.empty())
        return best;

    return {};
}

std::string DirectContainerKeywordLabel(
    const std::string& fname,
    const std::string& classFname,
    const std::string& dataAssetFname)
{
    auto socketSubtypeLabel = [](const std::string& probe) -> std::string {
        if (probe.empty())
            return {};
        const std::string fromSocket = ResolveSocketContainerDisplayName(probe);
        if (!fromSocket.empty() && !IsGenericWorldEspLabel(fromSocket))
            return fromSocket;
        return {};
    };

    for (const std::string& probe : {classFname, fname, dataAssetFname}) {
        if (const std::string subtype = socketSubtypeLabel(probe); !subtype.empty())
            return subtype;
    }

    // Concatenate every hint we have so a keyword wins no matter which field holds
    // it (encrypted actor fname but readable class fname, or vice versa).
    std::string combined;
    combined.reserve(fname.size() + classFname.size() + dataAssetFname.size() + 4);
    combined += ToLowerLocal(fname);
    combined += '|';
    combined += ToLowerLocal(classFname);
    combined += '|';
    combined += ToLowerLocal(dataAssetFname);
    if (combined.empty())
        return {};

    // Ordered most-specific-first; longest matching token wins so "electricalcabinet"
    // and "weaponcase" beat shorter substrings. Every token below is justified by a
    // real ARC fname pattern (Help.txt SDK dump) or ST_WorldObjects loc id.
    struct KeywordLabel { const char* token; const char* label; };
    static const KeywordLabel kMap[] = {
        // Loot-activity / mission containers
        {"deaddrop", "Dead Drop"},
        {"dead_drop", "Dead Drop"},
        {"raidercache", "Raider Cache"},
        {"raider_cache", "Raider Cache"},
        {"firstwaveraidercache", "First Wave Cache"},
        {"cargoship", "Arc Cargo"},
        {"arc_cargo", "Arc Cargo"},
        {"buried", "Buried Loot"},
        // Salvage / socket container props
        {"shippingcontainer", "Shipping Container"},
        {"weaponcase", "Weapon Case"},
        {"weapon_case", "Weapon Case"},
        {"ammobox", "Ammo Box"},
        {"ammo_box", "Ammo Box"},
        {"medicalbag", "Medical Bag"},
        {"medical_bag", "Medical Bag"},
        {"grenadecontainer", "Grenade Tube"},
        {"grenadetube", "Grenade Tube"},
        {"electricalcabinet", "Electrical Cabinet"},
        {"filingcabinet", "Filing Cabinet"},
        {"cabinet", "Cabinet"},
        {"cupboard", "Cupboard"},
        {"closet", "Closet"},
        {"drawer", "Drawer"},
        {"workbench", "Workbench"},
        {"footlocker", "Footlocker"},
        {"toolbox", "Toolbox"},
        {"lockers", "Locker"},
        {"locker", "Locker"},
        {"coffeemachine", "Coffee Machine"},
        {"benchkitchen", "Kitchen Bench"},
        {"cooler", "Cooler"},
        {"fridge", "Fridge"},
        {"refrigerator", "Refrigerator"},
        {"freezer", "Freezer"},
        {"icebox", "Cooler"},
        {"closet", "Closet"},
        {"cupboard", "Cupboard"},
        {"cupboardcafe", "Cupboard"},
        {"computer", "Computer"},
        {"servers", "Servers"},
        {"informationterminal", "Information Terminal"},
        {"rocketparts", "Rocket Parts"},
        {"trashbin", "Trash Can"},
        {"bintrash", "Trash Can"},
        {"trash", "Trash Can"},
        {"fieldcrate", "Field Crate"},
        {"field_crate", "Field Crate"},
        {"cartcrate", "Cart Crate"},
        {"cart_crate", "Cart Crate"},
        {"objectcrate", "Object Crate"},
        {"cooler", "Cooler"},
        {"vault", "Vault"},
        {"safe", "Safe"},
        {"trailercompressor", "Trailer Compressor"},
        {"ventmachine", "Vent Machine"},
        {"solarpanel", "Solar Panel"},
        {"seedconsole", "Seed Console"},
        {"highwayaddon", "Highway Addon"},
        {"roadbarrier", "Road Barrier"},
        {"weaponsrack", "Weapons Rack"},
        {"weaponrack", "Weapon Rack"},
        {"securitycamera", "Security Camera"},
        {"deployablebarricade", "Barricade"},
        {"barricade", "Barricade"},
        {"archusk", "Arc Husk"},
        {"supplycellstation", "Supply Call Station"},
        {"supplycell", "Supply Call Station"},
        {"truckutility", "Truck Utility"},
        {"machinem", "Machine"},
        {"doorright", "Machine"},
        {"compressor", "Compressor"},
    };

    const char* bestLabel = nullptr;
    size_t bestLen = 0;
    for (const KeywordLabel& kw : kMap) {
        const size_t tokLen = std::strlen(kw.token);
        if (tokLen <= bestLen)
            continue;
        // Guard "safe" against false hits like "safepocket"/"safecode" that are not
        // storage containers; require the storage-safe context if only "safe" matched.
        if (std::strcmp(kw.token, "safe") == 0
            && (combined.find("safepocket") != std::string::npos
                || combined.find("safecode") != std::string::npos))
            continue;
        if (combined.find(kw.token) != std::string::npos) {
            bestLabel = kw.label;
            bestLen = tokLen;
        }
    }
    return bestLabel ? std::string(bestLabel) : std::string{};
}

std::string ResolveContainerDisplayLabel(
    const std::string& fname,
    const std::string& currentDisplay)
{
    auto polish = [](std::string s) -> std::string {
        return s.empty() ? std::string{} : FormatEspDisplayLabel(s);
    };

    // Direct type-keyword match on the actor's own fname wins over every category /
    // CSV / world-object lookup below, so a "locker" is always a "Locker".
    if (const std::string kw = DirectContainerKeywordLabel(fname, {}, {}); !kw.empty())
        return polish(kw);

    if (!fname.empty()) {
        if (const std::string fromSocket = ResolveSocketContainerDisplayName(fname);
            !fromSocket.empty() && !IsGenericWorldEspLabel(fromSocket))
            return polish(fromSocket);
    }

    if (!fname.empty()) {
        std::string fromCsv;
        if (LookupAssetWorldPropByFName(fname, &fromCsv, nullptr) && !fromCsv.empty()
            && fromCsv != "Container" && fromCsv != "Crate")
            return polish(fromCsv);
        if (const std::string fromWorld = LookupWorldObjectByFName(fname);
            !fromWorld.empty() && fromWorld != "Container" && fromWorld != "Crate")
            return polish(fromWorld);
    }

    const std::string f = ToLowerLocal(fname);
    const std::string d = ToLowerLocal(currentDisplay);

    if (Contains(f, "deaddrop") || Contains(d, "dead drop"))
        return "Dead Drop";
    if (Contains(f, "burieddetectable") || Contains(f, "buried_detectable"))
        return "Buried Loot";
    if (Contains(f, "raidercache") || Contains(f, "raider_cache") || Contains(f, "bp_raidercache")
        || Contains(d, "raider cache") || Contains(d, "raider stock"))
        return "Raider Cache";
    if (Contains(f, "cargoship") || Contains(f, "arc_cargo") || Contains(f, "arc_cargoship")
        || Contains(d, "cargoship"))
        return "Arc Cargo";
    if (Contains(f, "simplelootactivity"))
        return "Raider Cache";
    if (Contains(f, "lockers") || Contains(f, "locker") || Contains(d, "locker"))
        return "Locker";
    if (Contains(f, "bintrash") || Contains(f, "trash") || Contains(d, "trash can")
        || Contains(d, "trash"))
        return "Trash Can";
    if (Contains(f, "safe") || Contains(d, "safe"))
        return "Safe";
    if (Contains(f, "vault") || Contains(d, "vault"))
        return "Vault";
    if (Contains(d, "ammo box") || Contains(f, "ammobox") || Contains(f, "ammo_box"))
        return "Ammo Box";
    if (Contains(d, "weapon case") || Contains(f, "weaponcase") || Contains(f, "weapon_case"))
        return "Weapon Case";
    if (Contains(d, "medical bag") || Contains(f, "medicalbag") || Contains(f, "medical_bag"))
        return "Medical Bag";
    if (Contains(d, "grenade tube") || Contains(f, "grenadecontainer") || Contains(f, "grenadetube"))
        return "Grenade Tube";
    if (Contains(d, "shipping container") || Contains(f, "shippingcontainer"))
        return "Shipping Container";
    if (Contains(f, "footlocker") || Contains(d, "footlocker"))
        return "Footlocker";
    if (Contains(f, "toolbox") || Contains(d, "toolbox"))
        return "Toolbox";
    if (Contains(f, "filingcabinet") || Contains(f, "filing") || Contains(d, "filing"))
        return "Filing Cabinet";
    if (Contains(f, "cartcrate") || Contains(f, "cart_crate") || Contains(d, "cart crate"))
        return "Cart Crate";
    if (Contains(f, "weaponrack") || Contains(d, "weapon rack"))
        return "Weapon Rack";
    if (Contains(f, "weaponsrack") || Contains(d, "weapons rack"))
        return "Weapons Rack";
    if (Contains(f, "securitycamera") || Contains(d, "security camera"))
        return "Security Camera";
    if ((Contains(f, "deployable") && Contains(f, "barricade"))
        || Contains(f, "deployablebarricade"))
        return "Barricade";
    if (Contains(f, "fieldcrate") || Contains(f, "field_crate") || Contains(d, "field crate"))
        return "Field Crate";
    if (Contains(f, "lootcontainersingle") || Contains(f, "lootcontainer")
        || Contains(f, "socketcontainer") || Contains(f, "salvagecontainer")
        || Contains(f, "bpsocketcontainer") || Contains(f, "bpsalvagecontainer")) {
        if (Contains(f, "locker") || Contains(f, "rsr_lockers") || Contains(f, "rsrlockers"))
            return "Locker";
        if (Contains(f, "trash") || Contains(f, "bintrash"))
            return "Trash Can";
        if (Contains(f, "ammo"))
            return "Ammo Box";
        if (const std::string human = HumanizeActorFName(fname);
            !human.empty() && !IsGenericWorldEspLabel(human)
            && !IsJunkWorldEspLabel(human) && IsPlausibleEspLabel(human))
            return polish(human);
    }

    if (currentDisplay == "Container" || currentDisplay == "Crate"
        || IsGenericWorldEspLabel(currentDisplay)) {
        if (!fname.empty()) {
            if (const std::string human = HumanizeActorFName(fname);
                !human.empty() && !IsGenericWorldEspLabel(human)
                && !IsJunkWorldEspLabel(human) && IsPlausibleEspLabel(human))
                return polish(human);
        }
        return {};
    }

    if (!currentDisplay.empty() && IsGenericWorldEspLabel(currentDisplay))
        return {};

    return polish(currentDisplay);
}

static bool FnameOrDisplayHasKeyToken(const std::string& f, const std::string& d)
{
    if (Contains(f, "da_item_keyitem") || Contains(f, "keyitem_"))
        return true;
    if (Contains(f, "utilitykey") || Contains(f, "hatchkey") || Contains(f, "patrolcarkey"))
        return true;
    if (Contains(f, "hotelkey") || Contains(f, "wavebreakerkey") || Contains(f, "cranehousekey"))
        return true;
    if (Contains(f, "controlledaccesszonekey") || Contains(f, "terminalkey") || Contains(f, "bunker_key"))
        return true;
    if (f.find("key") != std::string::npos && f.find("da_item_salvage") != std::string::npos)
        return true;
    if (Contains(d, "keycard") || Contains(d, "access card"))
        return true;
    if (d.size() >= 4 && d.compare(d.size() - 4, 4, " key") == 0)
        return true;
    if (d.find(" key ") != std::string::npos)
        return true;
    return false;
}

const char* WorldItemCategoryLabel(WorldItemCategory cat)
{
    switch (cat) {
    case WorldItemCategory::DroppedPickup: return "Dropped Pickup";
    case WorldItemCategory::Items: return "Items";
    case WorldItemCategory::Ammo: return "Ammo Box";
    case WorldItemCategory::ArcLoot: return "ARC Loot";
    case WorldItemCategory::Backpack: return "Backpack";
    case WorldItemCategory::Crate: return "Crate";
    case WorldItemCategory::Furniture: return "Furniture";
    case WorldItemCategory::Grenade: return "Grenade Tube";
    case WorldItemCategory::Harvestable: return "Harvestable";
    case WorldItemCategory::Industrial: return "Industrial Container";
    case WorldItemCategory::Medical: return "Medical Bag";
    case WorldItemCategory::Other: return "Other";
    case WorldItemCategory::Probe: return "ARC Probe";
    case WorldItemCategory::RaiderCache: return "Raider Cache";
    case WorldItemCategory::Vehicles: return "Vehicles";
    case WorldItemCategory::WeaponCase: return "Weapon Case";
    case WorldItemCategory::FieldCrate: return "Field Crate";
    case WorldItemCategory::SupplyCallStation: return "Supply Call Station";
    case WorldItemCategory::Corpse: return "Dead Body";
    case WorldItemCategory::RaiderStock: return "Raider Stock";
    case WorldItemCategory::ArcCargoship: return "Arc Cargo";
    case WorldItemCategory::Keys: return "Key";
    case WorldItemCategory::Locker: return "Locker";
    case WorldItemCategory::Trash: return "Trash Can";
    case WorldItemCategory::Safe: return "Safe";
    case WorldItemCategory::Buried: return "Buried Loot";
    case WorldItemCategory::DeadDrop: return "Dead Drop";
    case WorldItemCategory::OpenedContainer: return "Open Container";
    default: return "Unknown";
    }
}

std::string ContainerCategoryFallbackEspLabel(WorldItemCategory cat)
{
    switch (cat) {
    case WorldItemCategory::Crate:              return "Crate";
    case WorldItemCategory::FieldCrate:         return "Field Crate";
    case WorldItemCategory::WeaponCase:         return "Weapon Case";
    case WorldItemCategory::Locker:             return "Locker";
    case WorldItemCategory::Safe:               return "Safe";
    case WorldItemCategory::Trash:              return "Trash Can";
    case WorldItemCategory::Ammo:               return "Ammo Box";
    case WorldItemCategory::Medical:            return "Medical Crate";
    case WorldItemCategory::Grenade:            return "Grenade Crate";
    case WorldItemCategory::Backpack:           return "Backpack";
    case WorldItemCategory::Vehicles:           return "Vehicle";
    case WorldItemCategory::Industrial:         return "Industrial Container";
    case WorldItemCategory::Furniture:          return "Furniture";
    case WorldItemCategory::Probe:              return "Probe";
    case WorldItemCategory::RaiderCache:        return "Raider Cache";
    case WorldItemCategory::RaiderStock:        return "Raider Stock";
    case WorldItemCategory::ArcCargoship:       return "Arc Cargo";
    case WorldItemCategory::ArcLoot:            return "Arc Container";
    case WorldItemCategory::SupplyCallStation:  return "Supply Station";
    case WorldItemCategory::Buried:             return "Buried Loot";
    case WorldItemCategory::DeadDrop:           return "Dead Drop";
    case WorldItemCategory::Corpse:             return "Dead Body";
    case WorldItemCategory::OpenedContainer:    return "Open Container";
    default:                                    return "Container";
    }
}

namespace {

std::array<uint8_t, static_cast<size_t>(WorldItemCategory::Count)> g_containerRangeSp{};

void InitContainerRangeDefaults()
{
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;
    g_containerRangeSp.fill(0);
    const WorldItemCategory spOn[] = {
        WorldItemCategory::Crate,
        WorldItemCategory::Locker,
        WorldItemCategory::Safe,
        WorldItemCategory::Furniture,
        WorldItemCategory::FieldCrate,
        WorldItemCategory::WeaponCase,
        WorldItemCategory::Industrial,
        WorldItemCategory::Probe,
        WorldItemCategory::SupplyCallStation,
        WorldItemCategory::Buried,
        WorldItemCategory::DeadDrop,
        WorldItemCategory::Keys,
        WorldItemCategory::Trash,
        WorldItemCategory::Vehicles,
        WorldItemCategory::OpenedContainer,
    };
    for (WorldItemCategory cat : spOn)
        g_containerRangeSp[static_cast<size_t>(cat)] = 1;
}

} // namespace

bool WorldCategoryHasSpConfig(WorldItemCategory cat)
{
    if (WorldCategoryIsContainerProp(cat))
        return true;
    switch (cat) {
    case WorldItemCategory::DroppedPickup:
    case WorldItemCategory::Corpse:
    case WorldItemCategory::RaiderStock:
    case WorldItemCategory::ArcCargoship:
    case WorldItemCategory::RaiderCache:
        return true;
    default:
        return false;
    }
}

bool WorldCategoryIsContainerProp(WorldItemCategory cat)
{
    switch (cat) {
    case WorldItemCategory::Items:
    case WorldItemCategory::Ammo:
    case WorldItemCategory::ArcLoot:
    case WorldItemCategory::Backpack:
    case WorldItemCategory::Crate:
    case WorldItemCategory::Furniture:
    case WorldItemCategory::Grenade:
    case WorldItemCategory::Harvestable:
    case WorldItemCategory::Industrial:
    case WorldItemCategory::Medical:
    case WorldItemCategory::Other:
    case WorldItemCategory::Probe:
    case WorldItemCategory::Vehicles:
    case WorldItemCategory::WeaponCase:
    case WorldItemCategory::FieldCrate:
    case WorldItemCategory::SupplyCallStation:
    case WorldItemCategory::DroppedPickup:
    case WorldItemCategory::RaiderStock:
    case WorldItemCategory::ArcCargoship:
    case WorldItemCategory::Corpse:
    case WorldItemCategory::RaiderCache:
    case WorldItemCategory::Keys:
    case WorldItemCategory::Locker:
    case WorldItemCategory::Trash:
    case WorldItemCategory::Safe:
    case WorldItemCategory::Buried:
    case WorldItemCategory::DeadDrop:
    case WorldItemCategory::OpenedContainer:
        return true;
    default:
        return false;
    }
}

bool WorldCategoryUsesSpContainerRange(WorldItemCategory cat)
{
    InitContainerRangeDefaults();
    const size_t idx = static_cast<size_t>(cat);
    if (idx >= g_containerRangeSp.size())
        return false;
    return g_containerRangeSp[idx] != 0;
}

void SetContainerRangeSp(WorldItemCategory cat, bool useSp)
{
    InitContainerRangeDefaults();
    const size_t idx = static_cast<size_t>(cat);
    if (idx < g_containerRangeSp.size())
        g_containerRangeSp[idx] = useSp ? 1 : 0;
}

static bool CategoryRowUsesSp(WorldItemCategory cat)
{
    InitContainerRangeDefaults();
    const size_t idx = static_cast<size_t>(cat);
    if (idx >= g_containerRangeSp.size())
        return false;
    return g_containerRangeSp[idx] != 0;
}

float WorldCategoryMaxDrawMeters(WorldItemCategory cat)
{
    // Per-row SP: unchecked = loot_distance, checked = container_distance_sp.
    return CategoryRowUsesSp(cat)
        ? var::container_distance_sp
        : var::loot_distance;
}

bool WorldLootEntryLooksLikeContainer(const WorldLootFilterView& loot)
{
    if (!loot.actorName.empty() && FnameLooksLikeWorldContainer(loot.actorName))
        return true;

    std::string plain;
    plain.reserve(loot.itemDisplayName.size());
    for (unsigned char c : loot.itemDisplayName) {
        if (std::isalnum(c))
            plain.push_back(static_cast<char>(std::tolower(c)));
    }
    if (!plain.empty()) {
        static const char* kDisplayTokens[] = {
            "socketcontainer", "salvagecontainer", "weaponsrack", "weaponrack",
            "securitycamera", "deployablebarricade", "archusk", "barricade",
            "compressor", "electricalcabinet", "supplycellstation",
        };
        for (const char* token : kDisplayTokens) {
            if (plain.find(token) != std::string::npos)
                return true;
        }
        if (plain.find("husk") != std::string::npos
            && plain.find("arc") != std::string::npos)
            return true;
    }

    const auto wcat = static_cast<WorldItemCategory>(loot.worldCategory);
    return WorldCategoryIsContainerProp(wcat)
        && wcat != WorldItemCategory::DroppedPickup
        && wcat != WorldItemCategory::Items
        && wcat != WorldItemCategory::Harvestable;
}

float WorldLootPickupMaxDrawMeters(WorldItemCategory cat, const WorldLootFilterView* loot)
{
    const float maxM = WorldCategoryMaxDrawMeters(cat);
    // Row SP checkbox is authoritative — never downgrade to loot_distance via pickup path.
    if (CategoryRowUsesSp(cat))
        return maxM;
    if (!loot)
        return maxM;

    if (WorldLootEntryLooksLikeContainer(*loot))
        return maxM;

    if (!LootItemLooksLikePickup(*loot))
        return maxM;

    const float minValue = var::loot_min_value;
    const int minTier = LootMinRarityMenuToMinTier(var::loot_min_rarity);

    const bool meetsValue = minValue > 0.f
        && loot->lootValue > 0
        && static_cast<float>(loot->lootValue) >= minValue;
    const bool meetsRarity = minTier > 0
        && loot->lootRarityTier > 0
        && loot->lootRarityTier >= minTier;

    if (!meetsValue && !meetsRarity)
        return maxM;

    // Qualifying pickup: filter SP checkbox → SP or loot slider (menu tooltip).
    bool useSp = false;
    bool useLoot = false;
    if (meetsValue) {
        if (var::loot_min_val_sp)
            useSp = true;
        else
            useLoot = true;
    }
    if (meetsRarity) {
        if (var::loot_min_rar_sp)
            useSp = true;
        else
            useLoot = true;
    }
    if (useSp)
        return var::container_distance_sp;
    if (useLoot)
        return var::loot_distance;

    return maxM;
}

int LootMinRarityMenuToMinTier(int menuIndex)
{
    if (menuIndex <= 0)
        return 0;
    // Menu: 1=Uncommon+, 2=Rare+, 3=Epic+, 4=Legendary → tiers 2..5
    return menuIndex + 1;
}

bool LootItemLooksLikePickup(const WorldLootFilterView& loot)
{
    if (WorldLootEntryLooksLikeContainer(loot))
        return false;

    const auto wcat = static_cast<WorldItemCategory>(loot.worldCategory);
    // Loop-tab world categories keep category coloring, not pickup/rarity palette.
    if (wcat != WorldItemCategory::DroppedPickup
        && wcat != WorldItemCategory::Items
        && wcat != WorldItemCategory::Keys)
        return false;

    if (loot.lootValue > 0 || loot.lootRarityTier > 0)
        return true;

    if (wcat == WorldItemCategory::DroppedPickup || wcat == WorldItemCategory::Keys)
        return true;

    const auto hasPickupHint = [&](const std::string& s) {
        return s.find("pickup") != std::string::npos
            || s.find("Pickup") != std::string::npos;
    };

    return hasPickupHint(loot.actorName) || hasPickupHint(loot.itemDisplayName);
}

bool PassesLootPickupFilters(const WorldLootFilterView& loot)
{
    // Min value / rarity never hard-hide. They only select draw distance
    // (loot_distance vs SP) in WorldLootPickupMaxDrawMeters. Hiding low-tier
    // loot left 2m pickups blank while filters sat at Epic+/1068 in config.
    (void)loot;
    return true;
}

std::string ResolveWorldDrawLabel(
    uint8_t worldCategory,
    const std::string& actorName,
    const std::string& itemDisplayName)
{
    const auto cat = static_cast<WorldItemCategory>(worldCategory);
    std::string label = itemDisplayName;
    if (!label.empty() && IsPlausibleEspLabel(label) && !IsGenericWorldEspLabel(label))
        return label;

    WorldLootFilterView view{ worldCategory, actorName, itemDisplayName, 0, 0 };
    if (LootItemLooksLikePickup(view))
        return {}; // no placeholder "Loot"/"Pickup"/"Item"

    if (WorldCategoryIsContainerProp(cat)
        && cat != WorldItemCategory::DroppedPickup
        && cat != WorldItemCategory::Items) {
        if (const char* catLabel = WorldItemCategoryLabel(cat)) {
            const std::string s(catLabel);
            if (!s.empty() && s != "Unknown" && s != "Other" && s != "Items"
                && IsPlausibleEspLabel(s) && !IsGenericWorldEspLabel(s))
                return s;
        }
        return {};
    }

    if (const char* catLabel = WorldItemCategoryLabel(cat)) {
        const std::string s(catLabel);
        if (!s.empty() && s != "Unknown" && IsPlausibleEspLabel(s)
            && !IsGenericWorldEspLabel(s))
            return s;
    }

    return label.empty() ? std::string{} : label;
}

bool TrySetContainerRangeFromConfigSuffix(const char* suffix, bool useSp)
{
    if (!suffix || !*suffix)
        return false;
    for (size_t i = 0; i < static_cast<size_t>(WorldItemCategory::Count); ++i) {
        const auto cat = static_cast<WorldItemCategory>(i);
        if (!WorldCategoryHasSpConfig(cat))
            continue;
        const char* s = WorldItemCategoryConfigSuffix(cat);
        if (s && std::strcmp(s, suffix) == 0) {
            SetContainerRangeSp(cat, useSp);
            return true;
        }
    }
    return false;
}

const char* WorldItemCategoryConfigSuffix(WorldItemCategory cat)
{
    switch (cat) {
    case WorldItemCategory::Items: return "items";
    case WorldItemCategory::Ammo: return "ammo";
    case WorldItemCategory::ArcLoot: return "arc_loot";
    case WorldItemCategory::Backpack: return "backpack";
    case WorldItemCategory::Crate: return "crate";
    case WorldItemCategory::Furniture: return "furniture";
    case WorldItemCategory::Grenade: return "grenade";
    case WorldItemCategory::Harvestable: return "harvestable";
    case WorldItemCategory::Industrial: return "industrial";
    case WorldItemCategory::Medical: return "medical";
    case WorldItemCategory::Other: return "other";
    case WorldItemCategory::Probe: return "probe";
    case WorldItemCategory::Vehicles: return "vehicles";
    case WorldItemCategory::WeaponCase: return "weapon_case";
    case WorldItemCategory::FieldCrate: return "field_crate";
    case WorldItemCategory::SupplyCallStation: return "supply_station";
    case WorldItemCategory::DroppedPickup: return "dropped";
    case WorldItemCategory::Corpse: return "corpse";
    case WorldItemCategory::RaiderStock: return "raider_stock";
    case WorldItemCategory::RaiderCache: return "raider_cache";
    case WorldItemCategory::ArcCargoship: return "arc_entities";
    case WorldItemCategory::Keys: return "keys";
    case WorldItemCategory::Locker: return "locker";
    case WorldItemCategory::Trash: return "trash";
    case WorldItemCategory::Safe: return "safe";
    case WorldItemCategory::Buried: return "buried";
    case WorldItemCategory::DeadDrop: return "deaddrop";
    case WorldItemCategory::OpenedContainer: return "open_container";
    default: return nullptr;
    }
}

WorldItemCategory CategoryFromConfigSuffix(const char* suffix)
{
    if (!suffix || !*suffix)
        return WorldItemCategory::Invalid;
    for (size_t i = 0; i < static_cast<size_t>(WorldItemCategory::Count); ++i) {
        const auto cat = static_cast<WorldItemCategory>(i);
        const char* s = WorldItemCategoryConfigSuffix(cat);
        if (s && std::strcmp(s, suffix) == 0)
            return cat;
    }
    return WorldItemCategory::Invalid;
}

namespace {

WorldItemCategory ClassifyFromAssetIndexCategory(const std::string& fname)
{
    if (fname.empty())
        return WorldItemCategory::Invalid;

    std::string categorySuffix;
    if (!LookupAssetWorldPropByFName(fname, nullptr, &categorySuffix)
        || categorySuffix.empty())
        return WorldItemCategory::Invalid;

    return CategoryFromConfigSuffix(categorySuffix.c_str());
}

} // namespace

bool FnameOrDisplayLooksLikeHarvestable(
    const std::string& fname,
    const std::string& displayLower)
{
    if (!fname.empty() && FnameLooksLikeHarvestableActor(fname))
        return true;

    if (!displayLower.empty()) {
        static const char* kPlantDisplay[] = {
            "mullein", "moss", "mushroom", "agave", "apricot", "lemon",
            "berry", "ginger", "flora", "herb", "harvestable", "candleberry",
            "prickly", "olives", "resin", "fruit mix",
        };
        for (const char* token : kPlantDisplay) {
            if (displayLower.find(token) != std::string::npos)
                return true;
        }
    }

    std::string combined = fname;
    combined += displayLower;
    return MatchWorldPropToken(combined, nullptr) == WorldItemCategory::Harvestable;
}

WorldItemCategory ClassifyWorldActor(const std::string& fnameLower, const std::string& displayLower)
{
    const std::string& f = fnameLower;
    const std::string& d = displayLower;

    if (FnameOrDisplayLooksLikeHarvestable(f, d))
        return WorldItemCategory::Harvestable;

    if (const WorldItemCategory socketCat = ClassifySocketSalvageContainerFromFname(f);
        socketCat != WorldItemCategory::Invalid)
        return socketCat;

    std::string combined = f;
    combined += d;
    if (const WorldItemCategory fromTokens = MatchWorldPropToken(combined, nullptr);
        fromTokens != WorldItemCategory::Invalid)
        return fromTokens;

    if ((Contains(f, "corpse") || Contains(f, "deadplayer") || Contains(d, "corpse"))
        && !Contains(f, "pioneercharacter"))
        return WorldItemCategory::Corpse;

    if (Contains(f, "deaddrop") || Contains(d, "dead drop"))
        return WorldItemCategory::DeadDrop;

    if (Contains(f, "burieddetectable") || Contains(f, "buried_detectable") || Contains(d, "buried"))
        return WorldItemCategory::Buried;

    if (Contains(f, "raidercache") || Contains(f, "raider_cache") || Contains(f, "bp_raidercache")
        || Contains(d, "raider stock") || Contains(d, "raider cache"))
        return WorldItemCategory::RaiderCache;

    if (Contains(f, "cargoship") || Contains(f, "arc_cargo") || Contains(d, "cargoship"))
        return WorldItemCategory::ArcCargoship;

    if (const WorldItemCategory fromIndex = ClassifyFromAssetIndexCategory(f);
        fromIndex != WorldItemCategory::Invalid)
        return fromIndex;

    if (Contains(d, "field crate") || Contains(f, "fieldcrate") || Contains(f, "field_crate"))
        return WorldItemCategory::FieldCrate;

    if (Contains(d, "supply call") || Contains(f, "supplycall") || Contains(f, "supply_call")
        || Contains(d, "supply station")
        || Contains(f, "supplycell") || Contains(d, "supply cell"))
        return WorldItemCategory::SupplyCallStation;

    if (Contains(f, "lockers") || Contains(f, "locker") || Contains(d, "locker"))
        return WorldItemCategory::Locker;

    if (Contains(f, "bintrash") || Contains(f, "trash") || Contains(d, "trash"))
        return WorldItemCategory::Trash;

    if ((Contains(f, "safe") && !Contains(f, "safe pocket")) || Contains(d, "safe"))
        return WorldItemCategory::Safe;
    if (Contains(f, "vault") || Contains(d, "vault"))
        return WorldItemCategory::Safe;

    if (Contains(d, "ammo") || Contains(f, "ammo") || Contains(f, "ammobox") || Contains(f, "ammo_box"))
        return WorldItemCategory::Ammo;
    if (Contains(d, "backpack") || Contains(f, "backpack"))
        return WorldItemCategory::Backpack;
    if (Contains(d, "weapon case") || Contains(f, "weaponcase") || Contains(f, "weapon_case")
        || Contains(f, "container_weapon"))
        return WorldItemCategory::WeaponCase;
    if (Contains(d, "grenade") || Contains(f, "grenade") || Contains(f, "grenadetube")
        || Contains(f, "grenadecontainer"))
        return WorldItemCategory::Grenade;
    if (Contains(d, "medical") || Contains(d, "med kit") || Contains(d, "med bag")
        || Contains(f, "medical") || Contains(f, "medicalbag") || Contains(f, "medical_bag")
        || Contains(f, "medcrate") || Contains(f, "med_crate"))
        return WorldItemCategory::Medical;
    if (Contains(d, "harvestable") || Contains(d, "harvest") || Contains(f, "harvest")
        || Contains(f, "candleberry") || Contains(f, "fruitbasket")
        || Contains(f, "birdnest") || Contains(f, "beenest")
        || Contains(f, "lootsocket") || Contains(f, "pioneerlootsocket")
        || Contains(f, "consumable") || Contains(f, "bp_actor_consumable")
        || Contains(f, "environmentalloot")
        || Contains(d, "moss") || Contains(d, "mushroom")
        || Contains(d, "agave") || Contains(d, "apricot") || Contains(d, "lemon")
        || Contains(f, "agave") || Contains(f, "moss") || Contains(f, "mushroom")
        || Contains(d, "mullein") || Contains(f, "mullein")
        || Contains(d, "ginger") || Contains(f, "ginger")
        || Contains(d, "flora") || Contains(f, "flora")
        || Contains(d, "herb") || Contains(f, "herb")
        || Contains(d, "berry") || Contains(f, "berry"))
        return WorldItemCategory::Harvestable;
    if (Contains(d, "furniture") || Contains(f, "furniture")
        || Contains(f, "filing") || Contains(d, "filing")
        || Contains(f, "cabinet") || Contains(d, "cabinet")
        || Contains(f, "drawer") || Contains(d, "drawer"))
        return WorldItemCategory::Furniture;
    if (Contains(d, "vehicle") || Contains(f, "vehicle") || Contains(f, "patrolcar")
        || Contains(f, "patrol_car") || Contains(d, "patrol car")
        || Contains(f, "truck") || Contains(d, "truck")
        || Contains(f, "vehiclepart") || Contains(f, "vehicle_part"))
        return WorldItemCategory::Vehicles;
    if (Contains(f, "carryable") || Contains(d, "carryable"))
        return WorldItemCategory::Other;
    if (Contains(d, "probe") || Contains(f, "probe"))
        return WorldItemCategory::Probe;
    if (Contains(d, "industrial") || Contains(f, "industrial"))
        return WorldItemCategory::Industrial;
    if (Contains(f, "shippingcontainer") || Contains(d, "shipping container"))
        return WorldItemCategory::Crate;
    if (Contains(f, "lootcontainer") || Contains(f, "genericcontainer")
        || Contains(f, "socketcontainer") || Contains(f, "salvagecontainer"))
        return WorldItemCategory::Crate;
    if (Contains(f, "arc_loot") || Contains(f, "arcloot"))
        return WorldItemCategory::ArcLoot;

    if (Contains(f, "archusk") || Contains(f, "arc_husk")
        || (Contains(f, "husk") && Contains(f, "worldobject")))
        return WorldItemCategory::ArcCargoship;
    if (Contains(d, "husk") && (Contains(d, "arc") || Contains(f, "husk")))
        return WorldItemCategory::ArcCargoship;

    if (Contains(f, "deployablebarricade") || Contains(f, "deployable_barricade")
        || (Contains(f, "barricade") && Contains(f, "worldobject")))
        return WorldItemCategory::Other;

    if (Contains(f, "weaponsrack") || Contains(f, "weaponrack")
        || Contains(d, "weapons rack") || Contains(d, "weapon rack"))
        return WorldItemCategory::Furniture;

    if (Contains(f, "securitycamera") || Contains(f, "securityterminal")
        || Contains(d, "security camera"))
        return WorldItemCategory::Industrial;

    if (!f.empty() && FnameLooksLikeWorldContainer(f))
        return WorldItemCategory::Crate;

    if (Contains(f, "bp_pickupbase") || (Contains(f, "pickup") && !Contains(f, "container")))
        return WorldItemCategory::DroppedPickup;

    if ((Contains(f, "da_item") || Contains(f, "wid_"))
        && !FnameLooksLikeWorldContainer(f))
        return WorldItemCategory::DroppedPickup;

    if (IsStrictWorldLootFname(f) && !FnameLooksLikeWorldContainer(f))
        return WorldItemCategory::DroppedPickup;

    return WorldItemCategory::Invalid;
}

bool FnameLooksLikeEngineSubobjectClass(const std::string& fname)
{
    if (fname.empty())
        return false;

    std::string lower = fname;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "_c") == 0)
        lower.resize(lower.size() - 2);

    if (lower.size() >= 9
        && lower.compare(lower.size() - 9, 9, "component") == 0)
        return true;

    // Widget blueprints are UI, never world actors (debug-c190fb: class-fname
    // fallback drew "WBP Main Menu Carousel Backend Announcement Entry" as loot).
    if (lower.rfind("wbp_", 0) == 0 || lower.rfind("wbp ", 0) == 0
        || lower.find("_wbp_") != std::string::npos
        || lower.find("widgetblueprint") != std::string::npos)
        return true;

    static const char* kBlocked[] = {
        "lootinteraction",
        "itemcontainer",
        "lootstatemachine",
        "rootcollider",
        "pickup_rootcollider",
        "boxcomponent",
        "capsulecomponent",
        "spherecomponent",
        "scenecomponent",
        "staticmeshcomponent",
        "skeletalmeshcomponent",
        "interfaceproperty",
        "boolproperty",
        "structproperty",
        "objectproperty",
        "arrayproperty",
        "mapproperty",
        "setproperty",
        "enumproperty",
        "textproperty",
        "strproperty",
        "nameproperty",
        "classproperty",
        "softobjectproperty",
        "fproperty",
        "uproperty",
    };
    for (const char* token : kBlocked) {
        if (lower == token)
            return true;
    }
    return false;
}

bool IsGenericWorldEspLabel(const std::string& label)
{
    if (label == "Loot Item" || label == "World Item" || label == "WorldItem"
        || label == "Invalid" || label == "Other" || label == "Items" || label == "Item"
        || label == "Unknown" || label == "Loot" || label == "Dropped Pickup"
        || label == "Dropped pickup" || label == "Pickup"
        || label == "CHEST" || label == "LOOT"
        || label == "Container" || label == "Crate"
        || label == "Generic Container"
        || label == "Socket Container" || label == "Salvage Container"
        || label == "Arc" || label == "ARC"
        || label == "Pickup Base" || label == "Pickup base")
        return true;

    if (label == "Loot Interaction Component"
        || label == "Constructable Loot Interaction Component"
        || label == "Item Container Component"
        || label == "Loot State Machine Component")
        return true;

    return FnameLooksLikeEngineSubobjectClass(label);
}

bool IsPlausibleEspLabel(const std::string& label)
{
    if (label.size() < 2 || label.size() > 96)
        return false;
    if (IsGenericWorldEspLabel(label))
        return false;

    int upper = 0;
    int lower = 0;
    int spaces = 0;
    int digits = 0;
    int readable = 0;
    int bad = 0;
    for (unsigned char c : label) {
        if (c == '?')
            return false;
        if (std::isupper(c))
            ++upper;
        if (std::islower(c))
            ++lower;
        if (std::isdigit(c))
            ++digits;
        if (c == ' ')
            ++spaces;
        if (std::isalnum(c) || c == ' ' || c == '-' || c == '.' || c == '\''
            || c == '_' || c == '(' || c == ')')
            ++readable;
        else if (c < 32 || c > 126)
            return false;
        else
            ++bad;
    }

    const int letters = upper + lower;

    // Must be a real word, not a bare number/code like "01", "7", "1-2".
    if (letters < 2)
        return false;
    // Reject mostly-numeric labels (a proper name has more letters than digits).
    if (digits > letters)
        return false;

    // Reject decrypt/humanize sludge like "all hook medium wa".
    if (spaces >= 1 && upper == 0 && label.size() >= 8)
        return false;
    if (spaces >= 2 && upper <= 1 && label.size() >= 10)
        return false;

    if (readable < static_cast<int>(label.size()) / 2)
        return false;
    if (bad > static_cast<int>(label.size()) / 4)
        return false;
    return true;
}

bool DisplayLooksLikeWorldContainer(const std::string& displayLower)
{
    if (displayLower.empty())
        return false;

    static const char* kTokens[] = {
        "filing", "cabinet", "locker", "footlocker", "trash", "dumpster",
        "waste bin", "wastebin", "wastebasket", "drawer", "desk", "toolbox",
        "briefcase", "suitcase", "shipping container", "field crate",
        "weapon case", "medical bag", "ammo box", "grenade tube",
        "fruit basket", "safe", "vault", "dead drop", "buried",
        "socket container", "weapons rack", "weapon rack", "security camera",
        "barricade", "arc husk",
        "probe crashed", "crashed arc probe", "arc probe",
    };
    for (const char* token : kTokens) {
        if (Contains(displayLower, token))
            return true;
    }
    return false;
}

bool IsWorldPropLootContainer(
    bool hasLootInteraction,
    bool classChest,
    const std::string& fname,
    const std::string& display)
{
    if (classChest)
        return true;

    if (!fname.empty() && FnameLooksLikeDroppedPickup(fname))
        return false;

    if (!fname.empty() && FnameLooksLikeWorldContainer(fname))
        return true;

    if (!fname.empty()) {
        const std::string lower = ToLowerLocal(fname);
        if (lower.find("socketcontainer") != std::string::npos
            || lower.find("salvagecontainer") != std::string::npos)
            return true;
    }

    if (!hasLootInteraction)
        return false;

    if (!display.empty()) {
        const std::string displayLower = ToLowerLocal(display);
        if (DisplayLooksLikeWorldContainer(displayLower))
            return true;

        int tier = 0;
        int value = 0;
        if (LookupItemMeta(display, tier, value))
            return false;
    }

    return false;
}

namespace {

struct TArrayIntLocal {
    uintptr_t Data = 0;
    int32_t Num = 0;
    int32_t Max = 0;
};

bool SalvageContainerMeshVariantsLookValid(uintptr_t actor)
{
    const TArrayIntLocal meshVariants =
        Memory::read<TArrayIntLocal>(actor + Offsets::SalvageContainer_MeshVariants);
    if (meshVariants.Num < 1 || meshVariants.Num > 8)
        return false;
    if (meshVariants.Max < meshVariants.Num || meshVariants.Max > 16)
        return false;
    return meshVariants.Data != 0 && Memory::IsValidPtrFast2(meshVariants.Data);
}

} // namespace

bool IsRealSocketSalvageContainer(const std::string& fname, uintptr_t actor)
{
    if (actor && Memory::IsValidPtrFast2(actor)) {
        const uint32_t masked =
            ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
        if (ArcActorType::IsPlayerClassId(masked))
            return false;
        const uintptr_t playerState =
            Memory::read<uintptr_t>(actor + Offsets::APlayerState);
        if (playerState && engine.IsValidPointer(playerState)) {
            const uintptr_t mesh = engine.GetActorSkeletalMesh(actor);
            if (mesh && engine.IsValidPointer(mesh))
                return false;
        }
        std::string classProbe = engine.GetActorClassFName(actor);
        if (!classProbe.empty()) {
            std::string classLower = classProbe;
            for (char& c : classLower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (FnameExcludedFromContainerEsp(classLower))
                return false;
        }
    }

    if (!fname.empty()) {
        if (FnameLooksLikeDroppedPickup(fname) || FnameLooksLikeHarvestableActor(fname))
            return false;
        if (FnameHasSocketSalvagePrefix(fname))
            return true;
    }
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return false;

    std::string probe = fname;
    if (probe.empty()) {
        probe = engine.GetActorFNameStringCached(actor);
        if (probe.empty())
            probe = engine.GetActorFNameString(actor);
    }
    if (!probe.empty()) {
        if (FnameLooksLikeDroppedPickup(probe) || FnameLooksLikeHarvestableActor(probe))
            return false;
        if (FnameHasSocketSalvagePrefix(probe))
            return true;
    }

    const std::string classFname = engine.GetActorClassFName(actor);
    if (classFname.empty() || !FnameHasSocketSalvagePrefix(classFname))
        return false;
    if (!SalvageContainerMeshVariantsLookValid(actor))
        return false;

    const int32_t chosen =
        Memory::read<int32_t>(actor + Offsets::SalvageContainer_ChosenMesh);
    return chosen >= 0 && chosen <= 2;
}

bool IsSalvageContainerActor(const std::string& fname, uintptr_t actor)
{
    return IsRealSocketSalvageContainer(fname, actor);
}

bool PointerIsLootInteractionComponent(uintptr_t obj)
{
    if (!obj || !Memory::IsValidPtrFast2(obj))
        return false;

    std::string classFname = engine.GetActorClassFName(obj);
    if (classFname.empty())
        return false;

    std::string lower = classFname;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return lower.find("lootinteractioncomponent") != std::string::npos
        || lower.find("constructablelootinteractioncomponent") != std::string::npos;
}

bool LootInteractionOwnedByActor(uintptr_t component, uintptr_t actor)
{
    if (!component || !actor || !Memory::IsValidPtrFast2(component))
        return false;
    // UObject::OuterPrivate @ 0x20 — component must belong to this actor.
    // Strict: never treat a foreign LootInteraction pointer as owned (inflated admits).
    const uintptr_t outer = Memory::read<uintptr_t>(component + 0x20);
    return outer == actor;
}

bool FnameExcludedFromContainerEsp(const std::string& fnameLower)
{
    if (fnameLower.empty())
        return false;
    if (fnameLower.find("zipline") != std::string::npos
        || fnameLower.find("ziplineanchor") != std::string::npos
        || fnameLower.find("activatablezipline") != std::string::npos
        || fnameLower.find("placeablezipline") != std::string::npos
        || fnameLower.find("pioneercharacter") != std::string::npos
        || fnameLower.find("backpackcontainer") != std::string::npos
        || fnameLower.find("corpse") != std::string::npos
        || fnameLower.find("deadplayer") != std::string::npos
        || fnameLower.find("playerbody") != std::string::npos
        || fnameLower.find("raiderbody") != std::string::npos)
        return true;

    static const char* kBlocked[] = {
        "playercontroller",
        "pioneerplayer",
        "cameramanager",
        "playerstate",
        "gamemode",
        "gamestate",
        "gamehud",
        "playerhud",
        "dynamicshelf",
        "subobject",
        "defaultpawn",
        "playerstart",
        "spectatorpawn",
        "hudbase",
        "widgetcomponent",
        "scenecomponent",
        "actorcomponent",
        "embarkplayer",
        "embarksquad",
        "squadembark",
        "embarkstation",
        "playerstatus",
        "embarkplayerstatus",
        "statuscomponent",
        "playercharacter",
        "pioneergamestate",
        "inventorycontainer",
        "persistence",
        "gameinstance",
        "localplayer",
        "walladd",
        "wallpanel",
        "stowedweapon",
        "inventoryservice",
        "inventoryserviceitem",
        "fakeinventory",
        "flyingpad",
        "landingpad",
        "splinepath",
        "spline",
        "uaid",
        "cuaid",
        // POI trigger volumes + Slate FWindowStyle class misreads → floating
        // "F Window Style" ESP with no mesh (debug-5681af runId=names).
        "pointofinterest",
        "windowstyle",
        // Phase 2A baseline (debug-c190fb): these were admitted as containers and
        // labeled Crate/Door/junk via soft loot-pointer / LooksLikeContainerActor.
        "ddgivolume",
        "ddgi",
        "ambiencevolume",
        "indoorsvolume",
        "outdoorsvolume",
        "blockingvolume",
        "runtimevirtualtexture",
        "levelbounds",
        "staticmeshactor",
        "bp_staticspawner",
        "snapstaticspawner",
        "bp_snapstaticspawner",
        "sm_world_",
        "backdropisland",
        "waterprocessing",
        "processingbuilding",
        "embarkworldsettings",
        "defaultambience",
        "visualizerdummy",
        // debug-c190fb runId=crates: map-boundary volume admitted as cat-6
        // container with garbled label "Playable Area Limiter Salvageter
        // Treatment"; churned admit/evict every scan at 500-790m.
        "playablearealimiter",
        // debug-c190fb post-fix run: POI volumes / quest interactables / UI
        // widgets admitted as cat-6 containers ("buildings named as items").
        "wbp_",
        "questinteractable",
        "guideberm",
        "guide_berm",
        "processingfacilit",
        "researchfacilit",
        "fueldepot",
        "ventilationshaft",
        "playablespacevolume",
        // debug-c190fb runId=crates name_trace_world: these were admitted as
        // containers labeled "Crate" (Ruins12/15, AudioComponentManager,
        // StaticSpawner, BP_ParticleExclusionVolume_C) or as POI buildings
        // (SpillwayGates, CollapsedSpillway, WirelessElectricityTower4).
        "particleexclusion",
        "audiocomponentmanager",
        "staticspawner",
        "ruins",
        "spillway",
        "electricitytower",
        // debug-c190fb run 2 name_trace_world: underscore variant slipped the
        // "questinteractable" token (BP_Quest_Interactable_TheDam_Wheat1_1_C);
        // plus map regions (Area_SarnosVeins -> "Scene Root"), ping markers,
        // quest gadgets, and the generic carryable base class.
        "quest_interactable",
        "friendifier",
        "inworldping",
        "pingobject",
        "gateelevator",
        "carryable_object",
        "area_",
    };
    for (const char* token : kBlocked) {
        if (fnameLower.find(token) != std::string::npos)
            return true;
    }

    // HUD class tokens — require boundary so we do not hit unrelated substrings.
    if (fnameLower.find("hud") != std::string::npos
        && (fnameLower.find("_hud") != std::string::npos
            || fnameLower.find("hud_") != std::string::npos
            || fnameLower.rfind("hud", fnameLower.size() - 3) != std::string::npos))
        return true;

    // debug-c190fb run 2: GeneratorAdministrationBuilding admitted as container
    // "Generator". Whole buildings are never containers (item scan already has
    // the same suffix rule).
    if (fnameLower.size() >= 8
        && fnameLower.compare(fnameLower.size() - 8, 8, "building") == 0)
        return true;

    return false;
}

bool IsGarbledEspLabel(const std::string& label)
{
    if (label.size() < 6)
        return false;

    int tokens = 0;
    int shortTokens = 0;
    int letters = 0;
    std::string word;
    auto flushWord = [&]() {
        if (word.empty())
            return;
        ++tokens;
        for (unsigned char c : word) {
            if (std::isalpha(c))
                ++letters;
        }
        if (word.size() <= 2)
            ++shortTokens;
        word.clear();
    };

    for (size_t i = 0; i <= label.size(); ++i) {
        const char c = (i < label.size()) ? label[i] : ' ';
        if (c == ' ')
            flushWord();
        else
            word.push_back(c);
    }

    if (tokens >= 3 && shortTokens >= 2)
        return true;
    if (tokens >= 4 && letters > 0 && static_cast<double>(shortTokens) / tokens >= 0.5)
        return true;
    return false;
}

bool IsJunkWorldEspLabel(const std::string& label)
{
    if (label.empty())
        return false;

    const std::string lower = ToLowerLocal(label);

    static const char* kBlocked[] = {
        // Widget blueprints leaked as labels via humanized class fnames
        // ("WBP Main Menu Carousel Backend Announcement Entry", debug-c190fb).
        "wbp ",
        "wbp_",
        "widget blueprint",
        "main menu",
        "carousel",
        "announcement",
        "player controller",
        "pioneer player",
        "camera manager",
        "player state",
        "game mode",
        "game state",
        "gamemode",
        "playercontroller",
        "cameramanager",
        "playerstate",
        "dynamicshelf",
        "dynamic shelf",
        " component",
        "subobject",
        "pawn",
        "spectator",
        "game hud",
        "player hud",
        "embark player",
        "embark squad",
        "squad embark",
        "embark station",
        "player status",
        "embark status",
        "wall panel",
        "wall db",
        "status component",
        "inventory container",
        "inventory service",
        "inventory service item",
        "stowed weapon",
        "flying pad",
        "spline path",
        "spline",
        "uaid",
        "cuaid",
        "local player",
        "game instance",
        "explosion ref",
        "ui meta",
        "uimeta",
        "metadata",
        "weapon mod",
        "weaponmod",
        "collider",
        "root collider",
        "rootcollider",
        "scene component",
        "scenecomponent",
        "static mesh",
        "skeletal mesh",
        "interface property",
        "interfaceproperty",
        "bool property",
        "struct property",
        "object property",
        "array property",
        "map property",
        "set property",
        "enum property",
        "text property",
        "name property",
        "class property",
        "soft object property",
        "fproperty",
        "uproperty",
        "double property",
        "ddgi",
        "oi outfit",
        "outfit agile",
        "outfit abandoned",
        "agile astronaut",
        "abandoned astronaut",
        "white camo",
        "point of interest",
        "poi volume",
        "volume",
        "global illumination",
        "persistent level",
        "online store",
        "health service",
        "instance",
        " level instance",
        "level instance",
        // Humanized Slate FWindowStyle (POI volume class misread).
        "window style",
        "f window style",
        "f window",
        // Niagara / VFX / GameplayCue effects humanized into loot labels
        // (e.g. "NS Vision Cone Peppermint" = Queen sight-cone effect).
        "vision cone",
        "niagara",
        "gameplay cue",
        // Phase 2A baseline: SoftObject/loc misreads on volumes/spawners.
        "laser fire",
        "dialogue",
        "disco player",
        "squad wiped",
        "visualizer dummy",
        "peppermint lower",
        "hind cannon",
        // debug-c190fb item_evict: VFX + traversal actors reached the item
        // cache ("NS Bullet Trail Ribbon Launcher", "Actor Zipline") and
        // engine replication/audio actors reached bot discovery.
        "bullet trail",
        "ribbon launcher",
        "zipline",
        "effect cue",
        "replicator",
        "interpolator",
        "audio manager",
        "spot audio",
        // debug-c190fb run 2: Area_SarnosVeins drew as "Scene Root";
        // BP_InWorldPingObject_C drew as "In Ping Object".
        "scene root",
        "ping object",
        "friendifier",
    };
    for (const char* token : kBlocked) {
        if (lower.find(token) != std::string::npos)
            return true;
    }

    // Humanized UE Instantiators / Blueprint copies end up as "Foo Instance".
    if (lower == "instance" || lower.size() >= 9) {
        if (lower.size() >= 9
            && lower.compare(lower.size() - 9, 9, " instance") == 0)
            return true;
    }

    if (IsGarbledEspLabel(label))
        return true;

    if (IsBareNumericOrSpokenNumberLabel(label))
        return true;

    // Warehouse / district location prefixes — not display labels.
    if (lower == "wrh" || lower == "res" || lower == "rsr" || lower == "gvt"
        || lower == "cmr" || lower == "coms" || lower == "lb" || lower == "l b")
        return true;

    // Decrypt/instance coordinate tokens like X400z250 — not display names.
    if (lower.find(' ') == std::string::npos && label.size() >= 5 && label.size() <= 16) {
        int digits = 0;
        int uppers = 0;
        for (unsigned char c : label) {
            if (std::isdigit(c))
                ++digits;
            else if (std::isupper(c))
                ++uppers;
        }
        if (digits >= 2 && uppers >= 1)
            return true;
    }

    // Every word is 1–2 chars (e.g. "L B") — map/location codes, not loot names.
    {
        int words = 0;
        int longWords = 0;
        std::string word;
        auto flushWord = [&]() {
            if (word.empty())
                return;
            ++words;
            if (word.size() > 2)
                ++longWords;
            word.clear();
        };
        for (unsigned char c : lower) {
            if (c == ' ')
                flushWord();
            else
                word.push_back(static_cast<char>(c));
        }
        flushWord();
        if (words >= 1 && longWords == 0)
            return true;
    }

    // Bare humanized decorative-prop sludge (not curated container keywords).
    if (lower == "shelf" || lower == "dynamic shelf")
        return true;
    if (lower.size() >= 10 && lower.find(" shelf") != std::string::npos
        && lower.find("locker") == std::string::npos
        && lower.find("container") == std::string::npos
        && lower.find("cabinet") == std::string::npos)
        return true;

    return false;
}

bool IsFurniturePropLabel(const std::string& label)
{
    if (label.empty())
        return false;

    const std::string lower = ToLowerLocal(label);

    // Known furniture / structural / environment prop display names that pass
    // IsPlausibleEspLabel (clean English) but are never floor loot.
    // Derived from admitted-item audit of debug-c190fb.log — every entry here
    // was a false positive in the identityProven fallback.
    //
    // All substring tokens here must NOT appear inside legitimate item names.
    // Guard with word-boundary logic below to avoid e.g. "door" matching
    // inside "Commander" or "Gate" inside "Gate Unlock Circuit".
    static const char* kFurniture[] = {
        // Multi-word phrases — safe as substrings (unique enough)
        "filing cabinet", "kitchen bench", "centerbench",
        "wooden table", "metal table",
        "wooden chair", "metal chair",
        "work desk", "office desk",
        "bookshelf", "wall shelf",
        "double door", "sliding door", "metal door", "wooden door",
        "metal window", "glass window",
        "concrete wall", "metal wall", "brick wall",
        "floor tile", "concrete floor",
        "staircase", "stairway",
        "air vent", "exhaust vent",
        "metal pipe", "water pipe", "drain pipe",
        "air duct",
        "solar panel", "control panel", "metal panel",
        "decal crack", "crack tarmac",
        "landscape streaming proxy",
        "merged scatter", "forest backdrop",
        "constructable",
        "signal cue",
        "vent machine",
    };
    // Compound prop labels where furniture keyword is glued without space.
    static const char* kExactCompound[] = {
        "wasabipanel", "antennaset", "centerbench",
    };
    for (const char* token : kExactCompound) {
        if (lower == token)
            return true;
    }
    for (const char* token : kFurniture) {
        if (lower.find(token) != std::string::npos)
            return true;
    }

    // Single-word tokens that need word-boundary matching to avoid false
    // positives ("door" inside "commander", "gate" inside "gate circuit").
    // Check: token as a standalone word, or preceded/followed by space.
    static const char* kWord[] = {
        "bench", "chair", "desk", "door", "vent", "pipe",
        "fence", "panel", "button",
        "shelf", "drawer", "wardrobe", "cupboard",
        "sofa", "couch", "pillar", "column", "beam", "girder",
        "ceiling", "stair", "railing", "barricade",
        "duct", "lever", "valve", "switch",
        "antenna", "transmitter", "refrigerator", "fridge",
        "scatter", "backdrop",
    };
    for (const char* token : kWord) {
        const size_t tlen = strlen(token);
        size_t pos = 0;
        while ((pos = lower.find(token, pos)) != std::string::npos) {
            const bool beforeOk = (pos == 0) || (lower[pos - 1] == ' ');
            const size_t after = pos + tlen;
            const bool afterOk = (after >= lower.size()) || (lower[after] == ' ');
            if (beforeOk && afterOk)
                return true;
            ++pos;
        }
    }

    return false;
}

namespace {

void AppendUniqueLootPointer(std::vector<uintptr_t>& out, uintptr_t candidate)
{
    if (!candidate || !Memory::IsValidPtrFast2(candidate))
        return;
    if (!PointerIsLootInteractionComponent(candidate))
        return;
    for (const uintptr_t existing : out) {
        if (existing == candidate)
            return;
    }
    out.push_back(candidate);
}

bool ActorClassLooksLikeSimpleLootActivity(uintptr_t actor)
{
    std::string classFname = engine.GetActorClassFName(actor);
    if (classFname.empty())
        return false;

    return Contains(ToLowerLocal(classFname), "simplelootactivity");
}

bool FnameLooksLikeSimpleLootActivityContainer(const std::string& fnameLower)
{
    if (fnameLower.empty())
        return false;
    return Contains(fnameLower, "raidercache")
        || Contains(fnameLower, "raider_cache")
        || Contains(fnameLower, "bp_raidercache")
        || Contains(fnameLower, "cargoship")
        || Contains(fnameLower, "arc_cargo")
        || Contains(fnameLower, "arc_cargoship")
        || Contains(fnameLower, "simplelootactivity");
}

void CollectLootInteractionPointers(uintptr_t actor, const std::string& fnameLower,
    std::vector<uintptr_t>& out)
{
    out.clear();

    AppendUniqueLootPointer(out,
        Memory::read<uintptr_t>(actor + Offsets::LootInteractionComponent));
    AppendUniqueLootPointer(out,
        Memory::read<uintptr_t>(actor + Offsets::LootInteraction_Container));

    if (ActorClassLooksLikeSimpleLootActivity(actor)
        || FnameLooksLikeSimpleLootActivityContainer(fnameLower)) {
        AppendUniqueLootPointer(out,
            Memory::read<uintptr_t>(actor + Offsets::SimpleLootActivity_LootInteraction));
    }
}

bool ActorLooksLikeSalvageContainer(uintptr_t actor, const std::string& fnameLower)
{
    if (Contains(fnameLower, "salvagecontainer"))
        return true;
    const std::string classLower = ToLowerLocal(engine.GetActorClassFName(actor));
    return Contains(classLower, "salvagecontainer");
}

bool SalvageChosenMeshLooksOpened(uintptr_t actor, const std::string& fnameLower)
{
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return false;
    if (!IsRealSocketSalvageContainer(fnameLower, actor)
        && !SalvageContainerMeshVariantsLookValid(actor)
        && !ActorLooksLikeSalvageContainer(actor, fnameLower))
        return false;

    const int32_t chosen =
        Memory::read<int32_t>(actor + Offsets::SalvageContainer_ChosenMesh);
    return chosen == 2;
}

bool PointerIsItemContainerComponent(uintptr_t obj)
{
    if (!obj || !Memory::IsValidPtrFast2(obj))
        return false;

    const std::string lower = ToLowerLocal(engine.GetActorClassFName(obj));
    if (lower.empty())
        return false;

    return lower.find("itemcontainercomponent") != std::string::npos;
}

void AppendUniqueItemContainerPointer(std::vector<uintptr_t>& out, uintptr_t candidate)
{
    if (!PointerIsItemContainerComponent(candidate))
        return;
    for (const uintptr_t existing : out) {
        if (existing == candidate)
            return;
    }
    out.push_back(candidate);
}

bool ItemContainerPtrLooksOpened(uintptr_t containerComp)
{
    // OpenTime / alt@0x490 (DynamicLootType) caused room-wide false opens.
    // ItemContainer is not used as an open signal; keep helper for debug callers.
    (void)containerComp;
    return false;
}

void CollectActorItemContainerPointers(uintptr_t actor, std::vector<uintptr_t>& out)
{
    out.clear();

    AppendUniqueItemContainerPointer(out,
        Memory::read<uintptr_t>(actor + Offsets::LootContainer_ItemContainer));
    AppendUniqueItemContainerPointer(out,
        Memory::read<uintptr_t>(actor + Offsets::SimpleLootActivity_ItemContainer));

    const TArrayIntLocal instance =
        Memory::read<TArrayIntLocal>(actor + Offsets::Actor_InstanceComponents);
    if (instance.Num > 0 && instance.Num <= 48
        && instance.Max >= instance.Num && instance.Max <= 64
        && instance.Data && Memory::IsValidPtrFast2(instance.Data)) {
        for (int32_t i = 0; i < instance.Num; ++i) {
            AppendUniqueItemContainerPointer(out,
                Memory::read<uintptr_t>(
                    instance.Data + static_cast<uintptr_t>(i) * sizeof(uintptr_t)));
        }
    }
}

bool ActorItemContainerLooksOpened(uintptr_t actor)
{
    std::vector<uintptr_t> containers;
    CollectActorItemContainerPointers(actor, containers);
    for (const uintptr_t comp : containers) {
        if (ItemContainerPtrLooksOpened(comp))
            return true;
    }
    return false;
}

bool LootComponentLooksSearched(uintptr_t lootComp)
{
    if (!PointerIsLootInteractionComponent(lootComp))
        return false;

    // Offsets::LootInteraction_Searched == bHasBeenOpened @0x8A0 (mask 0x1).
    const uint8_t opened = Memory::read<uint8_t>(
        lootComp + static_cast<uint64_t>(Offsets::LootInteraction_Searched));
    return (opened & 0x1) != 0;
}

bool ActorLooksHiddenOrDestroyed(uintptr_t actor)
{
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return true;

    const uint8_t hiddenFlags =
        Memory::read<uint8_t>(actor + static_cast<uint64_t>(Offsets::Actor_bHiddenByte));
    if ((hiddenFlags & Offsets::Actor_bHiddenMask) != 0)
        return true;

    const uint8_t ddFlags =
        Memory::read<uint8_t>(actor + static_cast<uint64_t>(Offsets::Actor_FlagsDd));
    return (ddFlags & Offsets::Actor_bActorIsBeingDestroyedMask) != 0;
}

} // namespace

ContainerOpenSignal ProbeContainerOpenSignals(uintptr_t actor, const std::string& fnameHint)
{
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return ContainerOpenSignal::None;

    std::string fnameLower = ToLowerLocal(fnameHint);
    if (fnameLower.empty()) {
        std::string fname = engine.GetActorFNameStringCached(actor);
        if (fname.empty())
            fname = engine.GetActorFNameString(actor);
        fnameLower = ToLowerLocal(fname);
    }

    if (FnameExcludedFromContainerEsp(fnameLower))
        return ContainerOpenSignal::None;

    // Help SDK: LootContainerSingle::LootInteraction@0xB58,
    // LootInteractionComponent::bHasBeenOpened@0x8A0 mask=0x1.
    // Pointers read FROM the actor's own fields are already owned — do not
    // require OuterPrivate@0x20 (that gate silently blocked opens for minutes
    // while the shell stayed labeled as a closed crate).
    auto fieldLiOpened = [](uintptr_t li) -> bool {
        if (!li || !Memory::IsValidPtrFast2(li))
            return false;
        const uint8_t openedByte = Memory::read<uint8_t>(
            li + static_cast<uint64_t>(Offsets::LootInteraction_Searched));
        return (openedByte & 0x1) != 0;
    };
    if (fieldLiOpened(Memory::read<uintptr_t>(
            actor + static_cast<uint64_t>(Offsets::LootInteractionComponent)))
        || fieldLiOpened(Memory::read<uintptr_t>(
            actor + static_cast<uint64_t>(Offsets::LootInteraction_Container)))
        || fieldLiOpened(Memory::read<uintptr_t>(
            actor + static_cast<uint64_t>(Offsets::SimpleLootActivity_LootInteraction))))
        return ContainerOpenSignal::LootSearched;

    if (SalvageChosenMeshLooksOpened(actor, fnameLower))
        return ContainerOpenSignal::SalvageMesh;

    // Do not use ItemContainer OpenTime (SDK types it as FName; float reads FPed).

    std::vector<uintptr_t> lootPointers;
    CollectLootInteractionPointers(actor, fnameLower, lootPointers);
    for (const uintptr_t lootComp : lootPointers) {
        if (LootComponentLooksSearched(lootComp))
            return ContainerOpenSignal::LootSearched;
    }

    return ContainerOpenSignal::None;
}

bool ContainerLootLooksOpened(uintptr_t actor, const std::string& fnameHint)
{
    // Strong only: owned LootInteraction bHasBeenOpened. SalvageMesh / weak probes
    // false-positive closed crates → admit skip, Finalize Drawing=false, deplete erase
    // (labels missing for ~30s while standing on them).
    return ProbeContainerOpenSignals(actor, fnameHint) == ContainerOpenSignal::LootSearched;
}

bool ContainerLootLooksOpenedAny(uintptr_t actor, const std::string& fnameHint)
{
    return ProbeContainerOpenSignals(actor, fnameHint) != ContainerOpenSignal::None;
}

bool GroundLootPickupHasStrongSignal(GroundLootPickupSignal sig)
{
    using S = GroundLootPickupSignal;
    // Console proof (post deplete “weak-pair” removal): still admitted==depleted
    // every tick. StillHasAssetId|NoCollision false-positives on LIVE floor shells
    // when Actor_FlagsDd collision bit is unread/wrong. Only treat destroyed/hidden.
    if ((sig & S::HiddenOrDestroyed) != S::None)
        return true;
    (void)sig;
    return false;
}

GroundLootPickupSignal ProbeGroundLootPickupSignals(
    uintptr_t actor, const std::string& fnameHint)
{
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return GroundLootPickupSignal::HiddenOrDestroyed;

    GroundLootPickupSignal sig = GroundLootPickupSignal::None;

    if (ActorLooksHiddenOrDestroyed(actor))
        sig = sig | GroundLootPickupSignal::HiddenOrDestroyed;

    // After pickup the actor shell often stays for 1–2 minutes before GC, but
    // RootComponent / RootCollider flip bHiddenInGame immediately (syringe case).
    {
        auto sceneHiddenInGame = [](uintptr_t comp) -> bool {
            if (!comp || !Memory::IsValidPtrFast2(comp))
                return false;
            const uint8_t flags = Memory::read<uint8_t>(
                comp + static_cast<uint64_t>(Offsets::Scene_bHiddenInGameByte));
            return (flags & Offsets::Scene_bHiddenInGameMask) != 0;
        };
        const uintptr_t root =
            Memory::read<uintptr_t>(actor + static_cast<uint64_t>(Offsets::RootComponent));
        const uintptr_t collider =
            Memory::read<uintptr_t>(
                actor + static_cast<uint64_t>(Offsets::Pickup_RootCollider));
        if (sceneHiddenInGame(root) || sceneHiddenInGame(collider))
            sig = sig | GroundLootPickupSignal::HiddenOrDestroyed;
    }

    std::string fname = fnameHint;
    if (fname.empty())
        fname = engine.GetActorFNameStringCached(actor);
    if (fname.empty())
        fname = engine.GetActorFNameString(actor);

    // Do not collect container LootInteraction / bHasBeenOpened for pickups.

    const uint64_t itemDaPrimary =
        Memory::read<uint64_t>(actor + static_cast<uint64_t>(Offsets::ItemDataAsset));
    const uint64_t itemDaPickup =
        Memory::read<uint64_t>(actor + static_cast<uint64_t>(Offsets::Pickup_DefaultPickupDataAsset));
    const bool hasItemDa =
        (itemDaPrimary != 0 && Memory::IsValidPtrFast2(itemDaPrimary))
        || (itemDaPickup != 0 && Memory::IsValidPtrFast2(itemDaPickup));
    if (!hasItemDa)
        sig = sig | GroundLootPickupSignal::NoItemDa;

    const uint8_t ddFlags =
        Memory::read<uint8_t>(actor + static_cast<uint64_t>(Offsets::Actor_FlagsDd));
    if ((ddFlags & Offsets::Actor_bActorEnableCollisionMask) == 0)
        sig = sig | GroundLootPickupSignal::NoCollision;

    const std::string classFname = engine.GetActorClassFName(actor);
    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    const bool pickupLike = FnameLooksLikeDroppedPickup(fname)
        || FnameLooksLikeDroppedPickup(classFname)
        || ArcActorType::IsGroundLootClassId(masked);

    if (pickupLike) {
        struct TArrayIntLocal {
            uintptr_t Data = 0;
            int32_t Num = 0;
            int32_t Max = 0;
        };
        const TArrayIntLocal spawnItems =
            Memory::read<TArrayIntLocal>(
                actor + static_cast<uint64_t>(Offsets::BP_PickupBase_SpawnItems));
        if (spawnItems.Num == 0 && hasItemDa)
            sig = sig | GroundLootPickupSignal::SpawnItemsEmpty;
    }

    if (TryReadItemGameAssetIdFromActor(actor) != 0)
        sig = sig | GroundLootPickupSignal::StillHasAssetId;

    return sig;
}

bool GroundLootLooksPickedUp(uintptr_t actor, const std::string& fnameHint)
{
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return true;

    // Use strong-signal probe only. Returning true on missing ItemDataAsset or
    // empty SpawnItems wiped all live floor loot (ItemDataAsset often unset;
    // SpawnItems empty while asset id still present).
    const GroundLootPickupSignal sig =
        ProbeGroundLootPickupSignals(actor, fnameHint);
    return GroundLootPickupHasStrongSignal(sig);
}

namespace {
std::mutex g_pickupGoneMu;
std::unordered_set<uintptr_t> g_pickupGoneSticky;
} // namespace

void ClearGroundLootPickupStickyState()
{
    std::lock_guard<std::mutex> lock(g_pickupGoneMu);
    g_pickupGoneSticky.clear();
}

void MarkGroundPickupGoneSticky(uintptr_t actor)
{
    if (!actor)
        return;
    std::lock_guard<std::mutex> lock(g_pickupGoneMu);
    g_pickupGoneSticky.insert(actor);
}

bool IsGroundPickupGoneSticky(uintptr_t actor)
{
    if (!actor)
        return false;
    std::lock_guard<std::mutex> lock(g_pickupGoneMu);
    return g_pickupGoneSticky.contains(actor);
}

bool WorldLootCacheEntryDepleted(
    uintptr_t actor,
    const std::string& fnameHint,
    WorldItemCategory cat)
{
    if (!actor)
        return true;

    if (WorldCategoryIsContainerProp(cat) || cat == WorldItemCategory::OpenedContainer) {
        // Opened ≠ depleted. Erasing here removed crates from cache so they never
        // remapped to menu "Open Container" and kept showing closed for minutes.
        (void)fnameHint;
        return false;
    }

    if (cat == WorldItemCategory::DroppedPickup
        || cat == WorldItemCategory::Harvestable
        || cat == WorldItemCategory::Items
        || FnameLooksLikeDroppedPickup(fnameHint)
        || FnameLooksLikeHarvestableActor(fnameHint)) {
        return GroundLootLooksPickedUp(actor, fnameHint);
    }

    return false;
}

std::string ResolveContainerEspDisplayName(uintptr_t actor, const std::string& fnameHint)
{
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return {};

    auto labelFromBlueprintFname = [](const std::string& blueprintFname) -> std::string {
        if (blueprintFname.empty() || FnameLooksLikeEngineSubobjectClass(blueprintFname))
            return {};
        if (const std::string fromLabel = ResolveContainerDisplayLabel(blueprintFname, {});
            !fromLabel.empty() && !IsGenericWorldEspLabel(fromLabel))
            return fromLabel;
        if (const std::string fromSocket = ResolveSocketContainerDisplayName(blueprintFname);
            !fromSocket.empty() && !IsGenericWorldEspLabel(fromSocket))
            return fromSocket;
        if (const std::string human = HumanizeActorFName(blueprintFname);
            !human.empty() && !IsGenericWorldEspLabel(human)
            && !IsJunkWorldEspLabel(human) && IsPlausibleEspLabel(human))
            return human;
        return {};
    };

    if (const std::string mem = engine.GetEnglishItemName(actor);
        !mem.empty() && !IsGenericWorldEspLabel(mem) && IsPlausibleEspLabel(mem))
        return mem;

    std::string actorFname = engine.GetActorFNameStringCached(actor);
    if (actorFname.empty())
        actorFname = engine.GetActorFNameString(actor);
    if (actorFname.empty() || FnameLooksLikeEngineSubobjectClass(actorFname))
        actorFname = fnameHint;
    if (const std::string fromActor = labelFromBlueprintFname(actorFname); !fromActor.empty())
        return fromActor;

    if (const std::string dataAsset = GetActorDataAssetFName(actor); !dataAsset.empty()) {
        if (const std::string fromDa = labelFromBlueprintFname(dataAsset); !fromDa.empty())
            return fromDa;
    }

    const std::string classFname = engine.GetActorClassFName(actor);
    if (!FnameLooksLikeEngineSubobjectClass(classFname)) {
        if (const std::string fromClass = labelFromBlueprintFname(classFname); !fromClass.empty())
            return fromClass;
    }

    return ResolveWorldDisplayLabel(actor, actorFname.empty() ? fnameHint : actorFname, 0);
}

WorldItemCategory InferLootWorldCategory(
    const std::string& fnameLower,
    const std::string& displayLower,
    const std::string& bucketType,
    bool hasLootInteraction,
    bool classGroundLoot,
    bool classChest)
{
    const bool containerSignal =
        classChest
        || IsRealSocketSalvageContainer(fnameLower, 0)
        || DisplayLooksLikeWorldContainer(displayLower)
        || IsWorldPropLootContainer(
            hasLootInteraction, classChest, fnameLower, displayLower);

    if (bucketType == "Corpse")
        return WorldItemCategory::Corpse;
    if (bucketType == "Raider stock")
        return WorldItemCategory::RaiderCache;
    if (bucketType == "Arc Cargoship")
        return WorldItemCategory::ArcCargoship;

    if (hasLootInteraction && !FnameLooksLikeDroppedPickup(fnameLower)) {
        if (FnameLooksLikeHarvestableActor(fnameLower))
            return WorldItemCategory::Harvestable;
        const WorldItemCategory direct =
            ClassifyWorldActor(fnameLower, displayLower);
        if (direct != WorldItemCategory::Invalid
            && direct != WorldItemCategory::DroppedPickup
            && direct != WorldItemCategory::Items)
            return direct;
        return WorldItemCategory::Other;
    }

    if (classGroundLoot && !containerSignal) {
        if (FnameLooksLikeDroppedPickup(fnameLower))
            return WorldItemCategory::DroppedPickup;
        return WorldItemCategory::Invalid;
    }

    WorldItemCategory cat = ClassifyWorldActor(fnameLower, displayLower);
    if (cat == WorldItemCategory::Invalid && !displayLower.empty())
        cat = ClassifyWorldActor(std::string{}, displayLower);
    if (cat != WorldItemCategory::Invalid && cat != WorldItemCategory::Items
        && !(cat == WorldItemCategory::DroppedPickup && containerSignal))
        return cat;

    if (cat == WorldItemCategory::DroppedPickup && containerSignal)
        return WorldItemCategory::Other;

    if (!fnameLower.empty() && FnameLooksLikeDroppedPickup(fnameLower) && !containerSignal)
        return WorldItemCategory::DroppedPickup;

    if (containerSignal) {
        cat = ClassifyWorldActor(fnameLower, displayLower);
        if (cat == WorldItemCategory::Invalid)
            cat = ClassifyWorldActor(std::string{}, displayLower);
        if (cat != WorldItemCategory::Invalid && cat != WorldItemCategory::Items
            && cat != WorldItemCategory::DroppedPickup)
            return cat;
        return WorldItemCategory::Other;
    }

    if (bucketType == "Loot Item" || bucketType == "World Item") {
        if (!fnameLower.empty() && FnameLooksLikeDroppedPickup(fnameLower))
            return WorldItemCategory::DroppedPickup;

        if (!fnameLower.empty()) {
            cat = ClassifyWorldActor(fnameLower, displayLower);
            if (cat == WorldItemCategory::Invalid)
                cat = ClassifyWorldActor(fnameLower, std::string{});
            if (cat != WorldItemCategory::Invalid && cat != WorldItemCategory::Items)
                return cat;
        }

        if (hasLootInteraction && !fnameLower.empty()
            && FnameLooksLikeWorldContainer(fnameLower)) {
            cat = ClassifyWorldActor(fnameLower, displayLower);
            if (cat != WorldItemCategory::Invalid && cat != WorldItemCategory::Items)
                return cat;
        }

        if (hasLootInteraction && DisplayLooksLikeWorldContainer(displayLower)
            && !FnameLooksLikeDroppedPickup(fnameLower)) {
            cat = ClassifyWorldActor(fnameLower, displayLower);
            if (cat == WorldItemCategory::Invalid)
                cat = ClassifyWorldActor(std::string{}, displayLower);
            if (cat != WorldItemCategory::Invalid && cat != WorldItemCategory::Items)
                return cat;
            return WorldItemCategory::Other;
        }

        if (!fnameLower.empty()
            && (fnameLower.find("pickup") != std::string::npos
                || fnameLower.find("da_item") != std::string::npos
                || fnameLower.find("wid_") != std::string::npos))
            return WorldItemCategory::DroppedPickup;

        return WorldItemCategory::Invalid;
    }

    return WorldItemCategory::Invalid;
}

uint32_t WorldCategoryLabelColor(WorldItemCategory cat)
{
    switch (cat) {
    case WorldItemCategory::DroppedPickup:
        return EspDraw::ColorFromRGBA(var::color_dropped_items);
    case WorldItemCategory::Items:
        return EspDraw::ColorFromRGBA(var::color_world_items);
    case WorldItemCategory::Ammo:
        return EspDraw::ColorFromRGBA(var::color_world_ammo);
    case WorldItemCategory::ArcLoot:
        return EspDraw::ColorFromRGBA(var::color_world_arc_loot);
    case WorldItemCategory::Backpack:
        return EspDraw::ColorFromRGBA(var::color_world_backpack);
    case WorldItemCategory::Crate:
        return EspDraw::ColorFromRGBA(var::color_world_crate);
    case WorldItemCategory::Furniture:
        return EspDraw::ColorFromRGBA(var::color_world_furniture);
    case WorldItemCategory::Grenade:
        return EspDraw::ColorFromRGBA(var::color_world_grenade);
    case WorldItemCategory::Harvestable:
        return EspDraw::ColorFromRGBA(var::color_world_harvestable);
    case WorldItemCategory::Industrial:
        return EspDraw::ColorFromRGBA(var::color_world_industrial);
    case WorldItemCategory::Medical:
        return EspDraw::ColorFromRGBA(var::color_world_medical);
    case WorldItemCategory::Other:
        return EspDraw::ColorFromRGBA(var::color_world_other);
    case WorldItemCategory::Probe:
        return EspDraw::ColorFromRGBA(var::color_world_probe);
    case WorldItemCategory::RaiderCache:
        return EspDraw::ColorFromRGBA(var::color_raider_stock);
    case WorldItemCategory::Vehicles:
        return EspDraw::ColorFromRGBA(var::color_world_vehicles);
    case WorldItemCategory::WeaponCase:
        return EspDraw::ColorFromRGBA(var::color_world_weapon_case);
    case WorldItemCategory::FieldCrate:
        return EspDraw::ColorFromRGBA(var::color_world_field_crate);
    case WorldItemCategory::SupplyCallStation:
        return EspDraw::ColorFromRGBA(var::color_world_supply_station);
    case WorldItemCategory::Corpse:
        return EspDraw::ColorFromRGBA(var::color_world_corpses);
    case WorldItemCategory::RaiderStock:
        return EspDraw::ColorFromRGBA(var::color_raider_stock);
    case WorldItemCategory::ArcCargoship:
        return EspDraw::ColorFromRGBA(var::color_arc_entities);
    case WorldItemCategory::Keys:
        return EspDraw::ColorFromRGBA(var::color_world_keys);
    case WorldItemCategory::Locker:
        return EspDraw::ColorFromRGBA(var::color_world_locker);
    case WorldItemCategory::Trash:
        return EspDraw::ColorFromRGBA(var::color_world_trash);
    case WorldItemCategory::Safe:
        return EspDraw::ColorFromRGBA(var::color_world_safe);
    case WorldItemCategory::Buried:
        return EspDraw::ColorFromRGBA(var::color_world_buried);
    case WorldItemCategory::DeadDrop:
        return EspDraw::ColorFromRGBA(var::color_world_deaddrop);
    case WorldItemCategory::OpenedContainer:
        return EspDraw::ColorFromRGBA(var::color_world_open_container);
    default:
        return EspDraw::ColorFromRGBA(var::color_loot);
    }
}

uint32_t RarityTierColor(int lootTier)
{
    switch (lootTier) {
    case 1:
        return IM_COL32(180, 180, 180, 255);
    case 2:
        return IM_COL32(100, 200, 100, 255);
    case 3:
        return IM_COL32(80, 140, 220, 255);
    case 4:
        return IM_COL32(160, 80, 200, 255);
    case 5:
        return IM_COL32(220, 180, 60, 255);
    default:
        return EspDraw::ColorFromRGBA(var::color_loot);
    }
}

uint32_t WorldLootLabelColor(WorldItemCategory cat, int lootTier, bool isPickup)
{
    if (isPickup) {
        if (var::loot_rarity_color && lootTier > 0)
            return RarityTierColor(lootTier);
        return EspDraw::ColorFromRGBA(var::color_loot);
    }
    return WorldCategoryLabelColor(cat);
}

bool WorldCategoryVisibleOnRadar(const WorldLootFilterView& loot)
{
    if (!var::show_radar || !var::enable_world)
        return false;

    const auto cat = static_cast<WorldItemCategory>(loot.worldCategory);
    if (LootItemLooksLikePickup(loot)) {
        // Radar combo: 0=Rare+, 1=Epic+, 2=Legendary → tiers 3..5 (see DrawArcRadarTab).
        const int radarLootMinTier = var::radar_loot_min_rarity + 3;
        if (loot.lootRarityTier <= 0 || loot.lootRarityTier < radarLootMinTier)
            return false;
        if (var::loot_min_value > 0.f) {
            if (loot.lootValue <= 0
                || static_cast<float>(loot.lootValue) < var::loot_min_value)
                return false;
        }
        return true;
    }

    if (!var::show_radar_special)
        return false;
    if (!WorldCategoryHasSpConfig(cat))
        return false;
    // Radar special = only Loot-tab rows with SP checked, not every enabled container type.
    return WorldCategoryUsesSpContainerRange(cat);
}

bool WorldCategoryEnabled(int category)
{
    const auto cat = static_cast<WorldItemCategory>(category);
    if (var::show_radar && var::show_radar_special
        && WorldCategoryHasSpConfig(cat)
        && WorldCategoryUsesSpContainerRange(cat)) {
        return true;
    }
    // showLoot is the master draw switch; category toggles filter while it is on.
    if (!var::showLoot)
        return false;
    switch (cat) {
    case WorldItemCategory::DroppedPickup:
        return var::droppedItems || var::show_world_items;
    case WorldItemCategory::Items:
        return var::show_world_items || var::droppedItems;
    case WorldItemCategory::Ammo:
        return var::show_world_ammo;
    case WorldItemCategory::ArcLoot:
        return var::show_world_arc_loot;
    case WorldItemCategory::Backpack:
        return var::show_world_backpack;
    case WorldItemCategory::Crate:
        return var::show_world_crate;
    case WorldItemCategory::Furniture:
        return var::show_world_furniture;
    case WorldItemCategory::Grenade:
        return var::show_world_grenade;
    case WorldItemCategory::Harvestable:
        // Items/Dropped toggles also cover world plants so Prickly Pear etc.
        // aren't invisible when only loot/items ESP is on.
        return var::show_world_harvestable || var::droppedItems
            || var::show_world_items;
    case WorldItemCategory::Industrial:
        return var::show_world_industrial;
    case WorldItemCategory::Medical:
        return var::show_world_medical;
    case WorldItemCategory::Other:
        return var::show_world_other;
    case WorldItemCategory::Probe:
        return var::show_world_probe;
    case WorldItemCategory::RaiderCache:
        return var::raiderStock;
    case WorldItemCategory::Vehicles:
        return var::show_world_vehicles;
    case WorldItemCategory::WeaponCase:
        return var::show_world_weapon_case;
    case WorldItemCategory::FieldCrate:
        return var::show_world_field_crate;
    case WorldItemCategory::SupplyCallStation:
        return var::show_world_supply_station;
    case WorldItemCategory::Corpse:
        return var::showDeadPlayers;
    case WorldItemCategory::RaiderStock:
        return var::raiderStock;
    case WorldItemCategory::ArcCargoship:
        return var::showArc;
    case WorldItemCategory::Keys:
        return var::show_world_keys || var::droppedItems;
    case WorldItemCategory::Locker:
        return var::show_world_locker;
    case WorldItemCategory::Trash:
        return var::show_world_trash;
    case WorldItemCategory::Safe:
        return var::show_world_safe;
    case WorldItemCategory::Buried:
        return var::show_world_buried;
    case WorldItemCategory::DeadDrop:
        return var::show_world_deaddrop;
    case WorldItemCategory::OpenedContainer:
        return var::show_world_open_container;
    default:
        return false;
    }
}
