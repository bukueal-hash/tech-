#include "AssetNames.h"
#include "BotTypes.h"
#include "WorldItemCategory.h"
#include "Offsets.h"

#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/SteamDecrypt.hpp"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

std::string ToLowerCopy(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

namespace {

std::unordered_map<std::string, std::string> g_assetByName;
std::unordered_map<std::string, std::string> g_locByKey;
struct ItemMeta {
    int tier = 0;
    int value = 0;
};
std::unordered_map<std::string, ItemMeta> g_metaByNorm;
std::unordered_map<std::string, ItemMeta> g_metaByPlain;
std::unordered_map<std::string, ItemMeta> g_metaById;
std::unordered_map<std::string, ItemMeta> g_metaByAssetName;
std::unordered_map<std::string, std::string> g_assetIdToName;
std::unordered_map<std::string, std::string> g_assetIdToDisplay;
std::unordered_map<std::string, std::string> g_nameToAssetId;

struct WorldObjPattern {
    std::string token;
    std::string display;
};
std::vector<WorldObjPattern> g_worldObjPatterns;

struct FnameAssetPattern {
    std::string token;
    std::string display;
};
std::vector<FnameAssetPattern> g_fnameAssetPatterns;

struct AssetWorldPropPattern {
    std::string token;
    std::string display;
    std::string category;
};
std::vector<AssetWorldPropPattern> g_assetWorldCategoryPatterns;

struct EnemyBotPattern {
    std::string token;
    std::string robotType;
};
std::vector<EnemyBotPattern> g_enemyBotPatterns;
std::unordered_map<std::string, std::string> g_mapsById;
std::unordered_map<std::string, std::string> g_botClassTokenMap;

std::string NormalizePlainKey(std::string s);
bool IsBlockedFnameToken(const std::string& token);

static void TryRegisterEnemyBotToken(const std::string& token, const std::string& robotType)
{
    if (token.size() < 3 || robotType.empty() || IsBlockedFnameToken(token))
        return;

    for (const EnemyBotPattern& existing : g_enemyBotPatterns) {
        if (existing.token == token)
            return;
    }
    g_enemyBotPatterns.push_back({ token, robotType });
}

static bool IsSpecificBotClassToken(const std::string& classToken)
{
    if (classToken.empty() || classToken[0] == '_')
        return false;
    // Prefixed UE tokens (C_, Husk_, GC_, …) are safe; bare words like
    // "Heavy"/"Husk" substring-match too many unrelated actor fnames.
    if (classToken.find('_') != std::string::npos)
        return true;
    return classToken.size() >= 10;
}

static void RegisterBotClassToken(const std::string& classToken, const std::string& robotType)
{
    // Never register NormalizeBotDisplayName aliases (Heavy, Elite, Drone, …) —
    // bare tokens substring-match unrelated actors and flood scanners.
    if (classToken.size() < 3 || robotType.empty() || !IsSpecificBotClassToken(classToken))
        return;

    const std::string key = NormalizePlainKey(classToken);
    g_botClassTokenMap[key] = robotType;

    std::string stripped = classToken;
    while (!stripped.empty() && stripped.back() == '_')
        stripped.pop_back();
    if (stripped != classToken && stripped.size() >= 3)
        g_botClassTokenMap[NormalizePlainKey(stripped)] = robotType;

    TryRegisterEnemyBotToken(key, robotType);
    if (stripped != classToken && stripped.size() >= 3)
        TryRegisterEnemyBotToken(NormalizePlainKey(stripped), robotType);
}

static void RegisterEnemyBotFromAssetName(const std::string& assetName, const std::string& displayName)
{
    if (assetName.find("EnemyType") == std::string::npos)
        return;

    const std::string robotType = NormalizeBotDisplayName(displayName);
    if (robotType.empty())
        return;

    std::string probe = assetName;
    for (const char* prefix : { "DA_", "WID_", "BP_", "Item_" }) {
        const size_t plen = strlen(prefix);
        if (probe.size() > plen && probe.compare(0, plen, prefix) == 0) {
            probe = probe.substr(plen);
            break;
        }
    }

    TryRegisterEnemyBotToken(NormalizePlainKey(probe), robotType);
    TryRegisterEnemyBotToken(NormalizePlainKey(assetName), robotType);

    std::vector<std::string> parts;
    parts.reserve(8);
    {
        std::string segment;
        for (char ch : assetName) {
            if (ch == '_') {
                if (!segment.empty()) {
                    parts.push_back(segment);
                    segment.clear();
                }
            } else {
                segment.push_back(ch);
            }
        }
        if (!segment.empty())
            parts.push_back(segment);
    }

    for (const std::string& part : parts) {
        const std::string tok = NormalizePlainKey(part);
        if (tok.size() >= 3)
            TryRegisterEnemyBotToken(tok, robotType);
    }

    if (parts.size() >= 2) {
        TryRegisterEnemyBotToken(
            NormalizePlainKey(parts[parts.size() - 2] + parts[parts.size() - 1]),
            robotType);
    }
    if (parts.size() >= 3) {
        TryRegisterEnemyBotToken(
            NormalizePlainKey(parts[parts.size() - 3] + parts[parts.size() - 2] + parts[parts.size() - 1]),
            robotType);
    }
}

void FinalizeEnemyBotPatterns()
{
    std::sort(g_enemyBotPatterns.begin(), g_enemyBotPatterns.end(),
        [](const EnemyBotPattern& a, const EnemyBotPattern& b) {
            return a.token.size() > b.token.size();
        });
}

static void RegisterEnemyBotPattern(const std::string& upperKey, const std::string& display)
{
    if (upperKey.size() < 10 || display.empty())
        return;

    std::string token = upperKey;
    if (token.rfind("ID_ENEMY_", 0) != 0)
        return;
    token.erase(0, 9);
    const std::string suffix = "_NAME";
    if (token.size() > suffix.size()
        && token.compare(token.size() - suffix.size(), suffix.size(), suffix) == 0) {
        token.erase(token.size() - suffix.size());
    }
    token = NormalizePlainKey(std::move(token));
    if (token.size() < 3)
        return;

    const std::string robotType = NormalizeBotDisplayName(display);
    for (const EnemyBotPattern& existing : g_enemyBotPatterns) {
        if (existing.token == token)
            return;
    }
    g_enemyBotPatterns.push_back({ token, robotType });
}

bool IsBlockedFnameToken(const std::string& token)
{
    static const char* kBlocked[] = {
        "item", "loot", "ammo", "blue", "common", "default", "none", "null",
        "test", "data", "asset", "type", "base", "world", "object", "pickup",
    };
    for (const char* blocked : kBlocked) {
        if (token == blocked)
            return true;
    }
    return false;
}

void TryRegisterFnameToken(const std::string& token, const std::string& displayName)
{
    if (token.size() < 4 || displayName.empty() || IsBlockedFnameToken(token))
        return;
    for (const FnameAssetPattern& existing : g_fnameAssetPatterns) {
        if (existing.token == token)
            return;
    }
    g_fnameAssetPatterns.push_back({ token, displayName });
}

void RegisterFnameAssetPattern(const std::string& assetName, const std::string& displayName)
{
    if (assetName.size() < 4 || displayName.empty())
        return;

    std::string probe = assetName;
    for (const char* prefix : { "DA_", "WID_", "BP_", "Item_" }) {
        const size_t plen = strlen(prefix);
        if (probe.size() > plen && probe.compare(0, plen, prefix) == 0) {
            probe = probe.substr(plen);
            break;
        }
    }

    TryRegisterFnameToken(NormalizePlainKey(probe), displayName);

    std::string dispPlain = NormalizePlainKey(displayName);
    if (dispPlain.size() >= 5)
        TryRegisterFnameToken(dispPlain, displayName);
    dispPlain.erase(
        std::remove_if(dispPlain.begin(), dispPlain.end(), [](unsigned char c) { return c == ' '; }),
        dispPlain.end());
    if (dispPlain.size() >= 5)
        TryRegisterFnameToken(dispPlain, displayName);

    std::vector<std::string> parts;
    parts.reserve(8);
    {
        std::string segment;
        for (char ch : assetName) {
            if (ch == '_') {
                if (!segment.empty()) {
                    parts.push_back(segment);
                    segment.clear();
                }
            } else {
                segment.push_back(ch);
            }
        }
        if (!segment.empty())
            parts.push_back(segment);
    }

    for (const std::string& part : parts) {
        if (NormalizePlainKey(part).size() >= 5)
            TryRegisterFnameToken(NormalizePlainKey(part), displayName);
    }

    if (parts.size() >= 2) {
        const std::string tail2 =
            NormalizePlainKey(parts[parts.size() - 2] + parts[parts.size() - 1]);
        if (tail2.size() >= 5)
            TryRegisterFnameToken(tail2, displayName);
    }
    if (!parts.empty()) {
        const std::string tail1 = NormalizePlainKey(parts.back());
        if (tail1.size() >= 5)
            TryRegisterFnameToken(tail1, displayName);
    }
}

void FinalizeFnameAssetPatterns()
{
    std::sort(g_fnameAssetPatterns.begin(), g_fnameAssetPatterns.end(),
        [](const FnameAssetPattern& a, const FnameAssetPattern& b) {
            return a.token.size() > b.token.size();
        });
}

void FinalizeAssetWorldPropPatterns()
{
    std::sort(g_assetWorldCategoryPatterns.begin(), g_assetWorldCategoryPatterns.end(),
        [](const AssetWorldPropPattern& a, const AssetWorldPropPattern& b) {
            return a.token.size() > b.token.size();
        });
}

std::string ExeDirectory()
{
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
        return {};
    std::string dir(path);
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos)
        dir.resize(slash + 1);
    return dir;
}

std::string TrimCopy(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string CollapseSpaces(std::string s)
{
    std::string out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (unsigned char ch : s) {
        if (std::isspace(ch)) {
            if (!prevSpace && !out.empty()) {
                out.push_back(' ');
                prevSpace = true;
            }
        }
        else {
            out.push_back(static_cast<char>(ch));
            prevSpace = false;
        }
    }
    return TrimCopy(out);
}

std::string NormalizeNameKey(std::string s)
{
    s = CollapseSpaces(ToLowerCopy(TrimCopy(std::move(s))));
    return s;
}

std::string NormalizePlainKey(std::string s)
{
    s = NormalizeNameKey(std::move(s));
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
        return !std::isalnum(c) && c != ' ';
    }), s.end());
    s = CollapseSpaces(std::move(s));
    return s;
}

void RegisterLocEntry(const std::string& key, const std::string& value)
{
    if (key.empty() || value.empty())
        return;

    g_locByKey[key] = value;

    std::string upper = key;
    for (char& ch : upper)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    g_locByKey[upper] = value;

    if (upper.rfind("ID_ENEMY_", 0) == 0) {
        RegisterEnemyBotPattern(upper, value);
        return;
    }

    if (upper.find("ID_WORLDOBJECT") != 0)
        return;

    std::string token = upper;
    if (token.find("ID_WORLDOBJECTS_") == 0)
        token.erase(0, 16);
    else if (token.find("ID_WORLDOBJECT_") == 0)
        token.erase(0, 15);

    if (token.find("CONTAINER_") == 0)
        token.erase(0, 10);

    token = NormalizePlainKey(std::move(token));
    if (token.size() < 4)
        return;

    g_worldObjPatterns.push_back({ token, value });
}

