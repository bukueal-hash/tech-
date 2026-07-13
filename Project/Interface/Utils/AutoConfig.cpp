#include "AutoConfig.h"
#include "Variables/index.h"
#include "../../Core/WorldItemCategory.h"
#include "../../Hardware/KmBox.h"
#include "../OverlayHost.h"
#include "../Render.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

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
    if (sscanf_s(val.c_str(), "%f,%f,%f,%f", &r, &g, &b, &a) != 4)
        return false;
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
    return true;
}

void WriteColor4(std::ofstream& file, const char* key, const float col[4])
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
    bool health{};
    bool names{};
    bool show_weapon{};
    bool snaplines{};
    bool skeleton{};
    bool silhouette{};
    float silhouette_max_distance_m{};
    bool show_distance{};
    bool hide_allies{};
    bool show_radar{};
    bool show_debug_overlay{};
    float esp_text_scale{};
    float radar_scale{};
    float radar_range{};
    float radar_pos_x_norm{};
    float radar_pos_y_norm{};
    int radar_loot_min_rarity{};
    bool show_radar_special{};
    bool radar_shape_circle{};
    float esp_distance{};
    float esp_color_visible[4]{};
    float esp_color_invisible[4]{};

    bool bot_box{};
    bool bot_names{};
    bool bot_snaplines{};
    bool bot_show_distance{};
    bool bot_heart{};
    bool show_dead_bots{};
    float bot_esp_distance{};
    float color_dead_bots[4]{};
    float bot_color_visible[4]{};
    float bot_color_invisible[4]{};

    bool enable_aimbot{};
    bool enable_triggerbot{};
    bool robotAimEnabled{};
    int aim_hold_key{};
    int aim_bone_mode{};
    bool visiblecheck{};
    bool obstruction_check{};
    bool vischeck_auto_thin{};
    bool predict{};
    bool humanizer{};
    bool randombone{};
    float aimbot_fov{};
    int aimbot_priority{};
    bool show_fov{};
    float aimbot_distance{};
    float smoothness{};
    int aim_algorithm{};
    float aim_deadzone_px{};
    float aim_sensitivity{};
    float aim_hardware_speed{};
    bool sticky_target_lock{};
    float aim_sticky_fov_bias_px{};
    int aim_loss_of_sight_grace_ms{};
    bool aim_loss_of_sight_grace_enabled{};

    float aim_bullet_speed_cm_s{};

    bool enable_world{};
    bool droppedItems{};
    bool raiderStock{};
    bool showRobots{};
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
    bool show_world_field_crate{};
    bool show_world_supply_station{};
    bool show_world_keys{};
    bool show_world_locker{};
    bool show_world_safe{};
    bool show_world_buried{};
    bool show_world_deaddrop{};
    bool show_world_open_container{};
    float world_distance{};
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
    float color_world_field_crate[4]{};
    float color_world_supply_station[4]{};
    float color_world_keys[4]{};
    float color_world_locker[4]{};
    float color_world_safe[4]{};
    float color_world_buried[4]{};
    float color_world_deaddrop[4]{};
    float color_world_open_container[4]{};

    bool showLoot{};
    float loot_distance{};
    float container_distance_sp{};
    float color_loot[4]{};
    bool loot_rarity_color{};
    bool show_loot_value{};
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
        var::color_world_field_crate,
        var::color_world_supply_station,
        var::color_world_keys,
        var::color_world_locker,
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
    s.health = var::health;
    s.names = var::names;
    s.show_weapon = var::show_weapon;
    s.snaplines = var::snaplines;
    s.skeleton = var::skeleton;
    s.silhouette = var::silhouette;
    s.silhouette_max_distance_m = var::silhouette_max_distance_m;
    s.show_distance = var::show_distance;
    s.hide_allies = var::hide_allies;
    s.show_radar = var::show_radar;
    s.show_debug_overlay = var::show_debug_overlay;
    s.esp_text_scale = var::esp_text_scale;
    s.radar_scale = var::radar_scale;
    s.radar_range = var::radar_range;
    s.radar_pos_x_norm = var::radar_pos_x_norm;
    s.radar_pos_y_norm = var::radar_pos_y_norm;
    s.radar_loot_min_rarity = var::radar_loot_min_rarity;
    s.show_radar_special = var::show_radar_special;
    s.radar_shape_circle = var::radar_shape_circle;
    s.esp_distance = var::esp_distance;
    std::memcpy(s.esp_color_visible, var::esp_color_visible, sizeof(s.esp_color_visible));
    std::memcpy(s.esp_color_invisible, var::esp_color_invisible, sizeof(s.esp_color_invisible));
    s.bot_box = var::bot_box;
    s.bot_names = var::bot_names;
    s.bot_snaplines = var::bot_snaplines;
    s.bot_show_distance = var::bot_show_distance;
    s.bot_heart = var::bot_heart;
    s.show_dead_bots = var::show_dead_bots;
    s.bot_esp_distance = var::bot_esp_distance;
    std::memcpy(s.bot_color_visible, var::bot_color_visible, sizeof(s.bot_color_visible));
    std::memcpy(s.bot_color_invisible, var::bot_color_invisible, sizeof(s.bot_color_invisible));
    std::memcpy(s.color_dead_bots, var::color_dead_bots, sizeof(s.color_dead_bots));

    s.enable_aimbot = var::enable_aimbot;
    s.enable_triggerbot = var::enable_triggerbot;
    s.robotAimEnabled = var::robotAimEnabled;
    s.aim_hold_key = var::aim_hold_key;
    s.aim_bone_mode = static_cast<int>(var::aim_bone_mode);
    s.visiblecheck = var::visiblecheck;
    s.obstruction_check = var::obstruction_check;
    s.vischeck_auto_thin = var::vischeck_auto_thin;
    s.predict = var::predict;
    s.humanizer = var::humanizer;
    s.randombone = var::randombone;
    s.aimbot_fov = var::aimbot_fov;
    s.aimbot_priority = static_cast<int>(var::aimbot_priority);
    s.show_fov = var::show_fov;
    s.aimbot_distance = var::aimbot_distance;
    s.smoothness = var::smoothness;
    s.aim_algorithm = static_cast<int>(var::aim_algorithm);
    s.aim_deadzone_px = var::aim_deadzone_px;
    s.aim_sensitivity = var::aim_sensitivity;
    s.aim_hardware_speed = var::aim_hardware_speed;
    s.sticky_target_lock = var::sticky_target_lock;
    s.aim_sticky_fov_bias_px = var::aim_sticky_fov_bias_px;
    s.aim_loss_of_sight_grace_ms = var::aim_loss_of_sight_grace_ms;
    s.aim_loss_of_sight_grace_enabled = var::aim_loss_of_sight_grace_enabled;
    s.aim_bullet_speed_cm_s = var::aim_bullet_speed_cm_s;

    s.enable_world = var::enable_world;
    s.droppedItems = var::droppedItems;
    s.raiderStock = var::raiderStock;
    s.showRobots = var::showRobots;
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
    s.show_world_field_crate = var::show_world_field_crate;
    s.show_world_supply_station = var::show_world_supply_station;
    s.show_world_keys = var::show_world_keys;
    s.show_world_locker = var::show_world_locker;
    s.show_world_safe = var::show_world_safe;
    s.show_world_buried = var::show_world_buried;
    s.show_world_deaddrop = var::show_world_deaddrop;
    s.show_world_open_container = var::show_world_open_container;
    s.world_distance = var::world_distance;
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
    std::memcpy(s.color_world_field_crate, var::color_world_field_crate, sizeof(s.color_world_field_crate));
    std::memcpy(s.color_world_supply_station, var::color_world_supply_station, sizeof(s.color_world_supply_station));
    std::memcpy(s.color_world_keys, var::color_world_keys, sizeof(s.color_world_keys));
    std::memcpy(s.color_world_locker, var::color_world_locker, sizeof(s.color_world_locker));
    std::memcpy(s.color_world_safe, var::color_world_safe, sizeof(s.color_world_safe));
    std::memcpy(s.color_world_buried, var::color_world_buried, sizeof(s.color_world_buried));
    std::memcpy(s.color_world_deaddrop, var::color_world_deaddrop, sizeof(s.color_world_deaddrop));
    std::memcpy(s.color_world_open_container, var::color_world_open_container, sizeof(s.color_world_open_container));

    s.showLoot = var::showLoot;
    s.loot_distance = var::loot_distance;
    s.container_distance_sp = var::container_distance_sp;
    std::memcpy(s.color_loot, var::color_loot, sizeof(s.color_loot));
    s.loot_rarity_color = var::loot_rarity_color;
    s.show_loot_value = var::show_loot_value;
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
        a.health == b.health &&
        a.names == b.names &&
        a.show_weapon == b.show_weapon &&
        a.snaplines == b.snaplines &&
        a.skeleton == b.skeleton &&
        a.silhouette == b.silhouette &&
        a.silhouette_max_distance_m == b.silhouette_max_distance_m &&
        a.show_distance == b.show_distance &&
        a.hide_allies == b.hide_allies &&
        a.show_radar == b.show_radar &&
        a.show_debug_overlay == b.show_debug_overlay &&
        a.esp_text_scale == b.esp_text_scale &&
        a.radar_scale == b.radar_scale &&
        a.radar_range == b.radar_range &&
        a.radar_pos_x_norm == b.radar_pos_x_norm &&
        a.radar_pos_y_norm == b.radar_pos_y_norm &&
        a.radar_loot_min_rarity == b.radar_loot_min_rarity &&
        a.show_radar_special == b.show_radar_special &&
        a.radar_shape_circle == b.radar_shape_circle &&
        a.esp_distance == b.esp_distance &&
        ColorsEqual(a.esp_color_visible, b.esp_color_visible) &&
        ColorsEqual(a.esp_color_invisible, b.esp_color_invisible) &&
        a.bot_box == b.bot_box &&
        a.bot_names == b.bot_names &&
        a.bot_snaplines == b.bot_snaplines &&
        a.bot_show_distance == b.bot_show_distance &&
        a.bot_heart == b.bot_heart &&
        a.show_dead_bots == b.show_dead_bots &&
        a.bot_esp_distance == b.bot_esp_distance &&
        ColorsEqual(a.bot_color_visible, b.bot_color_visible) &&
        ColorsEqual(a.bot_color_invisible, b.bot_color_invisible) &&
        ColorsEqual(a.color_dead_bots, b.color_dead_bots) &&
        a.enable_aimbot == b.enable_aimbot &&
        a.enable_triggerbot == b.enable_triggerbot &&
        a.robotAimEnabled == b.robotAimEnabled &&
        a.aim_hold_key == b.aim_hold_key &&
        a.aim_bone_mode == b.aim_bone_mode &&
        a.visiblecheck == b.visiblecheck &&
        a.obstruction_check == b.obstruction_check &&
        a.vischeck_auto_thin == b.vischeck_auto_thin &&
        a.predict == b.predict &&
        a.humanizer == b.humanizer &&
        a.randombone == b.randombone &&
        a.aimbot_fov == b.aimbot_fov &&
        a.aimbot_priority == b.aimbot_priority &&
        a.show_fov == b.show_fov &&
        a.aimbot_distance == b.aimbot_distance &&
        a.smoothness == b.smoothness &&
        a.aim_algorithm == b.aim_algorithm &&
        a.aim_deadzone_px == b.aim_deadzone_px &&
        a.aim_sensitivity == b.aim_sensitivity &&
        a.aim_hardware_speed == b.aim_hardware_speed &&
        a.sticky_target_lock == b.sticky_target_lock &&
        a.aim_sticky_fov_bias_px == b.aim_sticky_fov_bias_px &&
        a.aim_loss_of_sight_grace_ms == b.aim_loss_of_sight_grace_ms &&
        a.aim_loss_of_sight_grace_enabled == b.aim_loss_of_sight_grace_enabled &&
        a.aim_bullet_speed_cm_s == b.aim_bullet_speed_cm_s &&
        a.enable_world == b.enable_world &&
        a.droppedItems == b.droppedItems &&
        a.raiderStock == b.raiderStock &&
        a.showRobots == b.showRobots &&
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
        a.show_world_field_crate == b.show_world_field_crate &&
        a.show_world_supply_station == b.show_world_supply_station &&
        a.show_world_keys == b.show_world_keys &&
        a.show_world_locker == b.show_world_locker &&
        a.show_world_safe == b.show_world_safe &&
        a.show_world_buried == b.show_world_buried &&
        a.show_world_deaddrop == b.show_world_deaddrop &&
        a.show_world_open_container == b.show_world_open_container &&
        a.world_distance == b.world_distance &&
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
        ColorsEqual(a.color_world_field_crate, b.color_world_field_crate) &&
        ColorsEqual(a.color_world_supply_station, b.color_world_supply_station) &&
        ColorsEqual(a.color_world_keys, b.color_world_keys) &&
        ColorsEqual(a.color_world_locker, b.color_world_locker) &&
        ColorsEqual(a.color_world_safe, b.color_world_safe) &&
        ColorsEqual(a.color_world_buried, b.color_world_buried) &&
        ColorsEqual(a.color_world_deaddrop, b.color_world_deaddrop) &&
        ColorsEqual(a.color_world_open_container, b.color_world_open_container) &&
        a.showLoot == b.showLoot &&
        a.loot_distance == b.loot_distance &&
        a.container_distance_sp == b.container_distance_sp &&
        ColorsEqual(a.color_loot, b.color_loot) &&
        a.loot_rarity_color == b.loot_rarity_color &&
        a.show_loot_value == b.show_loot_value &&
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
    if (key == "hideOpenedLoot") {
        if (ParseBool(val, false))
            var::show_world_open_container = false;
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
    if (key == "container_distance_long") {
        var::container_distance_sp = std::clamp(static_cast<float>(std::atof(val.c_str())), 20.f, var::kMaxDistanceSliderM);
        return true;
    }
    if (key.rfind("container_range_sp_", 0) == 0) {
        const std::string suffix = key.substr(19);
        if (TrySetContainerRangeFromConfigSuffix(suffix.c_str(), ParseBool(val, false)))
            return true;
    }
    if (key.rfind("container_range_long_", 0) == 0) {
        const std::string suffix = key.substr(21);
        if (TrySetContainerRangeFromConfigSuffix(suffix.c_str(), ParseBool(val, false)))
            return true;
    }
    if (key == "color_loot") { ParseColor4(val, var::color_loot); return true; }
    if (key == "loot_rarity_color") { var::loot_rarity_color = ParseBool(val, var::loot_rarity_color); return true; }
    if (key == "show_loot_value") { var::show_loot_value = ParseBool(val, var::show_loot_value); return true; }
    if (key == "loot_min_value") { var::loot_min_value = static_cast<float>(std::atof(val.c_str())); return true; }
    if (key == "loot_min_rarity") { var::loot_min_rarity = std::atoi(val.c_str()); return true; }
    if (key == "loot_min_val_sp") { var::loot_min_val_sp = ParseBool(val, var::loot_min_val_sp); return true; }
    if (key == "loot_min_rar_sp") { var::loot_min_rar_sp = ParseBool(val, var::loot_min_rar_sp); return true; }
    return false;
}

