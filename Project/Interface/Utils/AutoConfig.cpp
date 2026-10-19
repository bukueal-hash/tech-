#include "AutoConfig.h"
#include "Variables/index.h"
#include "Visuals/CrosshairConfig.hpp"
#include "UpgradeOptions.hpp"
#include "../../Core/EntityDiagnostics.hpp"
#include "../../Core/EntityFilters.hpp"
#include "../../Core/WorldItemCategory.h"
#include "../../Hardware/KmBox.h"
#include "../OverlayHost.h"
#include "../Render.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <locale>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char* kConfigFileName = "auto_config.ini";
constexpr int kSaveDelayMs = 500;

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

std::string ConfigPath()
{
    return ExeDirectory() + kConfigFileName;
}

bool ParseColor4(const std::string& val, float out[4])
{
    float r = 0.f, g = 0.f, b = 0.f, a = 1.f;
    char comma1 = 0, comma2 = 0, comma3 = 0;
    std::istringstream input(val);
    input.imbue(std::locale::classic());
    if (!(input >> r >> comma1 >> g >> comma2 >> b >> comma3 >> a)
        || comma1 != ',' || comma2 != ',' || comma3 != ',')
        return false;
    input >> std::ws;
    if (!input.eof())
        return false;
    if (!std::isfinite(r) || !std::isfinite(g)
        || !std::isfinite(b) || !std::isfinite(a))
        return false;
    r = std::clamp(r, 0.f, 1.f);
    g = std::clamp(g, 0.f, 1.f);
    b = std::clamp(b, 0.f, 1.f);
    a = std::clamp(a, 0.f, 1.f);
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
    return true;
}

void WriteColor4(std::ostream& file, const char* key, const float col[4])
{
    file << key << '=' << col[0] << ',' << col[1] << ',' << col[2] << ',' << col[3] << '\n';
}

bool ColorsEqual(const float a[4], const float b[4])
{
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(a[i] - b[i]) > 0.0001f)
            return false;
    }
    return true;
}

void TrimInPlace(std::string& s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    if (i > 0)
        s.erase(0, i);
}

struct Snapshot {
    bool enableesp{};
    bool box{};
    bool show_head_dot{};
    float color_hatches[4]{};
    float head_dot_radius_px{};
    float head_dot_color[4]{};
    int player_box_style{};
    bool player_box_outline{};
    bool player_box_fill{};
    float player_box_thickness{};
    float player_box_fill_color[4]{};
    bool health{};
    bool names{};
    bool show_weapon{};
    bool show_dbno_badge{};
    bool show_look_arrows{};
    bool show_steam_ids{};
    float color_look_arrow[4]{};
    bool snaplines{};
    bool skeleton{};
    bool silhouette{};
    bool silhouette_soft_fill{};
    float silhouette_max_distance_m{};
    bool show_distance{};
    bool show_squad_idx{};
    bool hide_allies{};
    bool show_radar{};
    bool show_debug_overlay{};
    bool debug_skeleton_lag{};
    bool debug_hatch_detect{};
    bool debug_ghost_bots{};
    bool debug_world_trace{};
    bool debug_esp_paint{};
    float esp_text_scale{};
    float radar_scale{};
    float radar_range{};
    float radar_pos_x_norm{};
    float radar_pos_y_norm{};
    int radar_loot_min_rarity{};
    bool show_radar_special{};
    bool show_raid_hud{};
    bool show_activity_feed{};
    bool show_objectives{};
    bool radar_shape_circle{};
    bool radar_map_mode{};
    bool radar_underground_dim{};
    bool radar_ally_arrows{};
    float radar_dot_scale{};
    bool radar_show_altitude{};
    bool radar_compass{};
    int radar_focus{};
    bool radar_ghost{};
    float radar_ghost_secs{};
    bool radar_ghost_suppress_poi{};
    bool show_pois{};
    bool poi_show_labels{};
    int poi_categories{};
    bool show_search_window{};
    int color_profile_slot{};
    float esp_distance{};
    float esp_color_visible[4]{};
    // (color_look_arrow lives next to its toggle in the block above)
    float esp_color_invisible[4]{};

    bool bot_box{};
    bool bot_names{};
    bool bot_snaplines{};
    bool esp_entity_debug{};
    bool bot_show_distance{};
    bool bot_heart{};
    bool show_dead_bots{};
    float bot_esp_distance{};
    float color_dead_bots[4]{};
    float bot_color_visible[4]{};
    float bot_color_invisible[4]{};

    bool enable_aimbot{};
    int aim_hold_key{};
    // Phase 1-4 aim settings.
    bool aim_autofire{};
    int aim_radius{};
    float aim_stop_zone{};
    int aim_speed{};
    int aim_response{};
    float aim_calib_x{};
    float aim_calib_y{};
    int aim_priority{};
    int aim_bone{};
    int aim_point_percent{};
    int aim_max_distance{};
    bool aim_sticky{};
    bool aim_require_enemy{};
    bool aim_require_visible{};
    bool aim_require_bones{};
    int aim_frame_fresh_ms{};
    int aim_autofire_radius{};
    int aim_autofire_interval{};
    int aim_click_hold{};
    bool aim_telemetry{};
    bool aim_deadzone{};
    bool aim_jitter{};
    bool aim_close_range{};
    float aim_close_distance{};
    float aim_recoil_comp{};
    bool aim_show_circle{};
    int debug_overlay_key{};
    bool show_crosshair{};
    bool camPovUsePending{};
    std::string upgrade_options;
    std::string entity_filters;
    std::string crosshair_options;
    int crosshair_style{};
    float crosshair_color[4]{};
    float crosshair_size{};
    float crosshair_thickness{};
    float crosshair_gap{};
    float crosshair_spin_rpm{};
    bool enable_world{};
    bool droppedItems{};
    bool raiderStock{};
    bool showRobots{};
    bool showHatches{};
    bool showArc{};
    bool showDeadPlayers{};
    bool show_world_items{};
    bool show_world_ammo{};
    bool show_world_arc_loot{};
    bool show_world_backpack{};
    bool show_world_crate{};
    bool show_world_furniture{};
    bool show_world_grenade{};
    bool show_world_harvestable{};
    bool show_world_industrial{};
    bool show_world_medical{};
    bool show_world_other{};
    bool show_world_probe{};
    bool show_world_vehicles{};
    bool show_world_weapon_case{};
    bool show_world_weapon_rack{};
    bool show_world_field_crate{};
    bool show_world_supply_station{};
    bool show_world_keys{};
    bool show_world_locker{};
    bool show_world_trash{};
    bool show_world_safe{};
    bool show_world_buried{};
    bool show_world_deaddrop{};
    bool show_world_open_container{};
    float color_dropped_items[4]{};
    float color_raider_stock[4]{};
    float color_arc_entities[4]{};
    float color_world_corpses[4]{};
    float color_world_items[4]{};
    float color_world_ammo[4]{};
    float color_world_arc_loot[4]{};
    float color_world_backpack[4]{};
    float color_world_crate[4]{};
    float color_world_furniture[4]{};
    float color_world_grenade[4]{};
    float color_world_harvestable[4]{};
    float color_world_industrial[4]{};
    float color_world_medical[4]{};
    float color_world_other[4]{};
    float color_world_probe[4]{};
    float color_world_vehicles[4]{};
    float color_world_weapon_case[4]{};
    float color_world_weapon_rack[4]{};
    float color_world_field_crate[4]{};
    float color_world_supply_station[4]{};
    float color_world_keys[4]{};
    float color_world_locker[4]{};
    float color_world_trash[4]{};
    float color_world_safe[4]{};
    float color_world_buried[4]{};
    float color_world_deaddrop[4]{};
    float color_world_open_container[4]{};

    bool showLoot{};
    bool show_world_quest{};
    bool quest_pulse{};
    bool show_near_loot_hud{};
    float loot_distance{};
    float container_distance_sp{};
    float color_loot[4]{};
    bool loot_rarity_color{};
    bool show_loot_value{};
    bool show_crate_contents{};
    bool show_stack_counts{};
    bool grey_looted_containers{};
    int state_opened_mode{};
    int state_dead_mode{};
    float state_dim_alpha{};
    bool show_armor_line{};
    bool show_player_kit{};
    bool show_bot_loadout{};
    bool show_bot_vision{};
    bool show_bot_alertness{};
    bool show_bot_parts{};
    float loot_min_value{};
    int loot_min_rarity{};
    bool loot_min_val_sp{};
    bool loot_min_rar_sp{};
    bool showmenu{};

    std::string kmbox_type;
    std::string kmbox_comPort;
    std::string kmbox_baudRate;
    std::string kmbox_ip;
    std::string kmbox_port;
    std::string kmbox_uuid;
    int kmbox_minDelay{};
    int kmbox_monitorIndex{};
};

Snapshot s_last{};
bool s_snapshotValid = false;
bool s_hadFileOnLoad = false;
bool s_dirty = false;
bool s_anyWorldColorKeyInIni = false;
std::chrono::steady_clock::time_point s_dirtySince{};

void MigrateWorldColorsFromLoot()
{
    if (s_anyWorldColorKeyInIni)
        return;

    float* const colors[] = {
        var::color_world_items,
        var::color_world_ammo,
        var::color_world_arc_loot,
        var::color_world_backpack,
        var::color_world_crate,
        var::color_world_furniture,
        var::color_world_grenade,
        var::color_world_harvestable,
        var::color_world_industrial,
        var::color_world_medical,
        var::color_world_other,
        var::color_world_probe,
        var::color_world_vehicles,
        var::color_world_weapon_case,
        var::color_world_weapon_rack,
        var::color_world_field_crate,
        var::color_world_supply_station,
        var::color_world_keys,
        var::color_world_locker,
        var::color_world_trash,
        var::color_world_open_container,
        var::color_world_safe,
        var::color_world_buried,
        var::color_world_deaddrop,
    };
    for (float* c : colors)
        std::memcpy(c, var::color_loot, sizeof(float) * 4);
}

