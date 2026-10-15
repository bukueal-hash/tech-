#pragma once
// World collision BVH: stages 3 and 4 of the visibility pipeline.
//
// Stage 3 builds one triangle BVH over every static-mesh instance whose
// FKAggregateGeom the reader decoded, with the shapes already placed in world
// space. Stage 4 answers the two query shapes the aim/ESP stages need:
//   rtcOccluded1  - "is anything between these two points"  (cheap, no hit data)
//   rtcIntersect1 - "what is the closest thing on the way"  (for the distance
//                   and the near-target tolerance)
//
// Two deliberate differences from the writeup, both recorded in the plan:
//   * A rebuilt scene is published as a shared_ptr that queries snapshot, so a
//     swap can never pull the old scene out from under a ray already tracing
//     through it. The writeup accepts that use-after-free; this does not.
//   * Convex hulls are their ElemBox (see Core/CollisionTess.hpp), which is
//     conservative for an occlusion answer and counted so the gap is visible.
//
// Nothing here writes to the target and nothing here draws: it is the geometry
// the later stages query.

#include "../Core/CollisionGeom.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace CollisionBvh {

// Full extents (width, depth, height, world units) of the coarse occluder boxes
// that were committed. A summary, not a per-box list: there can be thousands of
// occluders, and the record must stay bounded.
struct OccluderSizeSummary
{
    int count = 0;
    double min[3] = { 0.0, 0.0, 0.0 };
    double median[3] = { 0.0, 0.0, 0.0 };
    double max[3] = { 0.0, 0.0, 0.0 };
};

struct BuildStats
{
    bool built = false;
    int assets = 0;          // assets that contributed geometry
    int instances = 0;       // world placements tessellated
    int triangles = 0;
    // Provenance split of `triangles`: decoded collision against the coarse
    // bounds boxes. Logged and shown, so "the walls are 12-triangle boxes" is
    // visible in the record instead of deduced from boxOccluders.
    int decodedTriangles = 0;
    int occluderTriangles = 0;
    int boxes = 0;
    int spheres = 0;
    int capsules = 0;
    int convexHulls = 0;     // hulls tessellated from their real point cloud
    int convexAsBox = 0;     // hulls that did not decode, standing in as their box
    int convexNoIndex = 0;     // fallback reason: IndexData absent or under one triangle
    int convexTooLarge = 0;    // fallback reason: point cloud over the decode cap
    int convexNoVerts = 0;     // fallback reason: VertexData empty
    int convexReadFailed = 0;  // fallback reason: counts sane, a read failed
    // Coarse occluders: placed static meshes whose collision did not decode, each
    // contributing its bounds box (12 triangles). Kept separate from boxes so the
    // difference between "decoded collision" and "the mesh's bounding box" is
    // visible in every record and on the panel.
    int boxOccluders = 0;
    OccluderSizeSummary occluderSize;
    int skipped = 0;         // primitives with no usable size/transform
    double tessMs = 0.0;
    double commitMs = 0.0;   // rtcCommitScene: the BVH build itself
    double totalMs = 0.0;
    uint64_t generation = 0; // increments per successful publish
    std::string note;        // empty on success, else why there is no scene
};

/**
 * Tessellate every cached asset instance and publish a new BVH. Synchronous:
 * the caller owns the thread choice, because a level build is seconds of work
 * and must never run on the aim tick. No-op and reported (not thrown) when the
 * cache is empty, i.e. before a probe run.
 */
BuildStats RebuildFromCache();

/** Drop the scene. Queries answer "clear" while there is none. */
void Clear();

bool Ready();

struct QueryStats
{
    uint64_t occluded = 0;
    uint64_t clear = 0;
    uint64_t tooClose = 0;  // span shorter than the clearance: not judgeable
    uint64_t noScene = 0;   // asked with no BVH published
};

QueryStats Stats();

/**
 * Debug, opt-in: for every Occluded() query also ask a box-only and a
 * decoded-mesh-only sub-scene the same ray, and count how often each source
 * blocks. `mismatch` counts queries where the combined scene disagreed with the
 * union of the two sub-scenes (should stay 0). Takes effect on the next build;
 * turning it on resets the counters.
 */
