#pragma once
// In-app collision-geometry probe and per-asset cache.
//
// Phase 1 of the visibility work: read UStaticMeshComponent -> UStaticMesh ->
// UBodySetup -> FKAggregateGeom out of the live target and report what is
// actually there, before any query or gameplay code depends on it. The decoder
// itself lives in Core/CollisionGeom.hpp and is pure; this file is the DMA half.
//
// Gated behind UpgradeOptions::collisionProbe and run from the menu, on an owned
// joinable job (EngineThreads joins it at teardown). Nothing here touches the
// aim or ESP paths: it only reads, counts, and logs.

#include "../Core/CollisionGeom.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace CollisionRead {

/**
 * One placed copy of a cached asset: the component the shapes were read through
 * and that component's world transform. The cache is keyed per asset, but a BVH
 * needs world space, so the instances are recorded during the same pass that
 * reads the geometry rather than re-read by a second consumer.
 */
struct AssetInstance
{
    uintptr_t component = 0;
    FTransform transform{};
};

/**
 * A box standing in for one placed static mesh whose collision could NOT be
 * decoded.
 *
 * Why this exists: level meshes carry complex (per-poly, cooked tri-mesh)
 * collision, which the simple-shape reader does not parse, so a live raid decoded
 * 1 of 17 collision assets (16 declared an empty AggGeom). The committed scene was
 * therefore almost empty of the one thing a visibility check needs -- walls -- and
 * every ray answered "clear" over solid geometry. The asset's own bounds box is
 * already read in this pass for the decode cross-check, so placing that box costs
 * no new offset, and 12 triangles per instance is nothing for the BVH.
 *
 * The box is kept in the component's LOCAL frame and placed through `transform`,
 * so it is ORIENTED with the mesh. A world AABB (what this used to be) inflates a
 * rotated mesh on every diagonal and blocks the air beside the building -- the
 * "bot nearer than the building reads as behind it" report. The local box is the
 * asset's ExtendedBounds with the positive/negative bounds extensions folded back
 * out (see the generator). It is still coarse: a box is never smaller than the
 * shape it stands for, and the count is reported (BuildStats::boxOccluders) so the
 * crudeness stays visible.
 */
struct OccluderBox
{
    Vector3 center{};        // local (component) space centre
    Vector3 halfExtents{};   // local (component) space half extents
    FTransform transform{};  // component-to-world
};

/** Counters for one probe run. Bounded arrays, no per-asset payload. */
struct ProbeResult
{
    bool ran = false;
    bool running = false;

    // Chain walk
    int actorsSeen = 0;              // in the cached actor-pointer list
    int actorsWalked = 0;            // examined this run (capped)
    // True when the walk stopped at kMaxActorsWalked instead of at the end of the
    // roster. A truncated walk is a scene that describes only part of the world,
    // and until this was recorded that was indistinguishable in the log from a
    // world that simply had no more geometry in it.
    bool walkCapHit = false;
    int rootsResolved = 0;
    int rootsMissing = 0;
    int staticMeshComponentRoots = 0;
    int otherComponentRoots = 0;     // e.g. skeletal: static-mesh path does not apply
    int assetsUnresolved = 0;        // component had no usable StaticMesh asset

    // Per-asset outcome (this is the phase-0 question: is BodySetup populated?)
    int assetsUnique = 0;
    int assetsParsed = 0;
    int assetsBodySetupNull = 0;
    int assetsHeaderReadFailed = 0;
    int assetsAggGeomEmpty = 0;      // headers read, all nine arrays empty
    int assetsFromCache = 0;         // shared-asset hits: the dedup rate
    int assetsOverCacheCap = 0;      // assets that arrived after the cache filled

    // Coarse occluders: world AABBs recorded for placed static meshes whose
    // collision did not decode (see OccluderBox). These are the only geometry
    // some levels can offer, so the number is part of the coverage answer.
    int occluderBoxes = 0;