Snapshot CaptureSnapshot()
{
    Snapshot s{};
    s.enableesp = var::enableesp;
    s.box = var::box;
    s.show_head_dot = var::show_head_dot;
    s.radar_ghost_suppress_poi = var::radar_ghost_suppress_poi;
    s.show_search_window = var::show_search_window;
    std::copy_n(var::color_hatches, 4, s.color_hatches);
    s.head_dot_radius_px = var::head_dot_radius_px;
    std::copy_n(var::head_dot_color, 4, s.head_dot_color);
    s.player_box_style = var::player_box_style;
    s.player_box_outline = var::player_box_outline;
    s.player_box_fill = var::player_box_fill;
    s.player_box_thickness = var::player_box_thickness;
    std::copy_n(var::player_box_fill_color, 4, s.player_box_fill_color);
    s.health = var::health;
    s.names = var::names;
    s.show_weapon = var::show_weapon;
    s.show_dbno_badge = var::show_dbno_badge;
    s.show_look_arrows = var::show_look_arrows;
    s.show_steam_ids = var::show_steam_ids;
    std::memcpy(s.color_look_arrow, var::color_look_arrow, sizeof(s.color_look_arrow));
    s.snaplines = var::snaplines;
    s.skeleton = var::skeleton;
    s.silhouette = var::silhouette;
    s.silhouette_soft_fill = var::silhouette_soft_fill;
    s.silhouette_max_distance_m = var::silhouette_max_distance_m;
    s.show_distance = var::show_distance;
    s.show_squad_idx = var::show_squad_idx;
    s.hide_allies = var::hide_allies;
    s.show_radar = var::show_radar;
    s.show_debug_overlay = var::show_debug_overlay;
    s.debug_skeleton_lag = var::debug_skeleton_lag;
    s.debug_hatch_detect = var::debug_hatch_detect;
    s.debug_ghost_bots = var::debug_ghost_bots;
    s.debug_world_trace = var::debug_world_trace;
    s.debug_esp_paint = var::debug_esp_paint;
    s.esp_text_scale = var::esp_text_scale;
    s.radar_scale = var::radar_scale;
    s.radar_range = var::radar_range;
    s.radar_pos_x_norm = var::radar_pos_x_norm;
    s.radar_pos_y_norm = var::radar_pos_y_norm;
    s.radar_loot_min_rarity = var::radar_loot_min_rarity;
    s.show_radar_special = var::show_radar_special;
    s.show_raid_hud = var::show_raid_hud;
    s.show_activity_feed = var::show_activity_feed;
    s.show_objectives = var::show_objectives;
    s.radar_shape_circle = var::radar_shape_circle;
    s.radar_map_mode = var::radar_map_mode;
    s.radar_underground_dim = var::radar_underground_dim;
    s.radar_ally_arrows = var::radar_ally_arrows;
    s.radar_dot_scale = var::radar_dot_scale;
    s.radar_show_altitude = var::radar_show_altitude;
    s.radar_compass = var::radar_compass;
    s.radar_focus = var::radar_focus;
    s.radar_ghost = var::radar_ghost;
    s.radar_ghost_secs = var::radar_ghost_secs;
    s.show_pois = var::show_pois;
    s.poi_show_labels = var::poi_show_labels;
    s.poi_categories = var::poi_categories;
    s.color_profile_slot = var::color_profile_slot;
    s.esp_distance = var::esp_distance;
    std::memcpy(s.esp_color_visible, var::esp_color_visible, sizeof(s.esp_color_visible));
    std::memcpy(s.esp_color_invisible, var::esp_color_invisible, sizeof(s.esp_color_invisible));
    s.bot_box = var::bot_box;
    s.bot_names = var::bot_names;
    s.bot_snaplines = var::bot_snaplines;
    s.esp_entity_debug = var::esp_entity_debug;
    s.bot_show_distance = var::bot_show_distance;
    s.bot_heart = var::bot_heart;
    s.show_dead_bots = var::show_dead_bots;
    s.bot_esp_distance = var::bot_esp_distance;
    std::memcpy(s.bot_color_visible, var::bot_color_visible, sizeof(s.bot_color_visible));
    std::memcpy(s.bot_color_invisible, var::bot_color_invisible, sizeof(s.bot_color_invisible));
    std::memcpy(s.color_dead_bots, var::color_dead_bots, sizeof(s.color_dead_bots));

    s.enable_aimbot = var::enable_aimbot;
    s.aim_hold_key = var::aim_hold_key;
    s.aim_autofire = var::aim_autofire;
    s.aim_radius = var::aim_radius;
    s.aim_stop_zone = var::aim_stop_zone;
    s.aim_speed = var::aim_speed;
    s.aim_response = var::aim_response;
    s.aim_calib_x = var::aim_calib_x;
    s.aim_calib_y = var::aim_calib_y;
    s.aim_priority = var::aim_priority;
    s.aim_bone = var::aim_bone;
    s.aim_point_percent = var::aim_point_percent;
    s.aim_max_distance = var::aim_max_distance;
    s.aim_sticky = var::aim_sticky;
    s.aim_require_enemy = var::aim_require_enemy;
    s.aim_require_visible = var::aim_require_visible;
    s.aim_require_bones = var::aim_require_bones;
    s.aim_frame_fresh_ms = var::aim_frame_fresh_ms;
    s.aim_autofire_radius = var::aim_autofire_radius;
    s.aim_autofire_interval = var::aim_autofire_interval;
    s.aim_click_hold = var::aim_click_hold;
    s.aim_telemetry = var::aim_telemetry;
    s.aim_deadzone = var::aim_deadzone;
    s.aim_jitter = var::aim_jitter;
    s.aim_close_range = var::aim_close_range;
    s.aim_close_distance = var::aim_close_distance;
    s.aim_recoil_comp = var::aim_recoil_comp;
    s.aim_show_circle = var::aim_show_circle;
    s.debug_overlay_key = var::debug_overlay_key;
    s.show_crosshair = var::show_crosshair;
    s.camPovUsePending = var::camPovUsePending;
    s.upgrade_options = UpgradeOptions::Capture();
    s.entity_filters = EntityFilters::Capture();
    s.crosshair_options = ArcCrosshair::CaptureOptions();
    s.crosshair_style = var::crosshair_style;
    std::memcpy(s.crosshair_color, var::crosshair_color, sizeof(s.crosshair_color));
    s.crosshair_size = var::crosshair_size;
    s.crosshair_thickness = var::crosshair_thickness;
    s.crosshair_gap = var::crosshair_gap;
    s.crosshair_spin_rpm = var::crosshair_spin_rpm;
    s.enable_world = var::enable_world;
    s.droppedItems = var::droppedItems;
    s.raiderStock = var::raiderStock;
    s.showRobots = var::showRobots;
    s.showHatches = var::showHatches;
    s.showArc = var::showArc;
    s.showDeadPlayers = var::showDeadPlayers;
    s.show_world_items = var::show_world_items;
    s.show_world_ammo = var::show_world_ammo;
    s.show_world_arc_loot = var::show_world_arc_loot;
    s.show_world_backpack = var::show_world_backpack;
    s.show_world_crate = var::show_world_crate;
    s.show_world_furniture = var::show_world_furniture;
    s.show_world_grenade = var::show_world_grenade;
    s.show_world_harvestable = var::show_world_harvestable;
    s.show_world_industrial = var::show_world_industrial;
    s.show_world_medical = var::show_world_medical;
    s.show_world_other = var::show_world_other;
    s.show_world_probe = var::show_world_probe;
    s.show_world_vehicles = var::show_world_vehicles;
    s.show_world_weapon_case = var::show_world_weapon_case;
    s.show_world_weapon_rack = var::show_world_weapon_rack;
    s.show_world_field_crate = var::show_world_field_crate;
    s.show_world_supply_station = var::show_world_supply_station;
    s.show_world_keys = var::show_world_keys;
    s.show_world_locker = var::show_world_locker;
    s.show_world_trash = var::show_world_trash;
    s.show_world_safe = var::show_world_safe;
    s.show_world_buried = var::show_world_buried;
    s.show_world_deaddrop = var::show_world_deaddrop;
    s.show_world_open_container = var::show_world_open_container;
    std::memcpy(s.color_dropped_items, var::color_dropped_items, sizeof(s.color_dropped_items));
    std::memcpy(s.color_raider_stock, var::color_raider_stock, sizeof(s.color_raider_stock));
    std::memcpy(s.color_arc_entities, var::color_arc_entities, sizeof(s.color_arc_entities));
    std::memcpy(s.color_world_corpses, var::color_world_corpses, sizeof(s.color_world_corpses));
    std::memcpy(s.color_world_items, var::color_world_items, sizeof(s.color_world_items));
    std::memcpy(s.color_world_ammo, var::color_world_ammo, sizeof(s.color_world_ammo));
    std::memcpy(s.color_world_arc_loot, var::color_world_arc_loot, sizeof(s.color_world_arc_loot));
    std::memcpy(s.color_world_backpack, var::color_world_backpack, sizeof(s.color_world_backpack));
    std::memcpy(s.color_world_crate, var::color_world_crate, sizeof(s.color_world_crate));
    std::memcpy(s.color_world_furniture, var::color_world_furniture, sizeof(s.color_world_furniture));
    std::memcpy(s.color_world_grenade, var::color_world_grenade, sizeof(s.color_world_grenade));
    std::memcpy(s.color_world_harvestable, var::color_world_harvestable, sizeof(s.color_world_harvestable));
    std::memcpy(s.color_world_industrial, var::color_world_industrial, sizeof(s.color_world_industrial));
    std::memcpy(s.color_world_medical, var::color_world_medical, sizeof(s.color_world_medical));
    std::memcpy(s.color_world_other, var::color_world_other, sizeof(s.color_world_other));
    std::memcpy(s.color_world_probe, var::color_world_probe, sizeof(s.color_world_probe));
    std::memcpy(s.color_world_vehicles, var::color_world_vehicles, sizeof(s.color_world_vehicles));
    std::memcpy(s.color_world_weapon_case, var::color_world_weapon_case, sizeof(s.color_world_weapon_case));
    std::memcpy(s.color_world_weapon_rack, var::color_world_weapon_rack, sizeof(s.color_world_weapon_rack));
    std::memcpy(s.color_world_field_crate, var::color_world_field_crate, sizeof(s.color_world_field_crate));
    std::memcpy(s.color_world_supply_station, var::color_world_supply_station, sizeof(s.color_world_supply_station));
    std::memcpy(s.color_world_keys, var::color_world_keys, sizeof(s.color_world_keys));
    std::memcpy(s.color_world_locker, var::color_world_locker, sizeof(s.color_world_locker));
    std::memcpy(s.color_world_trash, var::color_world_trash, sizeof(s.color_world_trash));
    std::memcpy(s.color_world_safe, var::color_world_safe, sizeof(s.color_world_safe));
    std::memcpy(s.color_world_buried, var::color_world_buried, sizeof(s.color_world_buried));
    std::memcpy(s.color_world_deaddrop, var::color_world_deaddrop, sizeof(s.color_world_deaddrop));
    std::memcpy(s.color_world_open_container, var::color_world_open_container, sizeof(s.color_world_open_container));

    s.showLoot = var::showLoot;
    s.show_world_quest = var::show_world_quest;
    s.quest_pulse = var::quest_pulse;
    s.show_near_loot_hud = var::show_near_loot_hud;
    s.loot_distance = var::loot_distance;
    s.container_distance_sp = var::container_distance_sp;
    std::memcpy(s.color_loot, var::color_loot, sizeof(s.color_loot));
    s.loot_rarity_color = var::loot_rarity_color;
    s.show_loot_value = var::show_loot_value;
    s.show_crate_contents = var::show_crate_contents;
    s.show_stack_counts = var::show_stack_counts;
    s.grey_looted_containers = var::grey_looted_containers;
    s.state_opened_mode = var::state_opened_mode;
    s.state_dead_mode = var::state_dead_mode;
    s.state_dim_alpha = var::state_dim_alpha;
    s.show_armor_line = var::show_armor_line;
    s.show_player_kit = var::show_player_kit;
    s.show_bot_loadout = var::show_bot_loadout;
    s.show_bot_vision = var::show_bot_vision;
    s.show_bot_alertness = var::show_bot_alertness;
    s.show_bot_parts = var::show_bot_parts;
    s.loot_min_value = var::loot_min_value;
    s.loot_min_rarity = var::loot_min_rarity;
    s.loot_min_val_sp = var::loot_min_val_sp;
    s.loot_min_rar_sp = var::loot_min_rar_sp;

    s.showmenu = showmenu;

    s.kmbox_type = g_kmbox.kmboxConfig.type;
    s.kmbox_comPort = g_kmbox.kmboxConfig.comPort;
    s.kmbox_baudRate = g_kmbox.kmboxConfig.baudRate;
    s.kmbox_ip = g_kmbox.kmboxConfig.ip;
    s.kmbox_port = g_kmbox.kmboxConfig.port;
    s.kmbox_uuid = g_kmbox.kmboxConfig.uuid;
    s.kmbox_minDelay = g_kmbox.kmboxConfig.minDelay;
    s.kmbox_monitorIndex = g_kmbox.kmboxConfig.monitorIndex;
    return s;
}