struct OcclusionCompare
{
    uint64_t bothBlock = 0;   // box and mesh both block
    uint64_t boxOnly = 0;    // only the coarse box blocks
    uint64_t meshOnly = 0;   // only decoded mesh/hull blocks
    uint64_t neither = 0;    // neither blocks
    uint64_t mismatch = 0;   // combined scene disagreed with box || mesh
};

void SetOcclusionComparison(bool on);
OcclusionCompare CompareStats();

/**
 * The published scene's own triangles, as the BVH itself holds them.
 *
 * `vertexData` / `indexData` point at the very buffers attached to the scene's
 * geometry - the triangle set rtcCommitScene built the BVH over - so a debug draw
 * of these is the only thing that shows what the visibility answer is actually
 * computed against. Re-tessellating the collision cache instead could disagree
 * with the scene, which would hide exactly the bug an overlay exists to find.
 *
 * `lifetime` must be held for as long as the pointers are used: a rebuild
 * publishes a new scene, and this keeps the one being drawn alive across it.
 */
struct SceneTriangles
{
    uint64_t generation = 0;   // 0 when nothing is published
    int triangles = 0;         // committed triangles
    int vertices = 0;          // committed vertices (3 floats each)
    // Provenance: the decoded collision soup is appended before the coarse
    // occluder boxes, so one boundary index says which lane a triangle is in.
    int decodedTriangles = 0;  // [0, decodedTriangles) decoded, the rest boxes
    const float* vertexData = nullptr;
    const uint32_t* indexData = nullptr;
    std::shared_ptr<const void> lifetime;
    bool readable = false;     // false when nothing is published, or unreadable
};

/** A snapshot of the published scene's triangles. Cheap; hold and read, or drop. */
SceneTriangles AcquireSceneTriangles();

/**
 * True when something blocks the segment eye -> target.
 *
 * tnearCm keeps the ray from re-hitting the surface it starts on (the writeup's
 * 5 cm bias). clearanceCm shrinks the far end, so geometry within that distance
 * of the target - the target's own body, a prop it stands against - does not
 * count as cover in front of it. A span shorter than tnear + clearance is
 * reported as not judgeable rather than guessed at.
 */
bool Occluded(const Vector3& eye, const Vector3& target,
    double tnearCm = 5.0, double clearanceCm = 50.0);

struct Hit
{
    bool hit = false;
    double distanceCm = 0.0;   // along the segment, 0 when !hit
    double fraction = 0.0;     // 0..1 along the segment, for a cheap depth cue
};

/**
 * Occlusion for a FEATURE that must fail open, as opposed to a debug overlay that
 * may report anything.
 *
 * A consumer that hides or drops a target on this answer needs the difference
 * between "the scene says something is in the way" and "nothing was judged":
 * with no scene published (the probe has not run, or the level decoded nothing)
 * every segment must be treated as CLEAR, never as cover. That is the whole
 * contract of this wrapper: no scene, an unjudged span, or an unserialised query
 * all return false, so an empty geometry set can only ever leave a feature
 * behaving exactly as it did before the vischeck existed.
 */
bool BlockedForConsumer(const Vector3& eye, const Vector3& target,
    double clearanceCm = 50.0);

/** Closest blocker on the way, if any. */
Hit ClosestHit(const Vector3& eye, const Vector3& target, double tnearCm = 5.0);

struct SelfTestResult
{
    bool ran = false;
    bool passed = false;
    int cases = 0;
    int failed = 0;
    double buildMs = 0.0;
    std::string detail;  // one line per case: name=pass/fail with the observed value
};

/**
 * Exercise tessellation + BVH + both query paths on synthetic geometry, so the
 * pipeline can be proven on a machine that has no live target: a wall the ray
 * must hit, a path it must miss, the closest-hit distance, the scene's own
 * triangles read back through AcquireSceneTriangles, an emptied scene, and a span
 * too short to judge. Runs on the calling thread; takes milliseconds.
 */
SelfTestResult RunSelfTest();

/** One line for the menu: scene size and query counters. */
std::string Summary();

} // namespace CollisionBvh