    // Per-actor geometry index. Players and bots are characters or skeletal
    // meshes, so their root is NOT a static mesh component and the asset path
    // above never sees them - which is exactly why their shape outlines could
    // not draw. The actor key is now recorded for all three sources so a draw
    // (or a query) that has an actor can find its geometry:
    int actorPlacements = 0;        // actors with geometry or a stand-in recorded
    int actorDecodedShapes = 0;     // ...their mesh decoded (asset cache shapes)
    int actorCapsules = 0;          // ...Character::CapsuleComponent, read live
    int actorBoundsBoxes = 0;       // ...the component's own LocalBounds box
    int actorGeometryUnusable = 0;  // ...neither: no sane shape to record
    // Which gate dropped the stand-in attempts. "613 unusable" cannot be acted
    // on: loosening the transform check and loosening the bounds check have very
    // different consequences - the first publishes garbage occluders that the
    // vischeck would then use to hide a visible enemy, the second only widens a
    // box. These three counters say which one is actually rejecting them.
    int standInRootReject = 0;       // root missing/implausible
    int standInTransformReject = 0;  // component-to-world failed PlausibleComponentTransform
    int standInBoundsReject = 0;     // LocalBounds extent failed validation
    // Raw LocalBounds of the roots that failed that check AND are a real
    // UPrimitiveComponent subclass (BrushComponent). A rejected DDGIVolumeComponent
    // is correct - it is a USceneComponent and has no bounds there at all - so the
    // count alone cannot say whether the level's brushes are being read wrongly or
    // are genuinely degenerate. These samples show the actual numbers.
    int brushRoots = 0;              // brush roots whose BodySetup pointer resolved
    int brushNoBodySetup = 0;        // brush roots with no BodySetup at 0x698
    int brushBoundsRejects = 0;
    std::string brushBoundsSample[2];

    // Where the cached geometry actually is, split by kind. Brushes (BodySetups)
    // and StaticMeshes share one cache and one cap. This split was added to answer
    // "why is the vischeck scene nearly empty": it showed the meshes carry almost
    // no simple collision (complex tri-mesh) while the brushes carry the level's,
    // which is what justified feeding brushes to the BVH as well.
    int cachedBrushAssets = 0;
    int cachedMeshAssets = 0;
    int cachedBrushPrimitives = 0;
    int cachedMeshPrimitives = 0;
    int bvhEligibleInstances = 0;   // placed instances of mesh assets that can reach the BVH

    // Geometry
    int primitivesStored = 0;
    int declaredByShape[CollisionGeom::kShapeCount]{};
    int parsedByShape[CollisionGeom::kShapeCount]{};
    int unsupportedByShape[CollisionGeom::kShapeCount]{};
    int truncatedByShape[CollisionGeom::kShapeCount]{};
    int readFailedByShape[CollisionGeom::kShapeCount]{};
    int collisionEnabledHistogram[4]{};  // 0 No, 1 Query, 2 Physics, 3 Both
    int collisionEnabledOther = 0;

    // Cross-validation: the world AABB derived from the decoded shapes against
    // the asset's own ExtendedBounds, which is an independent read this overlay
    // already trusts for its wireframe. Both are geometry-derived, so a small
    // delta is evidence the decode is right; a large one is either complex-only
    // collision or a parser bug, and the counts say which way it leans.
    int boundsChecked = 0;
    int boundsAgree25cm = 0;
    int boundsAgree100cm = 0;
    double worstBoundsDeltaCm = 0.0;
    double medianBoundsDeltaCm = 0.0;

    // Bounded sample of the class names behind otherComponentRoots, so a count
    // of skipped actors can be acted on instead of just noticed. Each entry now
    // carries how many actors that class covers: a name alone said WHAT was
    // skipped but not how much, so a three-actor curiosity and the class holding
    // every building in the level looked identical in the record.
    int skippedRootClasses = 0;
    std::string skippedRootClassSample[3];
    int skippedRootClassCounts[3]{};
    int skippedRootClassesOther = 0;   // types beyond the three named ones

    // The other half of "missing buildings": static-mesh component roots whose
    // StaticMesh slot (0x6E8) read back null or implausible even though the class
    // check passed (assetsUnresolved). A count alone cannot say whether those are
    // one component kind being read at the wrong offset or many kinds that are
    // genuinely mesh-less here, so the component class names are tallied (bounded,
    // three names + other) and the owner actor's class is kept for the samples.
    // null vs garbage separates "wrong object entirely" from "wrong offset".
    int unresolvedMeshClasses = 0;
    std::string unresolvedMeshClassSample[3];
    int unresolvedMeshClassCounts[3]{};
    int unresolvedMeshClassesOther = 0;
    std::string unresolvedMeshActorClassSample[3];
    int unresolvedMeshNull = 0;        // StaticMesh slot held 0
    int unresolvedMeshGarbage = 0;     // StaticMesh slot held a non-null implausible value

