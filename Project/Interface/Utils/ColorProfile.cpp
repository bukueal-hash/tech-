#include "ColorProfile.h"

#include "Variables/index.h"
#include "../../Core/ColorProfiles.hpp"
#include "AutoConfig.h"
#include "UpgradeOptions.hpp"

#include <Windows.h>

#include <fstream>
#include <string>

namespace {

using ColorProfiles::Doc;
using ColorProfiles::Rgba;
using ColorProfiles::Slot;

struct Entry {
    const char* key;
    float* target;  // first element of a float[4] var::
};

// Every color the overlay owns, by the same name AutoConfig persists them
// under — so a profile key and an auto_config key are interchangeable.
// `crosshair_color` does not match the color_/esp_color_/bot_color_ prefixes
// the rest of the table uses, which is why this is a hand-written list rather
// than something generated from the headers.
const Entry* Table(int& outCount)
{
    static const Entry kTable[] = {
        { "bot_color_invisible",        var::bot_color_invisible },
        { "bot_color_occluded",          var::bot_color_occluded },
        { "bot_color_visible",          var::bot_color_visible },
        { "color_arc_entities",         var::color_arc_entities },
        { "color_dead_bots",            var::color_dead_bots },
        { "color_dropped_items",        var::color_dropped_items },
        { "color_hatches",              var::color_hatches },
        { "color_look_arrow",           var::color_look_arrow },
        { "color_loot",                 var::color_loot },
        { "color_raider_stock",         var::color_raider_stock },
        { "color_world_ammo",           var::color_world_ammo },
        { "color_world_arc_loot",       var::color_world_arc_loot },
        { "color_world_backpack",       var::color_world_backpack },
        { "color_world_buried",         var::color_world_buried },
        { "color_world_corpses",        var::color_world_corpses },
        { "color_world_crate",          var::color_world_crate },
        { "color_world_deaddrop",       var::color_world_deaddrop },
        { "color_world_field_crate",    var::color_world_field_crate },
        { "color_world_furniture",      var::color_world_furniture },
        { "color_world_grenade",        var::color_world_grenade },
        { "color_world_harvestable",    var::color_world_harvestable },
        { "color_world_industrial",     var::color_world_industrial },
        { "color_world_items",          var::color_world_items },
        { "color_world_keys",           var::color_world_keys },
        { "color_world_locker",         var::color_world_locker },
        { "color_world_medical",        var::color_world_medical },
        { "color_world_open_container", var::color_world_open_container },
        { "color_world_other",          var::color_world_other },
        { "color_world_probe",          var::color_world_probe },
        { "color_world_safe",           var::color_world_safe },
        { "color_world_supply_station", var::color_world_supply_station },
        { "color_world_trash",          var::color_world_trash },
        { "color_world_vehicles",       var::color_world_vehicles },
        { "color_world_weapon_case",    var::color_world_weapon_case },
        { "color_world_weapon_rack",    var::color_world_weapon_rack },
        { "crosshair_color",            var::crosshair_color },
        { "head_dot_color",             var::head_dot_color },
        { "player_box_fill_color",      var::player_box_fill_color },
        { "crosshair_outline_color",    var::crosshair_outline_color },
        { "crosshair_accent_color",     var::crosshair_accent_color },
        { "offscreen_arrow_color",      UpgradeOptions::arrowColor },
        { "esp_color_invisible",        var::esp_color_invisible },
        { "esp_color_visible",          var::esp_color_visible },
    };
    outCount = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
    return kTable;
}

// Same narrow-module-path convention as AutoConfig::ConfigPath() and
// PoiStore::FilePath(): plain std::ifstream keeps file IO uniform across the
// app instead of mixing wide and narrow open styles.
std::string FilePath()
{
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
        return "Data\\color_profiles.json";
    std::string dir(path);
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos)
        dir.resize(slash + 1);
    return dir + "Data\\color_profiles.json";
}

int& ActiveSlotRef()
{
    return var::color_profile_slot;
}

bool& LoadedRef()
{
    static bool loaded = false;
    return loaded;
}

bool& LoadWritableRef()
{
    // Failed/invalid existing user data must not be replaced by a new capture.
    static bool writable = false;
    return writable;
}

Doc& SlotsRef()
{
    static Doc slots;
    return slots;
}

bool IsValidSlot(int slot)
{
    return slot >= 0 && slot < ColorProfiles::kSlotCount;
}

} // namespace