void FinalizeWorldObjectPatterns()
{
    std::sort(g_worldObjPatterns.begin(), g_worldObjPatterns.end(),
        [](const WorldObjPattern& a, const WorldObjPattern& b) {
            return a.token.size() > b.token.size();
        });
}

int RarityToTier(const std::string& rarityRaw)
{
    const std::string rarity = ToLowerCopy(TrimCopy(rarityRaw));
    if (rarity == "common") return 1;
    if (rarity == "uncommon") return 2;
    if (rarity == "rare") return 3;
    if (rarity == "epic") return 4;
    if (rarity == "legendary") return 5;
    return 0;
}

std::string UnescapeJsonString(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 >= raw.size()) {
            out.push_back(raw[i]);
            continue;
        }
        const char next = raw[++i];
        switch (next) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(next); break;
        }
    }
    return out;
}

bool ReadFileText(const std::string& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::string> SplitCsvLine(const std::string& line)
{
    std::vector<std::string> cols;
    std::string cell;
    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (inQuotes && i + 1 < line.size() && line[i + 1] == '"') {
                cell.push_back('"');
                ++i;
            }
            else {
                inQuotes = !inQuotes;
            }
            continue;
        }
        if (ch == ',' && !inQuotes) {
            cols.push_back(cell);
            cell.clear();
            continue;
        }
        cell.push_back(ch);
    }
    cols.push_back(cell);
    return cols;
}

bool LoadAssetIndexCsv(const std::string& path)
{
    std::string text;
    if (!ReadFileText(path, text))
        return false;

    std::istringstream stream(text);
    std::string line;
    if (!std::getline(stream, line))
        return false;

    const auto header = SplitCsvLine(line);
    int assetIdCol = -1;
    int assetCol = -1;
    int displayCol = -1;
    int worldCategoryCol = -1;
    for (size_t i = 0; i < header.size(); ++i) {
        const std::string h = TrimCopy(header[i]);
        if (h == "asset_id") assetIdCol = static_cast<int>(i);
        else if (h == "asset_name") assetCol = static_cast<int>(i);
        else if (h == "display_name_en") displayCol = static_cast<int>(i);
        else if (h == "world_category") worldCategoryCol = static_cast<int>(i);
    }
    if (assetCol < 0)
        return false;

    size_t loaded = 0;
    while (std::getline(stream, line)) {
        if (line.empty())
            continue;
        const auto cols = SplitCsvLine(line);
        if (static_cast<int>(cols.size()) <= assetCol)
            continue;

        const std::string assetName = TrimCopy(cols[static_cast<size_t>(assetCol)]);
        if (assetName.empty())
            continue;

        std::string assetId;
        if (assetIdCol >= 0 && static_cast<int>(cols.size()) > assetIdCol)
            assetId = TrimCopy(cols[static_cast<size_t>(assetIdCol)]);
        if (!assetId.empty()) {
            g_assetIdToName[assetId] = assetName;
            g_nameToAssetId[assetName] = assetId;
        }

        std::string displayName;
        if (displayCol >= 0 && static_cast<int>(cols.size()) > displayCol)
            displayName = TrimCopy(cols[static_cast<size_t>(displayCol)]);
        if (!assetId.empty() && !displayName.empty())
            g_assetIdToDisplay[assetId] = displayName;

        std::string worldCategory;
        if (worldCategoryCol >= 0 && static_cast<int>(cols.size()) > worldCategoryCol)
            worldCategory = TrimCopy(cols[static_cast<size_t>(worldCategoryCol)]);

        if (!displayName.empty()) {
            g_assetByName[assetName] = displayName;
            RegisterFnameAssetPattern(assetName, displayName);
            RegisterEnemyBotFromAssetName(assetName, displayName);
        }

        if (!worldCategory.empty() && !displayName.empty()) {
            const std::string token = NormalizePlainKey(assetName);
            if (token.size() >= 4) {
                bool duplicate = false;
                for (const AssetWorldPropPattern& existing : g_assetWorldCategoryPatterns) {
                    if (existing.token == token) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    g_assetWorldCategoryPatterns.push_back({ token, displayName, worldCategory });
            }
        }
        ++loaded;
    }

    std::cout << "[AssetNames] asset_index.csv entries: " << loaded
        << " world_props=" << g_assetWorldCategoryPatterns.size() << std::endl;
    return loaded > 0;
}

bool LoadAssetLocalizationsCsv(const std::string& path)
{
    std::string text;
    if (!ReadFileText(path, text))
        return false;

    std::istringstream stream(text);
    std::string line;
    if (!std::getline(stream, line))
        return false;

    const auto header = SplitCsvLine(line);
    int assetIdCol = -1;
    int localeCol = -1;
    int displayCol = -1;
    for (size_t i = 0; i < header.size(); ++i) {
        const std::string h = TrimCopy(header[i]);
        if (h == "asset_id") assetIdCol = static_cast<int>(i);
        else if (h == "locale") localeCol = static_cast<int>(i);
        else if (h == "display_name") displayCol = static_cast<int>(i);
    }
    if (assetIdCol < 0 || localeCol < 0 || displayCol < 0)
        return false;

    size_t loaded = 0;
    while (std::getline(stream, line)) {
        if (line.empty())
            continue;
        const auto cols = SplitCsvLine(line);
        if (static_cast<int>(cols.size()) <= assetIdCol
            || static_cast<int>(cols.size()) <= localeCol
            || static_cast<int>(cols.size()) <= displayCol)
            continue;

        const std::string locale = TrimCopy(cols[static_cast<size_t>(localeCol)]);
        if (locale != "en")
            continue;

        const std::string assetId = TrimCopy(cols[static_cast<size_t>(assetIdCol)]);
        const std::string displayName = TrimCopy(cols[static_cast<size_t>(displayCol)]);
        if (assetId.empty() || displayName.empty())
            continue;

        const auto it = g_assetIdToName.find(assetId);
        if (it == g_assetIdToName.end())
            continue;

        const std::string& assetName = it->second;
        if (g_assetByName.find(assetName) == g_assetByName.end()) {
            g_assetByName[assetName] = displayName;
            RegisterFnameAssetPattern(assetName, displayName);
            RegisterEnemyBotFromAssetName(assetName, displayName);
            ++loaded;
        }
        g_assetIdToDisplay[assetId] = displayName;
    }

    std::cout << "[AssetNames] asset_localizations en fills: " << loaded << std::endl;
    return true;
}

void ParseKeysToEntriesBlock(const nlohmann::json& entries)
{
    if (!entries.is_object())
        return;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!it.value().is_string())
            continue;
        const std::string key = it.key();
        const std::string value = it.value().get<std::string>();
        if (!key.empty() && !value.empty())
            RegisterLocEntry(key, value);
    }
}

void WalkJsonForLocKeys(const nlohmann::json& node)
{
    if (node.is_object()) {
        if (node.contains("KeysToEntries"))
            ParseKeysToEntriesBlock(node["KeysToEntries"]);
        for (auto it = node.begin(); it != node.end(); ++it)
            WalkJsonForLocKeys(it.value());
    } else if (node.is_array()) {
        for (const auto& el : node)
            WalkJsonForLocKeys(el);
    }
}

bool LoadStJsonFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
        return false;

    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception& ex) {
        std::cout << "[AssetNames] JSON parse failed: " << path << " — " << ex.what() << std::endl;
        return false;
    }

    const size_t before = g_locByKey.size();
    WalkJsonForLocKeys(doc);
    const size_t added = g_locByKey.size() - before;
    if (added > 0)
        std::cout << "[AssetNames] " << path << " loc keys: " << added << std::endl;
    return added > 0;
}

void RegisterMetaEntry(
    const std::string& displayName,
    const std::string& metaId,
    const std::string& assetName,
    int tier,
    int value)
{
    if (tier <= 0)
        return;
    const ItemMeta meta{ tier, value };
    if (!displayName.empty()) {
        g_metaByNorm[NormalizeNameKey(displayName)] = meta;
        g_metaByPlain[NormalizePlainKey(displayName)] = meta;
    }
    if (!metaId.empty())
        g_metaById[NormalizePlainKey(metaId)] = meta;
    if (!assetName.empty())
        g_metaByAssetName[assetName] = meta;
}

bool LoadItemsMetaJson(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
        return false;

    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception& ex) {
        std::cout << "[AssetNames] JSON parse failed: " << path << " — " << ex.what() << std::endl;
        return false;
    }

    if (!doc.is_array())
        return false;

    size_t loaded = 0;
    for (const auto& row : doc) {
        if (!row.is_object())
            continue;

        const std::string name = row.value("name", std::string{});
        const std::string rarity = row.value("rarity", std::string{});
        int value = 0;
        if (row.contains("value") && row["value"].is_number())
            value = row["value"].get<int>();
        const std::string metaId = row.value("id", std::string{});
        const std::string assetName = row.value("asset", std::string{});
        if (name.empty())
            continue;

        const int tier = RarityToTier(rarity);
        if (tier <= 0)
            continue;

        RegisterMetaEntry(name, metaId, assetName, tier, value);
        ++loaded;
    }

    std::cout << "[AssetNames] items_meta entries: " << loaded << std::endl;
    return loaded > 0;
}

bool LoadBotsItemsMapsJson(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
        return false;

    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception& ex) {
        std::cout << "[AssetNames] JSON parse failed: " << path << " — " << ex.what() << std::endl;
        return false;
    }

    size_t itemsLoaded = 0;
    size_t arcLoaded = 0;
    size_t mapsLoaded = 0;

    if (doc.contains("items") && doc["items"].is_object()) {
        for (auto it = doc["items"].begin(); it != doc["items"].end(); ++it) {
            const std::string assetKey = it.key();
            if (assetKey.empty() || assetKey[0] == '_')
                continue;
            if (!it.value().is_object())
                continue;

            const std::string name = it.value().value("name", std::string{});
            const std::string rarity = it.value().value("rarity", std::string{});
            int value = 0;
            if (it.value().contains("value") && it.value()["value"].is_number())
                value = it.value()["value"].get<int>();
            if (name.empty())
                continue;

            g_assetByName[assetKey] = name;
            RegisterFnameAssetPattern(assetKey, name);
            const int tier = RarityToTier(rarity);
            if (tier > 0)
                RegisterMetaEntry(name, assetKey, assetKey, tier, value);
            ++itemsLoaded;
        }
    }

    if (doc.contains("arc") && doc["arc"].is_object()) {
        for (auto it = doc["arc"].begin(); it != doc["arc"].end(); ++it) {
            if (it.key().empty() || it.key()[0] == '_')
                continue;
            if (!it.value().is_string())
                continue;

            const std::string robotType = NormalizeBotDisplayName(it.value().get<std::string>());
            if (robotType.empty() || !IsRobotsListType(robotType))
                continue;

            RegisterBotClassToken(it.key(), robotType);
            ++arcLoaded;
        }
    }

    if (doc.contains("maps") && doc["maps"].is_object()) {
        for (auto it = doc["maps"].begin(); it != doc["maps"].end(); ++it) {
            if (!it.value().is_string())
                continue;
            g_mapsById[it.key()] = it.value().get<std::string>();
            ++mapsLoaded;
        }
    }

    std::cout << "[AssetNames] Bots_Items_Maps: items=" << itemsLoaded
        << " arc=" << arcLoaded << " maps=" << mapsLoaded << std::endl;
    return itemsLoaded > 0 || arcLoaded > 0 || mapsLoaded > 0;
}

