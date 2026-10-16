#pragma once
// Debug draw for the collision pipeline: what the BVH was built from, and what
// the rays answered.
//
// Three independent overlays, all bounded:
//   * shape outlines - the wireframe of every decoded primitive placed in world
//     space for one component, so you can SEE whether the geometry lines up with
//     the object it belongs to (a box twice as tall as the crate, a capsule on
//     the wrong axis) instead of inferring it from a count;
//   * visibility rays - the camera-to-target segment the vischeck actually
//     traces, coloured by the answer, with a marker at the closest blocker;
//   * scene triangles - the published BVH's OWN triangle buffers, i.e. the exact
//     geometry every query is answered against, in two lanes (decoded collision
//     and coarse occluder boxes) so a wall that is really a bounding box is
//     visible as one instead of being deduced from counters.
//
// Drawing only: nothing here reads the target, and nothing here changes what the
// ESP or the aim does. It exists so a wrong decode and a wrong query can be told
// apart by eye.
//
// The camera is passed in, never taken from a global: the paint pass already
// holds the frame camera, and a debug overlay that projected through a different
// camera than the ESP would disagree with the very drawing it is explaining.
// The tunables are read from UpgradeOptions, so the menu owns them.
//
// Wired into all three ESP sub-renderers (Functions/Esp.cpp): world items and
// containers, players, and bots. The ray target is whatever each one already
// draws toward - the head anchor for players and bots, the item position for
// world entries - so the line on screen points at the same place as the box it
// explains.

#include "../Core/Engine.h"

#include <cstdint>

struct ImDrawList;

