# ARCs Repository — Duplication & Redundancy Audit

Read-only audit of `F:\Test\ARCs`. No files were edited, built, committed, or pushed to produce this report.

Scope covered: all first-party code under `Project/Core`, `Project/Functions`, `Project/Interface`, `Project/DMA`, `Project/Hardware`, `Project/Input`, plus root scripts and `help/`. Vendor, build artifacts, logs, and data are catalogued separately (Section 4).

**Classification key:** First-party source = `Project/**` (excluding `ThirdParty/`, `lib/include/`, `Hardware/Makcu/*vendor*`). Everything in `Build/**`, `debug-*.log`, and `*/Data/**` is generated/data, not a source cleanup target.

---

## 1. High-confidence actionable duplicates/redundancies

### 1.1 Dead duplicate helpers in ItemList/ContainerList (`DistanceMeters`, `JsonEscShort*`)
- `Project/Functions/ItemList.cpp` and `Project/Functions/ContainerList.cpp` each define `DistanceMeters(...)` and a JSON escaper (`JsonEscShort` / `JsonEscShortCont`). Grep shows each symbol appears **once** (its own definition) — they are never called.
- **Why redundant:** Dead code, byte-for-byte logic duplicated between the two files.
- **Risk:** None (unused). Removing is safe; does not touch any ESP gate.
- **Direction:** Delete both `DistanceMeters` and both `JsonEscShort*` definitions. Do not "consolidate into a shared header" — there are no callers to serve.

### 1.2 Identical `esc` NDJSON-escape lambda (agent-log)
- `Project/Functions/ItemList.cpp:848` and `Project/Functions/ContainerList.cpp:1023` define an identical `auto esc = [](std::string s){...}` inside `#region agent log` name-discovery logging.
- **Why redundant:** Same lambda, same purpose, stale debug instrumentation.
- **Risk:** Low; both live in debug-only blocks.
- **Direction:** When trimming agent-log instrumentation (see 1.9), remove both together, or hoist one `JsonEscapeShort()` into `WorldScanCommon` used by both debug blocks.

### 1.3 Identical velocity-EMA blend: `UpdateBotVelocity` vs `applyVelocity`
- `Project/Functions/RobotList.cpp:537-538` and `Project/Functions/PositionRefreshPass.cpp:131-132` contain the same guard `if (mag2 < (3000.0 * 3000.0))` followed by the same `cachedVelocity = cachedVelocity*0.5 + newVel*0.5` blend (all three axes).
- **Why redundant:** Same physics smoothing implemented twice.
- **Risk:** Medium — both write cached velocity that feeds aim lead. Behavior must remain bit-identical.
- **Direction:** Extract one `BlendCachedVelocity(Vector3& cached, const Vector3& newVel, float dt)` into `WorldScanCommon` (position/velocity is already shared territory) and call from both. Validate bots + players aim tracking after.

### 1.4 Triplicated world-position extrapolation (velMag lead constants)
- `Project/Functions/Aimbot.cpp:492-494` (`ExtrapolateRobotWorldPosToNow`) and `Aimbot.cpp:543-545` (`ExtrapolatePlayerAimWorldToNow`) share the identical lead ramp `velMag>80 → +0.055s`, `velMag>400 → +0.035s`. A third copy of the same idea is the `extrapolateEntry` lambda in `Esp.cpp` `CollectEspRenderFrame`.
- **Why redundant:** Three copies of the same lead-time math; drift risk if one is tuned and others aren't.
- **Risk:** Medium-high — directly affects aim feel and ESP box position. Player vs robot copies differ only in the dt clamp (`0.20` vs `0.25`).
- **Direction:** One `ApplyVelocityLead(Vector3&, const Vector3& vel, float lastUpdate, float maxDt)` helper. Keep the two clamp values as parameters (they are intentional). Consolidate the Aimbot pair first; fold the Esp lambda only after runtime confirmation.

### 1.5 `LooksLikeUtf16Garbage` (Update.cpp) vs `LooksLikeUtf16GarbageQword` (Utils.cpp)
- Two near-identical UTF-16-plausibility validators. `IsPlausibleObjPtr` (`Update.cpp`) and `IsUsableObjectPtr` (`Utils.cpp`) are likewise near-duplicate pointer-sanity checks.
- **Why redundant:** Same validation logic, different file-local names.
- **Risk:** Medium — both are on hot pointer-resolution paths; semantics must be preserved exactly.
- **Direction:** Move one canonical pair into `Memory`/`Utils` shared scope and have `Update.cpp` call it. Grep both callers first; do not alter thresholds.

