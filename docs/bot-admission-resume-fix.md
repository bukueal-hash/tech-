# Bot Admission Resume-Frontier Fix — Riventides 16K-Actor Maps

**Date:** 2026-08-29
**Status:** Ready to implement — B4 (bot frontier) is already in the working tree but unbuilt; this plan completes it (B5), hardens it (verify backlog), and mirrors the pattern to the player ring, then builds and verifies.

## Evidence (live raid logs, debug-c190fb.log)

### 1. Bot ring deadlocked — Riventides, 16K actors

```
bot_admit_stats:  scanned:0 admitted:0 cache:0 drawing:0 quickPass:0
                  slice:0 (frozen)   enemyCount:42-77   actorCount:664 (persistent)
player_admit_stats: scanned:16384 admitN:16383 cache:11 slice:0 ringResets:2
```

- Bots: zero scan, zero cache, zero draw. The 8-slice admit ring never leaves slice 0.
- Players: `cache:11` still works but the ring is **also** frozen at slice 0 — only cached players draw; new players would never admit on this map.
- Streaming levels push total actors to `kMaxActorsTotal` (16384). A 2048-row slice can't complete its scout window inside the shared 90ms DMA budget.

### 2. Root-cause chain

```
16K actor arrays saturate the shared DMA link
  → admission probe scatter only lands ~1-3 of 43 chunks per pass
  → slicePartial=true → admitRingAdvanceOk=false  (ring never advances)
  → next pass restarts the SAME slice from row 0
  → zero-buffer rows (never scattered) still get fname-probed
  → mass negative-memoization → permanent blindness
  → cache:0, drawing:0
```

B4 (already in `RobotList.cpp`) fixes the **scan** restart: it introduces `s_admitResumeRow`, keeps deterministic `bandRows` order, and only processes freshly-scattered rows `[0, scatteredEnd)`. But it has two gaps:

1. **The frontier is captured before the screening loop.** If the 40ms screening (`procBudget`) or the 60ms serial-verify (`admitBudget`) breaks after a full scan, `s_admitResumeRow` was already reset to 0 — next pass re-scatters the whole band, and if the verify stage keeps blowing budget the ring livelocks on slice 0. This reproduces the deadlock even with B4 compiled.
2. **The verify stage sets `slicePartial=true` on budget break.** That freezes ring advancement based on a stage that must not gate discovery.

## Stage 1 — B5: monotonic frontier (resume from the screening frontier)

**File:** `Project/Functions/RobotList.cpp`

**Why:** the resume cursor must reflect how far the band has been *fully consumed* (scattered AND screened), not just how far it was scattered. A break anywhere in scatter or screening must persist progress — never rewind, never lose scanned rows.

Changes:

- **Move the frontier bookkeeping after the screening loop.** Keep `scatteredEnd = min(base, probeRows.size())` and the distance-sort gate exactly as B4. In the screening loop, capture `processEnd` (the row index the 40ms screening budget reached).
- **New frontier rule (B5):**

```cpp
// B5: resume from the SCREENING frontier — monotonic progress. Rows in
// [processEnd, scatteredEnd) were scattered but not screened; they are
// re-scattered next pass (cheap, chunked) and cannot be lost.
const size_t prioCount = prioRows.size();
const bool sortedSweep = scatteredEnd >= probeRows.size();
const bool consumedAll = sortedSweep && processEnd == scatteredEnd;
if (consumedAll) {
    s_admitResumeRow = 0;                 // full band consumed → advance ring
} else if (sortedSweep) {
    s_admitResumeRow = 0;                 // distance-sorted order ≠ band order;
                                          // replay the band (correct over fast)
    slicePartial = true;
} else {
    const size_t bandProcessed =
        (processEnd > prioCount) ? (processEnd - prioCount) : 0;
    s_admitResumeRow =
        resume + (std::min)(bandProcessed, bandRows.size() - resume);
    // slicePartial already true when scatter or screening broke; force it
    // when screening stopped before the scan frontier for the full-band case
    // handled above via sortedSweep.
}
```

- **Ring gate unchanged:** `admitRingAdvanceOk = !slicePartial` — now only scatter/screening partiality can stall the ring, and every partial state persists real progress.

## Stage 2 — Serial-verify backlog (RobotList)

**File:** `Project/Functions/RobotList.cpp`

**Why:** the 60ms `admitBudget` loop (fname + `VerifyBotActor`) is inherently serial and DMA-heavy. When it runs out of budget it currently marks `slicePartial=true` (ring stall) and *drops* the unprocessed candidates (they are only rebuilt from the current slice). Both are wrong.

Changes:

- New static `std::vector<uintptr_t> s_admitVerifyBacklog;` (cap 4096, keep the tail on overflow). Cleared in the `ringReset` block with `s_admitResumeRow`.
- Convert the `for (uintptr_t actor : admitCandidates)` loop to an indexed loop over `pending = backlog + admitCandidates`:

