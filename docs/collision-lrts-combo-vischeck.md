# Collision + LRTS Combined Vischeck — Implementation Plan

**Date:** 2026-08-29
**Status:** Ready to implement — evidence collected, offsets verified from this repo's own SDK dump.

## Evidence (collected live today)

### 1. AggGeom probe (in-raid, TheDam_02_P, 1245 actors)

```
actors 1245  roots 1198  mesh 459/31(legacy)
bodySetups 309  unique 105  nonEmpty 30
bodySetups rejected: 47
sph 1  box 8  sphyl 1  convex 101
tapered 0  levelSet 12  skinnedLevelSet 0
```

**AggGeom is NOT empty on this build.** 30 body setups carry real primitives:
101 convex hulls, 8 boxes, 1 sphere, 1 capsule, 12 level sets. The old
"0 primitives" conclusion (cooked-collision-visibility-plan.md) came from a
broken probe; the current probe chain reads them fine.

### 2. Element layouts — from this repo's SDK dump (help/help.txt, build 24710327, Aug 18 2026)

The UC forum source's element offsets are months stale — **do not use them.**
The SDK dump in this repo has the exact layouts for THIS build:

```
// KAggregateGeom @ UBodySetup+0xB8 (size 0x78)
SphereElems   0x0  // TArray<KSphereElem>   item 0x50
BoxElems      0x10 // TArray<KBoxElem>      item 0x70
SphylElems    0x20 // TArray<KSphylElem>    item 0x68
ConvexElems   0x30 // TArray<KConvexElem>   item 0x110
TaperedElems  0x40 // TArray<KTaperedCapsuleElem>
LevelSetElems 0x50
SkinnedElems  0x60

// KBoxElem (0x70)
Center   0x30 // Vector  3×double = 0x18
Rotation 0x48 // Rotator 3×double = 0x18
X        0x60 // float
Y        0x64
Z        0x68

// KConvexElem (0x110)
VertexData 0x30 // TArray<Vector>  item 0x18 (double3)
IndexData  0x40 // TArray<int32>
ElemBox    0x50 // Box 0x38
Transform  0x90 // Transform 0x60 (doubles)

// KSphereElem (0x50)
Center 0x30 // Vector
Radius 0x48 // float

// KSphylElem (0x68)
Center   0x30 // Vector
Rotation 0x48 // Rotator
Radius   0x60 // float
Length   0x64 // float
```

Verified chain (all from SDK dump, matches the working probe):
```
Actor.RootComponent                0x238
SceneComponent.ComponentToWorld    0x370   (FTransform doubles: q@+0x00 t@+0x20 s@+0x40)
StaticMeshComponent.StaticMesh     0x728
UStaticMesh.BodySetup              0x1F0
UBodySetup.AggGeom                 0xB8    (inline struct, 7 TArray headers)
```

FVector/FTransform are **double-precision** (LWC) — matches the UC source's
`FVec3d`/`FQuat4d` reads, so the transform math ports as-is.

---

## Stage 1 — CollisionMirror module (port, offsets corrected)

New file: `Project/Functions/CollisionMirror.h/.cpp`

Port the UC source's pipeline (`collision.hpp` + `collision.cpp` + the
AggGeom readers from `convex_hulls.cpp`) with offsets replaced by the table
above. Components:

1. **Read world triangles per StaticMeshComponent:**
   - Convex elems: VertexData (double3 array) + IndexData (int32 array) →
     world triangles via component transform (rot·(scale·v) + trans).
   - Box elems: center/rotator/extents → 12-triangle box.
   - Sphere elems: 8-triangle octahedron.
   - Sphyl elems: approximated as box (radius, radius, halfLen+radius).
   - Level-set elems: **skip** (no portable vertex layout; contribute 0).
   - Cap every read: counts ≤ 65536 verts / 2048 elems / 4096 max, finite
     checks, `IsValidPtrFast2` gates — mirror the probe's rejection style.
2. **KD-tree build** over collected world triangles (leaf ≤ 8 tris, AABB
   split by median — same as UC `detail::build`).