### 1.6 Repeated weapon prefix-strip `kPrefixes` loop (Utils.cpp)
- `Project/Functions/Utils.cpp` defines the `kPrefixes` strip array / stripping loop multiple times within `ReadPlayerInventory` and weapon-name resolution paths.
- **Why redundant:** Same lookup table + loop copy-pasted per call site.
- **Risk:** Low-medium (naming only; player weapon labels). This is player-code, so treat as "frozen unless fixing players" per workflow.
- **Direction:** Single file-static `kWeaponPrefixes[]` + one `StripWeaponPrefix()` helper in `Utils.cpp`. Purely local to Utils — does not widen shared bot/item/world gates.

### 1.7 float[4] → `IM_COL32` color conversion duplicated 4×
- `Project/Functions/Esp.cpp:466`, `Esp.cpp:478`, `Esp.cpp:2036`, and `Project/Core/WorldItemCategory.cpp:2506` each hand-roll `static_cast<int>(rgba[0]*255.f), ...` conversions.
- **Why redundant:** Same conversion; easy to standardize.
- **Risk:** Low (pure rendering).
- **Direction:** One `inline ImU32 ColorFromRGBA(const float c[4])` in `EspDraw.h`, used by all four. No gate impact.

### 1.8 Duplicated container/loot token lists (`raidercache`/`cargoship`, hub tokens)
- `raidercache`/`cargoship` tokens appear in `Utils.cpp` (4 hits) and `ContainerList.cpp` (12 hits) across `FnameHintsSimpleLootActivity`, `QuickContainerCandidate`, and `AdmitContainerActor`.
- Hub token lists are duplicated: `LooksLikeStrictHubWorld`'s `kStrictHubTokens` vs `MatchHubToken`'s `kTokens` (Update-side world gating).
- **Why redundant:** Same classification vocabulary maintained in several places → drift = misclassified containers/hubs.
- **Risk:** HIGH to consolidate carelessly — these are shared ESP classification gates. Merging container tokens into a single shared list could widen item/world admission.
- **Direction:** Only unify **exact-duplicate** token arrays that are already used for the *same* decision (e.g., the two hub lists). For container tokens, do **not** merge into a shared gate; at most centralize the array as a `constexpr` table each function references, keeping each function's own matching policy intact.