```cpp
WorldScan::ScanBudget admitBudget(std::chrono::milliseconds(60));
std::vector<uintptr_t> pending;
pending.reserve(s_admitVerifyBacklog.size() + admitCandidates.size());
pending.insert(pending.end(), s_admitVerifyBacklog.begin(), s_admitVerifyBacklog.end());
pending.insert(pending.end(), admitCandidates.begin(), admitCandidates.end());
s_admitVerifyBacklog.clear();
size_t vi = 0;
for (; vi < pending.size(); ++vi) {
    if (admitBudget.expired()) {
        // Requeue the remainder (best-effort). Verify never stalls the ring:
        // cached bots are re-verified cheaply (neg-memo TTL), and the backlog
        // drains across passes even on a saturated bus.
        const size_t rest = pending.size() - vi;
        if (s_admitVerifyBacklog.size() + rest <= kAdmitVerifyBacklogMax)
            s_admitVerifyBacklog.insert(s_admitVerifyBacklog.end(),
                pending.begin() + static_cast<std::ptrdiff_t>(vi), pending.end());
        else
            s_admitVerifyBacklog.assign(
                pending.end() - static_cast<std::ptrdiff_t>(kAdmitVerifyBacklogMax),
                pending.end());
        break;   // NO slicePartial here
    }
    const uintptr_t actor = pending[vi];
    ...existing body unchanged...
}
```

- **What this fixes:** the ring advances on scan+screening completeness alone; a verify overrun just defers the tail one pass. New spawns still surface immediately via the prior-actor diff + `kAdmitPrioNewMax` prio band, so the backlog cannot delay genuine new bots (they are re-listed as "new" every pass until admitted).

## Stage 3 — Mirror B5 to the player ring (EntityList)

**File:** `Project/Functions/EntityList.cpp`

**Why:** the player ring uses the pre-B4 pattern — unordered `probeSet` iteration, no resume frontier, screening over **all** rows (including zero-buffer ones beyond `scatteredEnd`), and distance-sort gated only on `!probeRows.empty()`. On Riventides it is frozen at slice 0 (surviving only on `cache:11`).

Changes:

- New static `s_playerAdmitResumeRow` (cleared in `ringReset`). A separate verify backlog is **not** needed here — EntityList's serial PS-chase/fname stage IS its screening loop, so the `processEnd` frontier covers partial passes (unlike RobotList which has a second serial verify stage).
- Replace the `probeSet` construction with B4's deterministic `bandRows` + `prioRows` (same band math, dedup, `kPlayerAdmitPrioNewMax` cap, prio rows lead). Neg-memo filtering (`PlayerScanNegMemoHit`) moves **into** the screening loop so `probeRows` stays a 1:1 projection of prio+band — the resume frontier stays positionally exact.
- After the chunked scatter loop, compute `scatteredEnd` (same as the bot path).
- Gate the distance-sort on `scatteredEnd >= probeRows.size()` (full sweep only).
- Screen loop becomes `for (size_t ri = 0; ri < scatteredEnd; ++ri)` with fresh 40ms `procBudget`, capturing `processEnd`, then the same B5 frontier rule and `slicePartial` semantics.

## Stage 4 — Build + field verification

- Compile + link + launch with `scripts\build.ps1 -Run` (self-elevates, closes the game, links, relaunches).
- In a Riventides raid watch `debug-c190fb.log`:

```
bot_admit_stats  → slice cycles 0..7, scanned>0, quickPass>0, cache climbs, drawing>0
player_admit_stats → slice cycles, cache grows past 11
```

- **Vischeck harness (already in the tree):** enable `Draw LOS rays` in the LRTS tab — green bots are admitted + LOS-clear, red are admitted + LOS-blocked. Bots must render before the rays mean anything; that is the end-to-end gate for this fix.
- Flicker/camera regression: `cam_lead_skip` stays 1-3, `flicker_score` all zeros (the chunked scatter + yield already protect the 8ms camera thread; the B5 resume is pure bookkeeping).

## Out of scope (deliberately)

- `kMaxActorsTotal` lowering, level-stream culling, or actor-list filtering upstream — the 16K scan stays; the ring now drains it over time instead of dying on it.
- ContainerList admission — world items already work via the couple-scatter world path; no ring, no fix needed.
- The KD-tree / LRTS vischeck internals — only used at verdict/draw time, doesn't gate admission.

## Reviewer checklist

- [ ] `GamePatched` — no offset changes; SDK dump verified for CL-1341255.
- [ ] RobotList B5: resume is computed AFTER screening; `consumedAll` → advance; `sortedSweep` partial → replay; else `bandProcessed`.
- [ ] RobotList backlog: `kAdmitVerifyBacklogMax` cap; cleared on `ringReset`; verify break does NOT set `slicePartial`.
- [ ] EntityList mirrors: deterministic bandRows, `scatteredEnd` gate on distance-sort, screen loop bounded by `scatteredEnd`, same frontier rule.
- [ ] Build OK; raid log shows both rings cycling slices and both caches growing.
- [ ] Bots render (esp box + label) with `enableesp`, behind-wall bots drop via vischeck, `cam_lead_skip` 1-3, `flicker_score` zeros.