bool SnapshotsEqual(const Snapshot& a, const Snapshot& b)
{
    return a.enableesp == b.enableesp &&
        a.box == b.box &&
        a.radar_ghost_suppress_poi == b.radar_ghost_suppress_poi &&
        a.show_search_window == b.show_search_window &&
        ColorsEqual(a.color_hatches, b.color_hatches) &&
        a.show_head_dot == b.show_head_dot &&
        a.head_dot_radius_px == b.head_dot_radius_px &&
        ColorsEqual(a.head_dot_color, b.head_dot_color) &&
        a.player_box_style == b.player_box_style &&
        a.player_box_outline == b.player_box_outline &&
        a.player_box_fill == b.player_box_fill &&
        a.player_box_thickness == b.player_box_thickness &&
        ColorsEqual(a.player_box_fill_color, b.player_box_fill_color) &&
        a.health == b.health &&
        a.names == b.names &&
        a.show_weapon == b.show_weapon &&
        a.show_dbno_badge == b.show_dbno_badge &&
        a.show_look_arrows == b.show_look_arrows &&
        a.show_steam_ids == b.show_steam_ids &&
        ColorsEqual(a.color_look_arrow, b.color_look_arrow) &&
        a.snaplines == b.snaplines &&
        a.skeleton == b.skeleton &&
        a.silhouette == b.silhouette &&
        a.silhouette_soft_fill == b.silhouette_soft_fill &&
        a.silhouette_max_distance_m == b.silhouette_max_distance_m &&
        a.show_distance == b.show_distance &&
        a.show_squad_idx == b.show_squad_idx &&
        a.hide_allies == b.hide_allies &&
        a.show_radar == b.show_radar &&
        a.show_debug_overlay == b.show_debug_overlay &&
        a.debug_skeleton_lag == b.debug_skeleton_lag &&
        a.debug_hatch_detect == b.debug_hatch_detect &&
        a.debug_ghost_bots == b.debug_ghost_bots &&
        a.debug_world_trace == b.debug_world_trace &&
        a.debug_esp_paint == b.debug_esp_paint &&
        a.esp_text_scale == b.esp_text_scale &&
        a.radar_scale == b.radar_scale &&
        a.radar_range == b.radar_range &&
        a.radar_pos_x_norm == b.radar_pos_x_norm &&
        a.radar_pos_y_norm == b.radar_pos_y_norm &&
        a.radar_loot_min_rarity == b.radar_loot_min_rarity &&
        a.show_radar_special == b.show_radar_special &&
        a.show_raid_hud == b.show_raid_hud &&
        a.show_activity_feed == b.show_activity_feed &&
        a.show_objectives == b.show_objectives &&
        a.radar_shape_circle == b.radar_shape_circle &&
        a.radar_map_mode == b.radar_map_mode &&
        a.radar_underground_dim == b.radar_underground_dim &&
        a.radar_ally_arrows == b.radar_ally_arrows &&
        a.radar_dot_scale == b.radar_dot_scale &&
        a.radar_show_altitude == b.radar_show_altitude &&
        a.radar_compass == b.radar_compass &&
        a.radar_focus == b.radar_focus &&
        a.radar_ghost == b.radar_ghost &&
        a.radar_ghost_secs == b.radar_ghost_secs &&
        a.show_pois == b.show_pois &&
        a.poi_show_labels == b.poi_show_labels &&
        a.poi_categories == b.poi_categories &&
        a.color_profile_slot == b.color_profile_slot &&
        a.esp_distance == b.esp_distance &&
        ColorsEqual(a.esp_color_visible, b.esp_color_visible) &&
        ColorsEqual(a.esp_color_invisible, b.esp_color_invisible) &&
        a.bot_box == b.bot_box &&
        a.bot_names == b.bot_names &&
        a.bot_snaplines == b.bot_snaplines &&
        a.esp_entity_debug == b.esp_entity_debug &&
        a.bot_show_distance == b.bot_show_distance &&
        a.bot_heart == b.bot_heart &&
        a.show_dead_bots == b.show_dead_bots &&
        a.bot_esp_distance == b.bot_esp_distance &&
        ColorsEqual(a.bot_color_visible, b.bot_color_visible) &&
        ColorsEqual(a.bot_color_invisible, b.bot_color_invisible) &&
        ColorsEqual(a.color_dead_bots, b.color_dead_bots) &&
        a.enable_aimbot == b.enable_aimbot &&
        a.aim_hold_key == b.aim_hold_key &&
        a.aim_autofire == b.aim_autofire &&
        a.aim_radius == b.aim_radius &&
        a.aim_stop_zone == b.aim_stop_zone &&
        a.aim_speed == b.aim_speed &&
        a.aim_response == b.aim_response &&
        a.aim_calib_x == b.aim_calib_x &&
        a.aim_calib_y == b.aim_calib_y &&
        a.aim_priority == b.aim_priority &&
        a.aim_bone == b.aim_bone &&
        a.aim_point_percent == b.aim_point_percent &&
        a.aim_max_distance == b.aim_max_distance &&
        a.aim_sticky == b.aim_sticky &&
        a.aim_require_enemy == b.aim_require_enemy &&
        a.aim_require_visible == b.aim_require_visible &&
        a.aim_require_bones == b.aim_require_bones &&
        a.aim_frame_fresh_ms == b.aim_frame_fresh_ms &&
        a.aim_autofire_radius == b.aim_autofire_radius &&
        a.aim_autofire_interval == b.aim_autofire_interval &&
        a.aim_click_hold == b.aim_click_hold &&
        a.aim_telemetry == b.aim_telemetry &&
        a.aim_deadzone == b.aim_deadzone &&
        a.aim_jitter == b.aim_jitter &&
        a.aim_close_range == b.aim_close_range &&
        a.aim_close_distance == b.aim_close_distance &&
        a.aim_recoil_comp == b.aim_recoil_comp &&
        a.aim_show_circle == b.aim_show_circle &&
        a.debug_overlay_key == b.debug_overlay_key &&
        a.show_crosshair == b.show_crosshair &&
        a.camPovUsePending == b.camPovUsePending &&
        a.upgrade_options == b.upgrade_options &&
        a.entity_filters == b.entity_filters &&
        a.crosshair_options == b.crosshair_options &&
        a.crosshair_style == b.crosshair_style &&
        ColorsEqual(a.crosshair_color, b.crosshair_color) &&
        a.crosshair_size == b.crosshair_size &&
        a.crosshair_thickness == b.crosshair_thickness &&
        a.crosshair_gap == b.crosshair_gap &&
        a.crosshair_spin_rpm == b.crosshair_spin_rpm &&
        a.enable_world == b.enable_world &&
        a.droppedItems == b.droppedItems &&
        a.raiderStock == b.raiderStock &&
        a.showRobots == b.showRobots &&
        a.showHatches == b.showHatches &&
        a.showArc == b.showArc &&
        a.showDeadPlayers == b.showDeadPlayers &&
        a.show_world_items == b.show_world_items &&
        a.show_world_ammo == b.show_world_ammo &&
        a.show_world_arc_loot == b.show_world_arc_loot &&
        a.show_world_backpack == b.show_world_backpack &&
        a.show_world_crate == b.show_world_crate &&
        a.show_world_furniture == b.show_world_furniture &&
        a.show_world_grenade == b.show_world_grenade &&
        a.show_world_harvestable == b.show_world_harvestable &&
        a.show_world_industrial == b.show_world_industrial &&
        a.show_world_medical == b.show_world_medical &&
        a.show_world_other == b.show_world_other &&
        a.show_world_probe == b.show_world_probe &&
        a.show_world_vehicles == b.show_world_vehicles &&
        a.show_world_weapon_case == b.show_world_weapon_case &&
        a.show_world_weapon_rack == b.show_world_weapon_rack &&
        a.show_world_field_crate == b.show_world_field_crate &&
        a.show_world_supply_station == b.show_world_supply_station &&
        a.show_world_keys == b.show_world_keys &&
        a.show_world_locker == b.show_world_locker &&
        a.show_world_trash == b.show_world_trash &&
        a.show_world_safe == b.show_world_safe &&
        a.show_world_buried == b.show_world_buried &&
        a.show_world_deaddrop == b.show_world_deaddrop &&
        a.show_world_open_container == b.show_world_open_container &&
        ColorsEqual(a.color_dropped_items, b.color_dropped_items) &&
        ColorsEqual(a.color_raider_stock, b.color_raider_stock) &&
        ColorsEqual(a.color_arc_entities, b.color_arc_entities) &&
        ColorsEqual(a.color_world_corpses, b.color_world_corpses) &&
        ColorsEqual(a.color_world_items, b.color_world_items) &&
        ColorsEqual(a.color_world_ammo, b.color_world_ammo) &&
        ColorsEqual(a.color_world_arc_loot, b.color_world_arc_loot) &&
        ColorsEqual(a.color_world_backpack, b.color_world_backpack) &&
        ColorsEqual(a.color_world_crate, b.color_world_crate) &&
        ColorsEqual(a.color_world_furniture, b.color_world_furniture) &&
        ColorsEqual(a.color_world_grenade, b.color_world_grenade) &&
        ColorsEqual(a.color_world_harvestable, b.color_world_harvestable) &&
        ColorsEqual(a.color_world_industrial, b.color_world_industrial) &&
        ColorsEqual(a.color_world_medical, b.color_world_medical) &&
        ColorsEqual(a.color_world_other, b.color_world_other) &&
        ColorsEqual(a.color_world_probe, b.color_world_probe) &&
        ColorsEqual(a.color_world_vehicles, b.color_world_vehicles) &&
        ColorsEqual(a.color_world_weapon_case, b.color_world_weapon_case) &&
        ColorsEqual(a.color_world_weapon_rack, b.color_world_weapon_rack) &&
        ColorsEqual(a.color_world_field_crate, b.color_world_field_crate) &&
        ColorsEqual(a.color_world_supply_station, b.color_world_supply_station) &&
        ColorsEqual(a.color_world_keys, b.color_world_keys) &&
        ColorsEqual(a.color_world_locker, b.color_world_locker) &&
        ColorsEqual(a.color_world_trash, b.color_world_trash) &&
        ColorsEqual(a.color_world_safe, b.color_world_safe) &&
        ColorsEqual(a.color_world_buried, b.color_world_buried) &&
        ColorsEqual(a.color_world_deaddrop, b.color_world_deaddrop) &&
        ColorsEqual(a.color_world_open_container, b.color_world_open_container) &&
        a.showLoot == b.showLoot &&
        a.show_world_quest == b.show_world_quest &&
        a.quest_pulse == b.quest_pulse &&
        a.show_near_loot_hud == b.show_near_loot_hud &&
        a.loot_distance == b.loot_distance &&
        a.container_distance_sp == b.container_distance_sp &&
        ColorsEqual(a.color_loot, b.color_loot) &&
        a.loot_rarity_color == b.loot_rarity_color &&
        a.show_loot_value == b.show_loot_value &&
        a.show_crate_contents == b.show_crate_contents &&
        a.show_stack_counts == b.show_stack_counts &&
        a.grey_looted_containers == b.grey_looted_containers &&
        a.state_opened_mode == b.state_opened_mode &&
        a.state_dead_mode == b.state_dead_mode &&
        a.state_dim_alpha == b.state_dim_alpha &&
        a.show_armor_line == b.show_armor_line &&
        a.show_player_kit == b.show_player_kit &&
        a.show_bot_loadout == b.show_bot_loadout &&
        a.show_bot_vision == b.show_bot_vision &&
        a.show_bot_alertness == b.show_bot_alertness &&
        a.show_bot_parts == b.show_bot_parts &&
        a.loot_min_value == b.loot_min_value &&
        a.loot_min_rarity == b.loot_min_rarity &&
        a.loot_min_val_sp == b.loot_min_val_sp &&
        a.loot_min_rar_sp == b.loot_min_rar_sp &&
        a.showmenu == b.showmenu &&
        a.kmbox_type == b.kmbox_type &&
        a.kmbox_comPort == b.kmbox_comPort &&
        a.kmbox_baudRate == b.kmbox_baudRate &&
        a.kmbox_ip == b.kmbox_ip &&
        a.kmbox_port == b.kmbox_port &&
        a.kmbox_uuid == b.kmbox_uuid &&
        a.kmbox_minDelay == b.kmbox_minDelay &&
        a.kmbox_monitorIndex == b.kmbox_monitorIndex;
}

