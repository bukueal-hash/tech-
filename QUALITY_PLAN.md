# Arc Raiders 9+ Code-Quality Plan

## Goal

Move the project from an estimated 5.5/10 toward a verified 9+/10 through measurable reliability, ownership, testing, and runtime evidence. A 9+ score requires clean Windows builds, passing tests, safe shutdown, failure handling, and runtime verification.

## Phase 0 — Establish a trustworthy baseline

- Preserve unrelated working-tree changes.
- Run offset and SDK-gap audits.
- Perform a clean Windows `Release|x64` build.
- Run the full doctest suite and record failures.
- Inventory detached threads, global mutable state, and oversized modules.
- Record startup, shutdown, DMA failure, world-transition, and worker behavior.

**Exit gate:** the source either builds and tests cleanly, or all existing failures are documented before refactoring.

## Phase 1 — Make background work safe and owned

- Remove detached production threads.
- Use explicit start, stop, cancel, restart, join, and exception-reporting behavior.
- Wake sleeping workers immediately during shutdown.
- Stop schedulers before joining dependent workers.
- Join diagnostic and rebuild jobs before releasing shared state.

## Phase 2 — Establish explicit runtime ownership

Separate engine lifecycle/configuration, raid/world transitions, target-memory reads, scan scheduling, cache publication, rendering, and UI state. Rendering consumes published snapshots; target-memory reads remain in worker code; world transitions invalidate stale data atomically.

## Phase 3 — Split the largest modules

Use compatibility wrappers while separating:

- `Update.cpp`: world resolution, player chain, raid lifecycle, camera, and frame coordination.
- `Esp.cpp`: player, robot, world, item, radar, labels, and draw commands.
- `EntityList.cpp`: discovery, admission, health/name reads, visibility, and reconciliation.
- `Utils.cpp`: strings, time, geometry, pointer validation, memory results, and logging.

## Phase 4 — Harden memory and SDK boundaries

- Create typed reads for values, optional values, arrays, and pointer resolution.
- Distinguish read failure from legitimate unknown values.
- Centralize pointer plausibility checks.
- Add DMA fault-injection and malformed-SDK tests.
- Mark runtime-only offsets explicitly.
- Keep structure-size assertions close to layout definitions.

## Phase 5 — Turn tests into quality gates

Cover lifecycle, concurrency, stale-generation rejection, cancellation, shutdown during scans, failed DMA reads, invalid pointers, partial FName decode, malformed structures, clock jumps, missing optional fields, application/test builds, warnings, and stale-output prevention.

## Phase 6 — Release and runtime hygiene

Separate source, SDK metadata, and generated/runtime output. Define supported DLL versions and checksums, make packaging reproducible, display build/offset/SDK/DMA/worker state, and prevent stale output from silently passing verification.

## Aim-control quality milestone

This milestone improves reliability and motion quality only; it does not target anti-cheat evasion or concealment.

- Consume generation-aware, time-bounded aim snapshots.
- Keep target selection, bone resolution, motion control, hardware output, trigger control, and diagnostics in explicit modules.
- Reset motion, prediction, lock, grace, and trigger state on target/world changes, disabled state, and shutdown.
- Release trigger hardware on every early return, failure, menu transition, and shutdown path.
- Use signed motion offsets, bounded smoothing, stable deadzone behavior, and convergence checks.
- Remove live DMA reads from the aim thread by publishing identity, dead state, and visibility in snapshots.
- Test stale frames, target-switch hysteresis, signed motion, prediction reset, trigger release, invalid pointers, and hardware failure recovery.
- Measure aim-tick latency, snapshot age, target switches, sign flips, overshoot, and convergence.

**Exit gate:** aim and trigger behavior is deterministic under stale data and failure injection, does not retain old-world state, releases hardware safely, and meets documented latency/convergence thresholds.

## ESP Reliability + Frame-Health Overlay milestone

This milestone targets product reliability and correct rendering only; it does not target anti-cheat evasion or concealment.

- Stamp every ESP frame with its world generation and collection time.
- Reject stale, old-generation, invalid, and missing-timestamp frames instead of retaining them indefinitely.
- Reset retained paint, aim, and radar frames during cache/world resets.
- Keep collected bot and loot state when a camera or scatter operation fails, while using a valid live camera when available.
- Preserve a valid bot root position; use a valid center position only when the root is invalid; reject only when both are invalid.
- Add a read-only frame-health overlay showing frame age, sequence, generations, camera source/status, bot/loot counts, collection results, failure reasons, and paint skip counters.
- Keep diagnostics DMA-free on the paint thread and ensure diagnostics never change ESP behavior.
- Add DMA-free regression tests for generation, freshness, reset, fallback, collection failure, and diagnostic transitions.

**Exit gate:** bot and loot ESP do not reuse an unbounded old-world frame; camera/scatter failures are visible and recoverable; the Windows build and runtime checklist pass.

## Feature-dependency and optional-data audit

This milestone audits every user-facing feature as an end-to-end path: menu state, AutoConfig persistence, worker admission/scanning, snapshot publication, rendering, and failure cleanup.

- Master switches must dominate near-field reveal, stale optional data, and sub-feature rendering.
- Radar, player ESP, bot ESP, world ESP, HUD, and activity feed must operate independently where the menu says they do.
- Robot targeting may run without bot ESP; robot aim must never implicitly draw bot boxes, names, or diagnostics.
- Trigger-only mode may acquire and fire on player targets but must never move the pointer; Toggle and Always activation must not depend on a target being present that tick.
- Aimbot-only player targets need the same high-frequency position refresh as ESP/radar targets.
- Optional Steam IDs, squad pointers, DBNO timers, loadouts, bot vision, and other presentation data must stop their DMA work when all consumers are disabled.
- AutoConfig snapshot/equality must cover every persisted setting, including legacy camera-mode state.
- Add pure policy tests for feature dependencies and run the full Windows build/test gate after changes.

**Exit gate:** feature toggles have one authoritative dependency policy, disabled features do not silently enable peer features, and persistence covers all configurable state.

## Runtime validation checklist

1. Enter a raid and confirm bots and loot appear together.
2. Enable the debug overlay and record frame sequence, age, generation, camera source, and entity counts.
3. Observe or induce a DMA/scatter stall and confirm the failure reason and failed collection count are visible.
4. Confirm the retained frame expires after the documented freshness limit.
5. Rotate the camera and move the player while checking camera-source and projection changes.
6. Confirm a bot with an invalid root sample can use a valid center sample.
7. Leave and re-enter the raid or transition worlds.
8. Confirm the old generation is rejected immediately after reset and no old-world bot/loot entries paint.
9. Confirm caches repopulate and bot/loot ESP resumes without restarting the process.
10. Save the verification log and compare frame age, failed collections, camera source, and published entity counts.

## Final certification gates

- Zero detached production threads.
- No known shutdown races.
- Worker failures are visible and handled.
- Mutable global state has documented ownership.
- Memory access is abstracted and fault-testable.
- Normal and failure paths have test coverage.
- Clean Windows builds pass.
- Full test suite passes.
- Runtime stress tests survive DMA stalls and world transitions.
- No stale binary or offset mismatch can pass verification.
