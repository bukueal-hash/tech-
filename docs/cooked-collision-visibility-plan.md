# Cooked Collision Visibility — Fact-Based Plan

**Date:** 2026-08-28  
**Status:** Research plan only. Do not implement or build until Stage 0 evidence passes.

## Executive conclusion

The current AggGeom implementation is not usable on this build. The in-raid
runtime dump showed:

```text
static-mesh actors found: 48
AggGeom primitives: spheres=0 boxes=0 capsules=0 convexes=0
cached primitives: 0
```

That does **not** prove the meshes have no collision. In Unreal, `FKAggregateGeom`
is simplified collision. Complex/cooked collision can live in
`UBodySetup::CookedFormatData`, and the cooked bytes are not a portable
vertex/index array.

The previous PhysX `NpScene` scanner is also abandoned: it found no validated
actor array and repeatedly selected false-positive scene candidates.

The next implementation must first identify the actual cooked format in this
build from a known wall/floor asset. No guessed offsets, no brute-force pointer
adoption, and no visibility wiring until the evidence is validated.

---

## Facts established by public documentation and this repository

### Unreal data model

Epic's `UBodySetup` documentation describes it as the owner of collision data for
an asset. It contains:

- `AggGeom` / `FKAggregateGeom`: simplified collision representation
- `CookedFormatData` / `FFormatContainer`: platform-specific cooked collision
  payloads

`FKAggregateGeom` may therefore be empty while a static mesh still has usable
complex collision.

### PhysX cooked data

NVIDIA PhysX documentation states that cooked triangle meshes are serialized
binary data intended for PhysX to load. They are not equivalent to a raw
`float3` vertex buffer plus indices. A valid implementation needs either:

1. The live `PxTriangleMesh`/`PxShape` objects from the game's physics scene, or
2. The correct cooked byte stream plus the exact PhysX version/serialization
   decoder, or
3. A separately extracted map collision asset in a known format.

The repository's former `WorldMirror` attempted option 1 but had no validated
scene pointer. It did not establish option 2.

### Repository facts

Already verified SDK plumbing:

- `WorldScan::CollectLevelActors(gWorld, persistentLevel, actors)` enumerates
  level actors.
- `Actor::RootComponent` = `0x238`.
- `SceneComponent::ComponentToWorld` = `0x370`.
- `SceneComponent::WorldLocation` = `0x390`.
- `StaticMeshComponent::StaticMesh` = `0x728`.
- `UStaticMesh::BodySetup` = `0x1F0`.
- `UBodySetup::AggGeom` = `0xB8`.

The first four/five offsets are useful anchors, but **the cooked-data container
layout and inner payload offsets are not yet verified**.

---

## Stage 0 — evidence collection (mandatory, no code changes)

Use one known static wall/floor actor in a live raid and record:

1. Actor address and RootComponent address.
2. StaticMesh pointer at `RootComponent + 0x728`.
3. BodySetup pointer at `StaticMesh + 0x1F0`.
4. The four AggGeom TArray headers at `BodySetup + 0xB8`.
5. A bounded memory window around the BodySetup object, for example 0x400–0x800
   bytes, using chunked reads.
6. All pointer/count/capacity-looking fields in that window.
7. Candidate cooked-format fields and their format identifiers/keys.
8. Whether the same BodySetup/mesh asset is shared by multiple actors.

The diagnostic must log addresses/counts only; do not dump unbounded memory or
write to the target process.

### Stage 0 acceptance criteria

Proceed only if at least one of these is proven:

- A non-empty `CookedFormatData` entry is found with a recognizable format key
  and a valid byte-array pointer/size; or
- A validated live physics object is found through a known game accessor and its
  vtable/module ownership plus back-references and mesh bounds all agree; or
- An offline extracted collision asset is identified with a documented parser.

If none passes, stop and report exactly which pointer/count checks failed.

---

## Stage 1 — identify the runtime physics format

The prior research covers multiple engines, so the implementation must identify
which path applies rather than assume:

### Path A: PhysX cooked/runtime mesh

Evidence required:

- Game module or SDK confirms PhysX version/build.
- A BodySetup cooked entry has a PhysX format key, or a live shape/mesh object
  can be traced from a validated actor/body instance.
- The mesh's bounds and one known vertex/triangle can be sanity-checked.

Implementation options:

- Prefer reading live runtime mesh objects if a validated scene accessor is
  found. Cache geometry locally and raycast on CPU.