std::string AssetNameToMetaId(std::string assetName)
{
    for (const char* prefix : {
             "DA_Item_Salvage_Trinket_",
             "DA_Item_Salvage_",
             "DA_Item_",
             "DA_OI_",
             "DA_",
             "WID_",
             "BP_",
         }) {
        const size_t plen = strlen(prefix);
        if (assetName.size() > plen && assetName.compare(0, plen, prefix) == 0) {
            assetName.erase(0, plen);
            break;
        }
    }

    std::string out;
    out.reserve(assetName.size());
    bool prevUnderscore = false;
    for (char ch : assetName) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
            prevUnderscore = false;
        } else if ((ch == '_' || ch == '-') && !prevUnderscore) {
            out.push_back('_');
            prevUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    return out;
}

void LinkMetaByDisplayToAssets()
{
    int tier = 0;
    int value = 0;
    size_t linked = 0;
    for (const auto& [assetName, display] : g_assetByName) {
        if (display.empty())
            continue;
        if (g_metaByAssetName.contains(assetName))
            continue;
        if (!LookupItemMeta(display, tier, value))
            continue;
        g_metaByAssetName[assetName] = ItemMeta{ tier, value };
        ++linked;
    }
    if (linked > 0)
        std::cout << "[AssetNames] asset meta aliases: " << linked << std::endl;
}

void LoadLocDirectory(const std::string& dir)
{
    WIN32_FIND_DATAA fd{};
    const std::string pattern = dir + "\\*.json";
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        LoadStJsonFile(dir + "\\" + fd.cFileName);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

} // namespace

std::string LookupBotClassToken(const std::string& actorFName)
{
    if (actorFName.empty() || g_botClassTokenMap.empty())
        return {};

    const std::string fnameNorm = NormalizePlainKey(actorFName);
    if (fnameNorm.empty())
        return {};

    // Separator-preserving key so short class tokens ("tick", "pop") can
    // word-boundary match — the min-5 substring rule blocked them entirely
    // (debug-c190fb: Ticks admitted with no label => no ESP).
    std::string fnameWords = ToLowerCopy(actorFName);
    for (char& c : fnameWords) {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = ' ';
    }
    fnameWords = CollapseSpaces(std::move(fnameWords));

    std::string best;
    size_t bestLen = 0;
    for (const auto& [token, robotType] : g_botClassTokenMap) {
        if (token.size() < 3 || token.size() <= bestLen)
            continue;
        bool hit = false;
        if (token.size() >= 5) {
            hit = fnameNorm.find(token) != std::string::npos;
        } else {
            size_t pos = 0;
            while ((pos = fnameWords.find(token, pos)) != std::string::npos) {
                const bool leftOk = (pos == 0)
                    || !std::isalnum(static_cast<unsigned char>(fnameWords[pos - 1]));
                const size_t end = pos + token.size();
                const bool rightOk = (end >= fnameWords.size())
                    || !std::isalnum(static_cast<unsigned char>(fnameWords[end]));
                if (leftOk && rightOk) {
                    hit = true;
                    break;
                }
                ++pos;
            }
        }
        if (!hit)
            continue;
        bestLen = token.size();
        best = robotType;
    }
    return best;
}

bool AssetNamesInit()
{
    try {
    g_assetByName.clear();
    g_locByKey.clear();
    g_metaByNorm.clear();
    g_metaByPlain.clear();
    g_metaById.clear();
    g_metaByAssetName.clear();
    g_assetIdToName.clear();
    g_assetIdToDisplay.clear();
    g_nameToAssetId.clear();
    g_worldObjPatterns.clear();
    g_fnameAssetPatterns.clear();
    g_assetWorldCategoryPatterns.clear();
    g_enemyBotPatterns.clear();
    g_mapsById.clear();
    g_botClassTokenMap.clear();

    const std::string dataDir = ExeDirectory() + "Data\\";
    const bool csvOk = LoadAssetIndexCsv(dataDir + "asset_index.csv");
    LoadAssetLocalizationsCsv(dataDir + "asset_localizations.csv");
    LoadLocDirectory(dataDir + "loc");
    const bool metaOk = LoadItemsMetaJson(dataDir + "items_meta.json");
    const bool botsItemsMapsOk =
        LoadBotsItemsMapsJson(dataDir + "Bots_Items_Maps\\en.json");
    LinkMetaByDisplayToAssets();
    FinalizeWorldObjectPatterns();
    FinalizeAssetWorldPropPatterns();
    FinalizeFnameAssetPatterns();
    FinalizeEnemyBotPatterns();

    std::cout << "[AssetNames] totals: assets=" << g_assetByName.size()
        << " loc=" << g_locByKey.size()
        << " meta=" << g_metaByNorm.size()
        << " worldObj=" << g_worldObjPatterns.size()
        << " worldProps=" << g_assetWorldCategoryPatterns.size()
        << " fnamePat=" << g_fnameAssetPatterns.size()
        << " enemyBot=" << g_enemyBotPatterns.size()
        << " maps=" << g_mapsById.size()
        << " (csv=" << (csvOk ? "ok" : "missing")
        << " meta=" << (metaOk ? "ok" : "missing")
        << " botsItemsMaps=" << (botsItemsMapsOk ? "ok" : "missing") << ")" << std::endl;

    return csvOk || !g_locByKey.empty() || metaOk || botsItemsMapsOk;
    } catch (const std::exception& ex) {
        std::cout << "[AssetNames] init failed: " << ex.what() << std::endl;
        return false;
    }
}

// SHARED GATE — grep callers before edit
std::string LookupByAssetName(const std::string& assetName)
{
    if (assetName.empty())
        return {};
    if (auto it = g_assetByName.find(assetName); it != g_assetByName.end())
        return it->second;

    for (const char* prefix : { "DA_", "WID_", "BP_", "Item_" }) {
        const size_t plen = strlen(prefix);
        if (assetName.size() > plen && assetName.compare(0, plen, prefix) == 0) {
            const std::string stripped = assetName.substr(plen);
            if (auto it2 = g_assetByName.find(stripped); it2 != g_assetByName.end())
                return it2->second;
            if (auto it3 = g_assetByName.find(prefix + stripped); it3 != g_assetByName.end())
                return it3->second;
        }
    }

    if (auto it = g_assetByName.find("DA_" + assetName); it != g_assetByName.end())
        return it->second;

    return {};
}

static std::string AssetIdToKey(int64_t assetId)
{
    return std::to_string(assetId);
}

std::string LookupDisplayByAssetId(int64_t assetId)
{
    if (assetId == 0)
        return {};

    const std::string key = AssetIdToKey(assetId);
    if (auto it = g_assetIdToDisplay.find(key); it != g_assetIdToDisplay.end())
        return it->second;

    if (auto nameIt = g_assetIdToName.find(key); nameIt != g_assetIdToName.end()) {
        if (const std::string fromName = LookupByAssetName(nameIt->second); !fromName.empty())
            return fromName;
    }

    return {};
}

int64_t TryReadItemGameAssetIdFromActor(uint64_t actor)
{
    if (!actor)
        return 0;

    auto readIdFromDataAsset = [](uint64_t dataAsset) -> int64_t {
        if (!dataAsset || !Memory::IsValidPtrFast2(dataAsset))
            return 0;

        if (Memory::read<uint8_t>(
                dataAsset + static_cast<uint64_t>(Offsets::ItemDataAsset_bOverrideItemAssetId)) != 0) {
            const int64_t overrideId = Memory::read<int64_t>(
                dataAsset + static_cast<uint64_t>(Offsets::ItemDataAsset_OverrideItemAssetId));
            if (overrideId != 0)
                return overrideId;
        }

        const std::string assetFname = steam_decrypt::GetActorFNameString(dataAsset);
        if (assetFname.empty())
            return 0;

        // CSV/JSON asset-id values must never throw from a scan thread:
        // std::invalid_argument / out_of_range would abort the process.
        auto safeStoll = [](const std::string& s) -> int64_t {
            try { return std::stoll(s); }
            catch (...) { return 0; }
        };

        if (auto it = g_nameToAssetId.find(assetFname); it != g_nameToAssetId.end())
            return safeStoll(it->second);
        for (const char* prefix : { "DA_", "WID_" }) {
            const size_t plen = strlen(prefix);
            if (assetFname.size() > plen && assetFname.compare(0, plen, prefix) == 0) {
                const std::string stripped = assetFname.substr(plen);
                if (auto it2 = g_nameToAssetId.find(stripped); it2 != g_nameToAssetId.end())
                    return safeStoll(it2->second);
                if (auto it3 = g_nameToAssetId.find(prefix + stripped); it3 != g_nameToAssetId.end())
                    return safeStoll(it3->second);
            }
        }
        return 0;
    };

    if (const uint64_t pickupDa = Memory::read<uint64_t>(
            actor + static_cast<uint64_t>(Offsets::Pickup_DefaultPickupDataAsset));
        pickupDa != 0) {
        if (const int64_t fromPickup = readIdFromDataAsset(pickupDa); fromPickup != 0)
            return fromPickup;
    }

    const uint64_t dataAsset =
        Memory::read<uint64_t>(actor + static_cast<uint64_t>(Offsets::ItemDataAsset));
    return readIdFromDataAsset(dataAsset);
}

std::string LookupByLocKey(const std::string& locKey)
{
    if (locKey.empty())
        return {};

    auto tryKey = [&](const std::string& key) -> std::string {
        if (key.empty())
            return {};
        if (auto it = g_locByKey.find(key); it != g_locByKey.end())
            return it->second;

        std::string upper = key;
        for (char& ch : upper)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (auto it = g_locByKey.find(upper); it != g_locByKey.end())
            return it->second;
        return {};
    };

    if (std::string hit = tryKey(locKey); !hit.empty())
        return hit;

    const size_t ns = locKey.rfind("::");
    if (ns != std::string::npos)
        return tryKey(locKey.substr(ns + 2));

    return {};
}

std::string LookupWorldObjectByFName(const std::string& actorFName)
{
    if (actorFName.empty())
        return {};

    const std::string fnameNorm = NormalizePlainKey(actorFName);
    if (fnameNorm.empty())
        return {};

    if (fnameNorm.find("armoredpatrolcar") != std::string::npos)
        return "Armored Patrol Car";
    if (fnameNorm.find("patrolcar") != std::string::npos)
        return "Patrol Car";
    if (fnameNorm == "patrolcar" || fnameNorm == "armoredpatrolcar")
        return fnameNorm == "armoredpatrolcar" ? "Armored Patrol Car" : "Patrol Car";

    for (const WorldObjPattern& pat : g_worldObjPatterns) {
        if (fnameNorm.find(pat.token) != std::string::npos)
            return pat.display;
    }

    return {};
}