bool ParseBool(const std::string& val, bool defaultVal)
{
    if (val == "1" || val == "true" || val == "yes" || val == "on")
        return true;
    if (val == "0" || val == "false" || val == "no" || val == "off")
        return false;
    return defaultVal;
}

bool ApplyLootConfigKey(const std::string& key, const std::string& val)
{
    if (key == "showLoot") { var::showLoot = ParseBool(val, var::showLoot); return true; }
    if (key == "show_world_quest") {
        var::show_world_quest = ParseBool(val, var::show_world_quest);
        return true;
    }
    if (key == "quest_pulse") {
        var::quest_pulse = ParseBool(val, var::quest_pulse);
        return true;
    }
    if (key == "show_near_loot_hud") {
        var::show_near_loot_hud = ParseBool(val, var::show_near_loot_hud);
        return true;
    }
    if (key == "loot_distance") {
        var::loot_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 20.f, var::kMaxDistanceSliderM);
        return true;
    }
    if (key == "container_distance_sp") {
        var::container_distance_sp = std::clamp(static_cast<float>(std::atof(val.c_str())), 20.f, var::kMaxDistanceSliderM);
        return true;
    }
    if (key.rfind("container_range_sp_", 0) == 0) {
        const std::string suffix = key.substr(19);
        if (TrySetContainerRangeFromConfigSuffix(suffix.c_str(), ParseBool(val, false)))
            return true;
    }
    if (key == "color_loot") { ParseColor4(val, var::color_loot); return true; }
    if (key == "loot_rarity_color") { var::loot_rarity_color = ParseBool(val, var::loot_rarity_color); return true; }
    if (key == "show_loot_value") { var::show_loot_value = ParseBool(val, var::show_loot_value); return true; }
    if (key == "show_crate_contents") { var::show_crate_contents = ParseBool(val, var::show_crate_contents); return true; }
    if (key == "show_stack_counts") { var::show_stack_counts = ParseBool(val, var::show_stack_counts); return true; }
    if (key == "grey_looted_containers") { var::grey_looted_containers = ParseBool(val, var::grey_looted_containers); return true; }
    if (key == "state_opened_mode") { var::state_opened_mode = std::clamp(std::atoi(val.c_str()), 0, 3); return true; }
    if (key == "state_dead_mode") { var::state_dead_mode = std::clamp(std::atoi(val.c_str()), 0, 3); return true; }
    if (key == "state_dim_alpha") { var::state_dim_alpha = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.f, 1.f); return true; }
    if (key == "show_armor_line") { var::show_armor_line = ParseBool(val, var::show_armor_line); return true; }
    if (key == "show_player_kit") { var::show_player_kit = ParseBool(val, var::show_player_kit); return true; }
    if (key == "show_bot_loadout") { var::show_bot_loadout = ParseBool(val, var::show_bot_loadout); return true; }
    if (key == "show_bot_vision") { var::show_bot_vision = ParseBool(val, var::show_bot_vision); return true; }
    if (key == "show_bot_alertness") { var::show_bot_alertness = ParseBool(val, var::show_bot_alertness); return true; }
    if (key == "show_bot_parts") { var::show_bot_parts = ParseBool(val, var::show_bot_parts); return true; }
    if (key == "loot_min_value") { var::loot_min_value = static_cast<float>(std::atof(val.c_str())); return true; }
    if (key == "loot_min_rarity") { var::loot_min_rarity = std::atoi(val.c_str()); return true; }
    if (key == "loot_min_val_sp") { var::loot_min_val_sp = ParseBool(val, var::loot_min_val_sp); return true; }
    if (key == "loot_min_rar_sp") { var::loot_min_rar_sp = ParseBool(val, var::loot_min_rar_sp); return true; }
    return false;
}

static bool ApplyAimbotConfigKey(const std::string& key, const std::string& val)
{
    if (key == "enable_aimbot") {
        var::enable_aimbot = ParseBool(val, var::enable_aimbot);
        return true;
    }
    return false;
}

static bool ApplyWorldColorConfigKey(const std::string& key, const std::string& val)
{
    struct ColorEntry {
        const char* key;
        float* color;
    };
    static const ColorEntry kColors[] = {
        {"color_dropped_items", var::color_dropped_items},
        {"color_raider_stock", var::color_raider_stock},
        {"color_arc_entities", var::color_arc_entities},
        {"color_world_corpses", var::color_world_corpses},
        {"color_world_items", var::color_world_items},
        {"color_world_ammo", var::color_world_ammo},
        {"color_world_arc_loot", var::color_world_arc_loot},
        {"color_world_backpack", var::color_world_backpack},
        {"color_world_crate", var::color_world_crate},
        {"color_world_furniture", var::color_world_furniture},
        {"color_world_grenade", var::color_world_grenade},
        {"color_world_harvestable", var::color_world_harvestable},
        {"color_world_industrial", var::color_world_industrial},
        {"color_world_medical", var::color_world_medical},
        {"color_world_other", var::color_world_other},
        {"color_world_probe", var::color_world_probe},
        {"color_world_vehicles", var::color_world_vehicles},
        {"color_world_weapon_case", var::color_world_weapon_case},
        {"color_world_weapon_rack", var::color_world_weapon_rack},
        {"color_world_field_crate", var::color_world_field_crate},
        {"color_world_supply_station", var::color_world_supply_station},
        {"color_world_keys", var::color_world_keys},
        {"color_world_locker", var::color_world_locker},
        {"color_world_trash", var::color_world_trash},
        {"color_world_open_container", var::color_world_open_container},
        {"color_world_safe", var::color_world_safe},
        {"color_world_buried", var::color_world_buried},
        {"color_world_deaddrop", var::color_world_deaddrop},
    };
    for (const ColorEntry& entry : kColors) {
        if (key != entry.key)
            continue;
        if (entry.key != std::string("color_dropped_items")
            && entry.key != std::string("color_raider_stock")
            && entry.key != std::string("color_arc_entities")
            && entry.key != std::string("color_world_corpses"))
            s_anyWorldColorKeyInIni = true;
        ParseColor4(val, entry.color);
        return true;
    }
    return false;
}

static bool ApplyWorldConfigKey(const std::string& key, const std::string& val)
{
    if (ApplyWorldColorConfigKey(key, val))
        return true;

    if (key == "enable_world") { var::enable_world = ParseBool(val, var::enable_world); return true; }
    if (key == "droppedItems") { var::droppedItems = ParseBool(val, var::droppedItems); return true; }
    if (key == "raiderStock") { var::raiderStock = ParseBool(val, var::raiderStock); return true; }
    if (key == "showRobots") { var::showRobots = ParseBool(val, var::showRobots); return true; }
    if (key == "showHatches") { var::showHatches = ParseBool(val, var::showHatches); return true; }
    if (key == "color_hatches") { ParseColor4(val, var::color_hatches); return true; }
    if (key == "show_world_items") { var::show_world_items = ParseBool(val, var::show_world_items); return true; }
    if (key == "show_world_ammo") { var::show_world_ammo = ParseBool(val, var::show_world_ammo); return true; }
    if (key == "show_world_arc_loot") { var::show_world_arc_loot = ParseBool(val, var::show_world_arc_loot); return true; }
    if (key == "show_world_backpack") { var::show_world_backpack = ParseBool(val, var::show_world_backpack); return true; }
    if (key == "show_world_crate") { var::show_world_crate = ParseBool(val, var::show_world_crate); return true; }
    if (key == "show_world_furniture") { var::show_world_furniture = ParseBool(val, var::show_world_furniture); return true; }
    if (key == "show_world_grenade") { var::show_world_grenade = ParseBool(val, var::show_world_grenade); return true; }
    if (key == "show_world_harvestable") { var::show_world_harvestable = ParseBool(val, var::show_world_harvestable); return true; }
    if (key == "show_world_industrial") { var::show_world_industrial = ParseBool(val, var::show_world_industrial); return true; }
    if (key == "show_world_medical") { var::show_world_medical = ParseBool(val, var::show_world_medical); return true; }
    if (key == "show_world_other") { var::show_world_other = ParseBool(val, var::show_world_other); return true; }
    if (key == "show_world_probe") { var::show_world_probe = ParseBool(val, var::show_world_probe); return true; }
    if (key == "show_world_vehicles") { var::show_world_vehicles = ParseBool(val, var::show_world_vehicles); return true; }
    if (key == "show_world_weapon_case") { var::show_world_weapon_case = ParseBool(val, var::show_world_weapon_case); return true; }
    if (key == "show_world_weapon_rack") { var::show_world_weapon_rack = ParseBool(val, var::show_world_weapon_rack); return true; }
    if (key == "show_world_field_crate") { var::show_world_field_crate = ParseBool(val, var::show_world_field_crate); return true; }
    if (key == "show_world_supply_station") { var::show_world_supply_station = ParseBool(val, var::show_world_supply_station); return true; }
    if (key == "show_world_keys") { var::show_world_keys = ParseBool(val, var::show_world_keys); return true; }
    if (key == "show_world_locker") { var::show_world_locker = ParseBool(val, var::show_world_locker); return true; }
    if (key == "show_world_trash") { var::show_world_trash = ParseBool(val, var::show_world_trash); return true; }
    if (key == "show_world_open_container") { var::show_world_open_container = ParseBool(val, var::show_world_open_container); return true; }
    if (key == "show_world_safe") { var::show_world_safe = ParseBool(val, var::show_world_safe); return true; }
    if (key == "show_world_buried") { var::show_world_buried = ParseBool(val, var::show_world_buried); return true; }
    if (key == "show_world_deaddrop") { var::show_world_deaddrop = ParseBool(val, var::show_world_deaddrop); return true; }
    if (key == "showArc") { var::showArc = ParseBool(val, var::showArc); return true; }
    if (key == "showDeadPlayers") { var::showDeadPlayers = ParseBool(val, var::showDeadPlayers); return true; }
    return false;
}