namespace CollisionDebug {

struct Options
{
    bool drawShapes = false;
    bool drawRays = false;
    bool drawHitMarkers = true;
    // Scene-triangle lanes. Two, for the same reason the rays have two: the
    // decoded soup is appended to the mesh before the coarse occluder boxes, so
    // one shared cap would always be spent inside the decoded range and never
    // reach a level's actual walls - the case this overlay exists to explain.
    bool drawSceneDecoded = true;      // triangles from decoded collision
    bool drawSceneBoxes = true;        // triangles from a coarse bounds box
    int maxSceneTrisPerLane = 3000;    // per frame, per lane
    float radiusM = 30.f;      // only draw geometry/rays within this of the camera
    int maxPrimitives = 400;   // per frame, across all components
    // Per lane, per frame. Two lanes, because one shared cap would let a screen
    // full of loot spend every ray before a single player or bot was traced -
    // exactly the rays this overlay exists to check. The worst case is twice
    // this many rtcOccluded1 calls, which costs microseconds against the BVH.
    int maxRays = 24;
};

struct VerdictSelfTest
{
    bool ran = false;
    bool passed = false;
    int cases = 0;
    int failed = 0;
    std::string detail;  // one line per case: name=pass/fail
};

/**
 * Prove the ray verdict classification without a target or a scene: that "no BVH
 * published" and "span too short" are reported as themselves and not as each
 * other, that no-scene wins when both counters move, and that a judged ray still
 * reports blocked and clear correctly. Runs on the calling thread in
 * microseconds; logs its own record.
 */
VerdictSelfTest RunVerdictSelfTest();

/**
 * Run the module's one-shot self-tests, at most once per process. Called from the
 * overlay loop rather than from BeginFrame: the paint pass returns early when no
 * live camera resolved, so a check hooked there would silently never run on the
 * sessions that have no target to draw.
 */
void EnsureSelfTestsRan();

struct Stats
{
    int components = 0;      // components whose shapes were drawn this frame
    int primitives = 0;      // primitives drawn
    int segments = 0;        // projected line segments emitted
    int worldRays = 0;       // world-item rays cast this frame
    int entityRays = 0;      // player + bot rays cast this frame
    int occluded = 0;
    int clear = 0;
    // Two DIFFERENT reasons a ray has no answer, kept apart because they mean
    // opposite things. A span shorter than the tnear+clearance biases is a fact
    // about the two points. No BVH published means the collision probe has not
    // run: with no scene every query answers clear BY CONTRACT, so folding those
    // into "clear" (or into "too close") hides the one state where the whole
    // vischeck is silently inert and the rays are decorative.
    int tooClose = 0;
    int noScene = 0;
    int culled = 0;          // primitives skipped by the radius or the cap
    int projectFailed = 0;   // shapes dropped because a point left the view
    // Of the primitives drawn: how many were decoded collision and how many were
    // stand-ins (a character capsule, a component bounds box). Split, because the
    // whole point of drawing them is to tell those two apart on screen.
    int decoded = 0;
    int standIns = 0;
    // Scene-triangle lane (DrawSceneWireframe). Counts describe the published
    // scene and what was drawn from it in the last painted frame.
    uint64_t sceneGeneration = 0;  // 0 when no scene is published
    int sceneTriangles = 0;        // triangles in that scene, committed
    int sceneDecoded = 0;          // decoded-collision triangles drawn
    int sceneBoxes = 0;            // coarse occluder-box triangles drawn
    int sceneCulled = 0;           // scene triangles skipped by the radius
    bool sceneTruncated = false;   // a lane's cap stopped that lane
    bool sceneNoScene = false;     // nothing published, or buffers unreadable
    bool raysBudgetExhausted = false;        // world lane
    bool entityRaysBudgetExhausted = false;  // player/bot lane
};

/** The tunables in force, read from the menu's options. */
Options CurrentOptions();

/** Call once at the top of a paint pass: resets the per-frame counters. */
void BeginFrame();

/** Per-frame counters, for the menu. */
Stats LastFrame();

/**
 * Draw one entity's shapes through `cam`, culled by radius and the frame budget.
 * Returns the number of primitives drawn.
 *
 * Two keys on purpose. `actor` resolves the per-actor index and is the ONLY key
 * that works for a player or a bot, whose root component is a capsule or a
 * skeletal mesh rather than a static mesh component. `component` is the original
 * key kept for the world-item path and as a fallback. Stand-in geometry (a
 * capsule read from CapsuleComponent, a component's LocalBounds box) is drawn in
 * its own colour so it can never be mistaken for a decoded shape.
 */
int DrawShapeOutlines(ImDrawList* drawList, Engine& engine,
    const Engine::CameraCache& cam, const Vector3& eye, uintptr_t component,
    uintptr_t actor = 0);

/**
 * Trace and draw one camera-to-target visibility ray. `entity` selects the lane:
 * false for world items, true for players and bots, each with its own per-frame
 * cap (see Options::maxRays). `label` is drawn at the target when non-null.
 *
 * Returns the query answer, so false means one of several different things and
 * the caller must not read them as one: the ray was never traced (over budget, or
 * out of radius), the span is too short to judge, or NO BVH IS PUBLISHED - in
 * which case every query answers clear by contract and the whole vischeck is
 * inert. Stats separates those two reasons (tooClose / noScene) and the panel
 * reports them on their own lines, because they need opposite fixes.
 */
bool DrawRay(ImDrawList* drawList, Engine& engine, const Engine::CameraCache& cam,
    const Vector3& eye, const Vector3& target, const char* label = nullptr,
    bool entity = false);

/**
 * Trace one per-bone visibility ray and draw it crosshair-to-bone in the caller's
 * two colours: `visibleColor` when the bone is clear, `hiddenColor` when cover is
 * between the camera and it. Returns whether the bone was blocked.
 *
 * Distinct from DrawRay because a bone ray reports the ESP's own visible and
 * not-visible colours rather than the debug verdict palette, so a partly exposed
 * body shows which parts are actually shootable. Drawing only: the query is a CPU
 * BVH traversal and reads nothing.
 */
bool DrawBoneRay(ImDrawList* drawList, Engine& engine, const Engine::CameraCache& cam,
    const Vector3& target, uint32_t visibleColor, uint32_t hiddenColor);

/**
 * Wireframe the published scene's own triangles through `cam`, culled by radius
 * and capped per lane. Returns the number of triangles drawn.
 *
 * Call ONCE per paint pass: the scene is world-global, so drawing it per entity
 * would draw the same geometry repeatedly and multiply the cost by the number of
 * targets. Colours: decoded collision is icy blue, a coarse bounds box amber, so
 * "this wall is really a bounding box" reads off the screen. Nothing is changed
 * by it: the scene is read, never queried or rebuilt.
 */
int DrawSceneWireframe(ImDrawList* drawList, Engine& engine,
    const Engine::CameraCache& cam, const Vector3& eye);

} // namespace CollisionDebug