bool LookupAssetWorldPropByFName(
    const std::string& actorFName,
    std::string* outDisplay,
    std::string* outCategorySuffix)
{
    if (actorFName.empty() || g_assetWorldCategoryPatterns.empty())
        return false;

    if (FnameLooksLikeDroppedPickup(actorFName))
        return false;

    const std::string fnameNorm = NormalizePlainKey(actorFName);
    if (fnameNorm.empty())
        return false;

    const auto applyMatch = [&](const AssetWorldPropPattern& pat) -> bool {
        if (pat.token == "safe"
            && (fnameNorm.find("safepocket") != std::string::npos
                || fnameNorm.find("safe pocket") != std::string::npos))
            return false;
        if (outDisplay)
            *outDisplay = pat.display;
        if (outCategorySuffix)
            *outCategorySuffix = pat.category;
        return true;
    };

    for (const AssetWorldPropPattern& pat : g_assetWorldCategoryPatterns) {
        if (fnameNorm == pat.token)
            return applyMatch(pat);
    }

    const AssetWorldPropPattern* bestPat = nullptr;
    size_t bestLen = 0;
    for (const AssetWorldPropPattern& pat : g_assetWorldCategoryPatterns) {
        if (pat.token.size() < 4)
            continue;
        if (pat.token == "bpsocketcontainerraider"
            || pat.token == "bpsocketcontainermedical"
            || pat.token == "bpsocketcontainerrsra"
            || pat.token == "bpsocketcontainergvt"
            || pat.token == "bpsocketcontainerresidential"
            || pat.token == "bpsocketcontainerindustrial"
            || pat.token == "bpsocketcontainer"
            || pat.token == "bpsalvagecontainer"
            || pat.token == "socketcontainer"
            || pat.token == "salvagecontainer")
            continue;
        if (fnameNorm.find(pat.token) == std::string::npos)
            continue;
        if (pat.token.size() > bestLen) {
            bestLen = pat.token.size();
            bestPat = &pat;
        }
    }
    if (bestPat)
        return applyMatch(*bestPat);

    return false;
}

std::string LookupEnemyBotByFName(const std::string& actorFName)
{
    if (actorFName.empty())
        return {};

    const std::string fnameNorm = NormalizePlainKey(actorFName);
    if (fnameNorm.empty())
        return {};

    // Boundary-preserving key: separators become spaces ("BP_Tick_C" ->
    // "bp tick c") so short tokens can word-boundary match. NormalizePlainKey
    // deletes separators outright ("bptickc"), which makes boundaries
    // meaningless and forced the old min-5 rule that blocked real short bot
    // names — Ticks were admitted but had no label, so no ESP (debug-c190fb).
    std::string fnameWords = ToLowerCopy(actorFName);
    for (char& c : fnameWords) {
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = ' ';
    }
    fnameWords = CollapseSpaces(std::move(fnameWords));

    auto tokenMatches = [&](const std::string& token) -> bool {
        if (token.size() < 3)
            return false;
        if (fnameNorm == token)
            return true;
        const std::string& haystack = fnameWords;
        size_t pos = 0;
        while ((pos = haystack.find(token, pos)) != std::string::npos) {
            const bool leftOk = (pos == 0)
                || !std::isalnum(static_cast<unsigned char>(haystack[pos - 1]));
            const size_t end = pos + token.size();
            const bool rightOk = (end >= haystack.size())
                || !std::isalnum(static_cast<unsigned char>(haystack[end]));
            if (leftOk && rightOk)
                return true;
            ++pos;
        }
        // Long tokens keep the old separator-free substring match ("bpsocket"
        // style compound fnames); short tokens require the boundary hit above.
        if (token.size() >= 5)
            return fnameNorm.find(token) != std::string::npos;
        return false;
    };

    std::string best;
    size_t bestLen = 0;
    for (const EnemyBotPattern& pat : g_enemyBotPatterns) {
        if (pat.token.size() < 3)
            continue;
        if (!tokenMatches(pat.token))
            continue;
        if (pat.token.size() > bestLen) {
            bestLen = pat.token.size();
            best = pat.robotType;
        }
    }
    return best;
}

std::string LookupEnemyBotDisplayLabel(const std::string& displayLabel)
{
    if (displayLabel.empty())
        return {};
    // Only return real robot-list types — never echo arbitrary strings
    // (old path made getAllowType accept "GC Electrified" as a bot).
    const std::string mapped = NormalizeBotDisplayName(displayLabel);
    if (IsRobotsListType(mapped))
        return mapped;
    if (IsRobotsListType(displayLabel))
        return displayLabel;
    return {};
}

// SHARED GATE — grep callers before edit
bool IsAcceptedBotEspLabel(
    Engine& eng,
    const std::string& label,
    const std::string& fnameHint)
{
    if (label.empty() || label == "ARC" || IsGenericWorldEspLabel(label))
        return false;
    if (label == "Loot Item" || label == "World Item" || label == "Corpse"
        || label == "Raider stock" || label == "Arc Cargoship")
        return false;
    if (label == "Bot" || label == "Oil")
        return false;

    if (eng.IsKnownRobotType(label))
        return true;
    if (const std::string fromPat = LookupEnemyBotByFName(label); !fromPat.empty()
        && eng.IsKnownRobotType(fromPat))
        return true;

    const std::string mapped = LookupEnemyBotDisplayLabel(label);
    if (!mapped.empty() && eng.IsKnownRobotType(mapped))
        return true;

    if (!fnameHint.empty()) {
        const std::string fromEntity = eng.getEntityType(fnameHint);
        if (fromEntity != "Invalid" && fromEntity != "ARC"
            && fromEntity == label && eng.IsKnownRobotType(fromEntity))
            return true;
    }

    const std::string normalized = NormalizeBotDisplayName(label);
    if (normalized != label && eng.IsKnownRobotType(normalized))
        return true;

    return false;
}

static std::string MapDisplayToRobotType(Engine& eng, const std::string& display)
{
    if (display.empty())
        return {};
    if (const std::string mapped = LookupEnemyBotDisplayLabel(display); !mapped.empty())
        return mapped;
    if (eng.IsKnownRobotType(display))
        return display;
    if (const std::string fromPat = LookupEnemyBotByFName(display); !fromPat.empty()
        && eng.IsKnownRobotType(fromPat))
        return fromPat;
    return {};
}

static void AppendUniqueFname(std::vector<std::string>& out, const std::string& s)
{
    if (s.empty())
        return;
    for (const std::string& existing : out) {
        if (existing == s)
            return;
    }
    out.push_back(s);
}

static std::string ReadConstructableAssetFNameAt(
    uint64_t actor,
    std::ptrdiff_t off,
    Engine& eng)
{
    const uint64_t da =
        Memory::read<uint64_t>(actor + static_cast<uint64_t>(off));
    if (!da || !Memory::IsValidPtrFast2(da))
        return {};

    std::string name = eng.GetActorFNameStringCached(da);
    if (name.empty())
        name = eng.GetActorFNameString(da);
    if (name.empty())
        name = steam_decrypt::GetActorFNameString(da);
    return name;
}

std::string GetEnemyTypeDataAssetFName(uint64_t actor)
{
    if (!actor)
        return {};

    extern Engine engine;

    auto acceptEnemyFname = [](const std::string& name) -> bool {
        if (name.empty())
            return false;
        if (name.find("DA_EnemyType_") == 0)
            return true;
        if (!LookupEnemyBotByFName(name).empty())
            return true;
        if (const std::string display = LookupByAssetName(name); !display.empty())
            return !LookupEnemyBotDisplayLabel(display).empty();
        return false;
    };

    if (const std::string primary = ReadConstructableAssetFNameAt(
            actor, Offsets::Constructable_EnemyTypeDataAsset, engine);
        acceptEnemyFname(primary))
        return primary;

    if (const std::string secondary = ReadConstructableAssetFNameAt(
            actor, Offsets::Constructable_AITemplateData, engine);
        acceptEnemyFname(secondary))
        return secondary;

    return {};
}

std::string ResolveRobotTypeFromFName(Engine& eng, const std::string& fname)
{
    if (fname.empty())
        return {};

    // getEntityType already covers LookupBotClassToken + LookupEnemyBotByFName.
    const std::string fromEntity = eng.getEntityType(fname);
    if (fromEntity != "Invalid" && fromEntity != "ARC"
        && IsAcceptedBotEspLabel(eng, fromEntity, fname))
        return fromEntity;

    if (const std::string fromAsset = LookupDisplayByFNameAssetIndex(fname); !fromAsset.empty()) {
        if (const std::string mapped = MapDisplayToRobotType(eng, fromAsset); !mapped.empty())
            return mapped;
    }

    if (fname.find("DA_EnemyType_") != std::string::npos
        || fname.find("EnemyType") != std::string::npos) {
        if (const std::string fromTable = LookupByAssetName(fname); !fromTable.empty()) {
            if (const std::string mapped = MapDisplayToRobotType(eng, fromTable); !mapped.empty())
                return mapped;
        }
    }

    if (const std::string human = HumanizeActorFName(fname); !human.empty()) {
        if (const std::string mapped = MapDisplayToRobotType(eng, human); !mapped.empty())
            return mapped;
    }

    return {};
}

std::string ResolveEnemyAssetBotLabel(uintptr_t actor)
{
    if (!actor)
        return {};

    extern Engine engine;

    auto resolveFname = [&](const std::string& name) -> std::string {
        if (name.empty())
            return {};
        if (const std::string fromDa = ResolveRobotTypeFromFName(engine, name); !fromDa.empty())
            return fromDa;
        if (const std::string display = LookupByAssetName(name); !display.empty()) {
            if (const std::string mapped = LookupEnemyBotDisplayLabel(display); !mapped.empty())
                return mapped;
        }
        return {};
    };

    if (const std::string validated = GetEnemyTypeDataAssetFName(actor); !validated.empty()) {
        if (const std::string label = resolveFname(validated); !label.empty())
            return label;
    }

    if (const std::string primary = resolveFname(ReadConstructableAssetFNameAt(
            actor, Offsets::Constructable_EnemyTypeDataAsset, engine));
        !primary.empty())
        return primary;

    return resolveFname(ReadConstructableAssetFNameAt(
        actor, Offsets::Constructable_AITemplateData, engine));
}

std::string ResolveRobotTypeForActor(
    Engine& eng,
    uintptr_t actor,
    const std::string& fnameHint)
{
    std::vector<std::string> candidates;
    AppendUniqueFname(candidates, fnameHint);

    if (actor) {
        AppendUniqueFname(candidates, GetEnemyTypeDataAssetFName(actor));

        const uintptr_t mesh = eng.GetActorSkeletalMesh(actor);
        if (mesh) {
            std::string meshFname = eng.GetActorFNameStringCached(mesh);
            if (meshFname.empty())
                meshFname = eng.GetActorFNameString(mesh);
            AppendUniqueFname(candidates, meshFname);
        }

        const uintptr_t embarkMesh =
            Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
        if (embarkMesh) {
            std::string emFname = eng.GetActorFNameStringCached(embarkMesh);
            if (emFname.empty())
                emFname = eng.GetActorFNameString(embarkMesh);
            AppendUniqueFname(candidates, emFname);
        }
    }

    for (const std::string& candidate : candidates) {
        if (const std::string resolved = ResolveRobotTypeFromFName(eng, candidate);
            !resolved.empty() && IsAcceptedBotEspLabel(eng, resolved, fnameHint))
            return resolved;
    }

    if (actor) {
        if (const std::string enemyAsset = GetEnemyTypeDataAssetFName(actor);
            !enemyAsset.empty()) {
            if (const std::string display = LookupByAssetName(enemyAsset); !display.empty()) {
                if (const std::string mapped = MapDisplayToRobotType(eng, display); !mapped.empty()
                    && IsAcceptedBotEspLabel(eng, mapped, fnameHint))
                    return mapped;
            }
            if (const std::string fromPat = LookupEnemyBotByFName(enemyAsset); !fromPat.empty())
                return fromPat;
        }
    }

    return {};
}