static bool ApplyAimbotConfigKey(const std::string& key, const std::string& val)
{
    if (key == "aim_sticky_fov_bias_px") {
        var::aim_sticky_fov_bias_px = static_cast<float>(std::atof(val.c_str()));
        var::aim_sticky_fov_bias_px = std::clamp(var::aim_sticky_fov_bias_px, 0.f, 120.f);
        return true;
    }
    if (key == "aim_loss_of_sight_grace_ms") {
        var::aim_loss_of_sight_grace_ms = std::clamp(std::atoi(val.c_str()), 0, 2000);
        return true;
    }
    if (key == "aim_loss_of_sight_grace_enabled") {
        var::aim_loss_of_sight_grace_enabled = ParseBool(val, var::aim_loss_of_sight_grace_enabled);
        return true;
    }
    if (key == "aim_bullet_speed_cm_s") {
        var::aim_bullet_speed_cm_s = static_cast<float>(std::atof(val.c_str()));
        var::aim_bullet_speed_cm_s = std::clamp(var::aim_bullet_speed_cm_s, 10000.f, 200000.f);
        return true;
    }
    if (key == "aim_deadzone_px") {
        var::aim_deadzone_px = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.f, 50.f);
        return true;
    }
    if (key == "aim_sensitivity") {
        var::aim_sensitivity = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.25f, 4.f);
        return true;
    }
    if (key == "aim_hardware_speed") {
        var::aim_hardware_speed = std::clamp(static_cast<float>(std::atof(val.c_str())), 1.f, 20.f);
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
        {"color_world_field_crate", var::color_world_field_crate},
        {"color_world_supply_station", var::color_world_supply_station},
        {"color_world_keys", var::color_world_keys},
        {"color_world_locker", var::color_world_locker},
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
    if (key == "show_world_field_crate") { var::show_world_field_crate = ParseBool(val, var::show_world_field_crate); return true; }
    if (key == "show_world_supply_station") { var::show_world_supply_station = ParseBool(val, var::show_world_supply_station); return true; }
    if (key == "show_world_keys") { var::show_world_keys = ParseBool(val, var::show_world_keys); return true; }
    if (key == "show_world_locker") { var::show_world_locker = ParseBool(val, var::show_world_locker); return true; }
    if (key == "show_world_open_container") { var::show_world_open_container = ParseBool(val, var::show_world_open_container); return true; }
    if (key == "show_world_safe") { var::show_world_safe = ParseBool(val, var::show_world_safe); return true; }
    if (key == "show_world_buried") { var::show_world_buried = ParseBool(val, var::show_world_buried); return true; }
    if (key == "show_world_deaddrop") { var::show_world_deaddrop = ParseBool(val, var::show_world_deaddrop); return true; }
    if (key == "showArc") { var::showArc = ParseBool(val, var::showArc); return true; }
    if (key == "showDeadPlayers") { var::showDeadPlayers = ParseBool(val, var::showDeadPlayers); return true; }
    if (key == "world_distance") {
        var::world_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.f, var::kMaxDistanceSliderM);
        return true;
    }
    return false;
}