3. **Background rebuild thread:** collect static-mesh roots within 60000
   units of player, dedupe by mesh-pointer+transform fingerprint, rebuild
   when moved > 3000 units or every 20s. Rebuild on worker thread; publish
   tree under mutex; queries take the tree pointer under a **try_lock** and
   raycast lock-free (paint never blocks — same rule as radar fix).
4. **`is_visible(from, to)`:** segment ray vs KD-tree (Moller-Trumbore,
   t_max = 1). No tree yet → visible (fail-open, like warmup LRTS).

## Stage 2 — Debug visualization (the part you asked for)

New var `collision_debug_draw` (default off) + `collision_debug_rays`,
wired into the LRTS menu tab as checkboxes (like `lrts_debug_trace`).

**Blocks:** when on, draw the collision geometry as wireframe:
- Every triangle in the published tree, projected via
  `ProjectWorldLocationToScreen` (Engine.h:69) and drawn with
  `g_renderQueue.addLine` (3 edges per tri) or ImGui directly.
- Color: gray/white for normal geometry, **red for the triangle a ray
  actually hits** (the hit tri is tagged during the LOS pass).
- Clamp to triangles within ~2000 units of the camera (full-map wireframe
  is unreadable + slow).

**Rays:** for each bot/player the ESP processes, draw camera→target line:
- **Green** = `is_visible` true, **red** = blocked.
- Drawn at the same time `actor.isVisible` is computed (worker), queued to
  `g_renderQueue`, flushed on paint. Reuses the existing double-buffered
  line queue — zero new paint-path work.

This stage doubles as the Stage-3 validation harness: with debug on, you can
*see* whether the tree matches the map (blocks on walls) and whether rays
flip at real occlusion edges — before any verdict is trusted.

## Stage 3 — LRTS + collision voting (the "real vischeck")

Hook into the existing per-entity chain (EntityList.cpp / RobotList.cpp)
where `actor.isVisible` is set. Decision table (LRTS primary, collision as
fallback + tie-breaker):

| LRTS verdict   | Collision ray   | Final            |
|----------------|-----------------|------------------|
| Visible        | blocked         | Occluded         | ← LRTS stamp can be stale behind walls (the 30-70s wasp case)
| Visible        | clear           | Visible          |
| Occluded       | any             | Occluded         | ← renderer truth wins
| Unknown        | blocked         | Occluded         | ← collision fills LRTS gaps
| Unknown        | clear           | Visible          |
| Unknown        | no tree (warmup)| Visible          | ← fail-open

Feeds the same `VerdictSmoother` (2-check occlusion confirm, Unknown holds
verdict) so the combo inherits the flicker-free behavior already in place.

**Performance:** the LOS raycast is pure math on cached triangles — no DMA
in the query. The rebuild (the only DMA-heavy part) stays on the worker and
is throttled by movement/interval exactly like the UC source.

## Stage 4 — Validation gates (before trusting it)

1. Debug-on visual check: blocks appear on walls/floors, rays turn red
   crossing a wall, green in open air.
2. Wall-pop test: strafe behind a wall — box hides within ~1s (LRTS fast
   path) or immediately (collision fast path), no ghost flicker.
3. `is_visible` hit-rate sanity: with debug rays on, the red/green split
   should match what you see on screen.
4. Tree stats in menu: tri count, mesh count, rebuild time, query time —
   add a small stats readout next to the checkboxes.

## Out of scope (deliberately)

- Level-set / tapered-capsule geometry (no portable layout; contribute 0
  triangles — the 101 convex hulls carry the weight).
- Persisting geometry across raids (rebuild is cheap; revisit only if the
  rebuild stalls).
- Dynamic occluders (doors/vehicles) — static meshes only, same as the UC
  source. LRTS covers the dynamic gaps.

## Reviewer checklist

- [ ] CollisionMirror compiles against verified offsets (no forum offsets).
- [ ] Probe + collision module agree: module counts ≈ probe counts (30
      nonEmpty, ~101 convex) in the same raid.
- [ ] Debug blocks visibly align with walls/floors in a raid.
- [ ] Debug rays flip green/red at real occlusion edges.
- [ ] Paint thread shows zero new `paint_stall` spikes with debug off.
- [ ] Combined voting wired into EntityList + RobotList, verdicts smooth.