### 1.9 Stale/duplicated agent-log NDJSON writers across scanners
- `#region agent log` blocks with hand-written `std::ofstream(... debug-*.log ...)` JSON emitters are duplicated (structurally near-identical) in `RobotList.cpp`, `ItemList.cpp`, `ContainerList.cpp`, `EntityList.cpp`, `Esp.cpp`, `PositionRefreshPass.cpp`, `Update.cpp`, and `Aimbot.cpp` (e.g. `WriteAimTrackNdjson`). They target `debug-c190fb.log`, `debug-5681af.log`, `debug-flicker.log`.
- **Why redundant:** Same file-open/throttle/JSON-emit boilerplate repeated; much of it is post-mortem instrumentation for already-closed investigations (runIds like `post-fix4`, `H-smooth`).
- **Risk:** Low functional risk, but high volume. Removing the per-tick file I/O also removes a known tickMs cost noted in the aim code comments.
- **Direction:** Introduce one throttled `AgentLog(session, location, kvpairs)` helper, or gate all blocks behind a single compile-time flag. Do this **after** the console `[debug*]` overlay tags are confirmed to cover the same signals (the overlay is the workflow's ground truth, so keep those).

### 1.10 Redundant thin wrappers with no added behavior
- `RobotList.cpp`: `ResolveBotLabelFromActor` / `ResolveBotLabelFromFName`, `HasConstructableEnemyAsset`, `IsLikelyContainerActor`, `IsLikelyWorldItemActor` are thin pass-throughs over `WorldScanCommon`/`AssetNames` primitives.
- **Why redundant:** Wrapper adds a name but no logic; increases the illusion of bot-specific gates.
- **Risk:** Low, but check each caller — some exist deliberately to keep bot resolution bot-only (that is desirable per the workflow).
- **Direction:** Inline only the ones that literally `return f(x);`. Keep any wrapper that exists to prevent bots from routing through shared item/world naming (that separation is intentional — see Section 3).

---

## 2. Medium-confidence candidates (need runtime validation)

### 2.1 ItemList vs ContainerList admission/prune skeleton
- `Project/Functions/ItemList.cpp` and `Project/Functions/ContainerList.cpp` share a large structural pattern: read classId → mask actor type → collect occupied keys → retain/prune loop → name-discovery debug block → build render entries.
- **Assessment:** Genuinely parallel, but they gate different categories (ground loot vs containers) with different admission predicates. Partial extraction (the cache-prune/retain loop) is plausible; the classification bodies should stay separate.
- **Validation:** Confirm the prune/retain timing constants match before extracting; test Items + World tabs.

### 2.2 GameState PlayerArray / LevelCollections walk duplicated
- `BuildGameStatePlayerStateAllowlist` (`EntityList.cpp`), `ResolveGameStateFromWorld` (`Utils.cpp`), and `TryWorldFromGameStateGlobal` (`Update.cpp`) each walk `LevelCollections`/GameState. Similar traversal, different outputs.
- **Validation:** These run on different threads (Update vs EntityList) — confirm whether a shared helper would introduce cross-thread reads before merging. Likely keep separate for thread isolation.

### 2.3 Camera POV resolution spread across Utils + Esp + Update
- `RefreshCameraFromViewTarget`, `TryBuildCameraFromPcmPov`, `BuildCameraCacheFromPovReads` (Utils) vs `ResolvePcmForEspFrame` / `ResolveLiveRenderCamera` (Esp) vs `PawnHasWorldPosition` (Utils) / `ChainHasWorldPosition` (Update, same magSq gate).
- **Validation:** `PawnHasWorldPosition` vs `ChainHasWorldPosition` look truly duplicate (same squared-magnitude plausibility gate) and are the best merge candidate here; the rest may be intentionally cached-vs-live variants.

### 2.4 Two radar projection paths
- `Engine::ProjectWorldLocationToRadar` (`Utils.cpp:504`, called only from `EntityList.cpp:781`) vs the `projectBlip` lambda inside `Esp.cpp` `RenderRadar` (used for players/robots/items).
- **Validation:** Determine whether the `EntityList` radar call is legacy (superseded by the Esp render-frame radar). If so, the `EntityList` path + `ProjectWorldLocationToRadar` may be removable dead-ish code.

### 2.5 `ContainerOpenMidProbe` open-detection helpers
- `ReadContainerOpenMidProbe` / `ContainerOpenMidProbeChanged` / `FnameHintsSimpleLootActivity` in `ContainerList.cpp` — confirm all are still wired into admission; `ContainerOpenMidProbeChanged` looked lightly used and may be an obsolete offset-probe fallback.
- **Validation:** Debug overlay `[debugContainer] opened/openSalvage/...` to confirm the probe still influences output before removing.

---

## 3. Intentional duplication — do NOT merge

- **`VisibleActor` vs `VisibleBotActor` (`VisCheck.cpp`)** — near-identical but intentionally differ in the order they probe `EmbarkMesh` vs `USkeletalMeshComponent`. Bots and players have different mesh layouts; merging risks visibility regressions on both. Keep separate.
- **`Memory::read` vs `Memory::read_nocache`** usage split (EntityList/PositionRefreshPass use nocache for fresh positions; scanners use cached) — this is deliberate cached-vs-live semantics, not duplication.
- **Bot-only resolvers in `RobotList.cpp`/`AssetNames.cpp`** that mirror item/world naming — intentional per the ESP workflow rule: bots must not route through shared item/world naming gates. Do not "de-duplicate" by widening shared gates.
- **Per-scanner caches** (`playerCache`, `robotCache`, `itemCache`, `containerCache`) and their mutexes — separate for thread isolation and category-specific eviction. Not a merge target.
- **Hardware backends `KmBox` / `KmBoxNet` / `Makcu`** — each `MoveAim`/`EnsureReady` is a distinct device transport (serial vs net vs Makcu). Same interface, different hardware; keep separate.
- **`AimAssistPlayer` vs `AimAssistRobot` (`Aimbot.cpp`)** — parallel structure but different eligibility (bot broken-flag, label resolution, ESP-frame robot pool). Intentional per-category isolation on the aim hot path.

---

## 4. Generated / vendor / build / log / data duplicates (NOT source cleanup targets)

- **Build artifacts:** `Build/Intermediate/*.obj`, `*.tlog`, `*.recipe`, `*.iobj/.ipdb`, `Build/ArcRaiders.exe`, `Build/ArcRaiders_new.exe` — the `_new` exe is the rebuild-run fallback output (`rebuild-run.ps1` builds it when the main exe is locked), an intentional dual-output, not source duplication.
- **Runtime DLLs:** `Build/leechcore.dll`, `Build/vmm.dll` — deployed copies of the DMA library; source of truth is `Project/lib`. Not cleanup targets.
- **Config:** `Build/auto_config.ini`, `Build/imgui.ini` — generated at runtime.
- **Data duplication (expected):** localization/asset JSON exists in both `Project/Data/**` and `Build/Data/**` (e.g. `loc/ST_ItemNames*.json`, `Bots_Items_Maps/en.json`, `items_meta.json`, `asset_index.csv`). `Build/Data` is the deploy copy of `Project/Data`. Keep one source of truth (`Project/Data`); the Build copy is a build step output.
- **Logs:** `debug-5681af.log`, `debug-c190fb.log`, `debug-flicker.log` — investigation output written by the agent-log blocks (Section 1.9). Safe to delete as files; the *code* that writes them is the actual cleanup target.
- **Vendor:** `Project/ThirdParty/ImGui/**`, `nlohmann/json`, `Project/lib/include/vmmdll.h`/`leechcore.h`, Makcu vendor sources — do not audit for internal duplication.
- **Build scripts:** `build.ps1`, `build_detailed.ps1`, and the build branch of `rebuild-run.ps1` all invoke the same MSBuild line. `build.ps1` vs `build_detailed.ps1` differ only in verbosity + `Tee-Object` logging — minor duplication; `rebuild-run.ps1` is the canonical workflow entry point. Optional: reduce `build*.ps1` to thin wrappers over `rebuild-run.ps1 -BuildOnly`, but low priority.

---

## 5. Recommended cleanup order (one focused change per cycle)

Follow the repo's ESP loop (ONE CHANGE → KILL EXE → BUILD → RUN → TEST ALL 4 → REPORT) for anything touching scanners/render/aim. Ordered lowest-risk → highest-coupling:

1. **Delete dead code** (no behavior change): `DistanceMeters` ×2 and `JsonEscShort*` ×2 (1.1). No runtime test needed beyond a clean build.
2. **Color helper** (1.7): add `ColorFromRGBA` in `EspDraw.h`, replace the 4 call sites. Test: Players/Bots/Items/World boxes still colored correctly.
3. **Weapon prefix helper** (1.6): local `Utils.cpp` consolidation only. Test: Players tab weapon labels (this is player code — verify no regression).
4. **UTF-16 / pointer validators** (1.5): unify `LooksLikeUtf16Garbage*` and the `IsPlausibleObjPtr`/`IsUsableObjectPtr` pair; grep all callers first. Test: all four areas (shared pointer paths).
5. **Velocity blend helper** (1.3): single `BlendCachedVelocity` for RobotList + PositionRefreshPass. Test: bot + player aim tracking and ESP box smoothness.
6. **Extrapolation helper** (1.4): consolidate the Aimbot robot/player pair first (keep dt clamps as params); fold the Esp lambda only after a clean report. Test: aim feel + ESP position on movers.
7. **Agent-log consolidation** (1.9 + 1.2): one throttled logger or a single compile flag; keep console `[debug*]` overlay tags intact (workflow ground truth). Test: overlay still reports numbers for all four scanners.
8. **Thin-wrapper inlining** (1.10): only literal pass-throughs; preserve bot-only separation.
9. **Medium candidates** (Section 2) individually, each with debug overlay on and a full 4-area report — especially anything near container/hub token gates (1.8), which must not widen shared admission.

Throughout: do not merge anything in Section 3, and do not centralize container/loot/hub classification tokens in a way that widens a shared bot/item/world gate.
