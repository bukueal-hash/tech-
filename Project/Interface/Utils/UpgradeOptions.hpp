#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <ostream>
#include <sstream>
#include <string>

#include "../../Core/AimCalibration.hpp"

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
inline bool offscreen = false, worldBounds = false;
// Collision-geometry probe (visibility phase 1). When on, the menu button reads
// UStaticMesh -> UBodySetup -> FKAggregateGeom on the target and reports what is
// there. Read-only, off by default: it costs DMA reads and changes no drawing.
inline bool collisionProbe = false;
// Collision debug overlays (visibility work): draw the decoded shape wireframes
// and the rays the vischeck traces. Drawing only, bounded per frame, off by
// default. These exist so a wrong decode and a wrong query can be told apart by
// eye instead of by inference from counters.
inline bool collisionDebugShapes = false;
inline bool collisionDebugRays = false;
inline bool collisionDebugHitMarkers = true;
// The published BVH's own triangles, wireframed. Two lanes (decoded collision,
// coarse bounds boxes) because the decoded soup is appended first and one shared
// cap would never reach a level's walls. ON by default: this is the overlay that
// answers "is this building blocking me, or is it a bounds box standing in for
// real collision" straight off the screen, and it is only a read of the scene.
inline bool collisionDebugSceneDecoded = true;
inline bool collisionDebugSceneBoxes = true;
inline int collisionDebugSceneTriCap = 3000;  // triangles per lane, per frame
// ── The vischeck as a feature, not a debug draw ─────────────────────────────
// vischeck_aim: a candidate the BVH says is behind cover is refused this tick
// (TargetRejection::Occluded). On by default, because aiming at someone behind a
// wall is wrong in every case; it is safe to default on because BOTH features
// fail open -- with no scene published they report clear, so this cannot change
// behaviour until the collision probe has built one.
// ESP does not hide anything: a bot behind cover keeps its box and is drawn in
// the not-visible colour (see BotEspColor) instead.
inline bool visCheckAim = true;
inline float collisionDebugRadiusM = 30.f;
inline int collisionDebugMaxPrims = 400;
inline int collisionDebugMaxRays = 24;
inline int playerPreset = 1;
inline int arcAimKey = 0, arcWeakspotKey = 0;
inline bool arcPrediction = false, arcSticky = true;
inline float arcFov = 50.f, arcDistance = 500.f;
// One shared aim profile: players and ARC bots use the same motion and
// calibration values. Targeting filters (FOV, distance, keys, sticky) stay per class.
inline float playerResponseMs = 120.f;
inline float playerRate = 125.f;
inline float playerCountsPerPixel = 1.f;
// T4.1 (Phase 4): per-axis calibration. X is the value the old single constant
// used to be; Y defaults to the same, so a fresh -- or an existing -- config
// resolves to the identity profile (Core/AimCalibration.hpp).
inline float playerCountsPerPixelY = 1.f;
// Response curve 0..1: 0 is exactly linear (the identity, and the default).
// Above it, error nearer than 30 px is pulled slower and error further away
// faster, so the curve trades settle time for overshoot rather than scaling
// everything at once.
inline float playerResponseCurve = 0.f;
// Absolute per-tick mouse-count cap. 0 defers to the existing distance-scaled
// burst cap -- the pre-T4.1 behaviour, and the default.
inline float playerMaxCountsPerTick = 0.f;
// T4.2 (Phase 4): the strict target gate, on by default as the plan requires for
// new profiles. Strict refuses a candidate whose aim data is unverified rather
// than aiming on it, and NAMES the refusal; switching it off restores exactly the
// pre-T4.2 candidate rules, because the gate is the only thing it changes.
inline bool playerStrictGate = true, arcStrictGate = true;
// T4.5 (Phase 4): which screen axis CameraCache::FOV describes. Off is the reading
// the projection has always used (scale from half the height) AND the reading the
// aim cone has always used (radius from half the width); turning it on moves the
// projection to the cone's horizontal reading, which is the geometrically correct
// one for a camera whose FOV spans the width. Off by default: nothing the user has
// already calibrated to may shift because a setting was added.
inline bool fovAxisHorizontal = false;