- Otherwise decode the exact cooked stream only after the format/version is
  identified. Do not cast cooked bytes to guessed structs.

### Path B: Chaos cooked/runtime mesh

Evidence required:

- Cooked format key identifies Chaos/Chaos-derived data, or SDK/runtime types
  identify `FImplicitObject`/BVH-style data.
- A valid serialized buffer and its size are found.

Implementation options:

- Parse only the exact observed format, starting with bounds and primitive types.
- For complex triangle data, recover the runtime BVH/implicit-object layout or
  use an offline extractor. Do not assume PhysX `PxTriangleMesh` layout.

### Path C: no readable runtime collision payload

Stop. Use an offline map-collision extraction pipeline or leave visibility
unfiltered. Do not enable a guessed parser in ESP.

---

## Stage 2 — parser and cache design

Create a new module only after Stage 0/1 identifies the format:

```text
CollisionMirror.h/.cpp
```

The module should:

- Accept current world/level pointers from `Update`.
- Enumerate level actors and deduplicate by StaticMesh/BodySetup asset pointer.
- Read each asset's collision payload once per level/map.
- Transform local collision into world coordinates using the component's verified
  `ComponentToWorld` transform, including scale.
- Publish immutable snapshots atomically to the query thread.
- Keep all DMA outside the per-target LOS call.
- Enforce byte, asset, and primitive limits.
- Reject malformed counts, capacities, pointers, bounds, and non-finite values.

Primitive priority:

1. Exact boxes/spheres/capsules where format provides them.
2. Exact convex hulls where vertices/faces are validated.
3. Triangle mesh/BVH only after index stride, count, bounds, and winding are
   validated against a known asset.

---

## Stage 3 — deterministic validation before ESP wiring

Build a standalone/testable raycast validator around captured geometry:

- Ray through a known wall: must block.
- Ray parallel to and outside the wall: must pass.
- Ray touching a boundary: consistent epsilon behavior.
- Rotated/scaled mesh: bounds and orientation remain correct.
- Multiple actors sharing one asset: no duplicate explosion.
- Malformed/stale payload: skipped safely.

Runtime diagnostic fields:

```text
map/world name
assets_seen
bodysetups_seen
cooked_entries_seen
format_key
bytes_read
primitives_published
invalid_entries
ray_tests
ray_hits
```

Only a nonzero, geometrically validated snapshot may be marked `ready`.

---

## Stage 4 — ESP integration

After Stage 3 passes:

```cpp
actor.isVisible = !CollisionMirror::Ready()
               || CollisionMirror::LineOfSight(camera, targetPoint);
```

Start with actor-origin LOS. Add per-bone checks only after wall tests pass.
Warmup behavior remains show-all; a malformed/empty snapshot must never mark
random actors invisible.

---

## Stage 5 — performance and map cache

Only after correctness:

- Cache immutable asset geometry by BodySetup/StaticMesh identity.
- Rebuild only on level/map transition or newly observed asset.
- Keep the background read budget bounded.
- Optionally persist verified geometry per map, keyed by map name and build
  identifier. Never reuse a cache across an unknown game build.

---

## Research sources

- Epic Games API: `UBodySetup` documentation — BodySetup owns `AggGeom` and
  `CookedFormatData`.
- NVIDIA PhysX `PxCooking` documentation — cooked meshes are generated for
  efficient collision queries.
- NVIDIA PhysX Serialization documentation — serialized collections/cooked
  meshes require the matching serialization/runtime interpretation.
- UnknownCheats UE4 PhysX writeup (Feb 2026) — confirms `AggGeom` is inline and
  explains why a live PhysX scene mirror is separate from the UE asset path.
- UnknownCheats Tarkov external raycasting thread — demonstrates that working
  implementations use validated, game-specific runtime scene/mesh layouts;
  offsets are not portable between games/builds.
- Repository `Project/Core/Offsets.h` and `Project/Functions/WorldScanCommon.*`
  — local SDK offsets and actor enumeration already verified here.

## Reviewer checklist

- [ ] Stage 0 dump for one known wall/floor asset exists.
- [ ] Cooked format key/version identified, not guessed.
- [ ] Pointer, count, capacity, bounds, and transform checks documented.
- [ ] One wall-blocking and one wall-clearing ray validated.
- [ ] Parser has malformed/stale data guards.
- [ ] No PhysX/NpScene brute-force fallback remains.
- [ ] Only then wire LOS into ESP and mark the stage complete.