    uint64_t reads = 0;
    uint64_t bytesRead = 0;
    double elapsedMs = 0.0;
    std::string note;  // empty on success; a human-readable reason otherwise
};

/** Kick a probe on an owned joinable job. No-op while one is already running. */
void StartProbe();

/**
 * Per-frame arm check, called from the overlay loop: when the probe is enabled
 * and has not run yet this session, start it as soon as the actor cache holds
 * actors. Arming is then the whole gesture and the menu button is what re-runs
 * it after a map load. Cheap and idempotent; does nothing while disabled.
 */
void TickAutoArm();

struct IndexSelfTestResult
{
    bool ran = false;
    bool passed = false;
    int cases = 0;
    int failed = 0;
    std::string detail;  // one line per case: name=pass/fail
};

/**
 * Prove the actor-keyed geometry index resolves, on a machine with no live
 * target. The reported bug was that player and bot shape outlines never drew
 * while world items did; the cause was a key, not a decode - the paint path holds
 * a root component for an entity whose geometry (if any) is stored per actor, and
 * a character's root is a capsule, so nothing was ever stored under that key.
 *
 * This inserts synthetic entries (under keys that cannot be real pointers, so a
 * real probe's index is never touched), asserts that an actor key resolves, that
 * a component of that actor resolves to the same geometry, that the stand-in flag
 * survives, and that an unknown key does NOT resolve, then removes them again.
 * Runs on the calling thread in microseconds; no target, no DMA.
 */
IndexSelfTestResult RunIndexSelfTest();

/** Snapshot of the last (or in-flight) probe. */
ProbeResult GetProbeResult();

/** Assets in the collision cache, and a human-readable summary for the panel. */
int CachedAssetCount();
std::string CacheSummary();

/**
 * Drop the per-asset cache. It is keyed by asset pointer and survives between
 * runs on purpose -- that reuse is the dedup rate this probe measures -- but an
 * asset pointer is only meaningful inside one level, so this is what a map
 * change needs before the next run.
 */
void ClearCache();

/**
 * Visit every asset that decoded geometry, with its placed instances. The
 * callback runs while the cache lock is held, so it must not call back into this
 * module or block; it exists so a consumer (the BVH) can build world space
 * without copying the whole cache and without a second read path.
 */
void ForEachParsedAsset(
    const std::function<void(uintptr_t asset,
        const std::vector<CollisionGeom::Primitive>& primitives,
        const std::vector<AssetInstance>& instances)>& fn);

/**
 * Visit the decoded shapes for one placed component, with the transform they
 * were read through. Returns false when that component has no geometry cached.
 * O(1) lookup, so a per-frame consumer (the debug draw) can call it per entity;
 * the callback still runs under the cache lock, so keep it to projection and
 * drawing, never a read.
 */
bool ForEachPrimitiveAtComponent(uintptr_t component,
    const std::function<void(const CollisionGeom::Primitive&, const FTransform&)>& fn);

/**
 * Visit the geometry recorded for one actor - or for one of its components.
 *
 * `key` may be the actor pointer (what the world scan walks) or a component of
 * it, because the probes that fill the actor index and the consumers that ask
 * about it do not always hold the same pointer: the item and bot paths hold a
 * root component, the player path holds a pawn. Both resolve here.
 *
 * `standIn` is true for geometry that is NOT decoded collision: a character's
 * capsule read from CapsuleComponent, or a primitive component's LocalBounds
 * box. Those are the only shapes an actor without FKAggregateGeom can offer, and
 * they are coarse by construction (a capsule is exact for a character; a bounds
 * box is never smaller than the mesh), so the caller is told which it got
 * instead of being handed both as if they were the same thing.
 *
 * O(1) lookup; the callback runs under the cache lock, so keep it to projection
 * and drawing, never a read.
 */
bool ForEachPrimitiveAtActor(uintptr_t key,
    const std::function<void(const CollisionGeom::Primitive&, const FTransform&,
        bool standIn)>& fn);

/**
 * Visit the coarse occluder boxes (see OccluderBox). The callback runs while the
 * cache lock is held, so it must not call back into this module or block. Keyed
 * per component, so re-running the probe replaces a placement instead of doubling
 * it, and bounded by the module's own cap.
 */
void ForEachOccluderBox(const std::function<void(const OccluderBox&)>& fn);

/** Ask the running probe to stop and join it. Called from EngineThreads teardown. */
void StopJobs();

} // namespace CollisionRead