bool IsStrictWorldLootFname(const std::string& actorFName)
{
    if (actorFName.empty())
        return false;

    const std::string lower = ToLowerCopy(actorFName);
    static const char* kStrictPrefixes[] = {
        "da_item_", "wid_", "bp_pickupbase", "bp_pickup", "pickupbase",
        "bp_item_", "bp_itemactor_",
        "lootcontainer", "worldobject",
        "corpse", "raidercache", "raider_cache",
        "cargoship", "fieldcrate", "weaponcase", "supplycall",
    };
    for (const char* prefix : kStrictPrefixes) {
        if (lower.find(prefix) != std::string::npos)
            return true;
    }
    if (lower.find("container_") != std::string::npos)
        return true;
    if (lower.find("_container") != std::string::npos)
        return true;
    return false;
}

bool IsInventoryWorldFnameExcluded(const std::string& actorFNameLower)
{
    if (actorFNameLower.empty())
        return false;

    // Do NOT use bare "itemactor" / "weaponactor" — those match every floor
    // BP_ItemActor_* (Canister, salvage, DA shells) and strip them from world ESP.
    // Held/UI-only names stay precise (inventoryservice, stowed, slots, stash…).
    static const char* kExcluded[] = {
        "inventory", "containerslot", "container_slot", "containeritem", "container_item",
        "augmentcontainer", "stashcontainer", "bonusstash", "expeditionstash", "secretstash",
        "safepocket", "backpackslot", "inventoryslot",
        "colonyrun", "equipped", "localcurrent",
        "stowedweapon", "inventoryservice", "inventoryserviceitem", "fakeinventory",
        "primaryitem", "secondaryitem", "loadout", "menu", "hud",
        "flashlight", "defibitem",
    };
    for (const char* token : kExcluded) {
        if (actorFNameLower.find(token) != std::string::npos)
            return true;
    }
    return false;
}

bool WorldObjectAdmitsByFName(const std::string& actorFName)
{
    if (actorFName.empty())
        return false;

    const std::string lower = ToLowerCopy(actorFName);
    if (lower.find("worldobject") != std::string::npos
        || lower.find("id_worldobject") != std::string::npos) {
        return true;
    }

    const std::string fnameNorm = NormalizePlainKey(actorFName);
    if (fnameNorm.empty())
        return false;

    constexpr size_t kMinAdmitTokenLen = 10;
    for (const WorldObjPattern& pat : g_worldObjPatterns) {
        if (pat.token.size() < kMinAdmitTokenLen)
            continue;
        if (fnameNorm.find(pat.token) != std::string::npos)
            return true;
    }
    return false;
}

std::string LookupByInternalToken(const std::string& token)
{
    if (token.empty())
        return {};

    const std::string tokenLower = ToLowerCopy(token);
    std::string best;
    size_t bestLen = 0;

    for (const auto& [key, value] : g_locByKey) {
        std::string probe = key;
        for (auto& ch : probe)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        const size_t pos = probe.find(tokenLower);
        if (pos == std::string::npos)
            continue;

        const size_t matchLen = tokenLower.size();
        if (matchLen > bestLen) {
            bestLen = matchLen;
            best = value;
        }
    }

    if (!best.empty())
        return best;

    const std::string tokenNorm = NormalizePlainKey(token);
    for (const auto& [asset, display] : g_assetByName) {
        const std::string assetNorm = NormalizePlainKey(asset);
        if (assetNorm.size() >= 4 && tokenNorm.find(assetNorm) != std::string::npos)
            return display;
        if (tokenNorm.size() >= 4 && assetNorm.find(tokenNorm) != std::string::npos)
            return display;

        std::string dispPlain = NormalizePlainKey(display);
        dispPlain.erase(
            std::remove_if(dispPlain.begin(), dispPlain.end(), [](unsigned char c) { return c == ' '; }),
            dispPlain.end());
        if (dispPlain.size() >= 4 && tokenNorm.find(dispPlain) != std::string::npos)
            return display;
    }

    return {};
}

bool LookupItemMeta(const std::string& displayName, int& outRarityTier, int& outValue)
{
    outRarityTier = 0;
    outValue = 0;
    if (displayName.empty())
        return false;

    const std::string norm = NormalizeNameKey(displayName);
    if (auto it = g_metaByNorm.find(norm); it != g_metaByNorm.end()) {
        outRarityTier = it->second.tier;
        outValue = it->second.value;
        return true;
    }

    const std::string plain = NormalizePlainKey(displayName);
    if (auto it = g_metaByPlain.find(plain); it != g_metaByPlain.end()) {
        outRarityTier = it->second.tier;
        outValue = it->second.value;
        return true;
    }

    return false;
}

bool LookupItemMetaByAssetName(const std::string& assetName, int& outRarityTier, int& outValue)
{
    outRarityTier = 0;
    outValue = 0;
    if (assetName.empty())
        return false;

    if (auto it = g_metaByAssetName.find(assetName); it != g_metaByAssetName.end()) {
        outRarityTier = it->second.tier;
        outValue = it->second.value;
        return true;
    }

    const std::string fromAsset = LookupByAssetName(assetName);
    if (!fromAsset.empty() && fromAsset != assetName)
        return LookupItemMeta(fromAsset, outRarityTier, outValue);

    return false;
}

bool LookupItemMetaById(const std::string& metaId, int& outRarityTier, int& outValue)
{
    outRarityTier = 0;
    outValue = 0;
    if (metaId.empty())
        return false;

    const std::string plain = NormalizePlainKey(metaId);
    if (auto it = g_metaById.find(plain); it != g_metaById.end()) {
        outRarityTier = it->second.tier;
        outValue = it->second.value;
        return true;
    }
    return false;
}

bool ResolveItemMetaForActor(
    Engine& eng,
    uintptr_t actor,
    const std::string& fnameHint,
    const std::string& displayHint,
    int& outRarityTier,
    int& outValue)
{
    outRarityTier = 0;
    outValue = 0;

    auto tryCandidate = [&](const std::string& candidate) -> bool {
        if (candidate.empty() || IsGenericWorldEspLabel(candidate))
            return false;
        return LookupItemMeta(candidate, outRarityTier, outValue);
    };

    auto tryAsset = [&](const std::string& assetName) -> bool {
        if (assetName.empty())
            return false;
        if (LookupItemMetaByAssetName(assetName, outRarityTier, outValue))
            return true;
        if (const std::string metaId = AssetNameToMetaId(assetName);
            LookupItemMetaById(metaId, outRarityTier, outValue))
            return true;
        return false;
    };

    if (actor) {
        if (const std::string dataAsset = GetActorDataAssetFName(actor); tryAsset(dataAsset))
            return true;
    }

    if (!fnameHint.empty()) {
        if (tryAsset(fnameHint))
            return true;
    }

    if (tryCandidate(displayHint))
        return true;

    if (actor) {
        if (const std::string fromHover = eng.GetEnglishItemName(actor); tryCandidate(fromHover))
            return true;
    }

    if (!fnameHint.empty()) {
        if (tryCandidate(LookupByAssetName(fnameHint)))
            return true;
        if (const std::string fromIdx = LookupDisplayByFNameAssetIndex(fnameHint); tryCandidate(fromIdx))
            return true;
        if (tryCandidate(HumanizeActorFName(fnameHint)))
            return true;
        if (tryCandidate(LookupWorldObjectByFName(fnameHint)))
            return true;
        if (const std::string metaId = AssetNameToMetaId(fnameHint);
            LookupItemMetaById(metaId, outRarityTier, outValue))
            return true;
    }

    return false;
}

std::string LookupDisplayByFNameAssetIndex(const std::string& actorFName)
{
    if (actorFName.empty())
        return {};

    const std::string fnameNorm = NormalizePlainKey(actorFName);
    if (fnameNorm.empty())
        return {};

    for (const FnameAssetPattern& pat : g_fnameAssetPatterns) {
        if (fnameNorm == pat.token) {
            if (!IsGenericWorldEspLabel(pat.display))
                return pat.display;
        }
    }

    constexpr size_t kMinSubstrTokenLen = 5;
    for (const FnameAssetPattern& pat : g_fnameAssetPatterns) {
        if (pat.token.size() < kMinSubstrTokenLen)
            continue;
        if (fnameNorm.find(pat.token) != std::string::npos) {
            if (!IsGenericWorldEspLabel(pat.display))
                return pat.display;
        }
    }
    return {};
}

static std::string StripUeClassSuffix(std::string s)
{
    const size_t slash = s.rfind('/');
    if (slash != std::string::npos)
        s = s.substr(slash + 1);

    if (s.size() > 2 && s.compare(s.size() - 2, 2, "_C") == 0)
        s.resize(s.size() - 2);

    const std::string lower = ToLowerCopy(s);
    if (lower.rfind("default__", 0) == 0)
        s = s.substr(9);

    // World Partition / UAID instance suffixes (help/esp.txt NormalizeClassName).
    {
        const std::string lo = ToLowerCopy(s);
        const size_t uaid = lo.find("_uaid_");
        if (uaid != std::string::npos)
            s.resize(uaid);
        // Trailing _<16 hex>
        if (s.size() > 17) {
            const size_t us = s.rfind('_');
            if (us != std::string::npos && s.size() - us - 1 == 16) {
                bool hex = true;
                for (size_t i = us + 1; i < s.size(); ++i) {
                    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                        hex = false;
                        break;
                    }
                }
                if (hex)
                    s.resize(us);
            }
        }
    }
    return s;
}

static bool FnameLooksWorldOrLoot(const std::string& actorFName)
{
    if (actorFName.empty())
        return false;
    if (IsStrictWorldLootFname(actorFName))
        return true;
    if (FnameAdmitsWorldActor(actorFName))
        return true;

    const std::string lower = ToLowerCopy(actorFName);
    static const char* kHints[] = {
        "container", "pickup", "worldobject", "loot", "crate", "locker",
        "trash", "medical", "grenade", "weaponcase", "harvest", "backpack",
        "bp_", "da_", "wid_",
    };
    for (const char* hint : kHints) {
        if (lower.find(hint) != std::string::npos)
            return true;
    }
    return false;
}

static bool IsLocationPrefixToken(const std::string& lower)
{
    static const char* kBlocked[] = {
        "wrh", "res", "rsr", "gvt", "cmr", "coms",
    };
    for (const char* token : kBlocked) {
        if (lower == token)
            return true;
    }
    return false;
}

static bool LabelNeedsTitleCase(const std::string& s)
{
    bool wordStart = true;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            wordStart = true;
            continue;
        }
        if (wordStart) {
            if (std::islower(c))
                return true;
            wordStart = false;
        }
    }
    return false;
}