void ApplyKeyValue(const std::string& key, const std::string& val)
{
    if (ApplyLootConfigKey(key, val))
        return;
    if (ApplyAimbotConfigKey(key, val))
        return;
    if (ApplyWorldConfigKey(key, val))
        return;

    if (key == "enableesp") var::enableesp = ParseBool(val, var::enableesp);
    else if (key == "box") var::box = ParseBool(val, var::box);
    else if (key == "health") var::health = ParseBool(val, var::health);
    else if (key == "names") var::names = ParseBool(val, var::names);
    else if (key == "show_weapon") var::show_weapon = ParseBool(val, var::show_weapon);
    else if (key == "snaplines") var::snaplines = ParseBool(val, var::snaplines);
    else if (key == "skeleton") var::skeleton = ParseBool(val, var::skeleton);
    else if (key == "silhouette") var::silhouette = ParseBool(val, var::silhouette);
    else if (key == "silhouette_max_distance_m") {
        var::silhouette_max_distance_m = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.f, var::kMaxDistanceSliderM);
    }
    else if (key == "show_distance") var::show_distance = ParseBool(val, var::show_distance);
    else if (key == "hide_allies") var::hide_allies = ParseBool(val, var::hide_allies);
    else if (key == "show_radar") var::show_radar = ParseBool(val, var::show_radar);
    else if (key == "show_debug_overlay") var::show_debug_overlay = ParseBool(val, var::show_debug_overlay);
    else if (key == "esp_text_scale") var::esp_text_scale = static_cast<float>(std::atof(val.c_str()));
    else if (key == "ui_text_scale") var::esp_text_scale = static_cast<float>(std::atof(val.c_str())); // back-compat: old key name
    else if (key == "radar_scale") var::radar_scale = static_cast<float>(std::atof(val.c_str()));
    else if (key == "radar_range") {
        var::radar_range = std::clamp(static_cast<float>(std::atof(val.c_str())), 20.f, var::kMaxDistanceSliderM);
    }
    else if (key == "radar_pos_x_norm") var::radar_pos_x_norm = static_cast<float>(std::atof(val.c_str()));
    else if (key == "radar_pos_y_norm") var::radar_pos_y_norm = static_cast<float>(std::atof(val.c_str()));
    else if (key == "radar_loot_min_rarity") var::radar_loot_min_rarity = std::atoi(val.c_str());
    else if (key == "show_radar_special") var::show_radar_special = ParseBool(val, var::show_radar_special);
    else if (key == "radar_shape_circle") var::radar_shape_circle = ParseBool(val, var::radar_shape_circle);
    else if (key == "esp_distance") {
        var::esp_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 50.f, var::kMaxDistanceSliderM);
    }
    else if (key == "esp_color_visible") ParseColor4(val, var::esp_color_visible);
    else if (key == "esp_color_invisible") ParseColor4(val, var::esp_color_invisible);
    else if (key == "bot_box") var::bot_box = ParseBool(val, var::bot_box);
    else if (key == "bot_names") var::bot_names = ParseBool(val, var::bot_names);
    else if (key == "bot_snaplines") var::bot_snaplines = ParseBool(val, var::bot_snaplines);
    else if (key == "bot_show_distance") var::bot_show_distance = ParseBool(val, var::bot_show_distance);
    else if (key == "bot_heart") var::bot_heart = ParseBool(val, var::bot_heart);
    else if (key == "show_dead_bots") var::show_dead_bots = ParseBool(val, var::show_dead_bots);
    else if (key == "color_dead_bots") ParseColor4(val, var::color_dead_bots);
    else if (key == "bot_esp_distance") {
        var::bot_esp_distance = std::clamp(static_cast<float>(std::atof(val.c_str())), 50.f, var::kMaxDistanceSliderM);
    }
    else if (key == "bot_color_visible") ParseColor4(val, var::bot_color_visible);
    else if (key == "bot_color_invisible") ParseColor4(val, var::bot_color_invisible);
    else if (key == "enable_aimbot") var::enable_aimbot = ParseBool(val, var::enable_aimbot);
    else if (key == "enable_triggerbot") var::enable_triggerbot = ParseBool(val, var::enable_triggerbot);
    else if (key == "robotAimEnabled") var::robotAimEnabled = ParseBool(val, var::robotAimEnabled);
    else if (key == "aim_hold_key") var::aim_hold_key = std::atoi(val.c_str());
    else if (key == "aim_bone_mode") {
        const int m = std::clamp(std::atoi(val.c_str()), 0, 5);
        var::aim_bone_mode = static_cast<AimBoneMode>(m);
    }
    else if (key == "visiblecheck") var::visiblecheck = ParseBool(val, var::visiblecheck);
    else if (key == "obstruction_check") var::obstruction_check = ParseBool(val, var::obstruction_check);
    else if (key == "vischeck_auto_thin") var::vischeck_auto_thin = ParseBool(val, var::vischeck_auto_thin);
    else if (key == "predict" || key == "Prediction") var::predict = ParseBool(val, var::predict);
    else if (key == "humanizer") var::humanizer = ParseBool(val, var::humanizer);
    else if (key == "randombone") var::randombone = ParseBool(val, var::randombone);
    else if (key == "aimbot_fov") var::aimbot_fov = static_cast<float>(std::atof(val.c_str()));
    else if (key == "aimbot_priority") {
        const int prio = std::clamp(std::atoi(val.c_str()), 0, 4);
        var::aimbot_priority = static_cast<AimbotPriority>(prio);
    }
    else if (key == "show_fov") var::show_fov = ParseBool(val, var::show_fov);
    else if (key == "aimbot_distance") {
        var::aimbot_distance = static_cast<float>(std::atof(val.c_str()));
        var::aimbot_distance = std::clamp(var::aimbot_distance, 0.f, var::kMaxDistanceSliderM);
    }
    else if (key == "smoothness") {
        var::smoothness = std::clamp(static_cast<float>(std::atof(val.c_str())), 1.f, 20.f);
    }
    else if (key == "aim_algorithm") {
        const int algo = std::atoi(val.c_str());
        var::aim_algorithm = (algo == 0) ? AimAlgorithm::Linear : AimAlgorithm::Accelerated;
    }
    else if (key == "sticky_target_lock") var::sticky_target_lock = ParseBool(val, var::sticky_target_lock);
    else if (key == "showmenu") showmenu = ParseBool(val, showmenu);
    else if (key == "kmbox_type" || key == "typeName")
        g_kmbox.kmboxConfig.type = val;
    else if (key == "kmbox_comPort" || key == "comPort")
        g_kmbox.kmboxConfig.comPort = val;
    else if (key == "kmbox_baudRate" || key == "baudRate")
        g_kmbox.kmboxConfig.baudRate = val;
    else if (key == "kmbox_ip" || key == "ip")
        g_kmbox.kmboxConfig.ip = val;
    else if (key == "kmbox_port" || key == "port")
        g_kmbox.kmboxConfig.port = val;
    else if (key == "kmbox_uuid" || key == "uuid")
        g_kmbox.kmboxConfig.uuid = val;
    else if (key == "kmbox_minDelay" || key == "minDelay")
        g_kmbox.kmboxConfig.minDelay = std::atoi(val.c_str());
    else if (key == "kmbox_monitorIndex" || key == "monitorIndex") {
        g_kmbox.kmboxConfig.monitorIndex = std::atoi(val.c_str());
        OverlayDisplay_SetSelectedMonitor(g_kmbox.kmboxConfig.monitorIndex);
    }
    else if (key == "type") {
        const int typeCode = std::atoi(val.c_str());
        if (typeCode == 0)
            g_kmbox.kmboxConfig.type = "BPro";
        else if (typeCode == 1)
            g_kmbox.kmboxConfig.type = "Net";
        else if (typeCode == 2)
            g_kmbox.kmboxConfig.type = "MAKCU";
    }
}

