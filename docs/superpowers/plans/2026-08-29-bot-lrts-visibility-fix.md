# Bot LRTS Visibility Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the LRTS visibility check work for bots the way it already works for players, and clean up two accumulated issues (a stale contradictory comment and ungated trace I/O).

**Architecture:** Bots fail because `Engine::GetActorSkeletalMesh` (BoneList.cpp:45) prefers the `USkeletalMeshComponent` slot at offset 0x438, which for `PioneerConstructablePawn` bots is frequently NOT a bound skeletal mesh — so the `bRecentlyRendered` byte at `+0x997` reads 0x00 and `LrtsVis::CheckRendered` returns `Unknown` (treated as visible). Players work because `ResolvePlayerSkeletalMesh` (EntityList.cpp:267) prefers the `EmbarkMesh` slot at 0x7E8, which is a genuine `USkinnedMeshComponent`. The fix makes the bot path resolve the same way players do — try 0x7E8 first, validate it actually carries skeletal-mesh fields — and, as a safety net, makes the bot LRTS chain try the alternate mesh slot when the primary one reads `brr == 0`.

**Tech Stack:** C++17, DMA external reads (`Memory::read_nocache`), MSBuild Release|x64 via `rebuild-run.ps1`. No unit-test framework — verification is a Release build plus a live raid run watching `debug-c190fb.log` NDJSON and the LRTS menu tab.

## Global Constraints

