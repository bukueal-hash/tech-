# [ARCHIVED / SUPERSEDED] PhysX Mirror Fix

> **Status: SUPERSEDED 2026-08-28.** PhysX scene discovery removed as
> non-functional. See **`docs/collision-mirror-plan.md`** for the AggGeom approach. — Research & Stage Plan

**Progress: 0 / 5 stages complete**
Companion page: [physx-fix-plan.html](physx-fix-plan.html). Sources: zarboz UE4 PhysX writeup (UC, Feb 2026), junglepuppy Tarkov PhysX recreation (UC, Nov 2024), Arc Raiders obstruction thread (UC, Feb 2026), PUBG reversal thread.

---

## 1 — The wrong conclusion, corrected

- ✗ **Earlier claim:** "No PhysX DLLs loaded → game uses Chaos, mirror must be rewritten."
- **Why it was wrong:** the module list was taken from `ArcRaiders.exe` on *this* PC — that is our DMA client, not the game. The game runs on the target machine. Meanwhile `help.txt` contains `Gu::HeightField (PhysX3Common_x64.dll, IDA-verified)` structures — PhysX 3.4 is statically linked into the game.
- ✓ **Community confirms:** jouh + zarboz on UC (Feb 2026): "the game uses physx not chaos, you can make perfect vis check pretty easily."

## 2 — Root cause of the 0-actor failure

1. **Wrong object accepted as PxScene.** `UWorld+0x4E0` is labeled `PHYS_SCENE` in help.txt but the note says *PhysicsField* — not FPhysScene. `LooksLikePxScene()` only checked "vtable is a usermode address."
2. **Back-pointer validation not implemented.** zarboz's check — `FPhysScene+0xA0 == UWorld` — is in physx-mirror-plan.md but was never coded.
3. **Sig-scan loses to hints.** `48 8B 81 ?? ?? ?? ?? C3` scan exists, but hint offsets are tried first and garbage at 0x4E0 wins.
4. **Garbage in, garbage out.** The NpScene scanner read 1MB of the wrong object: 85K read failures, 2551 fake candidates, concreteType 0/65/44768 instead of 6/7.

## 3 — What other games do

| Game | Engine / Physics | Approach | Status |
|------|------------------|----------|--------|
| Escape from Tarkov | Unity + PhysX 4.1 | `physx_sdk` → scene array → `NpScene+0x23D8` rigid actors → shapes via ptr_table; SceneQueryManager for statics (~90% less traversal); convex hull parsing | Working; ~98% accuracy reported |
| PUBG | UE4.16 + PhysX (static) | PhysicsScene in UWorld; both RigidActors + SceneQueryManager; convex hulls from `hull_data` | Working; streaming gotcha |
| Arc Raiders | Embark UE + PhysX 3.4 (static) | UWorld → FPhysScene → +0xB0 PxScene → NpScene scan; scatter reads; Embree BVH; FKAggregateGeom alt path | "Stable for months" (zarboz) |
| Marvel Rivals / Dead by Daylight | UE5 + Chaos | No PxScene/NpScene; `FPBDRigidsSolver` → SOAs particles; internal LineOfSight only | Unsolved externally |
| PUBG (newer builds) | UE5 + Chaos | `FPhysScene_Chaos @ UWorld+0x228`, ChaosInterface traits | Chaos case |

**Takeaway:** every success reads PhysX NpScene and/or UE's own `FKAggregateGeom`, validates with concreteType 6/7, caches locally, raycasts on CPU. Chaos games are the stuck ones. We are on the PhysX side.

## 4 — Two data paths, one query engine

- **Path A (primary):** UWorld → FPhysScene *(validated: +0xA0 back-ptr == UWorld)* → PxScene +0xB0 *(vtable in module range)* → NpScene scan → PxRigidActor[] → shapes → geometry.
- **Path B (fallback):** Level actor → `StaticMeshComponent.StaticMesh 0x728` → `StaticMesh.BodySetup 0x1F0` → `BodySetup.AggGeom 0xB8` (KAggregateGeom 0x78: SphereElems 0x0 / BoxElems 0x10 / SphylElems 0x20 / ConvexElems 0x30). Dedupe by StaticMesh asset ptr. Zero PhysX internals — immune to NpScene drift.
- Both feed the existing grid broadphase + prim raycast + `LineOfSight()` (already built).

All Path B offsets verified in our SDK dump (CL-1341255).

## Stage 1 — Discovery hardening
- [ ] **Validate FPhysScene properly** — require `candidate+0xA0 == UWorld` for every UWorld candidate. **ETA: 1 session**
- [ ] **Module-range vtable check** — game exe + PhysX3Common_x64.dll base/size via DMA; reject heap "vtables" like `0x14B3E2D20`. **ETA: same session**
- [ ] **Sig-scan first, hints second** — drop `0x4E0` from hints (it's PhysicsField); try +0xB0 standard before in-object scan. **ETA: same session**

## Stage 2 — NpScene scan on the real scene
- [ ] **Restore strict validators** — concreteType 6/7 at +0x08, min 3 rigids, count 1..50000, cap ≥ count; reference 0x23D8, scan don't hardcode. **ETA: 1 session**
- [ ] **Cache last-known-good offset** — retry first each pass; invalidate on raid transition. **ETA: same session**

## Stage 3 — AggGeom fallback path
- [ ] **Harvest from level actors** — `StaticMesh 0x728` → `BodySetup 0x1F0` → `AggGeom 0xB8`; sphere/box/sphyl/convex elems; c2w × elem transform. **ETA: 2 sessions**
- [ ] **Dedupe by asset** — one geometry read per unique StaticMesh; byte budget per tick. **ETA: same session**

## Stage 4 — Wiring + raid validation
- [ ] **Feed the existing engine** — normalize into prim cache; `LineOfSight()` unchanged; 50cm tolerance; +35cm Z aim offset. **ETA: 1 raid**
- [ ] **Sanity checks** — blocked vs known cover, clear in open; counts in thousands. **ETA: same raid**

## Stage 5 — Performance & hardening
- [ ] **Batched reads** — fewest DMA transactions possible (~100× vs sequential per zarboz). **ETA: 1–2 sessions**
- [ ] **Re-enable overlay rows** only when `Ready()` is real. **ETA: same session**

## Build rule
One stage at a time → stop exe → build → raid-test → tick. Discovery is invalid until `+0xA0` back-pointer passes — no more trusting pretty vtables.