namespace ColorProfile {

int ActiveSlot()
{
    return ActiveSlotRef();
}

void SetActiveSlot(int slot)
{
    ActiveSlotRef() = IsValidSlot(slot) ? slot : -1;
    AutoConfig_MarkDirty();
}

void EnsureLoaded()
{
    if (LoadedRef())
        return;
    // Set first: a missing file must not be retried every frame.
    LoadedRef() = true;

    const std::string path = FilePath();
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            LoadWritableRef() = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        return;
    }
    constexpr std::streamoff kMaxProfileBytes = 1024 * 1024;
    const auto size = in.tellg();
    if (size < std::streampos(0) || size > std::streampos(kMaxProfileBytes))
        return;
    in.seekg(0);
    if (!in)
        return;
    // Read only the measured size: a concurrently growing file must not turn
    // the load budget into an unbounded stream copy. Reject short/grown reads.
    std::string text(static_cast<size_t>(size), '\0');
    if (!text.empty())
        in.read(&text[0], static_cast<std::streamsize>(text.size()));
    if (!in || in.peek() != std::char_traits<char>::eof() || in.bad())
        return;
    const auto root = nlohmann::json::parse(text, nullptr, false);
    if (!root.is_array() || root.size() > static_cast<size_t>(ColorProfiles::kSlotCount))
        return;
    for (const auto& slot : root) {
        if (!slot.is_object())
            return;
        const auto colors = slot.find("colors");
        if (colors != slot.end() && !colors->is_object())
            return;
        const auto name = slot.find("name");
        if (name != slot.end() && !name->is_string())
            return;
        if (colors != slot.end()) {
            for (const auto& color : colors->items()) {
                Rgba value{};
                if (color.key().empty() || !ColorProfiles::detail::ToRgba(color.value(), value))
                    return;
            }
        }
    }
    SlotsRef() = ColorProfiles::Parse(text);
    LoadWritableRef() = true;
}

bool SaveToDisk()
{
    EnsureLoaded();
    if (!LoadWritableRef())
        return false;
    const std::string text = ColorProfiles::Serialize(SlotsRef());
    // A saved document must fit the same budget used on the next launch.
    if (text.empty() || text.size() >= 1024u * 1024u)
        return false;

    const std::string path = FilePath();
    const std::string temporary = path + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << text << '\n';
    out.flush();
    const bool written = out.good();
    out.close();
    if (!written || out.fail()) {
        DeleteFileA(temporary.c_str());
        return false;
    }
    if (!MoveFileExA(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary.c_str());
        return false;
    }
    return true;
}

void ApplySlot(int slot)
{
    if (!IsValidSlot(slot))
        return;
    EnsureLoaded();
    if (slot >= static_cast<int>(SlotsRef().size()))
        return;

    int count = 0;
    const Entry* table = Table(count);
    const auto& colors = SlotsRef()[static_cast<size_t>(slot)].colors;
    for (int i = 0; i < count; ++i) {
        const auto it = colors.find(table[i].key);
        if (it == colors.end())
            continue;   // older profile: leave this color alone
        for (int c = 0; c < ColorProfiles::kRgba; ++c)
            table[i].target[c] = it->second[static_cast<size_t>(c)];
    }
    AutoConfig_MarkDirty();
}

bool CaptureSlot(int slot)
{
    if (!IsValidSlot(slot))
        return false;
    EnsureLoaded();
    if (!LoadWritableRef())
        return false;
    const Doc previous = SlotsRef();
    if (static_cast<int>(SlotsRef().size()) <= slot)
        SlotsRef().resize(static_cast<size_t>(slot) + 1);

    Slot& s = SlotsRef()[static_cast<size_t>(slot)];
    if (s.name.empty())
        s.name = "Slot " + std::to_string(slot + 1);

    s.colors.clear();
    int count = 0;
    const Entry* table = Table(count);
    for (int i = 0; i < count; ++i) {
        Rgba rgba{};
        for (int c = 0; c < ColorProfiles::kRgba; ++c)
            rgba[static_cast<size_t>(c)] = table[i].target[c];
        s.colors.emplace(table[i].key, rgba);
    }

    if (!SaveToDisk()) {
        SlotsRef() = previous;
        return false;
    }
    AutoConfig_MarkDirty();
    return true;
}

bool ClearSlot(int slot)
{
    if (!IsValidSlot(slot))
        return false;
    EnsureLoaded();
    if (slot >= static_cast<int>(SlotsRef().size()))
        return false;

    if (!LoadWritableRef())
        return false;
    const Slot previous = SlotsRef()[static_cast<size_t>(slot)];
    SlotsRef()[static_cast<size_t>(slot)] = Slot{};
    if (!SaveToDisk()) {
        SlotsRef()[static_cast<size_t>(slot)] = previous;
        return false;
    }
    if (ActiveSlotRef() == slot)
        ActiveSlotRef() = -1;
    AutoConfig_MarkDirty();
    return true;
}

std::string SlotName(int slot)
{
    EnsureLoaded();
    if (!IsValidSlot(slot) || slot >= static_cast<int>(SlotsRef().size()))
        return {};
    return SlotsRef()[static_cast<size_t>(slot)].name;
}

int ColorCount()
{
    int count = 0;
    (void)Table(count);
    return count;
}

} // namespace ColorProfile
