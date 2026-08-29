# Arc Raiders LRTS Visibility — Implementation Plan

**Status:** Plan only — do not build until the verification gates pass.  
**Scope:** LRTS / `LastRenderTimeOnScreen` only. No WorldMirror, AggGeom, cooked collision, PhysX scene scan, or geometry fallback.

## Objective

Use each enemy's encrypted skeletal-mesh render timestamp to determine whether
it was actually rendered on screen and was not occluded by world geometry.

```text
visible = abs(worldTime - decryptedLastRenderTimeOnScreen) < 0.5 seconds
```

During startup, world changes, failed reads, or an unverified key, visibility
must remain `true` so the ESP does not hide targets because the detector is
warming up.

## Existing state

- EntityList and RobotList currently default `isVisible` to `true`.
- The supplied algorithm provides the intended encrypted LRTS discovery and
  per-mesh visibility test.

## Verified algorithm facts

### Candidate fields

- `LRTS` = `LastRenderTimeOnScreen`; intended occlusion-aware timestamp.
- `LRT` = `LastRenderTime`; frustum/render timestamp, not wall-accurate.
- `bRecentlyRendered` is only a frustum signal and is used to select a rendered
  mesh for key discovery, not as the final visibility answer.

### Encryption/decryption

```cpp
uint32_t bits = _byteswap_ulong(encrypted ^ key);
float value;
memcpy(&value, &bits, sizeof(value));
```

For a candidate encrypted value from a known recently-rendered mesh:

```text
key = rawCandidate XOR byteswap(worldTimeBits)
```

The key is provisional until the field changes over a short delay and decrypts
to the advanced world time.

## Stage 0 — build-specific verification (mandatory)

Before wiring visibility, verify these against the current Arc Raiders build:

1. **Skeletal mesh component pointer** used by EntityList/RobotList is the actual
   `USkeletalMeshComponent`, not a parent actor or stale cached pointer.
2. **`BRR_OFFSET = 0x997`, `BRR_MASK = 0x20`** identify the intended
   `bRecentlyRendered` bit. Confirm with a visible enemy and a non-rendered or
   off-frustum mesh.
3. The candidate field range **`mesh + 0x400 .. mesh + 0x600`** contains the
   encrypted render-time fields. Confirm at least one candidate decrypts near
   `worldTime` and changes after approximately 120 ms.
4. Confirm the source and type of `worldTime` (`UWorld::TimeSeconds`) and that
   its precision/unit matches the render timestamps.
5. Confirm the LRTS field is the field that becomes stale behind a wall while
   LRT remains frustum-based.

### Stage 0 evidence log

Log only bounded diagnostic values:

```text
world/map
mesh pointer
BRR value
worldTime
candidate offset
candidate raw value
candidate derived key
value before/after 120ms
delta after verification
```

Never lock an offset/key based only on one static value.

## Stage 1 — isolated LRTS module

Create a small module:

```text
Project/Functions/LrtsVisibility.h
Project/Functions/LrtsVisibility.cpp
```

Keep discovery state encapsulated:

```cpp
struct State {
    uint32_t lrtsOffset;
    uint32_t lrtOffset;
    uint32_t keys[6];
    int keyCount;
    int scanAttempts;
    uint64_t lastWorld;
    bool verified;
};
```

Required API:

```cpp
void Reset(uint64_t world);
void Observe(uint64_t mesh, float worldTime);  // candidate discovery
Result Check(uint64_t mesh, float worldTime);  // visible/hidden/unknown
```

`Result` must distinguish:

- `Visible`
- `Occluded`
- `Unknown` (warmup, invalid mesh, failed read, unverified key)

The ESP maps `Unknown` to visible/show-all.

## Stage 2 — candidate discovery

For up to 10 attempts:

1. Skip invalid meshes and `worldTime < 10`.
2. Read `bRecentlyRendered`.
3. Only scan meshes currently marked recently rendered.
4. Read the bounded candidate range in one safe memory operation.
5. For each aligned dword, derive a provisional key from `worldTime`.
6. Keep only finite values close to `worldTime`.
7. Wait about 120 ms, reread candidates, and require:
   - raw value changed;
   - decrypted value advanced with time;
   - finite value and sane range.
8. Prefer an adjacent LRT/LRTS pair separated by 4 or 8 bytes.
9. If no pair is found, permit a single verified LRTS field only.
10. Mark the detector verified only after the dynamic check passes.

No candidate may be accepted solely because it decrypts to a plausible float.

## Stage 3 — runtime key maintenance

At runtime:

- Keep up to six verified session keys.
- If all known keys fail on a recently-rendered mesh, schedule delayed key
  verification rather than immediately declaring it hidden.
- Reject duplicate keys.
- Reset all offsets, keys, pending state, and attempt count when the world
  pointer changes.
- Never use an unverified key for final visibility.

## Stage 4 — visibility decision

For each valid enemy skeletal mesh:

1. If detector is not verified: return `Unknown`.
2. Read the encrypted LRTS dword.
3. Try every verified key.
4. Accept a decoded value only if finite and within a sane time range.
5. Compute `delta = worldTime - decodedLrts`.
6. `abs(delta) < 0.5` => `Visible`.
7. Otherwise => `Occluded`.
8. Failed reads or failed key selection => `Unknown`, not `Occluded`.

The threshold must be configurable for testing, with 0.5 seconds as the initial
value from the supplied algorithm.

## Stage 5 — ESP integration

Replace the temporary `isVisible = true` assignments only after Stage 0/1
verification:

```cpp
const auto result = LrtsVisibility::Check(mesh, worldTime);
actor.isVisible = result != LrtsVisibility::Result::Occluded;
```

Use the existing enemy mesh pointers and existing camera/world-time plumbing.
Do not scan every actor indiscriminately; call only for admitted players/bots.
Do not introduce a second geometry or PhysX visibility system.

## Stage 6 — diagnostics and acceptance tests

Add low-rate diagnostics, not per-frame spam:

```text
lrts_state=unverified|verified
lrts_offset
lrt_offset
key_count
scan_attempts
visible_count
occluded_count
unknown_count
read_failures
world_resets
```

Acceptance tests:

1. **Visible target:** LRTS tracks world time and returns `Visible`.
2. **Behind wall:** LRTS becomes stale and returns `Occluded`.
3. **Frustum-only/off-screen:** LRT must not be mistaken for LRTS.
4. **Warmup:** all results are `Unknown` and ESP continues showing targets.
5. **World transition:** offsets/keys reset and rediscover.
6. **Static candidate:** rejected because its raw value does not change.
7. **Bad read:** returns `Unknown`, never falsely hides the target.
8. **No flicker:** debounce/hold the last valid result briefly if the existing
   ESP cadence causes one-frame read gaps.

## Explicit non-goals

- No AggGeom or `UBodySetup` parsing.
- No cooked collision parser.
- No PhysX `NpScene`/`PxScene` discovery.
- No geometry raycasting.
- No fallback visibility oracle. The only detector is verified LRTS; before it is
  ready, the safe result is show-all.

## Review gate before implementation

The reviewing AI should reject implementation until the Stage 0 evidence proves:

- the mesh pointer is correct;
- `BRR_OFFSET` is correct;
- the candidate range contains a changing timestamp;
- the derived key remains valid after the delayed reread;
- the selected field becomes stale specifically when a wall occludes the mesh.