// The per-axis calibration in force for a class (T4.1). ONE definition, so the
// aim path, the debug panel and the run report cannot disagree about what is
// actually calibrated.
inline AimCalibration::Profile CalibrationProfile()
{
    AimCalibration::Profile p;
    p.countsPerPixel.x = playerCountsPerPixel;
    p.countsPerPixel.y = playerCountsPerPixelY;
    p.responseMs = playerResponseMs;
    p.curve = playerResponseCurve;
    p.maxCountsPerTick = playerMaxCountsPerTick;
    return p;
}
inline float playerMaxCounts = 35.f;
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
//   vis_enabled            -> visCheckAim        (the vischeck as a feature)
//   collision_debug_draw   -> collisionDebugShapes
// The old file's own values (1 and 0) are what today's defaults already imply,
// so honouring them cannot silently flip a working setup.
struct LegacyBoolKey { const char* key; bool* value; };
inline LegacyBoolKey legacyBools[] = {
    {"vis_enabled", &visCheckAim},
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
    {"offscreen_arrows", &offscreen}, {"world_bounds", &worldBounds},
    {"collision_probe", &collisionProbe},
    {"collision_debug_shapes", &collisionDebugShapes},
    {"collision_debug_rays", &collisionDebugRays},
    {"collision_debug_hit_markers", &collisionDebugHitMarkers},
    {"collision_debug_scene_decoded", &collisionDebugSceneDecoded},
    {"collision_debug_scene_boxes", &collisionDebugSceneBoxes},
    // The vischeck as a FEATURE, not a debug draw. Both fail open when no BVH
    // scene is published (Functions/CollisionBvh.h, BlockedForConsumer), so they
    // change nothing until the collision probe has built a scene.
    {"vischeck_aim", &visCheckAim},
    {"arc_aim_prediction", &arcPrediction}, {"arc_aim_sticky", &arcSticky},
    {"player_strict_target_gate", &playerStrictGate},
    {"arc_strict_target_gate", &arcStrictGate},
    {"fov_axis_horizontal", &fovAxisHorizontal}
};
// How a float key validates on load (T4.1):
//   refuseOutOfRange  -- reject the line instead of clamping it, and record why.
//                         The previous value stays live. Clamping would leave the
//                         user calibrating against a number they never chose.
//   zeroMeansDeferred -- 0 is a meaningful "unset" value accepted outside [lo,hi]
//                         (the tick caps: 0 = defer to the distance burst cap).
struct FloatKey {
    const char* key;
    float* value;
    float lo, hi;
    bool refuseOutOfRange = false;
    bool zeroMeansDeferred = false;
};
inline FloatKey floats[] = {
    {"warning_range_m", &warningRange, 5.f, 500.f}, {"warning_closing_m_s", &warningClosing, 0.5f, 20.f},
    {"warning_debounce_s", &warningDebounce, 0.f, 3.f}, {"platform_badge_size", &badgeSize, 10.f, 28.f},
    {"offscreen_arrow_size", &arrowSize, 5.f, 30.f}, {"offscreen_arrow_inset", &arrowInset, 20.f, 200.f},
    {"arc_aim_fov", &arcFov, 1.f, 360.f}, {"arc_aim_distance", &arcDistance, 5.f, 500.f},
    {"player_response_ms", &playerResponseMs, 20.f, 500.f},
    {"player_aim_rate", &playerRate, 30.f, 250.f},
    {"player_counts_per_pixel_x", &playerCountsPerPixel, 0.05f, 10.f, true},
    {"player_counts_per_pixel_y", &playerCountsPerPixelY, 0.05f, 10.f, true},
    {"player_response_curve", &playerResponseCurve, 0.f, 1.f, true},
    {"player_max_counts_per_tick", &playerMaxCountsPerTick, 1.f, 100.f, true, true},
    {"player_max_counts", &playerMaxCounts, 1.f, 100.f},
    {"collision_debug_radius_m", &collisionDebugRadiusM, 1.f, 200.f}
};
struct IntKey { const char* key; int* value; int hi; };
inline IntKey ints[] = {
    {"arc_aim_key", &arcAimKey, 271}, {"arc_weakspot_key", &arcWeakspotKey, 271},
    {"player_aim_preset", &playerPreset, 3},
    {"collision_debug_max_prims", &collisionDebugMaxPrims, 4000},
    {"collision_debug_max_rays", &collisionDebugMaxRays, 256},
    {"collision_debug_scene_tri_cap", &collisionDebugSceneTriCap, 100000}
};
inline void ApplyPreset(int preset) {
    if (preset < 0 || preset > 2) return;
    playerResponseMs = preset == 0 ? 65.f : preset == 1 ? 120.f : 220.f;
    playerMaxCounts = preset == 0 ? 50.f : preset == 1 ? 35.f : 20.f;
    playerPreset = preset;
}
inline void Save(std::ostream& out) {
    for (const auto& k : bools) out << k.key << '=' << (*k.value ? 1 : 0) << '\n';
    for (const auto& k : floats) out << k.key << '=' << *k.value << '\n';
    for (const auto& k : ints) out << k.key << '=' << *k.value << '\n';
    out << "combat_key=" << combatKey << '\n' << "combat_mode=" << combatMode << '\n';
    for (int i=0; i<4; ++i) out << "offscreen_color_" << i << '=' << arrowColor[i] << '\n';
}
// ── Refused calibration lines (T4.1) ──────────────────────────────────────
// A refused value is neither applied nor clamped: the previous (or default)
// value stays live, and this bounded record says which line was ignored and what
// range it had to be in. One record plus a count, so a config full of bad lines
// reads as a number instead of as silence, and memory cannot grow with them.
struct CalibrationRejection {
    const char* key = "none";   // a string literal from the table, never owned
    float value = 0.f;
    float lo = 0.f, hi = 0.f;
    bool parsed = false;        // false: the text was not a usable number at all
};
inline CalibrationRejection lastCalibrationRejection{};
inline int calibrationRejections = 0;

