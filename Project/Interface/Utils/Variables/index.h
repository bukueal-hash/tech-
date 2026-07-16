#pragma once

enum class AimbotPriority : int {
    Fov = 0,
    Distance = 1,
    Threat = 2,
    LowHealth = 3,
    FovDistance = 4,
};

enum class AimBoneMode : int {
    Head = 0,
    Chest = 1,
    Pelvis = 2,
    Arms = 3,
    Legs = 4,
    ClosestBone = 5,
};

enum class AimAlgorithm : int {
    Linear = 0,
    Accelerated = 1,
};

namespace var {
    /** Max range (m) for all distance sliders — matches practical game replication limit. */
    inline constexpr float kMaxDistanceSliderM = 500.f;

    /* Esp */
    extern bool enableesp;
    extern bool box;
    extern bool health;
    extern bool names;
    extern bool show_weapon;
    extern bool snaplines;
    extern bool skeleton;
    extern bool silhouette;
    /** Soft translucent silhouette fill (child of silhouette). */
    extern bool silhouette_soft_fill;
    /** Silhouette fill max range (m); 0 = use esp_distance. */
    extern float silhouette_max_distance_m;
    extern bool show_distance;
    extern bool hide_allies;
    extern float esp_distance;
    inline float EffectiveSilhouetteMaxM()
    {
        return silhouette_max_distance_m > 0.f ? silhouette_max_distance_m : 25.f;
    }
    extern float esp_color_visible[4];
    extern float esp_color_invisible[4];

    /* Bot ESP */
    extern bool bot_box;
    extern bool bot_names;
    extern bool bot_snaplines;
    extern bool bot_show_distance;
    extern bool bot_heart;
    extern bool show_dead_bots;
    extern float bot_esp_distance;
    extern float color_dead_bots[4];
    extern float bot_color_visible[4];
    extern float bot_color_invisible[4];

    /* Aimbot */
    extern bool enable_aimbot;
    extern bool robotAimEnabled;
    extern int aim_hold_key;
    extern AimBoneMode aim_bone_mode;
    extern bool visiblecheck;
    extern bool predict;
    extern bool humanizer;
    extern bool randombone;
    extern float aimbot_fov;
    extern AimbotPriority aimbot_priority;
    extern bool show_fov;
    extern float aimbot_distance;
    extern float aim_deadzone_px;
    /** Aim smoothing. 1 = instant snap. Higher = smoother. Range [1, 20]. */
    extern float smoothness;
    /** Streck-style aim curve: Linear or Accelerated (default). */
    extern AimAlgorithm aim_algorithm;
    /** Extra divisor on hardware aim step. Lower = faster pull. */
    extern float aim_sensitivity;
    /** Fraction-of-distance gain per aim tick. Higher = faster (1–20). */
    extern float aim_hardware_speed;
    /** Prefer the previously locked target within an enlarged FOV to reduce churn. */
    extern bool sticky_target_lock;
    /** Extra pixels of FOV granted to the locked target so a marginal rival can't steal lock. */
    extern float aim_sticky_fov_bias_px;
    /** Milliseconds we keep the lock after losing visibility (0 = drop immediately). */
    extern int aim_loss_of_sight_grace_ms;
    extern bool aim_loss_of_sight_grace_enabled;
    /** Bullet speed for lead prediction (cm/s). */
    extern float aim_bullet_speed_cm_s;

    /* World */
    extern bool enable_world;
    extern bool droppedItems;
    extern bool raiderStock;
    extern bool showRobots;
    extern bool showArc;
    extern bool showDeadPlayers;
    extern bool show_world_items;
    extern bool show_world_ammo;
    extern bool show_world_arc_loot;
    extern bool show_world_backpack;
    extern bool show_world_crate;
    extern bool show_world_furniture;
    extern bool show_world_grenade;
    extern bool show_world_harvestable;
    extern bool show_world_industrial;
    extern bool show_world_medical;
    extern bool show_world_other;
    extern bool show_world_probe;
    extern bool show_world_vehicles;
    extern bool show_world_weapon_case;
    extern bool show_world_field_crate;
    extern bool show_world_supply_station;
    extern bool show_world_keys;
    extern bool show_world_locker;
    extern bool show_world_trash;
    extern bool show_world_safe;
    extern bool show_world_buried;
    extern bool show_world_deaddrop;
    extern bool show_world_open_container;
    extern float world_distance;
    extern float color_dropped_items[4];
    extern float color_raider_stock[4];
    extern float color_arc_entities[4];
    extern float color_world_corpses[4];
    extern float color_world_items[4];
    extern float color_world_ammo[4];
    extern float color_world_arc_loot[4];
    extern float color_world_backpack[4];
    extern float color_world_crate[4];
    extern float color_world_furniture[4];
    extern float color_world_grenade[4];
    extern float color_world_harvestable[4];
    extern float color_world_industrial[4];
    extern float color_world_medical[4];
    extern float color_world_other[4];
    extern float color_world_probe[4];
    extern float color_world_vehicles[4];
    extern float color_world_weapon_case[4];
    extern float color_world_field_crate[4];
    extern float color_world_supply_station[4];
    extern float color_world_keys[4];
    extern float color_world_locker[4];
    extern float color_world_trash[4];
    extern float color_world_safe[4];
    extern float color_world_buried[4];
    extern float color_world_deaddrop[4];
    extern float color_world_open_container[4];

    /* Loot */
    extern bool showLoot;
    extern float loot_distance;
    extern float container_distance_sp;
    extern float color_loot[4];
    extern bool loot_rarity_color;
    extern bool show_loot_value;
    extern float loot_min_value;
    extern int loot_min_rarity;
    extern bool loot_min_val_sp;
    extern bool loot_min_rar_sp;

    /* UI */
    /** Global ESP text size multiplier (0.5–3.0); applies on top of distance scaling. Does not affect menu fonts. */
    extern float esp_text_scale;

    /* Debug */
    extern bool show_debug_overlay;

    /* Radar */
    extern bool show_radar;
    extern float radar_scale;
    extern float radar_range;
    extern float radar_pos_x_norm;
    extern float radar_pos_y_norm;
    extern int radar_loot_min_rarity;
    extern bool show_radar_special;
    extern bool radar_shape_circle;
}

bool WorldCategoryEnabled(int category);

inline bool AnyWorldEspEnabled()
{
    return var::enable_world && (
        var::droppedItems || var::raiderStock || var::showArc ||
        var::showDeadPlayers || var::showLoot ||
        var::show_world_items || var::show_world_ammo || var::show_world_arc_loot ||
        var::show_world_backpack || var::show_world_crate || var::show_world_furniture ||
        var::show_world_grenade || var::show_world_harvestable || var::show_world_industrial ||
        var::show_world_medical || var::show_world_other || var::show_world_probe ||
        var::show_world_vehicles || var::show_world_weapon_case ||
        var::show_world_field_crate || var::show_world_supply_station ||
        var::show_world_keys || var::show_world_locker || var::show_world_trash ||
        var::show_world_safe || var::show_world_buried || var::show_world_deaddrop ||
        var::show_world_open_container);
}