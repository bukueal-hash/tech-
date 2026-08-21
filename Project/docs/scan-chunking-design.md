# Scan Chunking Design: 200ms Hard Cap per Gated Scanner Turn

## Problem

The 50% total duty budget with GAP=12ms requires each scanner's heldMs <= 1.3ms.
Measured P99 bursts are 30-88ms, max bursts 431-781ms. No realistic GAP change
fixes this without destroying cadence. The only structural solution is breaking
monolithic scans into sub-budget chunks with gate release between them.

## Design: Cooperative Elapsed Budget

Each scanner's work is split into "phases" that run across multiple gated turns.
Between phases, the gate is released, the 12ms idle gap fires, and ungated
passes (camera, position, aim, frame builder) get their turn on the bus.

### Core API

```cpp
// In EngineThreads.cpp or a new ScanBudget.h

struct ScanBudget {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start;
    int maxMs;

    ScanBudget(int maxMs) : start(Clock::now()), maxMs(maxMs) {}

    // Returns true if budget is exhausted (> maxMs elapsed).
    // Call this at phase boundaries inside the scanner lambda.
    bool expired() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count() >= maxMs;
    }

    // Elapsed milliseconds since scan started.
    int elapsedMs() const {
        return (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count();
    }
};
```

### RunGatedScan — unchanged

The gate itself doesn't need modification. The cap is enforced *inside* the
scanner lambda by checking `budget.expired()` at phase boundaries and
returning early. The gate is released when the lambda returns normally.

```cpp
// Existing RunGatedScan stays the same:
template<typename F>
void RunGatedScan(const char* scanner, F&& fn) {
    // ... acquire gate, idle gap, log ...
    fn();                           // scanner runs, respects its own budget
    // ... release gate, log ...
}
```

### Scanner-side chunking pattern

Each scanner keeps persistent state (e.g. "which actor batch am I on?")
across turns. On each gated turn, it processes one phase, checks the
budget, and either continues to the next phase or returns early.

```cpp
// Example: EntityList scan with 200ms budget
void Engine::EntityListScan() {
    static int s_actorIdx = 0;  // persistent across turns

    ScanBudget budget(200);  // 200ms hard cap per turn

    // Phase 1: read actor pointers (if not done)
    if (s_actorIdx == 0) {
        // ... scatter read actor pointers ...
        if (budget.expired()) return;  // yield gate
    }

    // Phase 2: classify and update players (batched)
    for (; s_actorIdx < actorCount; ++s_actorIdx) {
        // ... process one player ...
        if (budget.expired()) return;  // yield gate, resume next turn
    }

    // Phase 3: final bookkeeping
    s_actorIdx = 0;  // reset for next turn
    // ...
}
```

## Per-Scanner Chunking Plan

### EntityList (current worst: 781ms, avg: 39ms)

**Budget: 50ms per turn** (allows ~2 turns/sec of full work, fits in 50% duty)

Chunk boundaries:
1. Scatter-read actor pointers (one scatter, ~5ms) — always runs
2. Classify and update players in batches of 64 — yields after each batch
3. Final sweep: dead/missing player cleanup — always runs

State: `m_entityBatchIdx` (which player we're processing)
Resume: continues from `m_entityBatchIdx` on next gated turn

### Update (current worst: 778ms, avg: 11ms)

**Budget: 50ms per turn**

Chunk boundaries:
1. GWorld resolution + PersistentLevel — always runs (fast, <5ms)
2. GameInstance + PlayerController chain — always runs
3. Actor enumeration in batches of 128 — yields after each batch
4. Health/weapon/armor reads per player in batches of 32 — yields per batch

State: `m_updatePhase`, `m_updateBatchIdx`
Resume: continues from batch on next turn

### RobotList (current worst: 631ms, avg: 25ms)

**Budget: 50ms per turn**

Chunk boundaries:
1. Read robot actor list — always runs
2. Classify and update robots in batches of 64 — yields per batch
3. Bot part position reads — yields per batch

State: `m_robotBatchIdx`

### ContainerList + ItemList (current worst: 431ms/561ms, avg: 1ms)

**Budget: 100ms per turn** (these are usually fast; cap only for cold-cache bursts)

These already tend to be fast (1ms avg). The cap only fires during
cold-cache bursts when many new items appear.

## Where the cap slots in — file-level changes

### New: Project/Interface/Utils/Threads/ScanBudget.h
- `struct ScanBudget` with `expired()` and `elapsedMs()`
- ~20 lines

### Modified: Project/Functions/EngineThreads.cpp
- No changes to `RunGatedScan` itself (it's already a pass-through)
- Add a `kScanBudgetMs` constant (50 for most, 100 for world items)
- Each scanner's lambda wraps its body with `ScanBudget budget(kScanBudgetMs)`

### Modified: Project/Functions/Update.cpp
- `Engine::Update()` — add phase state (`m_updatePhase`, `m_updateBatchIdx`)
- Break the monolithic Update() into 4 phases with `budget.expired()` checks
- Reset batch state on raid transitions

### Modified: Project/Functions/EntityList.cpp (or wherever EntityList runs)
- Add `m_entityBatchIdx` state
- Break into 3 phases with budget checks

### Modified: Project/Functions/RobotList.cpp
- Add `m_robotBatchIdx` state
- Break into 2 phases with budget checks

## Effect on duty cycle

With 200ms cap and 5 scanners:
- Worst case per scanner: 200ms held, eff period = 212ms, duty = 94%
- Total worst case: 5 * 94% = 470% — still too high

With 50ms cap:
- Worst case per scanner: 50ms held, eff period = 62ms, duty = 81%
- Total worst case: 5 * 81% = 403% — still high but now each chunk is small
  enough that the gate releases between chunks, letting ungated passes through

The key insight: **the absolute duty number doesn't matter for the ungated
passes — what matters is that no single bus hold exceeds their tick period**.
With 50ms chunks, the camera thread (8ms) can still interleave because the
bus is released for 12ms between chunks. The camera's worst-case stall is
now 50ms (one chunk) instead of 781ms (one monolithic scan).

### Camera stall reduction

| Scenario | Before (monolithic) | After (50ms chunks) |
|---|---|---|
| EntityList worst case | 781ms camera stall | 50ms + 12ms gap + 50ms + ... |
| RobotList worst case | 631ms camera stall | 50ms + 12ms gap + ... |
| All scanners worst | 3182ms total stall | 50ms per chunk, ungated passes interleave |

The camera thread's effective max stall drops from 781ms to ~62ms (one
chunk + one gap). That's a 12x improvement.

## Trade-offs

**Pros:**
- Camera/aim/position cadence protected (max stall = chunk + gap = 62ms)
- No GAP change needed (12ms idle gap still works)
- Scanner total throughput preserved (same work, just spread across turns)
- Self-healing: if a chunk is fast, the scanner runs multiple phases per turn

**Cons:**
- State management complexity (each scanner needs persistent batch indices)
- More gate transitions (5 chunks * 5 scanners = 25 gate turns per full cycle)
- Slightly more overhead from repeated gate acquire/release/idle-gap
- Scanner code becomes multi-phase instead of single-pass

**Mitigation for overhead:**
The 12ms gap per gate transition * 25 transitions = 300ms of idle time
per full cycle. That's the cost of protecting the ungated passes. But
most scanners won't need 5 chunks — EntityList at avg 39ms fits in 1
chunk, RobotList at avg 25ms fits in 1. Only the burst cases trigger
multiple chunks, which is exactly when the protection is needed.