inline void NoteCalibrationRejection(const char* key, float value, float lo, float hi, bool parsed)
{
    lastCalibrationRejection = { key, value, lo, hi, parsed };
    ++calibrationRejections;
}
inline void ClearCalibrationRejections()
{
    lastCalibrationRejection = {};
    calibrationRejections = 0;
}

// Accept one calibration value, or refuse it and record why. False means the
// caller keeps the value it already had.
inline bool AcceptCalibrationValue(
    const char* key, const std::string& text, float lo, float hi, float& out)
{
    char* end = nullptr;
    const float v = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) {
        NoteCalibrationRejection(key, 0.f, lo, hi, false);
        return false;
    }
    if (v < lo || v > hi) {
        NoteCalibrationRejection(key, v, lo, hi, true);
        return false;
    }
    out = v;
    return true;
}

inline std::string Capture() { std::ostringstream out; Save(out); return out.str(); }


inline bool Load(const std::string& key, const std::string& value) {
    // ── Legacy alias (T4.1) ───────────────────────────────────────────────
    // Before Phase 4 one constant set the single scale both axes shared, so an
    // existing auto_config.ini keeps meaning what it meant: this key now sets
    // BOTH axes. Refused rather than clamped, like its _x/_y replacements.
    if (key == "player_counts_per_pixel") {
        float v = 0.f;
        if (AcceptCalibrationValue("player_counts_per_pixel", value, 0.05f, 10.f, v)) {
            playerCountsPerPixel = v;
            playerCountsPerPixelY = v;
        }
        return true;
    }

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
        if (wellFormed && std::isfinite(v)) {
            const bool inRange = (v >= k.lo && v <= k.hi)
                || (k.zeroMeansDeferred && v == 0.f);
            if (inRange)
                *k.value = v;
            else if (k.refuseOutOfRange)
                NoteCalibrationRejection(k.key, v, k.lo, k.hi, true);
            else
                *k.value = std::clamp(v, k.lo, k.hi);
        } else if (k.refuseOutOfRange) {
            NoteCalibrationRejection(k.key, 0.f, k.lo, k.hi, false);
        }
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
    const bool savedAim = visCheckAim;
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
        { "vis_enabled", "vischeck_aim", &visCheckAim, false },
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
    const bool schemaHasModern = schema.find("vischeck_aim=") != std::string::npos
        && schema.find("collision_debug_shapes=") != std::string::npos;
    if (!schemaHasModern)
        ++r.failed;
    r.detail += std::string("schema_has_modern_names=") + (schemaHasModern ? "pass " : "fail ");

    visCheckAim = savedAim;
    collisionDebugShapes = savedShapes;    r.passed = (r.failed == 0);
    return r;
}

} // namespace UpgradeOptions