static std::string ApplyEspTitleCase(std::string s)
{
    static const char* kAcronyms[] = {
        "ARC", "SP", "WRH", "DA", "BP", "ID", "UI", "HP", "XP", "AI", "SOS",
    };

    std::string out;
    out.reserve(s.size());
    std::string word;
    auto flushWord = [&]() {
        if (word.empty())
            return;
        for (const char* acr : kAcronyms) {
            if (_stricmp(word.c_str(), acr) == 0) {
                out += acr;
                word.clear();
                return;
            }
        }
        word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        for (size_t i = 1; i < word.size(); ++i)
            word[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(word[i])));
        out += word;
        word.clear();
    };

    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            flushWord();
            if (!out.empty() && out.back() != ' ')
                out.push_back(' ');
        } else {
            word.push_back(c);
        }
    }
    flushWord();
    return TrimCopy(std::move(out));
}

static bool IsEspFnameJunkSegment(const std::string& seg)
{
    if (seg.empty())
        return true;
    const std::string lower = ToLowerCopy(seg);
    static const char* kJunk[] = {
        "wa", "medium", "small", "large", "deployable", "wid", "witem", "w",
        "default", "dynamic", "lid", "c", "instance", "inst",
    };
    for (const char* junk : kJunk) {
        if (lower == junk)
            return true;
    }
    if (lower.size() == 2 && lower[0] == 't' && std::isdigit(static_cast<unsigned char>(lower[1])))
        return true;
    if (lower.size() == 2 && std::isdigit(static_cast<unsigned char>(lower[0]))
        && std::isdigit(static_cast<unsigned char>(lower[1])))
        return true;
    return false;
}

static bool IsEspDisplayJunkWord(const std::string& word)
{
    if (word.empty())
        return true;
    const std::string lower = ToLowerCopy(word);
    static const char* kJunk[] = {
        "wa", "medium", "small", "large", "deployable", "wid", "witem", "w",
        "item", "bp", "da", "default", "dynamic", "world", "instance", "inst",
    };
    for (const char* junk : kJunk) {
        if (lower == junk)
            return true;
    }
    if (lower.size() == 2 && lower[0] == 't' && std::isdigit(static_cast<unsigned char>(lower[1])))
        return true;
    if (lower.size() == 2 && std::isdigit(static_cast<unsigned char>(lower[0]))
        && std::isdigit(static_cast<unsigned char>(lower[1])))
        return true;
    return false;
}

static std::string StripEspDisplayJunkWords(std::string s)
{
    std::string out;
    out.reserve(s.size());
    std::string word;
    auto flushWord = [&]() {
        if (word.empty())
            return;
        if (!IsEspDisplayJunkWord(word)) {
            if (!out.empty())
                out.push_back(' ');
            out += word;
        }
        word.clear();
    };
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)))
            flushWord();
        else
            word.push_back(c);
    }
    flushWord();
    return CollapseSpaces(TrimCopy(std::move(out)));
}

static void StripEspFnamePrefixes(std::string& cleaned)
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* prefix : {
                 "da_item_salvage_", "item_salvage_", "salvage_",
                 "da_item_deployable_", "item_deployable_", "deployable_",
                 "da_item_", "bp_witem_", "witem_", "w_item_", "wid_item_", "wid_",
                 "socketcontainer_", "salvagecontainer_",
                 "bp_pickupbase_", "bp_pickup_", "bp_lootcontainer_", "lootcontainer_",
                 "id_worldobject_", "bp_worldobject_", "worldobject_",
                 "pickupbase_", "bp_", "da_", "wid_" }) {
            const size_t plen = strlen(prefix);
            if (cleaned.size() > plen &&
                _strnicmp(cleaned.c_str(), prefix, static_cast<unsigned>(plen)) == 0) {
                cleaned = cleaned.substr(plen);
                changed = true;
                break;
            }
        }
    }
}

static std::string FixArcHuskBrandCasing(std::string s)
{
    size_t pos = 0;
    while (pos + 8 <= s.size()) {
        if (_strnicmp(s.c_str() + pos, "arc husk", 8) == 0
            && (pos + 8 == s.size()
                || std::isspace(static_cast<unsigned char>(s[pos + 8])))) {
            s.replace(pos, 3, "Arc");
            pos += 8;
            continue;
        }
        ++pos;
    }
    return s;
}

static std::string NormalizeEspBarricadeLabel(const std::string& plainKey)
{
    if (plainKey.find("barricade") == std::string::npos)
        return {};
    if (plainKey.find("kit") != std::string::npos
        || plainKey.find("blueprint") != std::string::npos
        || plainKey.find("recipe") != std::string::npos)
        return {};
    if (plainKey == "barricade"
        || plainKey.find("deployable") != std::string::npos)
        return "Barricade";
    return {};
}

static std::string NormalizeEspHuskLabel(const std::string& plainKey, std::string spaced)
{
    if (plainKey.find("husk") == std::string::npos)
        return {};

    static const struct {
        const char* key;
        const char* label;
    } kNamed[] = {
        { "rocketeerhusk", "Rocketeer Husk" },
        { "wasphusk", "Wasp Husk" },
        { "baronhusk", "Baron Husk" },
        { "deforesterhusk", "ARC Deforester Husk" },
        { "arcdeforesterhusk", "ARC Deforester Husk" },
    };
    for (const auto& entry : kNamed) {
        if (plainKey.find(entry.key) != std::string::npos)
            return entry.label;
    }

    spaced = StripEspDisplayJunkWords(std::move(spaced));
    const std::string strippedPlain = NormalizePlainKey(spaced);
    if (strippedPlain.find("husk") == std::string::npos)
        return {};

    if (strippedPlain.find("arc") != std::string::npos
        || strippedPlain == "husk"
        || plainKey.find("archusk") != std::string::npos)
        return "Arc Husk";

    return {};
}

static const char* const* CompoundSplitTokenTable(size_t* outCount)
{
    static const char* kTokens[] = {
        "supplycallstation", "armoredpatrolcar", "burieddetectable", "firstwaveraidercache",
        "electricalcabinet", "informationterminal", "shippingcontainer", "genericcontainer",
        "salvagecontainer", "socketcontainer", "lootcontainer", "grenadecontainer",
        "filingcabinet", "storagelocker", "trailercompressor", "candleberrybush",
        "crashedarcprobe", "arcdeforesterhusk", "multipowerstation", "multipowerrack",
        "accesscapacitor", "strongroomdoor", "benchkitchen", "coffeemachine",
        "patrolcar", "patrol_car", "armoredpatrolcar", "truckutility",
        "weaponcase", "fieldcrate", "cartcrate", "objectcrate", "deployablebarricade",
        "weaponrack", "weaponsrack", "securitycamera", "securityterminal",
        "medicalbag", "ammobox", "footlocker", "raidercache", "raiderbackpack",
        "grenadetube", "trashbin", "trashcan", "wastebin", "wastebasket", "icebox",
        "supplycall", "deaddrop", "cargoship", "arcdeforester", "arcassessor",
        "highwayaddon", "roadbarrier", "rocketparts", "ventmachine", "solarpanel",
        "seedconsole", "fruitbasket", "wornbackpack", "suspiciousbackpack",
        "motionsensor", "metaldetector", "filtrationsystem",
        "purificationsystem", "magneticdecryptor", "digitalogbook", "satellitedish",
        "commsterminal", "observationdeck", "secludedshelter", "highwayshelter",
        "securityconsole", "computerterminal", "industrialrecharger", "accesscardprinter",
        "emergencysiren", "highgainantenna", "submergedbeacon", "wallpanel",
        "weapons", "weapon", "security", "camera", "barricade", "deployable",
        "field", "cart", "crate", "locker", "lockers", "medical", "ammo",
        "grenade", "trash", "supply", "station", "raider", "cache", "buried", "loot",
        "dead", "drop", "shipping", "container", "socket", "salvage", "filing",
        "cabinet", "drawer", "closet", "cupboard", "workbench", "toolbox", "cooler",
        "fridge", "freezer", "compressor", "generator", "patrol", "armored", "probe",
        "cargo", "vault", "safe", "computer", "servers", "case", "bag", "rack",
        "box", "bin", "can", "dumpster", "foot", "storage", "kitchen", "bench",
        "coffee", "machine", "electrical", "information", "terminal", "solar", "panel",
        "vent", "seed", "console", "highway", "addon", "road", "barrier", "rocket",
        "parts", "fruit", "basket", "candleberry", "bush", "object", "generic",
        "industrial", "residential", "waste", "call", "arc", "husk", "car", "desk", "tube",
        "truck", "utility", "pickup", "flatbed", "wagon", "trailer", "compartment",
        "passenger", "service", "sedan", "ambulance",
    };
    *outCount = sizeof(kTokens) / sizeof(kTokens[0]);
    return kTokens;
}

static std::string SplitConcatenatedPlainKey(const std::string& plain)
{
    if (plain.size() < 4 || plain.find(' ') != std::string::npos)
        return {};

    size_t tokenCount = 0;
    const char* const* tokens = CompoundSplitTokenTable(&tokenCount);
    std::vector<const char*> sorted;
    sorted.reserve(tokenCount);
    for (size_t i = 0; i < tokenCount; ++i)
        sorted.push_back(tokens[i]);
    std::sort(sorted.begin(), sorted.end(),
        [](const char* a, const char* b) { return std::strlen(a) > std::strlen(b); });

    std::vector<std::string> words;
    size_t pos = 0;
    while (pos < plain.size()) {
        bool matched = false;
        for (const char* tok : sorted) {
            const size_t tlen = std::strlen(tok);
            if (tlen == 0 || pos + tlen > plain.size())
                continue;
            if (plain.compare(pos, tlen, tok) != 0)
                continue;
            words.emplace_back(tok);
            pos += tlen;
            matched = true;
            break;
        }
        if (!matched)
            return {};
    }
    if (words.empty())
        return {};

    std::string joined;
    joined.reserve(plain.size() + words.size());
    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0)
            joined.push_back(' ');
        joined += words[i];
    }
    return joined;
}