void ApplyKeyValue(const std::string& key, const std::string& val)
{
    if (UpgradeOptions::Load(key, val))
        return;
    if (EntityFilters::Load(key, val))
        return;
    if (ArcCrosshair::LoadOption(key, val))
        return;
    if (ApplyLootConfigKey(key, val))
        return;
    if (ApplyAimbotConfigKey(key, val))
        return;
    if (ApplyWorldConfigKey(key, val))
        return;

    if (key == "enableesp") var::enableesp = ParseBool(val, var::enableesp);
    else if (key == "box") var::box = ParseBool(val, var::box);
    else if (key == "show_head_dot") var::show_head_dot = ParseBool(val, var::show_head_dot);
    else if (key == "radar_ghost_suppress_poi") var::radar_ghost_suppress_poi = ParseBool(val, var::radar_ghost_suppress_poi);
    else if (key == "show_search_window") var::show_search_window = ParseBool(val, var::show_search_window);
    else if (key == "head_dot_radius_px") {
        const float radius = static_cast<float>(std::atof(val.c_str()));
        if (std::isfinite(radius)) var::head_dot_radius_px = std::clamp(radius, 1.f, 10.f);
    }
    else if (key == "head_dot_color") ParseColor4(val, var::head_dot_color);
    else if (key == "player_box_style") var::player_box_style = std::clamp(std::atoi(val.c_str()), 0, 3);
    else if (key == "player_box_outline") var::player_box_outline = ParseBool(val, var::player_box_outline);
    else if (key == "player_box_fill") var::player_box_fill = ParseBool(val, var::player_box_fill);
    else if (key == "player_box_thickness") {
        const float v = static_cast<float>(std::atof(val.c_str()));
        if (std::isfinite(v)) var::player_box_thickness = std::clamp(v, 0.5f, 5.f);
    }
    else if (key == "player_box_fill_color") ParseColor4(val, var::player_box_fill_color);
    else if (key == "health") var::health = ParseBool(val, var::health);
    else if (key == "names") var::names = ParseBool(val, var::names);
    else if (key == "show_weapon") var::show_weapon = ParseBool(val, var::show_weapon);
    else if (key == "show_dbno_badge") var::show_dbno_badge = ParseBool(val, var::show_dbno_badge);
    else if (key == "show_look_arrows") var::show_look_arrows = ParseBool(val, var::show_look_arrows);
    else if (key == "show_steam_ids") var::show_steam_ids = ParseBool(val, var::show_steam_ids);
    else if (key == "color_look_arrow") ParseColor4(val, var::color_look_arrow);
    else if (key == "snaplines") var::snaplines = ParseBool(val, var::snaplines);
    else if (key == "skeleton") var::skeleton = ParseBool(val, var::skeleton);
    else if (key == "silhouette") var::silhouette = ParseBool(val, var::silhouette);
    else if (key == "silhouette_soft_fill") var::silhouette_soft_fill = ParseBool(val, var::silhouette_soft_fill);
    else if (key == "silhouette_max_distance_m") {
        var::silhouette_max_distance_m = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.f, var::kMaxDistanceSliderM);
    }
    else if (key == "show_distance") var::show_distance = ParseBool(val, var::show_distance);
    else if (key == "show_squad_idx") var::show_squad_idx = ParseBool(val, var::show_squad_idx);
    else if (key == "hide_allies") var::hide_allies = ParseBool(val, var::hide_allies);
    else if (key == "show_radar") var::show_radar = ParseBool(val, var::show_radar);
    else if (key == "show_debug_overlay") var::show_debug_overlay = ParseBool(val, var::show_debug_overlay);
    else if (key == "debug_skeleton_lag") var::debug_skeleton_lag = ParseBool(val, var::debug_skeleton_lag);
    else if (key == "debug_hatch_detect") var::debug_hatch_detect = ParseBool(val, var::debug_hatch_detect);
    else if (key == "debug_ghost_bots") var::debug_ghost_bots = ParseBool(val, var::debug_ghost_bots);
    else if (key == "debug_world_trace") var::debug_world_trace = ParseBool(val, var::debug_world_trace);
    else if (key == "debug_esp_paint") var::debug_esp_paint = ParseBool(val, var::debug_esp_paint);
    else if (key == "esp_text_scale") var::esp_text_scale = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.5f, 3.f);
    else if (key == "radar_scale") var::radar_scale = std::clamp(static_cast<float>(std::atof(val.c_str())), 30.f, 120.f);
    else if (key == "radar_range") {
        var::radar_range = std::clamp(static_cast<float>(std::atof(val.c_str())), 20.f, var::kMaxDistanceSliderM);
    }
    else if (key == "radar_pos_x_norm") var::radar_pos_x_norm = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.f, 1.f);
    else if (key == "radar_pos_y_norm") var::radar_pos_y_norm = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.f, 1.f);
    else if (key == "radar_loot_min_rarity") var::radar_loot_min_rarity = std::atoi(val.c_str());
    else if (key == "show_radar_special") var::show_radar_special = ParseBool(val, var::show_radar_special);
    else if (key == "show_raid_hud") var::show_raid_hud = ParseBool(val, var::show_raid_hud);
    else if (key == "show_activity_feed") var::show_activity_feed = ParseBool(val, var::show_activity_feed);
    else if (key == "show_objectives") var::show_objectives = ParseBool(val, var::show_objectives);
    else if (key == "radar_shape_circle") var::radar_shape_circle = ParseBool(val, var::radar_shape_circle);
    else if (key == "radar_map_mode") var::radar_map_mode = ParseBool(val, var::radar_map_mode);
    else if (key == "radar_underground_dim") var::radar_underground_dim = ParseBool(val, var::radar_underground_dim);
    else if (key == "radar_ally_arrows") var::radar_ally_arrows = ParseBool(val, var::radar_ally_arrows);
    else if (key == "radar_dot_scale") var::radar_dot_scale = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.5f, 2.5f);
    else if (key == "radar_show_altitude") var::radar_show_altitude = ParseBool(val, var::radar_show_altitude);
    else if (key == "radar_compass") var::radar_compass = ParseBool(val, var::radar_compass);
    else if (key == "radar_focus") var::radar_focus = std::clamp(std::atoi(val.c_str()), 0, 3);
    else if (key == "radar_ghost") var::radar_ghost = ParseBool(val, var::radar_ghost);
    else if (key == "radar_ghost_secs") var::radar_ghost_secs = std::clamp(static_cast<float>(std::atof(val.c_str())), 1.f, 30.f);
    else if (key == "show_pois") var::show_pois = ParseBool(val, var::show_pois);
    else if (key == "poi_show_labels") var::poi_show_labels = ParseBool(val, var::poi_show_labels);
    else if (key == "poi_categories") var::poi_categories = std::atoi(val.c_str());
    else if (key == "color_profile_slot") var::color_profile_slot = std::clamp(std::atoi(val.c_str()), -1, 7);
    else if (key == "esp_distance") {
        var::esp_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 50.f, var::kMaxDistanceSliderM);
    }
    else if (key == "esp_color_visible") ParseColor4(val, var::esp_color_visible);
    else if (key == "esp_color_invisible") ParseColor4(val, var::esp_color_invisible);
    else if (key == "bot_box") var::bot_box = ParseBool(val, var::bot_box);
    else if (key == "bot_names") var::bot_names = ParseBool(val, var::bot_names);
    else if (key == "bot_snaplines") var::bot_snaplines = ParseBool(val, var::bot_snaplines);
    else if (key == "esp_entity_debug") var::esp_entity_debug = ParseBool(val, var::esp_entity_debug);
    else if (key == "bot_show_distance") var::bot_show_distance = ParseBool(val, var::bot_show_distance);
    else if (key == "bot_heart") var::bot_heart = ParseBool(val, var::bot_heart);
    else if (key == "show_dead_bots") var::show_dead_bots = ParseBool(val, var::show_dead_bots);
    else if (key == "color_dead_bots") ParseColor4(val, var::color_dead_bots);
    else if (key == "bot_esp_distance") {
        var::bot_esp_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 50.f, var::kMaxDistanceSliderM);
    }
    else if (key == "bot_color_visible") ParseColor4(val, var::bot_color_visible);
    else if (key == "bot_color_invisible") ParseColor4(val, var::bot_color_invisible);
    else if (key == "aim_hold_key") var::aim_hold_key = std::atoi(val.c_str());
    else if (key == "aim_autofire") var::aim_autofire = ParseBool(val, var::aim_autofire);
    else if (key == "aim_radius") var::aim_radius = std::clamp(std::atoi(val.c_str()), 10, 120);
    else if (key == "aim_stop_zone") {
        var::aim_stop_zone = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.5f, 6.f);
    }
    else if (key == "aim_speed") var::aim_speed = std::clamp(std::atoi(val.c_str()), 10, 300);
    else if (key == "aim_response") var::aim_response = std::clamp(std::atoi(val.c_str()), 1, 100);
    else if (key == "aim_calib_x") {
        var::aim_calib_x = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.1f, 20.f);
    }
    else if (key == "aim_calib_y") {
        var::aim_calib_y = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.1f, 20.f);
    }
    else if (key == "aim_priority") var::aim_priority = std::clamp(std::atoi(val.c_str()), 0, 2);
    else if (key == "aim_bone") var::aim_bone = std::clamp(std::atoi(val.c_str()), 0, 19);
    else if (key == "aim_point_percent") {
        var::aim_point_percent = std::clamp(std::atoi(val.c_str()), 2, 45);
    }
    else if (key == "aim_max_distance") {
        var::aim_max_distance = std::clamp(std::atoi(val.c_str()), 5, 500);
    }
    else if (key == "aim_sticky") var::aim_sticky = ParseBool(val, var::aim_sticky);
    else if (key == "aim_require_enemy") var::aim_require_enemy = ParseBool(val, var::aim_require_enemy);
    else if (key == "aim_require_visible") var::aim_require_visible = ParseBool(val, var::aim_require_visible);
    else if (key == "aim_require_bones") var::aim_require_bones = ParseBool(val, var::aim_require_bones);
    else if (key == "aim_frame_fresh_ms") {
        var::aim_frame_fresh_ms = std::clamp(std::atoi(val.c_str()), 80, 1000);
    }
    else if (key == "aim_autofire_radius") {
        var::aim_autofire_radius = std::clamp(std::atoi(val.c_str()), 3, 12);
    }
    else if (key == "aim_autofire_interval") {
        var::aim_autofire_interval = std::clamp(std::atoi(val.c_str()), 80, 1000);
    }
    else if (key == "aim_click_hold") {
        var::aim_click_hold = std::clamp(std::atoi(val.c_str()), 10, 100);
    }
    else if (key == "aim_telemetry") var::aim_telemetry = ParseBool(val, var::aim_telemetry);
    else if (key == "aim_deadzone") var::aim_deadzone = ParseBool(val, var::aim_deadzone);
    else if (key == "aim_jitter") var::aim_jitter = ParseBool(val, var::aim_jitter);
    else if (key == "aim_close_range") var::aim_close_range = ParseBool(val, var::aim_close_range);
    else if (key == "aim_close_distance") {
        var::aim_close_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 2.f, 10.f);
    }
    else if (key == "aim_recoil_comp") {
        var::aim_recoil_comp = std::clamp(static_cast<float>(std::atof(val.c_str())), 1.f, 2.f);
    }
    else if (key == "aim_show_circle") var::aim_show_circle = ParseBool(val, var::aim_show_circle);
    else if (key == "debug_overlay_key") var::debug_overlay_key = std::atoi(val.c_str());
    else if (key == "camPovUsePending") var::camPovUsePending = ParseBool(val, var::camPovUsePending);
    else if (key == "show_crosshair") var::show_crosshair = ParseBool(val, var::show_crosshair);
    else if (key == "crosshair_style") {
        var::crosshair_style = std::clamp(std::atoi(val.c_str()), 0, static_cast<int>(IM_ARRAYSIZE(ArcCrosshair::kStyles)) - 1);
    }
    else if (key == "crosshair_color") ParseColor4(val, var::crosshair_color);
    else if (key == "crosshair_size") var::crosshair_size = static_cast<float>(std::atof(val.c_str()));
    else if (key == "crosshair_thickness") var::crosshair_thickness = static_cast<float>(std::atof(val.c_str()));
    else if (key == "crosshair_gap") var::crosshair_gap = static_cast<float>(std::atof(val.c_str()));
    else if (key == "crosshair_spin_rpm") var::crosshair_spin_rpm = static_cast<float>(std::atof(val.c_str()));
    else if (key == "showmenu") showmenu = ParseBool(val, showmenu);
    else if (key == "kmbox_type") {
        if (val == "BPro")
            g_kmbox.kmboxConfig.type = "MAKCU";
        else if (val == "Net" || val == "MAKCU")
            g_kmbox.kmboxConfig.type = val;
    }
    else if (key == "kmbox_comPort")
        g_kmbox.kmboxConfig.comPort = val;
    else if (key == "kmbox_baudRate")
        g_kmbox.kmboxConfig.baudRate = val;
    else if (key == "kmbox_ip")
        g_kmbox.kmboxConfig.ip = val;
    else if (key == "kmbox_port")
        g_kmbox.kmboxConfig.port = val;
    else if (key == "kmbox_uuid")
        g_kmbox.kmboxConfig.uuid = val;
    else if (key == "kmbox_minDelay")
        g_kmbox.kmboxConfig.minDelay = std::atoi(val.c_str());
    else if (key == "kmbox_monitorIndex") {
        g_kmbox.kmboxConfig.monitorIndex = std::atoi(val.c_str());
        OverlayDisplay_SetSelectedMonitor(g_kmbox.kmboxConfig.monitorIndex);
    }
}

