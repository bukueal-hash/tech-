#include "index.h"

namespace var {
    /* Esp */
    bool enableesp = true;
    bool box = true;
    bool health = false;
    bool names = false;
    bool show_weapon = false;
    bool snaplines = false;
    bool skeleton = true;
    bool silhouette = false;
    bool silhouette_soft_fill = true;
    float silhouette_max_distance_m = 25.f;
    bool show_distance = false;
    bool hide_allies = true;
    float esp_distance = 500.f;
    float esp_color_visible[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float esp_color_invisible[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

    /* Bot ESP */
    bool bot_box = true;
    bool bot_names = true;
    bool bot_snaplines = false;
    bool bot_show_distance = true;
    bool bot_heart = true;
    float bot_esp_distance = 500.f;
    float bot_color_visible[4] = { 0.25f, 0.55f, 1.0f, 1.0f };
    float bot_color_invisible[4] = { 0.15f, 0.35f, 0.85f, 1.0f };

    /* Aimbot */
    bool enable_aimbot = false;
    bool robotAimEnabled = false;
    int aim_hold_key = 0x10; // VK_SHIFT
    AimBoneMode aim_bone_mode = AimBoneMode::Chest;
    bool predict = true;
    bool humanizer = false;
    bool randombone = true;
    float aimbot_fov = 50.f;
    AimbotPriority aimbot_priority = AimbotPriority::Fov;
    bool show_fov = true;
    float aimbot_distance = 500.f;
    float aim_deadzone_px = 0.f;
    float smoothness = 3.f;
    AimAlgorithm aim_algorithm = AimAlgorithm::Accelerated;
    float aim_sensitivity = 1.0f;
    float aim_hardware_speed = 15.f;
    bool sticky_target_lock = true;
    float aim_sticky_fov_bias_px = 35.f;
    int aim_loss_of_sight_grace_ms = 250;
    bool aim_loss_of_sight_grace_enabled = true;
    float aim_bullet_speed_cm_s = 80000.f;

    /* World */
    bool enable_world = true;
    bool droppedItems = true;
    bool raiderStock = false;
    bool showRobots = true;
    bool show_dead_bots = false;
    bool showArc = false;
    bool show_world_items = true;
    bool show_world_ammo = true;
    bool show_world_arc_loot = true;
    bool show_world_backpack = true;
    bool show_world_crate = true;
    bool show_world_furniture = true;
    bool show_world_grenade = true;
    bool show_world_harvestable = true;
    bool show_world_industrial = true;
    bool show_world_medical = true;
    bool show_world_other = true;
    bool show_world_probe = true;
    bool show_world_vehicles = true;
    bool show_world_weapon_case = true;
    bool show_world_field_crate = true;
    bool show_world_supply_station = true;
    bool show_world_keys = true;
    bool show_world_locker = true;
    bool show_world_trash = true;
    bool show_world_safe = true;
    bool show_world_buried = true;
    bool show_world_deaddrop = true;
    bool show_world_open_container = true;
    float color_dead_bots[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    bool showDeadPlayers = true;
    float color_dropped_items[4] = { 1.0f, 1.0f, 0.0f, 1.0f };
    float color_raider_stock[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
    float color_arc_entities[4] = { 0.0f, 1.0f, 1.0f, 1.0f };
    float color_world_corpses[4] = { 144.f / 255.f, 238.f / 255.f, 144.f / 255.f, 1.0f };
    float color_world_items[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_ammo[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_arc_loot[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_backpack[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_crate[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_furniture[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_grenade[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_harvestable[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_industrial[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_medical[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_other[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_probe[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_vehicles[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_weapon_case[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_field_crate[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_supply_station[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    float color_world_keys[4] = { 1.0f, 215.f / 255.f, 0.0f, 1.0f };
    float color_world_locker[4] = { 0.6f, 0.85f, 1.0f, 1.0f };
    float color_world_trash[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    float color_world_safe[4] = { 0.85f, 0.75f, 0.2f, 1.0f };
    float color_world_buried[4] = { 0.75f, 0.55f, 0.35f, 1.0f };
    float color_world_deaddrop[4] = { 0.9f, 0.4f, 0.9f, 1.0f };
    float color_world_open_container[4] = { 0.55f, 0.55f, 0.55f, 1.0f };

    /* Loot */
    bool showLoot = true;
    bool show_near_loot_hud = false;
    float loot_distance = 500.f;
    float container_distance_sp = 500.f;
    float color_loot[4] = { 1.0f, 165.f / 255.f, 0.0f, 1.0f };
    bool loot_rarity_color = true;
    bool show_loot_value = false;
    float loot_min_value = 0.f;
    int loot_min_rarity = 0;

    bool loot_min_val_sp = false;
    bool loot_min_rar_sp = false;

    /* UI */
    float esp_text_scale = 1.0f;

    /* Debug */
    bool show_debug_overlay = false;

    /* Radar */
    bool show_radar = false;
    float radar_scale = 80.f;
    float radar_range = 100.f;
    float radar_pos_x_norm = 0.92f;
    float radar_pos_y_norm = 0.12f;
    int radar_loot_min_rarity = 0;
    bool show_radar_special = false;
    bool radar_shape_circle = true;
}