bool LoadIniFile(const std::string& path)
{
    s_anyWorldColorKeyInIni = false;

    std::ifstream file(path);
    if (!file.is_open())
        return false;

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
        ApplyKeyValue(key, val);
    }

    if (g_kmbox.kmboxConfig.monitorIndex < 0)
        g_kmbox.kmboxConfig.monitorIndex = 0;

    g_kmbox.SetRememberConfig(true);
    return true;
}

void WriteIni()
{
    const std::string path = ConfigPath();
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return;
    }

    file << "# Arc Raiders auto config (saved automatically)\n";
    file << "configVersion=1\n\n";

    file << "enableesp=" << (var::enableesp ? 1 : 0) << '\n';
    file << "box=" << (var::box ? 1 : 0) << '\n';
    file << "health=" << (var::health ? 1 : 0) << '\n';
    file << "names=" << (var::names ? 1 : 0) << '\n';
    file << "show_weapon=" << (var::show_weapon ? 1 : 0) << '\n';
    file << "snaplines=" << (var::snaplines ? 1 : 0) << '\n';
    file << "skeleton=" << (var::skeleton ? 1 : 0) << '\n';
    file << "silhouette=" << (var::silhouette ? 1 : 0) << '\n';
    file << "silhouette_max_distance_m=" << var::silhouette_max_distance_m << '\n';
    file << "show_distance=" << (var::show_distance ? 1 : 0) << '\n';
    file << "hide_allies=" << (var::hide_allies ? 1 : 0) << '\n';
    file << "show_radar=" << (var::show_radar ? 1 : 0) << '\n';
    file << "show_debug_overlay=" << (var::show_debug_overlay ? 1 : 0) << '\n';
    file << "esp_text_scale=" << var::esp_text_scale << '\n';
    file << "radar_scale=" << var::radar_scale << '\n';
    file << "radar_range=" << var::radar_range << '\n';
    file << "radar_pos_x_norm=" << var::radar_pos_x_norm << '\n';
    file << "radar_pos_y_norm=" << var::radar_pos_y_norm << '\n';
    file << "radar_loot_min_rarity=" << var::radar_loot_min_rarity << '\n';
    file << "show_radar_special=" << (var::show_radar_special ? 1 : 0) << '\n';
    file << "radar_shape_circle=" << (var::radar_shape_circle ? 1 : 0) << '\n';
    file << "esp_distance=" << var::esp_distance << '\n';
    WriteColor4(file, "esp_color_visible", var::esp_color_visible);
    WriteColor4(file, "esp_color_invisible", var::esp_color_invisible);
    file << "bot_box=" << (var::bot_box ? 1 : 0) << '\n';
    file << "bot_names=" << (var::bot_names ? 1 : 0) << '\n';
    file << "bot_snaplines=" << (var::bot_snaplines ? 1 : 0) << '\n';
    file << "bot_show_distance=" << (var::bot_show_distance ? 1 : 0) << '\n';
    file << "bot_heart=" << (var::bot_heart ? 1 : 0) << '\n';
    file << "show_dead_bots=" << (var::show_dead_bots ? 1 : 0) << '\n';
    WriteColor4(file, "color_dead_bots", var::color_dead_bots);
    file << "bot_esp_distance=" << var::bot_esp_distance << '\n';
    WriteColor4(file, "bot_color_visible", var::bot_color_visible);
    WriteColor4(file, "bot_color_invisible", var::bot_color_invisible);

    file << "enable_aimbot=" << (var::enable_aimbot ? 1 : 0) << '\n';
    file << "enable_triggerbot=" << (var::enable_triggerbot ? 1 : 0) << '\n';
    file << "robotAimEnabled=" << (var::robotAimEnabled ? 1 : 0) << '\n';
    file << "aim_hold_key=" << var::aim_hold_key << '\n';
    file << "aim_bone_mode=" << static_cast<int>(var::aim_bone_mode) << '\n';
    file << "visiblecheck=" << (var::visiblecheck ? 1 : 0) << '\n';
    file << "obstruction_check=" << (var::obstruction_check ? 1 : 0) << '\n';
    file << "vischeck_auto_thin=" << (var::vischeck_auto_thin ? 1 : 0) << '\n';
    file << "predict=" << (var::predict ? 1 : 0) << '\n';
    file << "humanizer=" << (var::humanizer ? 1 : 0) << '\n';
    file << "randombone=" << (var::randombone ? 1 : 0) << '\n';
    file << "aimbot_fov=" << var::aimbot_fov << '\n';
    file << "aimbot_priority=" << static_cast<int>(var::aimbot_priority) << '\n';
    file << "show_fov=" << (var::show_fov ? 1 : 0) << '\n';
    file << "aimbot_distance=" << var::aimbot_distance << '\n';
    file << "smoothness=" << var::smoothness << '\n';
    file << "aim_algorithm=" << static_cast<int>(var::aim_algorithm) << '\n';
    file << "aim_deadzone_px=" << var::aim_deadzone_px << '\n';
    file << "aim_sensitivity=" << var::aim_sensitivity << '\n';
    file << "aim_hardware_speed=" << var::aim_hardware_speed << '\n';
    file << "sticky_target_lock=" << (var::sticky_target_lock ? 1 : 0) << '\n';
    file << "aim_sticky_fov_bias_px=" << var::aim_sticky_fov_bias_px << '\n';
    file << "aim_loss_of_sight_grace_ms=" << var::aim_loss_of_sight_grace_ms << '\n';
    file << "aim_loss_of_sight_grace_enabled=" << (var::aim_loss_of_sight_grace_enabled ? 1 : 0) << '\n';
    file << "aim_bullet_speed_cm_s=" << var::aim_bullet_speed_cm_s << '\n';

    file << "enable_world=" << (var::enable_world ? 1 : 0) << '\n';
    file << "droppedItems=" << (var::droppedItems ? 1 : 0) << '\n';
    file << "raiderStock=" << (var::raiderStock ? 1 : 0) << '\n';
    file << "showRobots=" << (var::showRobots ? 1 : 0) << '\n';
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
    file << "show_world_field_crate=" << (var::show_world_field_crate ? 1 : 0) << '\n';
    file << "show_world_supply_station=" << (var::show_world_supply_station ? 1 : 0) << '\n';
    file << "show_world_keys=" << (var::show_world_keys ? 1 : 0) << '\n';
    file << "show_world_locker=" << (var::show_world_locker ? 1 : 0) << '\n';
    file << "show_world_open_container=" << (var::show_world_open_container ? 1 : 0) << '\n';
    file << "show_world_safe=" << (var::show_world_safe ? 1 : 0) << '\n';
    file << "show_world_buried=" << (var::show_world_buried ? 1 : 0) << '\n';
    file << "show_world_deaddrop=" << (var::show_world_deaddrop ? 1 : 0) << '\n';
    file << "showArc=" << (var::showArc ? 1 : 0) << '\n';
    file << "showDeadPlayers=" << (var::showDeadPlayers ? 1 : 0) << '\n';
    file << "world_distance=" << var::world_distance << '\n';
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
    WriteColor4(file, "color_world_field_crate", var::color_world_field_crate);
    WriteColor4(file, "color_world_supply_station", var::color_world_supply_station);
    WriteColor4(file, "color_world_keys", var::color_world_keys);
    WriteColor4(file, "color_world_locker", var::color_world_locker);
    WriteColor4(file, "color_world_open_container", var::color_world_open_container);
    WriteColor4(file, "color_world_safe", var::color_world_safe);
    WriteColor4(file, "color_world_buried", var::color_world_buried);
    WriteColor4(file, "color_world_deaddrop", var::color_world_deaddrop);

    file << "showLoot=" << (var::showLoot ? 1 : 0) << '\n';
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
    const std::string path = ConfigPath();
    WriteIni();
    s_last = CaptureSnapshot();
    s_snapshotValid = true;
    s_dirty = false;
}

void AutoConfig_Tick()
{
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
        AutoConfig_SaveNow();
    }
}