- Every game-memory read in the LRTS path MUST use `Memory::read_nocache` (the VMM cache serves the same bytes twice and breaks the scan's change-detection).
- Do not touch the player path (`ResolvePlayerSkeletalMesh`) — it works; only mirror its ordering for bots.
- `IsValidPointer` (BoneList.cpp:19, range `0x1000 <= p < 0x7FFFFFFFFFFF`) is the bot-path validity gate; `Memory::IsValidPtrFast2` (range `0x10000 <= p < 0x7FFFFFFFFFFF`) is the stricter player-path gate.
- No changes that move actor positions, bones, or the bot list structure — this plan is visibility-only.
- Keep `LrtsVisibility.h` dependency-free (no `#include "../Core/Offsets.h"` in that header).
- Verification always ends with a full rebuild: `pwsh -File rebuild-run.ps1 -FullRebuild -BuildOnly`.

---

### Task 1: Fix the stale contradictory comment in `LrtsVisibility.h`

**Files:**
- Modify: `Project/Functions/LrtsVisibility.h:226-231`

**Interfaces:**
- Consumes: nothing
- Produces: nothing

The `Scan()` phase-1 comment claims "On this build the byte reads 0x00 for every mesh, so treating it as authoritative rejected every candidate and no key could ever lock." This directly contradicts the verified note at `LrtsVisibility.h:167-171` ("bit was set on 10 meshes and clear on 131, with the byte itself non-zero throughout") and the live trace data in `debug-c190fb.log` (brr values 0x29/0x9/0x58/0xa1 all non-zero and perfectly discriminating). The stale comment is why the gate logic treats `brr == 0` as "proceed" — it was written when a wrong offset made every read zero.

- [ ] **Step 1: Replace the stale comment**

Replace the block comment at lines 226-231 with:

```cpp
// Prefer meshes the frustum flag says are rendered, but do not require it:
// for bots, the 0x438 slot frequently isn't a bound skeletal mesh, so its
// flag byte may legitimately read 0x00 even when the bot is on screen.
// Phase 2 is the real proof anyway — it demands the value changed between
// two reads, which a stale mesh cannot fake.
```

- [ ] **Step 2: Rebuild to confirm the header still compiles**

Run: `pwsh -File rebuild-run.ps1 -FullRebuild -BuildOnly`
Expected: `[+] Build OK: F:\Test\ARCs\Build\ArcRaiders.exe`

- [ ] **Step 3: Commit**

```bash
git add Project/Functions/LrtsVisibility.h
git commit -m "docs: fix stale brr gate comment in LrtsVisibility.h"
```

---

### Task 2: Fix bot mesh resolution to prefer a real skeletal mesh (root cause)

**Files:**
- Modify: `Project/Functions/BoneList.cpp:45-56` (`Engine::GetActorSkeletalMesh`)
- Modify: `Project/Functions/BoneList.cpp:19-21` (`Engine::IsValidPointer`) — optional harden, see step 2

**Interfaces:**
- Consumes: none
- Produces: `GetActorSkeletalMesh` now prefers `Offsets::EmbarkMesh` (0x7E8) over `Offsets::USkeletalMeshComponent` (0x438), mirroring the working player resolver.

**Current bug path:** `RobotList.cpp:911, 1942, 2212` all call `GetActorSkeletalMesh` for bots; it returns the 0x438 slot first, which on constructive-pawns isn't a bound skeleton, so `actor.Mesh + 0x997` is 0x00 and the bot LRTS chain never leaves `Unknown`.

- [ ] **Step 1: Swap the slot preference in `GetActorSkeletalMesh`**

Replace the body (lines 49-55) with:

```cpp
// Prefer EmbarkMesh (0x7E8) first — it is a bound USkinnedMeshComponent on
// every pawn class, exactly like the player resolver does. Fall back to the
// USkeletalMeshComponent slot (0x438) which constructive-pawn bots often
// leave unbound. Validate with the stricter Fast2 range like the player path.
uintptr_t mesh = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
if (mesh && Memory::IsValidPtrFast2(mesh))
    return mesh;
mesh = Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
if (mesh && Memory::IsValidPtrFast2(mesh))
    return mesh;
return 0;
```

Note: `Memory::IsValidPtrFast2` is declared in `Project/Core/Memory.h` (static member, range 0x10000..0x7FFFFFFFFFFF). Wire `accessor: F:\Test\ARCs\Project\Functions\BoneList.cpp` — confirm BoneList.cpp already sees `Memory.h` via its existing includes (it calls `Memory::ReadRaw` at line 35, so it does).

- [ ] **Step 2: Rebuild and confirm linkage**

Run: `pwsh -File rebuild-run.ps1 -FullRebuild -BuildOnly`
Expected: `[+] Build OK` with no C2011/C2039/C2065 errors.

- [ ] **Step 3: Commit**

```bash
git add Project/Functions/BoneList.cpp
git commit -m "fix: prefer EmbarkMesh over USkeletalMeshComponent for bot meshes"
```

---

### Task 3: LRTS safety net — try the alternate mesh slot when `brr == 0`

**Files:**
- Modify: `Project/Functions/RobotList.cpp:2471-2503` (bot LRTS chain)

**Interfaces:**
- Consumes: `LrtsVis::BrrOffset` (0x997), `LrtsVis::BrrMask` (0x20), `Offsets::EmbarkMesh` (0x7E8), `Memory::read_nocache<uint8_t>`
- Produces: a helper `ResolveBotVisMesh(actor, primaryMesh)` returning the mesh whose flag byte is non-zero (or the primary if none qualifies).

Even with Task 2, some bots may still bind their skeleton in the 0x438 slot only. This task adds a per-actor retry: when the resolved mesh's `brr` byte is 0x00, retry the whole LRTS chain against the other slot (`EmbarkMesh` 0x7E8) before declaring Unknown.

- [ ] **Step 1: Add the helper before the bot LRTS block**

Insert at RobotList.cpp, just above `if (var::LrtsVisActive() && actor.Mesh && worldTime > 10.f) {` (line 2471):

```cpp
// LRTS fallback: if the primary mesh slot has no recently-rendered byte
// (constructive-pawns leave 0x438 unbound), retry the EmbarkMesh slot.
static uintptr_t ResolveBotVisMesh(uintptr_t actor, uintptr_t primaryMesh)
{
    if (primaryMesh) {
        if (Memory::read_nocache<uint8_t>(primaryMesh + LrtsVis::BrrOffset) != 0)
            return primaryMesh;
        if (!engine.IsValidPointer(primaryMesh))
            return Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
        return primaryMesh;
    }
    const uintptr_t embark = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    return (embark && engine.IsValidPointer(embark)) ? embark : primaryMesh;
}
```

- [ ] **Step 2: Use the helper in the bot LRTS chain**

Replace the mesh selection at the top of the block:

```cpp
uintptr_t visMesh = ResolveBotVisMesh(actor, actor.Mesh);
if (!visMesh)
    visMesh = actor.Mesh;
auto vis = LrtsVis::CheckRendered(
    [](uint64_t a) { return Memory::read_nocache<uint8_t>(a); },
    visMesh, LrtsVis::g_session,
    LrtsVis::BrrOffset, LrtsVis::BrrMask);
if (vis == LrtsVis::Result::Unknown) {
    const uintptr_t alt = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    if (alt && alt != visMesh && engine.IsValidPointer(alt)) {
        vis = LrtsVis::CheckRendered(
            [](uint64_t a) { return Memory::read_nocache<uint8_t>(a); },
            alt, LrtsVis::g_session,
            LrtsVis::BrrOffset, LrtsVis::BrrMask);
        if (vis != LrtsVis::Result::Unknown)
            visMesh = alt;
    }
}
```

Then pass `visMesh` (not `actor.Mesh`) into `CheckDirect`, `CheckRaw`, `Scan`/`Check` on the Unknown fallbacks. Specifically:

- Replace `actor.Mesh` with `visMesh` in the `CheckDirect` call target and the `MeshState` lookup key (`s_lrtsBotMeshStates[visMesh]` and `ms.meshComp = visMesh`).
- Keep `actor.Mesh` unchanged everywhere else in the file — drawing/positions must not move.

- [ ] **Step 3: Rebuild**

Run: `pwsh -File rebuild-run.ps1 -FullRebuild -BuildOnly`
Expected: Build OK.

- [ ] **Step 4: Commit**

```bash
git add Project/Functions/RobotList.cpp
git commit -m "fix: retry EmbarkMesh slot for bot LRTS when primary mesh brr is zero"
```

---

### Task 4: Gate the debug trace I/O behind a flag

**Files:**
- Modify: `Project/Functions/RobotList.cpp:1516-1535` (vis_trace) and `:2540-2559` (vis_gate)
- Modify: `Project/Interface/Utils/Variables/index.h` (add a flag; the Header already has `vis_enabled` at index.h:58)

**Interfaces:**
- Consumes: nothing
- Produces: `var::lrts_debug_trace` — when false, no per-second NDJSON appends from the bot vischeck.

The two blocks currently `std::ofstream(...).open(kArcVerifyPath, std::ios::app)` up to 9×/second into the 27 MB crash log. Gate both behind `var::lrts_debug_trace` (default off) so production runs stop writing; keep the counters in the LRTS tab as the always-on diagnostics.

- [ ] **Step 1: Add the flag**

In `Project/Interface/Utils/Variables/index.h`, next to `vis_enabled` (line 58):

```cpp
extern bool lrts_debug_trace;   // append per-second vis trace NDJSON rows
```

In `Project/Interface/Utils/Variables/index.cpp`, define it near `vis_enabled`'s definition:

```cpp
bool lrts_debug_trace = false;
```

- [ ] **Step 2: Wrap the two trace blocks**

At RobotList.cpp:1516, wrap the vis_trace `std::ofstream` block with:

```cpp
if (var::lrts_debug_trace) {
    ...existing vis_trace block unchanged...
}
```

At RobotList.cpp:2540, wrap the vis_gate `std::ofstream` block identically.

- [ ] **Step 3: Add a menu checkbox in the LRTS tab**

In `Project/Interface/Overlay/MenuSidebar.cpp` LRTS tab (after the "Enable LRTS" checkbox at line 410):

```cpp
ArcMenuLayout::Checkbox("Trace to log", &var::lrts_debug_trace);
ArcMenuHoverTooltip("Append per-second vis_trace/vis_gate NDJSON rows to debug-c190fb.log.");
```

- [ ] **Step 4: Rebuild**

Run: `pwsh -File rebuild-run.ps1 -FullRebuild -BuildOnly`
Expected: Build OK.

- [ ] **Step 5: Commit**

```bash
git add Project/Interface/Utils/Variables/index.h Project/Interface/Utils/Variables/index.cpp Project/Functions/RobotList.cpp Project/Interface/Overlay/MenuSidebar.cpp
git commit -m "feat: gate bot vis trace I/O behind lrts_debug_trace flag"
```

---

### Task 5: Field verification raid

**Files:**
- None (verification only; may produce `F:/Test/ARCs/debug-c190fb.log` changes)

**Interfaces:**
- Consumes: the build from Tasks 1-4 and a live ARC Raiders raid with bots in view.

- [ ] **Step 1: Enable the trace flag in the LRTS tab** (or flip `var::lrts_debug_trace` temporarily to `true`), build with `rebuild-run.ps1 -FullRebuild`, and run a raid.

- [ ] **Step 2: Confirm bot verdicts, not Unknown**

After a raid with visible and walled-off bots, extract the trace rows:

```powershell
$all = Get-Content "F:\Test\ARCs\debug-c190fb.log"
$vis = $all | Where-Object { $_ -match '"message":"vis_trace"' }
$vis | ForEach-Object { if ($_ -match '"brr":"(0x[0-9A-Fa-f]+)".*"vis":(-?[0-9]+)') { "$($matches[1])=$($matches[2])" } } |
    Group-Object | ForEach-Object { "{0,-10} {1}" -f $_.Name, $_.Count }
```

**Success** = the brr `0x0=0` bucket (Unknown) has collapsed to near zero relative to before, i.e. bots now get `0x9`/`0x58`-style Occluded and `0x29`/`0xa1`-style Visible verdicts, and behind-wall bots actually drop the ESP box (check `actor.isVisible` flows to `Esp.cpp:1309/1589`).

**Acceptance:**
- [ ] Bot ESP boxes visibly hide behind walls, exactly like players.
- [ ] No crashes, no stale-comment confusion (Task 1).
- [ ] Trace I/O is silent when `lrts_debug_trace` is off (file size stable).

- [ ] **Step 3: Commit any verification-only changes**

```bash
git add -A
git commit -m "test: verify bot LRTS visibility in raid"
```

---

## Self-Review

**Spec coverage:**
- Bot LRTS broken → Task 2 (root-cause slot order) + Task 3 (alt-slot safety net). ✓
- Players unaffected → all edits scoped to bot path; `ResolvePlayerSkeletalMesh` untouched. ✓
- Stale comment → Task 1. ✓
- Trace I/O behind flag → Task 4. ✓
- Field proof → Task 5. ✓

**Placeholder scan:** All steps carry concrete code or commands; no TBDs. ✓

**Type consistency:** `ResolveBotVisMesh` returns `uintptr_t` and is consumed by `visMesh` in the chain; `var::lrts_debug_trace` declared in index.h is used in RobotList.cpp and MenuSidebar.cpp; `actor.Mesh` assignments at RobotList.cpp:911/1942/2212 continue to use `GetActorSkeletalMesh` (updated by Task 2). No signature drift. ✓