static std::string LookupExplicitCompoundLabel(const std::string& plainKey)
{
    if (plainKey.empty())
        return {};

    static const struct {
        const char* key;
        const char* label;
    } kExplicit[] = {
        { "cartcrate", "Cart Crate" },
        { "cart crate", "Cart Crate" },
        { "objectcrate", "Object Crate" },
        { "weaponcase", "Weapon Case" },
        { "weapon case", "Weapon Case" },
        { "weaponrack", "Weapon Rack" },
        { "weapon rack", "Weapon Rack" },
        { "weaponsrack", "Weapons Rack" },
        { "weapons rack", "Weapons Rack" },
        { "securitycamera", "Security Camera" },
        { "security camera", "Security Camera" },
        { "deployablebarricade", "Barricade" },
        { "deployable barricade", "Barricade" },
        { "fieldcrate", "Field Crate" },
        { "field crate", "Field Crate" },
        { "medicalbag", "Medical Bag" },
        { "medical bag", "Medical Bag" },
        { "ammobox", "Ammo Box" },
        { "ammo box", "Ammo Box" },
        { "grenadetube", "Grenade Tube" },
        { "grenade tube", "Grenade Tube" },
        { "grenadecontainer", "Grenade Tube" },
        { "shippingcontainer", "Shipping Container" },
        { "shipping container", "Shipping Container" },
        { "genericcontainer", "Generic Container" },
        { "generic container", "Generic Container" },
        { "lootcontainer", "Loot Container" },
        { "loot container", "Loot Container" },
        { "socketcontainer", "Socket Container" },
        { "salvagecontainer", "Salvage Container" },
        { "filingcabinet", "Filing Cabinet" },
        { "filing cabinet", "Filing Cabinet" },
        { "footlocker", "Footlocker" },
        { "storagelocker", "Storage Locker" },
        { "storage locker", "Storage Locker" },
        { "trashcan", "Trash Can" },
        { "trash can", "Trash Can" },
        { "trashbin", "Trash Can" },
        { "bintrash", "Trash Can" },
        { "wastebin", "Waste Bin" },
        { "wastebasket", "Waste Basket" },
        { "raidercache", "Raider Cache" },
        { "raider cache", "Raider Cache" },
        { "deaddrop", "Dead Drop" },
        { "dead drop", "Dead Drop" },
        { "burieddetectable", "Buried Loot" },
        { "buried loot", "Buried Loot" },
        { "supplycallstation", "Supply Call Station" },
        { "supply call station", "Supply Call Station" },
        { "supplycellstation", "Supply Call Station" },
        { "supply cell station", "Supply Call Station" },
        { "supplycall", "Supply Call Station" },
        { "supplycell", "Supply Call Station" },
        { "raiderbackpack", "Worn Backpack" },
        { "wornbackpack", "Worn Backpack" },
        { "suspiciousbackpack", "Suspicious Backpack" },
        { "armoredpatrolcar", "Armored Patrol Car" },
        { "patrolcar", "Patrol Car" },
        { "patrol car", "Patrol Car" },
        { "truckutility", "Truck Utility" },
        { "truck utility", "Truck Utility" },
        { "cargoship", "Arc Cargo" },
        { "arc cargo", "Arc Cargo" },
        { "arcprobe", "ARC Probe" },
        { "crashedarcprobe", "Crashed ARC Probe" },
        { "trailercompressor", "Trailer Compressor" },
        { "greatmullein", "Great Mullein" },
        { "torchginger", "Great Mullein" },
        { "coffeemachine", "Coffee Machine" },
        { "informationterminal", "Information Terminal" },
        { "electricalcabinet", "Electrical Cabinet" },
        { "solarpanel", "Solar Panel" },
        { "ventmachine", "Vent Machine" },
        { "seedconsole", "Seed Console" },
        { "highwayaddon", "Highway Addon" },
        { "roadbarrier", "Road Barrier" },
        { "candleberrybush", "Candleberry Bush" },
        { "fruitbasket", "Basket of Fruit" },
        { "benchkitchen", "Kitchen Bench" },
        { "icebox", "Cooler" },
        { "multipowerrack", "Access Capacitor Rack" },
        { "accesscapacitor", "Access Capacitor" },
        { "access capacitor", "Access Capacitor" },
        { "arcdeforesterhusk", "ARC Deforester Husk" },
        { "arc deforester husk", "ARC Deforester Husk" },
        { "rocketeerhusk", "Rocketeer Husk" },
        { "wasphusk", "Wasp Husk" },
        { "baronhusk", "Baron Husk" },
        { "archusk", "Arc Husk" },
        { "arc husk", "Arc Husk" },
        { "doorright", "Machine" },
        { "machinem", "Machine" },
    };
    for (const auto& entry : kExplicit) {
        if (plainKey == entry.key)
            return entry.label;
    }

    const char* bestLabel = nullptr;
    size_t bestLen = 0;
    for (const AssetWorldPropPattern& pat : g_assetWorldCategoryPatterns) {
        if (pat.token.size() < 4 || pat.token.size() <= bestLen)
            continue;
        if (plainKey == pat.token || plainKey.find(pat.token) != std::string::npos) {
            bestLabel = pat.display.c_str();
            bestLen = pat.token.size();
        }
    }
    return bestLabel ? std::string(bestLabel) : std::string{};
}

static std::string StripSalvageDisplayPrefix(std::string s)
{
    for (;;) {
        const std::string lower = ToLowerCopy(s);
        if (lower.rfind("salvage ", 0) == 0 && s.size() > 8) {
            s = TrimCopy(s.substr(8));
            continue;
        }
        break;
    }
    return CollapseSpaces(std::move(s));
}

std::string FormatEspDisplayLabel(const std::string& label)
{
    if (label.empty())
        return {};

    std::string s = CollapseSpaces(TrimCopy(label));
    if (s.size() < 2)
        return {};

    s = StripSalvageDisplayPrefix(std::move(s));
    if (s.size() < 2)
        return {};

    s = StripEspDisplayJunkWords(std::move(s));
    if (s.size() < 2)
        return {};

    std::string plain = NormalizePlainKey(s);
    if (plain.empty())
        return {};

    if (const std::string barricade = NormalizeEspBarricadeLabel(plain); !barricade.empty())
        return barricade;

    if (const std::string husk = NormalizeEspHuskLabel(plain, s); !husk.empty())
        return husk;

    if (const std::string mapped = LookupExplicitCompoundLabel(plain); !mapped.empty())
        return mapped;

    if (plain.find(' ') == std::string::npos && plain.size() >= 4) {
        if (const std::string split = SplitConcatenatedPlainKey(plain); !split.empty()) {
            if (const std::string remapped = LookupExplicitCompoundLabel(NormalizePlainKey(split));
                !remapped.empty())
                return remapped;
            s = split;
            plain = NormalizePlainKey(s);
        }
    }

    if (const std::string barricade2 = NormalizeEspBarricadeLabel(plain); !barricade2.empty())
        return barricade2;
    if (const std::string husk2 = NormalizeEspHuskLabel(plain, s); !husk2.empty())
        return husk2;

    if (LabelNeedsTitleCase(s))
        s = ApplyEspTitleCase(std::move(s));

    return FixArcHuskBrandCasing(std::move(s));
}

// SHARED GATE — grep callers before edit
std::string HumanizeActorFName(const std::string& actorFName)
{
    if (actorFName.empty())
        return {};
    if (FnameLooksLikeEngineSubobjectClass(actorFName))
        return {};

    {
        const std::string lower = ToLowerCopy(actorFName);
        if (lower.find("da_oi_outfit") != std::string::npos
            || lower.find("oi_outfit") != std::string::npos)
            return {};
    }

    std::string cleaned = StripUeClassSuffix(actorFName);
    StripEspFnamePrefixes(cleaned);

    if (cleaned.size() > 2 && cleaned.compare(cleaned.size() - 2, 2, "_C") == 0)
        cleaned.resize(cleaned.size() - 2);

    {
        const std::string plain = NormalizePlainKey(cleaned);
        if (IsLocationPrefixToken(plain))
            return {};
        if (const std::string mapped = LookupExplicitCompoundLabel(plain); !mapped.empty())
            return mapped;
        if (const std::string split = SplitConcatenatedPlainKey(plain); !split.empty())
            return FormatEspDisplayLabel(split);
    }

    std::string split;
    split.reserve(cleaned.size() * 2);
    for (size_t i = 0; i < cleaned.size(); ++i) {
        char c = cleaned[i];
        if (i > 0 && std::isupper(static_cast<unsigned char>(c))) {
            const char prev = cleaned[i - 1];
            const bool prevLower = std::islower(static_cast<unsigned char>(prev));
            const bool prevUpper = std::isupper(static_cast<unsigned char>(prev));
            if (prevLower
                || (prevUpper && i + 1 < cleaned.size()
                    && std::islower(static_cast<unsigned char>(cleaned[i + 1]))))
                split.push_back(' ');
        }
        if (c == '_')
            c = ' ';
        split.push_back(c);
    }
    cleaned = CollapseSpaces(std::move(split));

    {
        std::string filtered;
        filtered.reserve(cleaned.size());
        std::string word;
        auto flushWord = [&]() {
            if (word.empty() || IsEspFnameJunkSegment(word)) {
                word.clear();
                return;
            }
            if (!filtered.empty())
                filtered.push_back(' ');
            filtered += word;
            word.clear();
        };
        for (char c : cleaned) {
            if (std::isspace(static_cast<unsigned char>(c)))
                flushWord();
            else
                word.push_back(c);
        }
        flushWord();
        cleaned = CollapseSpaces(std::move(filtered));
    }

    static const char* kStripWords[] = {
        " Socket", "Socket ", " Container", "Container ",
        " Res", "Res ", " Wrh", "Wrh ",
        " Medium", "Medium ", " Small", "Small ", " Large", "Large ",
        " WA", "WA ", " Wa", "Wa ",
        " Deployable", "Deployable ", " W Item", "W Item ", " Wid", "Wid ",
        " 01", "01 ", " 02", "02 ", " 03", "03 ",
    };
    for (const char* word : kStripWords) {
        size_t pos = cleaned.find(word);
        while (pos != std::string::npos) {
            cleaned.erase(pos, strlen(word));
            pos = cleaned.find(word, pos);
        }
    }

    cleaned = TrimCopy(cleaned);
    if (cleaned.size() < 2)
        return {};

    if (IsLocationPrefixToken(ToLowerCopy(cleaned)))
        return {};

    {
        const std::string plain = NormalizePlainKey(cleaned);
        if (const std::string mapped = LookupExplicitCompoundLabel(plain); !mapped.empty())
            return mapped;
    }

    {
        bool hasLetter = false;
        bool onlyNumericish = true;
        for (unsigned char c : cleaned) {
            if (std::isalpha(c))
                hasLetter = true;
            else if (!std::isspace(c) && !std::isdigit(c) && c != '-')
                onlyNumericish = false;
        }
        if (!hasLetter && onlyNumericish)
            return {};
    }

    if (ToLowerCopy(cleaned) == "other" || ToLowerCopy(cleaned) == "loot item")
        return {};
    return FormatEspDisplayLabel(cleaned);
}

bool FnameLooksLikeHarvestableActor(const std::string& actorFName)
{
    if (actorFName.empty())
        return false;

    const std::string lower = ToLowerCopy(actorFName);
    static const char* kTokens[] = {
        "bp_actor_consumable",
        "bpactorconsumable",
        "consumable_moss",
        "consumablemoss",
        "consumable_mushroom",
        "consumablemushroom",
        "consumable_agave",
        "consumableagave",
        "consumable_lemon",
        "consumable_apricot",
        "consumable_olives",
        "consumable_prickly",
        "consumable_resin",
        "consumable_mullein",
        "consumablemullein",
        "greatmullein",
        "torchginger",
        "torch_ginger",
        "lootsocket",
        "pioneerlootsocket",
        "environmentalloot",
        "fruitbasket",
        "candleberry",
        "birdnest",
        "beenest",
    };
    for (const char* token : kTokens) {
        if (lower.find(token) != std::string::npos)
            return true;
    }
    return false;
}