void SerializeIni(std::ostream& file);

bool LoadIniFile(const std::string& path)
{

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;
    constexpr std::streamoff kMaxConfigBytes = 1024 * 1024;
    const auto size = file.tellg();
    if (size < std::streampos(0) || size > std::streampos(kMaxConfigBytes))
        return false;
    file.seekg(0);

    std::vector<std::pair<std::string, std::string>> pending;
    std::string line;
    while (std::getline(file, line)) {
        TrimInPlace(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[')
            continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        TrimInPlace(key);
        TrimInPlace(val);
        pending.emplace_back(std::move(key), std::move(val));
    }
    if (file.bad())
        return false;

    // Use the persisted key inventory, including delegated registries, as the
    // allowlist. Validate everything before mutating live settings.
    std::ostringstream current;
    current.imbue(std::locale::classic());
    current.precision(std::numeric_limits<float>::max_digits10);
    SerializeIni(current);
    std::map<std::string, std::string> known;
    std::istringstream schema(current.str());
    while (std::getline(schema, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos)
            known.emplace(line.substr(0, eq), line.substr(eq + 1));
    }
    // Legacy key names the current schema no longer emits must stay loadable.
    // The shipped auto_config.ini still carries vis_enabled /
    // collision_vis_enabled / collision_debug_draw, and because the allowlist is
    // built from what SerializeIni emits TODAY, those lines were dropped without
    // a word — after which the next save rewrote the file under the new names.
    // UpgradeOptions owns the mapping (its legacyBools table); this only admits
    // the names so ApplyKeyValue can reach them.
    for (const auto& legacy : UpgradeOptions::legacyBools)
        known.emplace(legacy.key, "0");

    std::vector<std::pair<std::string, std::string>> valid;
    int droppedKeys = 0;
    char firstDroppedKey[64]{};
    for (auto& entry : pending) {
        const auto it = known.find(entry.first);
        if (it == known.end()) {
            // A key this build does not know: counted and remembered so it can
            // be reported instead of vanishing. Silently discarding it is
            // indistinguishable, from the user's side, from a setting that
            // simply refuses to stick.
            ++droppedKeys;
            if (!firstDroppedKey[0])
                std::snprintf(firstDroppedKey, sizeof(firstDroppedKey), "%.48s",
                    entry.first.c_str());
            continue;
        }
        std::string& value = entry.second;
        const bool text = entry.first.rfind("kmbox_", 0) == 0
            && entry.first != "kmbox_minDelay" && entry.first != "kmbox_monitorIndex";
        if (!text) {
            if (it->second.find(',') != std::string::npos) {
                float rgba[4]{};
                if (!ParseColor4(value, rgba))
                    return false;
            } else if (entry.first.rfind("filter_", 0) == 0) {
                char* end = nullptr;
                errno = 0;
                (void)std::strtoull(value.c_str(), &end, 10);
                if (value.empty() || value.front() == '-' || errno == ERANGE
                    || end == value.c_str() || *end != '\0')
                    return false;
            } else {
                // Preserve accepted legacy boolean spellings.
                if (value == "true" || value == "yes" || value == "on") value = "1";
                if (value == "false" || value == "no" || value == "off") value = "0";
                char* end = nullptr;
                errno = 0;
                const double number = std::strtod(value.c_str(), &end);
                if (errno == ERANGE || end == value.c_str() || *end != '\0'
                    || !std::isfinite(number)
                    || number < (std::numeric_limits<int>::min)()
                    || number > (std::numeric_limits<int>::max)())
                    return false;
            }
        }
        valid.push_back(entry);
    }
    if (valid.empty())
        return false;
    s_anyWorldColorKeyInIni = false;
    for (const auto& entry : valid)
        ApplyKeyValue(entry.first, entry.second);

    // Report the lines that were ignored, through the diagnostics ledger: the
    // overlay's stdout/stderr never reaches disk, so std::cerr alone would leave
    // this invisible. One bounded record per load.
    if (droppedKeys) {
        char rec[256];
        std::snprintf(rec, sizeof(rec),
            "{\"location\":\"AutoConfig.cpp:LoadIniFile\",\"message\":\"config_keys_dropped\","
            "\"timestamp\":%llu,\"dropped\":%d,\"applied\":%zu,\"firstKey\":\"%.48s\"}\n",
            static_cast<unsigned long long>(EntityDiagnostics::NowMs()),
            droppedKeys, valid.size(), firstDroppedKey);
        EntityDiagnostics::LogRaw(rec);
        std::cerr << "[config] " << droppedKeys << " key(s) in " << kConfigFileName
            << " are not recognised by this build (first: " << firstDroppedKey
            << "); they were ignored.\n";
    }

    if (g_kmbox.kmboxConfig.monitorIndex < 0)
        g_kmbox.kmboxConfig.monitorIndex = 0;

    g_kmbox.SetRememberConfig(true);
    return true;
}

void SerializeIni(std::ostream& file)
{
    file << "# Arc Raiders auto config (saved automatically)\n\n";

    file << "enableesp=" << (var::enableesp ? 1 : 0) << '\n';
    file << "box=" << (var::box ? 1 : 0) << '\n';
    file << "show_head_dot=" << var::show_head_dot << '\n';
    file << "radar_ghost_suppress_poi=" << (var::radar_ghost_suppress_poi ? 1 : 0) << '\n';
    file << "show_search_window=" << (var::show_search_window ? 1 : 0) << '\n';
    file << "head_dot_radius_px=" << var::head_dot_radius_px << '\n';
    WriteColor4(file, "head_dot_color", var::head_dot_color);
    file << "player_box_style=" << var::player_box_style << '\n';
    file << "player_box_outline=" << var::player_box_outline << '\n';
    file << "player_box_fill=" << var::player_box_fill << '\n';
    file << "player_box_thickness=" << var::player_box_thickness << '\n';
    WriteColor4(file, "player_box_fill_color", var::player_box_fill_color);
    file << "health=" << (var::health ? 1 : 0) << '\n';
    file << "names=" << (var::names ? 1 : 0) << '\n';
    file << "show_weapon=" << (var::show_weapon ? 1 : 0) << '\n';
    file << "show_dbno_badge=" << (var::show_dbno_badge ? 1 : 0) << '\n';
    file << "show_look_arrows=" << (var::show_look_arrows ? 1 : 0) << '\n';
    file << "show_steam_ids=" << (var::show_steam_ids ? 1 : 0) << '\n';
    WriteColor4(file, "color_look_arrow", var::color_look_arrow);
    file << "snaplines=" << (var::snaplines ? 1 : 0) << '\n';
    file << "skeleton=" << (var::skeleton ? 1 : 0) << '\n';
    file << "silhouette=" << (var::silhouette ? 1 : 0) << '\n';
    file << "silhouette_soft_fill=" << (var::silhouette_soft_fill ? 1 : 0) << '\n';
    file << "silhouette_max_distance_m=" << var::silhouette_max_distance_m << '\n';
    file << "show_distance=" << (var::show_distance ? 1 : 0) << '\n';
    file << "show_squad_idx=" << (var::show_squad_idx ? 1 : 0) << '\n';
    file << "hide_allies=" << (var::hide_allies ? 1 : 0) << '\n';
    file << "show_radar=" << (var::show_radar ? 1 : 0) << '\n';
    file << "show_debug_overlay=" << (var::show_debug_overlay ? 1 : 0) << '\n';
    file << "debug_skeleton_lag=" << (var::debug_skeleton_lag ? 1 : 0) << '\n';
    file << "debug_hatch_detect=" << (var::debug_hatch_detect ? 1 : 0) << '\n';
    file << "debug_ghost_bots=" << (var::debug_ghost_bots ? 1 : 0) << '\n';
    file << "debug_world_trace=" << (var::debug_world_trace ? 1 : 0) << '\n';
    file << "debug_esp_paint=" << (var::debug_esp_paint ? 1 : 0) << '\n';
    file << "esp_text_scale=" << var::esp_text_scale << '\n';
    file << "radar_scale=" << var::radar_scale << '\n';
    file << "radar_range=" << var::radar_range << '\n';
    file << "radar_pos_x_norm=" << var::radar_pos_x_norm << '\n';
    file << "radar_pos_y_norm=" << var::radar_pos_y_norm << '\n';
    file << "radar_loot_min_rarity=" << var::radar_loot_min_rarity << '\n';
    file << "show_radar_special=" << (var::show_radar_special ? 1 : 0) << '\n';
    file << "show_raid_hud=" << (var::show_raid_hud ? 1 : 0) << '\n';
    file << "show_activity_feed=" << (var::show_activity_feed ? 1 : 0) << '\n';
    file << "show_objectives=" << (var::show_objectives ? 1 : 0) << '\n';
    file << "radar_shape_circle=" << (var::radar_shape_circle ? 1 : 0) << '\n';
    file << "radar_map_mode=" << (var::radar_map_mode ? 1 : 0) << '\n';
    file << "radar_underground_dim=" << (var::radar_underground_dim ? 1 : 0) << '\n';
    file << "radar_ally_arrows=" << (var::radar_ally_arrows ? 1 : 0) << '\n';
    file << "radar_dot_scale=" << var::radar_dot_scale << '\n';
    file << "radar_show_altitude=" << (var::radar_show_altitude ? 1 : 0) << '\n';
    file << "radar_compass=" << (var::radar_compass ? 1 : 0) << '\n';
    file << "radar_focus=" << var::radar_focus << '\n';
    file << "radar_ghost=" << (var::radar_ghost ? 1 : 0) << '\n';
    file << "radar_ghost_secs=" << var::radar_ghost_secs << '\n';
    file << "show_pois=" << (var::show_pois ? 1 : 0) << '\n';
    file << "poi_show_labels=" << (var::poi_show_labels ? 1 : 0) << '\n';
    file << "poi_categories=" << var::poi_categories << '\n';
    file << "color_profile_slot=" << var::color_profile_slot << '\n';
    file << "esp_distance=" << var::esp_distance << '\n';
    WriteColor4(file, "esp_color_visible", var::esp_color_visible);
    WriteColor4(file, "esp_color_invisible", var::esp_color_invisible);
    file << "bot_box=" << (var::bot_box ? 1 : 0) << '\n';
    file << "bot_names=" << (var::bot_names ? 1 : 0) << '\n';
    file << "bot_snaplines=" << (var::bot_snaplines ? 1 : 0) << '\n';
    file << "esp_entity_debug=" << (var::esp_entity_debug ? 1 : 0) << '\n';
    file << "bot_show_distance=" << (var::bot_show_distance ? 1 : 0) << '\n';
    file << "bot_heart=" << (var::bot_heart ? 1 : 0) << '\n';
    file << "show_dead_bots=" << (var::show_dead_bots ? 1 : 0) << '\n';
    WriteColor4(file, "color_dead_bots", var::color_dead_bots);
    file << "bot_esp_distance=" << var::bot_esp_distance << '\n';
    WriteColor4(file, "bot_color_visible", var::bot_color_visible);
    WriteColor4(file, "bot_color_invisible", var::bot_color_invisible);

    file << "enable_aimbot=" << (var::enable_aimbot ? 1 : 0) << '\n';
    file << "aim_autofire=" << (var::aim_autofire ? 1 : 0) << '\n';
    file << "aim_hold_key=" << var::aim_hold_key << '\n';
    file << "aim_radius=" << var::aim_radius << '\n';
    file << "aim_stop_zone=" << var::aim_stop_zone << '\n';
    file << "aim_speed=" << var::aim_speed << '\n';
    file << "aim_response=" << var::aim_response << '\n';
    file << "aim_calib_x=" << var::aim_calib_x << '\n';
    file << "aim_calib_y=" << var::aim_calib_y << '\n';
    file << "aim_priority=" << var::aim_priority << '\n';
    file << "aim_bone=" << var::aim_bone << '\n';
    file << "aim_point_percent=" << var::aim_point_percent << '\n';
    file << "aim_max_distance=" << var::aim_max_distance << '\n';
    file << "aim_sticky=" << (var::aim_sticky ? 1 : 0) << '\n';
    file << "aim_require_enemy=" << (var::aim_require_enemy ? 1 : 0) << '\n';
    file << "aim_require_visible=" << (var::aim_require_visible ? 1 : 0) << '\n';
    file << "aim_require_bones=" << (var::aim_require_bones ? 1 : 0) << '\n';
    file << "aim_frame_fresh_ms=" << var::aim_frame_fresh_ms << '\n';
    file << "aim_autofire_radius=" << var::aim_autofire_radius << '\n';
    file << "aim_autofire_interval=" << var::aim_autofire_interval << '\n';
    file << "aim_click_hold=" << var::aim_click_hold << '\n';
    file << "aim_telemetry=" << (var::aim_telemetry ? 1 : 0) << '\n';
    file << "aim_deadzone=" << (var::aim_deadzone ? 1 : 0) << '\n';
    file << "aim_jitter=" << (var::aim_jitter ? 1 : 0) << '\n';
    file << "aim_close_range=" << (var::aim_close_range ? 1 : 0) << '\n';
    file << "aim_close_distance=" << var::aim_close_distance << '\n';
    file << "aim_recoil_comp=" << var::aim_recoil_comp << '\n';
    file << "aim_show_circle=" << (var::aim_show_circle ? 1 : 0) << '\n';
    file << "debug_overlay_key=" << var::debug_overlay_key << '\n';
    file << "camPovUsePending=" << (var::camPovUsePending ? 1 : 0) << '\n';
    file << "show_crosshair=" << (var::show_crosshair ? 1 : 0) << '\n';
    UpgradeOptions::Save(file);
    EntityFilters::Save(file);
    ArcCrosshair::SaveOptions(file);
    file << "crosshair_style=" << var::crosshair_style << '\n';
    WriteColor4(file, "crosshair_color", var::crosshair_color);
    file << "crosshair_size=" << var::crosshair_size << '\n';
    file << "crosshair_thickness=" << var::crosshair_thickness << '\n';
    file << "crosshair_gap=" << var::crosshair_gap << '\n';
    file << "crosshair_spin_rpm=" << var::crosshair_spin_rpm << '\n';
    file << "enable_world=" << (var::enable_world ? 1 : 0) << '\n';
    file << "droppedItems=" << (var::droppedItems ? 1 : 0) << '\n';
    file << "raiderStock=" << (var::raiderStock ? 1 : 0) << '\n';
    file << "showRobots=" << (var::showRobots ? 1 : 0) << '\n';
    file << "showHatches=" << (var::showHatches ? 1 : 0) << '\n';
    WriteColor4(file, "color_hatches", var::color_hatches);
    file << "show_world_items=" << (var::show_world_items ? 1 : 0) << '\n';
    file << "show_world_ammo=" << (var::show_world_ammo ? 1 : 0) << '\n';
    file << "show_world_arc_loot=" << (var::show_world_arc_loot ? 1 : 0) << '\n';
    file << "show_world_backpack=" << (var::show_world_backpack ? 1 : 0) << '\n';
    file << "show_world_crate=" << (var::show_world_crate ? 1 : 0) << '\n';
    file << "show_world_furniture=" << (var::show_world_furniture ? 1 : 0) << '\n';
    file << "show_world_grenade=" << (var::show_world_grenade ? 1 : 0) << '\n';
    file << "show_world_harvestable=" << (var::show_world_harvestable ? 1 : 0) << '\n';
    file << "show_world_industrial=" << (var::show_world_industrial ? 1 : 0) << '\n';
    file << "show_world_medical=" << (var::show_world_medical ? 1 : 0) << '\n';
    file << "show_world_other=" << (var::show_world_other ? 1 : 0) << '\n';
    file << "show_world_probe=" << (var::show_world_probe ? 1 : 0) << '\n';
    file << "show_world_vehicles=" << (var::show_world_vehicles ? 1 : 0) << '\n';
    file << "show_world_weapon_case=" << (var::show_world_weapon_case ? 1 : 0) << '\n';
    file << "show_world_weapon_rack=" << (var::show_world_weapon_rack ? 1 : 0) << '\n';
    file << "show_world_field_crate=" << (var::show_world_field_crate ? 1 : 0) << '\n';
    file << "show_world_supply_station=" << (var::show_world_supply_station ? 1 : 0) << '\n';
    file << "show_world_keys=" << (var::show_world_keys ? 1 : 0) << '\n';
    file << "show_world_locker=" << (var::show_world_locker ? 1 : 0) << '\n';
    file << "show_world_trash=" << (var::show_world_trash ? 1 : 0) << '\n';
    file << "show_world_open_container=" << (var::show_world_open_container ? 1 : 0) << '\n';
    file << "show_world_safe=" << (var::show_world_safe ? 1 : 0) << '\n';
    file << "show_world_buried=" << (var::show_world_buried ? 1 : 0) << '\n';
    file << "show_world_deaddrop=" << (var::show_world_deaddrop ? 1 : 0) << '\n';
    file << "showArc=" << (var::showArc ? 1 : 0) << '\n';
    file << "showDeadPlayers=" << (var::showDeadPlayers ? 1 : 0) << '\n';
    WriteColor4(file, "color_dropped_items", var::color_dropped_items);
    WriteColor4(file, "color_raider_stock", var::color_raider_stock);
    WriteColor4(file, "color_arc_entities", var::color_arc_entities);
    WriteColor4(file, "color_world_corpses", var::color_world_corpses);
    WriteColor4(file, "color_world_items", var::color_world_items);
    WriteColor4(file, "color_world_ammo", var::color_world_ammo);
    WriteColor4(file, "color_world_arc_loot", var::color_world_arc_loot);
    WriteColor4(file, "color_world_backpack", var::color_world_backpack);
    WriteColor4(file, "color_world_crate", var::color_world_crate);
    WriteColor4(file, "color_world_furniture", var::color_world_furniture);
    WriteColor4(file, "color_world_grenade", var::color_world_grenade);
    WriteColor4(file, "color_world_harvestable", var::color_world_harvestable);
    WriteColor4(file, "color_world_industrial", var::color_world_industrial);
    WriteColor4(file, "color_world_medical", var::color_world_medical);
    WriteColor4(file, "color_world_other", var::color_world_other);
    WriteColor4(file, "color_world_probe", var::color_world_probe);
    WriteColor4(file, "color_world_vehicles", var::color_world_vehicles);
    WriteColor4(file, "color_world_weapon_case", var::color_world_weapon_case);
    WriteColor4(file, "color_world_weapon_rack", var::color_world_weapon_rack);
    WriteColor4(file, "color_world_field_crate", var::color_world_field_crate);
    WriteColor4(file, "color_world_supply_station", var::color_world_supply_station);
    WriteColor4(file, "color_world_keys", var::color_world_keys);
    WriteColor4(file, "color_world_locker", var::color_world_locker);
    WriteColor4(file, "color_world_trash", var::color_world_trash);
    WriteColor4(file, "color_world_open_container", var::color_world_open_container);
    WriteColor4(file, "color_world_safe", var::color_world_safe);
    WriteColor4(file, "color_world_buried", var::color_world_buried);
    WriteColor4(file, "color_world_deaddrop", var::color_world_deaddrop);

    file << "showLoot=" << (var::showLoot ? 1 : 0) << '\n';
    file << "show_world_quest=" << (var::show_world_quest ? 1 : 0) << '\n';
    file << "quest_pulse=" << (var::quest_pulse ? 1 : 0) << '\n';
    file << "show_near_loot_hud=" << (var::show_near_loot_hud ? 1 : 0) << '\n';
    file << "loot_distance=" << var::loot_distance << '\n';
    file << "container_distance_sp=" << var::container_distance_sp << '\n';
    for (size_t i = 0; i < static_cast<size_t>(WorldItemCategory::Count); ++i) {
        const auto cat = static_cast<WorldItemCategory>(i);
        if (!WorldCategoryHasSpConfig(cat))
            continue;
        const char* suffix = WorldItemCategoryConfigSuffix(cat);
        if (!suffix)
            continue;
        file << "container_range_sp_" << suffix << '='
             << (WorldCategoryUsesSpContainerRange(cat) ? 1 : 0) << '\n';
    }
    WriteColor4(file, "color_loot", var::color_loot);
    file << "loot_rarity_color=" << (var::loot_rarity_color ? 1 : 0) << '\n';
    file << "show_loot_value=" << (var::show_loot_value ? 1 : 0) << '\n';
    file << "show_crate_contents=" << (var::show_crate_contents ? 1 : 0) << '\n';
    file << "show_stack_counts=" << (var::show_stack_counts ? 1 : 0) << '\n';
    file << "grey_looted_containers=" << (var::grey_looted_containers ? 1 : 0) << '\n';
    file << "state_opened_mode=" << var::state_opened_mode << '\n';
    file << "state_dead_mode=" << var::state_dead_mode << '\n';
    file << "state_dim_alpha=" << var::state_dim_alpha << '\n';
    file << "show_armor_line=" << (var::show_armor_line ? 1 : 0) << '\n';
    file << "show_player_kit=" << (var::show_player_kit ? 1 : 0) << '\n';
    file << "show_bot_loadout=" << (var::show_bot_loadout ? 1 : 0) << '\n';
    file << "show_bot_vision=" << (var::show_bot_vision ? 1 : 0) << '\n';
    file << "show_bot_alertness=" << (var::show_bot_alertness ? 1 : 0) << '\n';
    file << "show_bot_parts=" << (var::show_bot_parts ? 1 : 0) << '\n';
    file << "loot_min_value=" << var::loot_min_value << '\n';
    file << "loot_min_rarity=" << var::loot_min_rarity << '\n';
    file << "loot_min_val_sp=" << (var::loot_min_val_sp ? 1 : 0) << '\n';
    file << "loot_min_rar_sp=" << (var::loot_min_rar_sp ? 1 : 0) << '\n';
    file << "showmenu=" << (showmenu ? 1 : 0) << '\n';

    file << "kmbox_type=" << g_kmbox.kmboxConfig.type << '\n';
    file << "kmbox_comPort=" << g_kmbox.kmboxConfig.comPort << '\n';
    file << "kmbox_baudRate=" << g_kmbox.kmboxConfig.baudRate << '\n';
    file << "kmbox_ip=" << g_kmbox.kmboxConfig.ip << '\n';
    file << "kmbox_port=" << g_kmbox.kmboxConfig.port << '\n';
    file << "kmbox_uuid=" << g_kmbox.kmboxConfig.uuid << '\n';
    file << "kmbox_minDelay=" << g_kmbox.kmboxConfig.minDelay << '\n';
    file << "kmbox_monitorIndex=" << g_kmbox.kmboxConfig.monitorIndex << '\n';
}

// Serializes the live settings to text. Pure in-memory work, safe on paint.
std::string SerializeIniToString()
{
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.precision(std::numeric_limits<float>::max_digits10);
    SerializeIni(text);
    return text.str();
}

// Writes text via temp file + atomic replace. Does the disk I/O only.
bool WriteTextAtomic(const std::string& path, const std::string& text)
{
    const std::string temporary = path + ".tmp";
    std::ofstream file(temporary, std::ios::trunc);
    if (!file.is_open())
        return false;
    file << text;
    file.flush();
    const bool written = file.good();
    file.close();
    if (!written || file.fail()) {
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

bool WriteIniTo(const std::string& path)
{
    return WriteTextAtomic(path, SerializeIniToString());
}

// The one in-flight background save, if any.
std::future<bool> s_pendingSave;

// Collects the background write. Returns false only while it is still in flight
// (non-blocking mode). A failed write re-arms the debounced save.
bool ReapPendingSave(bool block)
{
    if (!s_pendingSave.valid())
        return true;
    if (!block && s_pendingSave.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;
    if (!s_pendingSave.get()) {
        std::cerr << "[config] Background save failed; retry pending.\n";
        AutoConfig_MarkDirty();
    }
    return true;
}

} // namespace

bool AutoConfig_HadFileOnLoad()
{
    return s_hadFileOnLoad;
}

void AutoConfig_Load()
{
    const std::string path = ConfigPath();
    const bool loaded = LoadIniFile(path);
    s_hadFileOnLoad = loaded;
    if (!loaded) {
        (void)g_kmbox.LoadKmboxConfig();
    } else {
        MigrateWorldColorsFromLoot();
    }

    s_last = CaptureSnapshot();
    s_snapshotValid = true;
    s_dirty = false;
}

void AutoConfig_MarkDirty()
{
    s_dirty = true;
    s_dirtySince = std::chrono::steady_clock::now();
}

void AutoConfig_SaveNow()
{
    // Exit path: wait out any background write so the two never race on the file.
    ReapPendingSave(true);
    if (!WriteIniTo(ConfigPath())) {
        s_dirty = true;
        s_dirtySince = std::chrono::steady_clock::now();
        std::cerr << "[config] Save failed; previous config retained and retry pending.\n";
        return;
    }
    s_last = CaptureSnapshot();
    s_snapshotValid = true;
    s_dirty = false;
}

// Serializes on the caller (no I/O) and writes the file on a background thread.
// Safe to call from paint: it never waits on the disk.
void AutoConfig_SaveNowAsync()
{
    if (!ReapPendingSave(false)) {
        // Previous write still in flight: keep dirty so Tick retries it.
        AutoConfig_MarkDirty();
        return;
    }
    std::string text = SerializeIniToString();
    s_pendingSave = std::async(std::launch::async,
        [path = ConfigPath(), text = std::move(text)]() {
            return WriteTextAtomic(path, text);
        });
    s_last = CaptureSnapshot();
    s_snapshotValid = true;
    s_dirty = false;
}

void AutoConfig_Tick()
{
    ReapPendingSave(false);

    if (!s_snapshotValid) {
        s_last = CaptureSnapshot();
        s_snapshotValid = true;
        return;
    }

    const Snapshot now = CaptureSnapshot();
    if (!SnapshotsEqual(now, s_last)) {
        s_last = now;
        AutoConfig_MarkDirty();
    }

    if (!s_dirty)
        return;

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - s_dirtySince).count();
    if (elapsed >= kSaveDelayMs) {
        AutoConfig_SaveNowAsync();
    }
}

// ---------------------------------------------------------------------------
// D20 — baseline, diff, export, apply.
// ---------------------------------------------------------------------------

namespace {

std::string DataFilePath(const char* name)
{
    const std::string cfg = ConfigPath();
    const size_t slash = cfg.find_last_of("\\/");
    const std::string dir = (slash == std::string::npos)
        ? std::string()
        : cfg.substr(0, slash + 1);
    return dir + "Data\\" + name;
}

// Read a key=value config file into a map for diffing. Comments, blank lines
// and [section] headers are skipped exactly the way LoadIniFile skips them, so
// both sides of the diff agree on what counts as a setting.
bool ParseKeyValues(const std::string& path, std::map<std::string, std::string>& out)
{
    std::ifstream in(path);
    if (!in)
        return false;

    std::string line;
    while (std::getline(in, line)) {
        TrimInPlace(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        TrimInPlace(key);
        TrimInPlace(val);
        out[key] = val;
    }
    return true;
}

// Export directly through the same serializer used by autosave.
bool CopyConfigTo(const std::string& dst)
{
    // Serialize live values directly. A failed autosave must never cause an
    // apparently successful export of stale disk settings.
    return WriteIniTo(dst);
}

} // namespace

std::string AutoConfig_BaselinePath()
{
    return DataFilePath("config_baseline.ini");
}

std::string AutoConfig_ExportPath()
{
    return DataFilePath("config_export.ini");
}

bool AutoConfig_SnapshotBaseline()
{
    return CopyConfigTo(AutoConfig_BaselinePath());
}

bool AutoConfig_ExportCurrent()
{
    return CopyConfigTo(AutoConfig_ExportPath());
}

bool AutoConfig_ImportExported()
{
    const std::string path = AutoConfig_ExportPath();
    if (!LoadIniFile(path))
        return false;
    AutoConfig_MarkDirty();
    return true;
}

std::vector<std::string> AutoConfig_DiffBaseline()
{
    std::vector<std::string> out;

    std::map<std::string, std::string> base;
    if (!ParseKeyValues(AutoConfig_BaselinePath(), base))
        return out;   // no baseline taken yet — nothing to compare against

    std::map<std::string, std::string> cur;
    std::ostringstream live;
    live.imbue(std::locale::classic());
    live.precision(std::numeric_limits<float>::max_digits10);
    SerializeIni(live);
    std::istringstream input(live.str());
    std::string line;
    while (std::getline(input, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos)
            cur[line.substr(0, eq)] = line.substr(eq + 1);
    }

    for (const auto& kv : cur) {
        const auto it = base.find(kv.first);
        if (it == base.end())
            out.push_back("+ " + kv.first + " (new) = " + kv.second);
        else if (it->second != kv.second)
            out.push_back(kv.first + ": " + it->second + " -> " + kv.second);
    }
    for (const auto& kv : base) {
        if (cur.find(kv.first) == cur.end())
            out.push_back("- " + kv.first + " (removed, was " + kv.second + ")");
    }
    return out;
}
