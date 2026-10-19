#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <ostream>
#include <sstream>
#include <string>


namespace UpgradeOptions {
inline bool combatEnabled = false, combatShowArcs = true;
inline int combatKey = 0, combatMode = 0;
inline bool combatActive = false; // transient, never saved
inline bool warnings = false, warningBehind = true, warningApproach = true;
inline float warningRange = 60.f, warningClosing = 2.f, warningDebounce = 0.4f;
inline bool platformBadges = false, playerStates = false;
inline float badgeSize = 14.f;
inline bool stateIdle = true, stateWalking = true, stateDowned = true;
inline bool systemHud = false, hudFps = true, hudCpu = true, hudGpu = true, hudResolution = true, hudClock = true;
inline bool offscreen = false;
// Small live aim monitor panel in the top-left during a raid (menu closed):
// key held, moves accepted, current error and the latest aim status line.
inline bool aimMonitorPanel = false;
// Collision debug overlays (visibility work): draw the decoded shape wireframes
// and the rays the vischeck traces. Drawing only, bounded per frame, off by
// default. These exist so a wrong decode and a wrong query can be told apart by
// eye instead of by inference from counters.
inline bool collisionDebugShapes = false;
inline bool collisionDebugRays = false;
inline bool collisionDebugHitMarkers = true;
inline int collisionDebugSceneTriCap = 6000;  // world-mesh triangles drawn per frame
// Dynamic obstruction layer (Core/Obstruction.hpp): smoke, gas and deployable
// barriers are visual effects with no collision shape, so the geometry BVH cannot
// see them and the vischeck calls an enemy behind them "visible". When on, a ray
// against cached obstruction spheres also blocks. Conservative on purpose.
inline bool visibilityObstructionSpheres = false;
// ── The vischeck as a feature, not a debug draw ─────────────────────────────
// ESP does not hide anything: a bot behind cover keeps its box and is drawn in
// the not-visible colour (see BotEspColor) instead.
// Per-bone raycast visibility check. This is THE vischeck switch: when on, the
// Embree world BVH is built from static meshes and a camera-to-bone ray is cast
// for every skeleton bone; each bone segment is colored visible/behind-cover and
// the box is visible when any bone ray is clear. Off = everything draws visible.
inline bool visCheckPerBone = false;
// How far vischeck rays reach, in metres. A target (player bone or bot body)
// farther than this is not traced at all: no ray cast, no ray drawn, and the
// target counts as visible. 0 = rays reach every target in the loaded world.
inline float rayRangeM = 500.f;
// Draw dynamic obstruction wireframes (orange boxes/spheres) when debug overlay is on.
inline bool visDebugObstructions = false;
// Optional buildings-only filters for the coarse bounds boxes (the "Include
// building bounds boxes" toggle). They exist to trim boxes that fill air a target
// can stand in, but a box is never the real shape, so they are OFF by default: a
// max-axis or max-corner cap drops whole buildings and trees (anything larger than
// the cap), and a min-height or vertical-thin drops low walls and rocks -- the
// exact scenery these boxes are the only representation of until real mesh
// triangles are decoded. Raise a value to re-enable that trim:
//  - collisionBvhBoxMaxHalfCm: reject a box any local axis of which is longer;
//  - collisionBvhBoxMaxCornerCm: reject a box whose scaled corner is longer;
//  - collisionBvhBoxMinWorldHeightCm: reject a box shorter than this vertically;
//  - collisionBvhBoxVerticalThin: thin the most-vertical axis by this fraction.
// A cm threshold of 0 disables that test; a thin factor of 0 (or >= 1) disables
// the thinning.
inline float collisionBvhBoxMaxHalfCm = 0.f;
inline float collisionBvhBoxMaxCornerCm = 0.f;
inline float collisionBvhBoxMinWorldHeightCm = 0.f;
inline float collisionBvhBoxVerticalThin = 0.f;
// Whole-building wrapper skip (reference reader's IsWrapperVolume): drop an
// occluder box whose WORLD extent is at least wrapperMin on EVERY axis AND at
// least wrapperMax on its longest -- the collision wrapper around a whole
// building, which fills air the building does not occupy. 0 on either disables
// the test. Tunable live so the exact cut can be found by eye against the
// labelled box overlay instead of being guessed in code.
inline float collisionBvhBoxWrapperMinCm = 500.f;
inline float collisionBvhBoxWrapperMaxCm = 900.f;
inline float collisionDebugRadiusM = 200.f;
inline int collisionDebugMaxPrims = 400;
inline int collisionDebugMaxRays = 24;
inline float arrowSize = 12.f, arrowInset = 45.f;
inline float arrowColor[4] = {1.f, 0.3f, 0.2f, 0.9f};
// ── Legacy boolean key aliases ─────────────────────────────────────────────
// Key names an EARLIER build wrote into auto_config.ini, mapped onto the
// variables that replaced them. The shipped Build/auto_config.ini still carries
// these lines, and the loader's allowlist (AutoConfig.cpp LoadIniFile) accepts
// only the key names the current schema emits — so those lines were discarded in
// silence and then overwritten by the next save. From the user's side that reads
// as "the keys in auto_config.ini do not apply", which is exactly what it was.
//
// Exposed (rather than kept private to Load) so the loader can accept these keys
// in its allowlist and report anything it still cannot place.
//
//   collision_debug_draw   -> collisionDebugShapes
// The old file's own values (1 and 0) are what today's defaults already imply,
// so honouring them cannot silently flip a working setup.
struct LegacyBoolKey { const char* key; bool* value; };
inline LegacyBoolKey legacyBools[] = {
    {"collision_debug_draw", &collisionDebugShapes},
};

struct BoolKey { const char* key; bool* value; };
inline BoolKey bools[] = {
    {"combat_enabled", &combatEnabled}, {"combat_show_arcs", &combatShowArcs},
    {"threat_warnings", &warnings}, {"warning_behind", &warningBehind}, {"warning_approach", &warningApproach},
    {"platform_badges", &platformBadges}, {"player_states", &playerStates},
    {"state_idle", &stateIdle}, {"state_walking", &stateWalking}, {"state_downed", &stateDowned},
    {"system_hud", &systemHud}, {"hud_fps", &hudFps}, {"hud_cpu", &hudCpu}, {"hud_gpu", &hudGpu},
    {"hud_resolution", &hudResolution}, {"hud_clock", &hudClock},
    {"offscreen_arrows", &offscreen},
    {"aim_monitor_panel", &aimMonitorPanel},
    {"collision_debug_shapes", &collisionDebugShapes},
    {"collision_debug_rays", &collisionDebugRays},
    {"collision_debug_hit_markers", &collisionDebugHitMarkers},
    {"visibility_obstruction_spheres", &visibilityObstructionSpheres},
    {"vis_debug_obstructions", &visDebugObstructions},
    {"vis_check_per_bone", &visCheckPerBone}
};
struct FloatKey {
    const char* key;
    float* value;
    float lo, hi;
};
inline FloatKey floats[] = {
    {"ray_range_m", &rayRangeM, 0.f, 500.f},
    {"warning_range_m", &warningRange, 5.f, 500.f}, {"warning_closing_m_s", &warningClosing, 0.5f, 20.f},
    {"warning_debounce_s", &warningDebounce, 0.f, 3.f}, {"platform_badge_size", &badgeSize, 10.f, 28.f},
    {"offscreen_arrow_size", &arrowSize, 5.f, 30.f}, {"offscreen_arrow_inset", &arrowInset, 20.f, 200.f},
    {"collision_debug_radius_m", &collisionDebugRadiusM, 1.f, 200.f},
{"collision_bvh_box_max_half_cm", &collisionBvhBoxMaxHalfCm, 0.f, 2000.f},
{"collision_bvh_box_max_corner_cm", &collisionBvhBoxMaxCornerCm, 0.f, 3000.f},
{"collision_bvh_box_min_world_height_cm", &collisionBvhBoxMinWorldHeightCm, 0.f, 1000.f},
{"collision_bvh_box_vertical_thin", &collisionBvhBoxVerticalThin, 0.f, 1.f},
{"collision_bvh_box_wrapper_min_cm", &collisionBvhBoxWrapperMinCm, 0.f, 3000.f},
{"collision_bvh_box_wrapper_max_cm", &collisionBvhBoxWrapperMaxCm, 0.f, 5000.f}
};
struct IntKey { const char* key; int* value; int hi; };
inline IntKey ints[] = {
    {"collision_debug_max_prims", &collisionDebugMaxPrims, 4000},
    {"collision_debug_max_rays", &collisionDebugMaxRays, 256},
    {"collision_debug_scene_tri_cap", &collisionDebugSceneTriCap, 100000}
};
inline void Save(std::ostream& out) {
    for (const auto& k : bools) out << k.key << '=' << (*k.value ? 1 : 0) << '\n';
    for (const auto& k : floats) out << k.key << '=' << *k.value << '\n';
    for (const auto& k : ints) out << k.key << '=' << *k.value << '\n';
    out << "combat_key=" << combatKey << '\n' << "combat_mode=" << combatMode << '\n';
    for (int i=0; i<4; ++i) out << "offscreen_color_" << i << '=' << arrowColor[i] << '\n';
}
inline std::string Capture() { std::ostringstream out; Save(out); return out.str(); }


inline bool Load(const std::string& key, const std::string& value) {
    // ── Legacy names first ─────────────────────────────────────────────
    // Before the modern tables, so an old line still lands on its replacement.
    // Unlike the tables below these accept the "on"/"off"/"yes"/"no"
    // spellings the loader normalises to 1/0 anyway.
    for (const auto& k : legacyBools) if (key == k.key) {
        if (value == "1" || value == "true" || value == "on" || value == "yes")
            *k.value = true;
        else if (value == "0" || value == "false" || value == "off" || value == "no")
            *k.value = false;
        return true;
    }
    for (const auto& k : bools) if (key == k.key) {
        if (value == "1" || value == "true") *k.value = true;
        else if (value == "0" || value == "false") *k.value = false;
        return true;
    }
    for (const auto& k : floats) if (key == k.key) {
        char* end = nullptr; const float v = std::strtof(value.c_str(), &end);
        const bool wellFormed = (end != value.c_str() && *end == '\0');
        if (wellFormed && std::isfinite(v))
            *k.value = std::clamp(v, k.lo, k.hi);
        return true;
    }
    for (const auto& k : ints) if (key == k.key) {
        char* end = nullptr; errno = 0; const long v = std::strtol(value.c_str(), &end, 10);
        if (errno != ERANGE && end != value.c_str() && *end == '\0' && v>=0 && v<=k.hi) *k.value = static_cast<int>(v);
        return true;
    }
    if (key == "combat_key" || key == "combat_mode") {
        char* end = nullptr; errno = 0; const long v = std::strtol(value.c_str(), &end, 10);
        if (errno != ERANGE && end != value.c_str() && *end == '\0' && v >= 0 && v <= (key == "combat_mode" ? 1 : 271))
            (key == "combat_key" ? combatKey : combatMode) = static_cast<int>(v);
        return true;
    }
    for (int i=0; i<4; ++i) if (key == "offscreen_color_" + std::to_string(i)) {
        char* end = nullptr; const float v = std::strtof(value.c_str(), &end);
        if (end != value.c_str() && *end == '\0' && std::isfinite(v)) arrowColor[i] = std::clamp(v, 0.f, 1.f);
        return true;
    }
    return false;
}
// ── Executed proof that a legacy ini line still lands ───────────────────────
// The shipped auto_config.ini carries key names this build no longer writes, and
// the loader's allowlist drops anything the current schema does not emit. This
// checks the whole round trip per alias, in the shipping exe: the old line is
// accepted, it moves the variable, and the variable is still written under its
// MODERN name — because a mapping onto a name the schema no longer emits would
// apply a setting that the very next save erases.
struct LegacyKeySelfTest {
    bool passed = false;
    int cases = 0;
    int failed = 0;
    std::string detail;
};

inline LegacyKeySelfTest RunLegacyKeySelfTest()
{
    LegacyKeySelfTest r;
    const bool savedShapes = collisionDebugShapes;

    std::ostringstream schemaOut;
    Save(schemaOut);
    const std::string schema = schemaOut.str();

    const struct Case {
        const char* legacy;
        const char* modern;
        bool* value;
        bool probe;
    } cases[] = {
        { "collision_debug_draw", "collision_debug_shapes", &collisionDebugShapes, true },
    };

    for (const Case& c : cases) {
        *c.value = !c.probe;   // so a no-op Load cannot pass by accident
        ++r.cases;
        const bool applied = Load(c.legacy, c.probe ? "1" : "0") && *c.value == c.probe;
        std::ostringstream nowOut;
        Save(nowOut);
        const bool reemitted = nowOut.str().find(
            std::string(c.modern) + "=" + (c.probe ? "1" : "0")) != std::string::npos;
        if (!applied || !reemitted)
            ++r.failed;
        r.detail += std::string(c.legacy) + "->" + c.modern
            + (applied ? "=applied" : "=IGNORED")
            + (reemitted ? "/saved " : "/NOT-SAVED ");
    }

    // The modern names must be in the schema BEFORE any legacy line is loaded:
    // the loader builds its allowlist from exactly this text, so a missing key
    // would mean the saved file itself is dropped on the next launch.
    ++r.cases;
    const bool schemaHasModern = schema.find("collision_debug_shapes=") != std::string::npos;
    if (!schemaHasModern)
        ++r.failed;
    r.detail += std::string("schema_has_modern_names=") + (schemaHasModern ? "pass " : "fail ");

    collisionDebugShapes = savedShapes;
    r.passed = (r.failed == 0);
    return r;
}

} // namespace UpgradeOptions