bool FnameAdmitsWorldActor(const std::string& actorFName)
{
    if (actorFName.empty())
        return false;

    if (IsStrictWorldLootFname(actorFName))
        return true;

    if (FnameLooksLikeHarvestableActor(actorFName))
        return true;

    if (FnameLooksLikeWorldContainer(actorFName))
        return true;

    if (WorldObjectAdmitsByFName(actorFName))
        return true;

    if (!LookupByAssetName(actorFName).empty())
        return true;

    const std::string lower = ToLowerCopy(actorFName);
    if (IsInventoryWorldFnameExcluded(lower))
        return false;

    static const char* kBroadHints[] = {
        "chest", "crate", "loot", "pickup", "locker", "trash", "safe",
        "vault", "ammo", "grenade", "medical", "backpack", "probe",
        "vehicle", "furniture", "industrial", "buried", "deaddrop",
        "harvestable", "harvest", "container",
    };
    for (const char* token : kBroadHints) {
        if (lower.find(token) != std::string::npos)
            return true;
    }

    return false;
}

static bool FnameLooksLikePlacedWorldInteractable(const std::string& lower)
{
    static const char* kPlacedProps[] = {
        "archusk", "arc_husk", "deployablebarricade", "deployable_barricade",
        "weaponsrack", "weaponrack", "securitycamera", "securityterminal",
        "trailercompressor", "compressor", "ventmachine", "electricalcabinet",
        "supplycellstation", "supplycell", "seedconsole", "solarpanel",
        "highwayaddon", "roadbarrier", "rocketparts", "informationterminal",
    };
    for (const char* token : kPlacedProps) {
        if (lower.find(token) != std::string::npos)
            return true;
    }
    if (lower.find("husk") != std::string::npos
        && (lower.find("worldobject") != std::string::npos
            || lower.find("bp_worldobject") != std::string::npos))
        return true;
    if (lower.find("barricade") != std::string::npos
        && (lower.find("worldobject") != std::string::npos
            || lower.find("bp_worldobject") != std::string::npos))
        return true;
    return false;
}

bool FnameLooksLikeDroppedPickup(const std::string& actorFName)
{
    if (actorFName.empty())
        return false;

    const std::string lower = ToLowerCopy(actorFName);

    // Pickup tokens win. Historically "itemactor" in inventory-exclude blocked
    // every BP_ItemActor_* floor drop (Canister) while BP_PickupBase still matched.
    // itemactor/weaponactor were removed from IsInventoryWorldFnameExcluded.
    static const char* kPickupTokens[] = {
        "bp_pickupbase", "pickupbase", "bp_pickup", "bp_itemactor_", "bp_item_",
        "da_item_", "wid_",
    };
    for (const char* token : kPickupTokens) {
        if (lower.find(token) != std::string::npos)
            return true;
    }

    if (IsInventoryWorldFnameExcluded(lower))
        return false;

    if (FnameLooksLikePlacedWorldInteractable(lower))
        return false;

    return false;
}

bool FnameLooksLikeWorldContainer(const std::string& actorFName)
{
    if (actorFName.empty())
        return false;

    const std::string lower = ToLowerCopy(actorFName);
    if (IsInventoryWorldFnameExcluded(lower))
        return false;

    if (FnameLooksLikePlacedWorldInteractable(lower))
        return true;

    if (FnameLooksLikeDroppedPickup(actorFName))
        return false;

    static const char* kTokens[] = {
        "bpsocketcontainer", "bpsalvagecontainer",
        "socketcontainer", "salvagecontainer", "burieddetectable", "raidercache",
        "cargoship", "arc_cargo", "arc_cargoship", "bp_arc_cargoship",
        "lootcontainer", "genericcontainer", "shippingcontainer",
        "fieldcrate", "field_crate",
        "weaponcase", "weapon_case", "container_weapon",
        "medicalbag", "medical_bag", "container_medical",
        "ammobox", "ammo_box", "container_ammo",
        "grenadecontainer", "grenadetube", "container_grenade",
        "locker", "lockers", "footlocker", "storagelocker",
        // Bare crate/drawer/cabinet — not only fieldcrate / *_container.
        // Without these, drawers/crates that lack a live LI pointer at 0xB58
        // fail QuickContainerCandidate → preSkip (user standing on banks).
        "crate", "drawer", "cabinet", "chest", "lootcrate", "cartcrate",
        "objectcrate", "weaponchest",
        "bintrash", "trashbin", "trashcan", "dumpster", "wastebin", "wastebasket",
        "deaddrop", "dead_drop", "buried_detectable",
        "supplycall", "supply_call", "supplystation",
        // Crashed ARC Probe is a lootable world container (not BP_PickupBase).
        // Log proof: BP_ProbeCrashed_02_C admitted as item cat=DroppedPickup.
        "probecrashed", "probe_crashed", "bp_probe", "crashedarcprobe", "arcprobe",
        "probe",
    };
    for (const char* token : kTokens) {
        if (lower.find(token) != std::string::npos)
            return true;
    }

    if (lower.find("container_") != std::string::npos
        && lower.find("containerslot") == std::string::npos
        && lower.find("inventory") == std::string::npos)
        return true;

    if (lower.find("_container") != std::string::npos
        && lower.find("containerslot") == std::string::npos
        && lower.find("inventory") == std::string::npos
        && lower.find("sample_container") == std::string::npos)
        return true;

    return false;
}

extern Engine engine;

std::string ResolveWorldLabel(uintptr_t actor, const std::string& actorFName)
{
    auto polish = [](std::string s) -> std::string {
        return s.empty() ? std::string{} : FormatEspDisplayLabel(s);
    };

    if (actor != 0) {
        const std::string fromMem = engine.GetEnglishItemName(actor);
        if (!fromMem.empty() && !IsGenericWorldEspLabel(fromMem))
            return polish(fromMem);

        const bool pickupLike = FnameLooksLikeDroppedPickup(actorFName)
            || FnameLooksLikeDroppedPickup(engine.GetActorClassFName(actor));
        if (pickupLike) {
            if (const int64_t gameAssetId = TryReadItemGameAssetIdFromActor(actor); gameAssetId != 0) {
                if (const std::string fromId = LookupDisplayByAssetId(gameAssetId); !fromId.empty())
                    return polish(fromId);
            }
            if (const std::string dataAsset = GetActorDataAssetFName(actor); !dataAsset.empty()) {
                if (const std::string fromAsset = LookupByAssetName(dataAsset); !fromAsset.empty())
                    return polish(fromAsset);
            }
        }
    }

    if (actorFName.empty())
        return {};

    if (const std::string human = HumanizeActorFName(actorFName);
        !human.empty() && !IsGenericWorldEspLabel(human))
        return human;

    if (const std::string fromWorld = LookupWorldObjectByFName(actorFName);
        !fromWorld.empty() && !IsGenericWorldEspLabel(fromWorld))
        return polish(fromWorld);

    if (const std::string fromAsset = LookupByAssetName(actorFName); !fromAsset.empty())
        return polish(fromAsset);

    const std::string stripped = StripUeClassSuffix(actorFName);
    if (stripped != actorFName) {
        if (const std::string fromStripped = LookupByAssetName(stripped); !fromStripped.empty())
            return polish(fromStripped);
    }

    if (const std::string fromFname = LookupDisplayByFNameAssetIndex(actorFName); !fromFname.empty())
        return polish(fromFname);

    if (const std::string fromToken = LookupByInternalToken(actorFName); !fromToken.empty())
        return polish(fromToken);

    return {};
}

std::string ResolveWorldDisplayLabel(uintptr_t actor, const std::string& fnameHint, int worldCategory)
{
    std::string effectiveFname = fnameHint;
    if (effectiveFname.empty() && actor != 0) {
        effectiveFname = engine.GetActorFNameStringCached(actor);
        if (effectiveFname.empty())
            effectiveFname = engine.GetActorFNameString(actor);
    }

    std::string classFname;
    if (actor != 0)
        classFname = engine.GetActorClassFName(actor);

    auto vehicleFromFname = [](const std::string& s) -> std::string {
        if (s.empty())
            return {};
        const std::string norm = NormalizePlainKey(s);
        if (norm.find("armoredpatrolcar") != std::string::npos)
            return "Armored Patrol Car";
        if (norm.find("patrolcar") != std::string::npos)
            return "Patrol Car";
        return {};
    };

    if (const std::string vehicle = vehicleFromFname(effectiveFname); !vehicle.empty())
        return vehicle;
    if (const std::string vehicle = vehicleFromFname(classFname); !vehicle.empty())
        return vehicle;

    std::string dataAssetFname;
    if (actor != 0)
        dataAssetFname = GetActorDataAssetFName(actor);

    if (!classFname.empty()) {
        if (const std::string kw = DirectContainerKeywordLabel({}, classFname, dataAssetFname);
            !kw.empty() && !IsGenericWorldEspLabel(kw) && !IsJunkWorldEspLabel(kw))
            return kw;
        if (const std::string fromSocket = ResolveSocketContainerDisplayName(classFname);
            !fromSocket.empty() && !IsGenericWorldEspLabel(fromSocket)
            && !IsJunkWorldEspLabel(fromSocket))
            return fromSocket;
        if (const std::string fromClass = ResolveContainerDisplayLabel(classFname, {});
            !fromClass.empty() && !IsGenericWorldEspLabel(fromClass)
            && !IsJunkWorldEspLabel(fromClass))
            return fromClass;
    }

    // Fname / asset / CSV before hover memory so UI-meta garbage cannot win.
    if (const std::string resolved = ResolveWorldLabel(actor, effectiveFname);
        !resolved.empty() && !IsGenericWorldEspLabel(resolved)
        && !IsJunkWorldEspLabel(resolved) && IsPlausibleEspLabel(resolved)
        && !IsGarbledEspLabel(resolved))
        return resolved;

    if (!effectiveFname.empty()) {
        if (const std::string fromWorld = LookupWorldObjectByFName(effectiveFname);
            !fromWorld.empty() && !IsGenericWorldEspLabel(fromWorld)
            && !IsJunkWorldEspLabel(fromWorld))
            return FormatEspDisplayLabel(fromWorld);
        if (const std::string fromAsset = LookupByAssetName(effectiveFname);
            !fromAsset.empty() && !IsGenericWorldEspLabel(fromAsset)
            && !IsJunkWorldEspLabel(fromAsset))
            return FormatEspDisplayLabel(fromAsset);
        if (const std::string human = HumanizeActorFName(effectiveFname);
            !human.empty() && !IsGenericWorldEspLabel(human)
            && !IsJunkWorldEspLabel(human) && IsPlausibleEspLabel(human)
            && !IsGarbledEspLabel(human))
            return human;
    }

    if (!dataAssetFname.empty()) {
        if (const std::string fromAsset = LookupByAssetName(dataAssetFname);
            !fromAsset.empty() && !IsGenericWorldEspLabel(fromAsset)
            && !IsJunkWorldEspLabel(fromAsset))
            return FormatEspDisplayLabel(fromAsset);
        if (const std::string fromWorld = LookupWorldObjectByFName(dataAssetFname);
            !fromWorld.empty() && !IsGenericWorldEspLabel(fromWorld)
            && !IsJunkWorldEspLabel(fromWorld))
            return FormatEspDisplayLabel(fromWorld);
    }

    if (actor != 0) {
        if (const std::string mem = engine.GetEnglishItemName(actor); !mem.empty()
            && !IsGenericWorldEspLabel(mem) && !IsJunkWorldEspLabel(mem)
            && IsPlausibleEspLabel(mem) && !IsGarbledEspLabel(mem))
            return mem;
    }

    const auto cat = static_cast<WorldItemCategory>(worldCategory);
    if (cat == WorldItemCategory::Harvestable)
        return "Harvestable";

    return {};
}
