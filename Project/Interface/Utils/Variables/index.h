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

/** When VisibleOnly, aim/trigger skip targets with stale LRTS (behind wall). */
enum class AimVisMode : int {
    Always = 0,
    VisibleOnly = 1,
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
    /** Silhouette fill max range (m); 0 = default 25 m (not esp_distance). */
    extern float silhouette_max_distance_m;
    extern bool show_distance;
    extern bool show_squad_idx;
    extern bool hide_allies;
    extern float esp_distance;
    inline float EffectiveSilhouetteMaxM()
    {
        return silhouette_max_distance_m > 0.f ? silhouette_max_distance_m : 25.f;
    }
    extern float esp_color_visible[4];
    extern float esp_color_invisible[4];
    extern bool vis_enabled;    // LRTS occlusion-based visibility check
    extern bool lrts_debug_trace; // append per-second vis trace NDJSON rows
    extern bool lrts_debug_tree;  // include component tree bytes in each row
    extern bool lrts_debug_burst; // full-frame-rate one-bot latency capture
    extern bool collision_debug_draw; // draw collision KD-tree wireframe blocks
    extern bool collision_debug_rays; // draw camera→target LOS rays (green/red)
    extern bool collision_vis_enabled; // LRTS+collision combined voting
    extern AimVisMode aim_vis_mode;

    /** Run LRTS reads when ESP vis and/or aim visible-only need isVisible. */
    inline bool LrtsVisActive()
    {
        return vis_enabled || aim_vis_mode == AimVisMode::VisibleOnly;
    }

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
    extern bool predict;
    extern bool humanizer;
    extern float humanizer_intensity;
    extern float humanizer_react_ms;
    extern float humanizer_overshoot;
    extern bool randombone;
    extern float aimbot_fov;
    extern AimbotPriority aimbot_priority;
    extern bool show_fov;
    extern bool show_crosshair;
    extern int crosshair_style;
    extern float crosshair_color[4];
    extern float crosshair_size;
    extern float crosshair_thickness;
    extern float crosshair_gap;
    extern float crosshair_spin_rpm;
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

    /* Triggerbot */
    extern bool enable_triggerbot;
    extern int trigger_hold_mode; // 0=Hold, 1=Toggle, 2=Always
    extern int trigger_hold_key;
    extern float trigger_deadzone_px;
    extern int trigger_fire_delay_ms;
    extern bool trigger_auto_hold;

    /* World */
    extern bool enable_world;
    extern bool droppedItems;
    extern bool raiderStock;
    extern bool showRobots;
    extern bool showHatches;
    extern float color_hatches[4];
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
    /** On-screen list of nearby ground pickups; F7 marks nearest as picked. */
    extern bool show_near_loot_hud;
    extern float loot_distance;
    extern float container_distance_sp;
    extern float color_loot[4];
    extern bool loot_rarity_color;
    extern bool show_loot_value;
    /** Crate contents preview: "[Bandage x2, ...]" listing on container labels. */
    extern bool show_crate_contents;
    /** Ground-loot stack size: append " xN" when the stack has more than one. */
    extern bool show_stack_counts;
    /** Looted containers draw grey + "(Looted)" instead of hiding/re-homing. */
    extern bool grey_looted_containers;
    // Loadout readout (guns/armor/ammo feature): armor tier + plate line for
    // players, best-effort weapon/armor line for bots.
    extern bool show_armor_line;
    /** Full kit readout: stowed guns/tool, safe pouch (rarity), belt/pack slots. */
    extern bool show_player_kit;
    extern bool show_bot_loadout;
    // Player intel (features #4-#6): DBNO badge + revive ring, look arrows,
    // permanent SteamID64 labels (+ arrow color).
    extern bool show_dbno_badge;
    extern bool show_look_arrows;
    extern bool show_steam_ids;
    extern float color_look_arrow[4];
    // Bot intel (features #7-#8): vision cones + alertness tag, part pips.
    extern bool show_bot_vision;
    extern bool show_bot_alertness;
    extern bool show_bot_parts;
    // Raid intel (features #9-#11): dashboard panel, activity feed, map radar.
    extern bool show_raid_hud;
    extern bool show_activity_feed;
    extern bool radar_map_mode;
    extern bool radar_underground_dim;
    extern float loot_min_value;
    extern int loot_min_rarity;
    extern bool loot_min_val_sp;
    extern bool loot_min_rar_sp;

    /* UI */
    /** Global ESP text size multiplier (0.5–3.0); applies on top of distance scaling. Does not affect menu fonts. */
    extern float esp_text_scale;

    /* Debug */
    extern bool show_debug_overlay;
    extern bool debug_skeleton_lag;
    extern bool debug_aim_shake;
    extern bool debug_hatch_detect;
    /** Log each bot rejection/ghost decision to entity_diagnostics.ndjson. */
    extern bool debug_ghost_bots;


    /* Camera debug */
    extern bool camPovUsePending;  // false=live ViewTarget(0x4C8..), true=PendingViewTarget(0xCA0..)

    /* Radar */
    extern bool show_radar;
    extern float radar_scale;
    extern float radar_range;
    extern float radar_pos_x_norm;
    extern float radar_pos_y_norm;
    extern int radar_loot_min_rarity;
    extern bool show_radar_special;
    extern bool radar_shape_circle;
    extern bool radar_ally_arrows;
}

bool WorldCategoryEnabled(int category);

inline bool AnyWorldEspEnabled()
{
    // The menu defines Show loot as the world-draw master switch. Keep scanner,
    // frame collection, and renderer on the same gate; radar has its own
    // independent world-blip path.
    return var::enable_world && var::showLoot && (
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
        var::show_world_open_container || var::showHatches);
}