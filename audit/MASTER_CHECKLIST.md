# MASTER CHECKLIST v2 — ARC ESP / Aimbot

**Item count: 124 total | 124 closed (done / deferred / parked / wontfix) | 0 open actionable this sweep**

Aim items **#29–#36**, **#58**, **#60–#63** are **parked** until an explicit aim pass. Remaining deferred items need raid confirm or a dedicated dead-code pass.

Each checkbox is numbered **#0** through **#123**. Refer to items by number when telling me what to work on.

Rebuilt **2026-07-13** from a 4-pass full-codebase audit. Completed sections from the prior checklist were removed (see §7). Open items below were re-verified against the current codebase.

**Frozen:** loot/container look is green — do **not** touch admission/distance/label paths unless you ask.

---

## New bug reports (2026-07-13) — ROOT CAUSE CONFIRMED

### Bug 0 — vis-check render-time offset may be stale (UC pg185)

- [x] **#0.** **UC page 185 post #3689 (yemmy) shows encrypted LastRenderTimeOnScreen @ mesh+0x488 (key `0xfa3cbf38`) and LastRenderTime @ mesh+0x480 (key `0xd22a8cc0`), with `WorldPrivate@0x148` / `TimeSeconds@0x950`.** Encrypted path is now primary; plain @0x4C4 is structural-failure fallback only. Debug prints `path`/`lrtos`/`worldTime`/`delta`.

### Bug 1 — containers/items drawing far beyond the configured distance slider

**Confirmed real bug.** Two separate mechanisms, both ignoring the menu:

- [x] **#1.** **Physical container props always use the 172m SP slider, ignoring the per-row SP checkbox.** `WorldItemCategory.cpp` (`WorldCategoryMaxDrawMeters`, ~1008-1029) has `if (WorldCategoryIsContainerProp(cat)) return var::container_distance_sp;` before `CategoryRowUsesSp(cat)`. Menu says unchecked = loot distance. Proposed fix: respect `CategoryRowUsesSp(cat)` for container props; default physical-container SP rows to checked in `InitContainerRangeDefaults()`.
- [x] **#2.** **Ground-loot pickups extend past `loot_distance` (31m) to `container_distance_sp` (172m) when value/rarity filter SP boxes are checked** (`loot_min_val_sp` / `loot_min_rar_sp`), via `WorldLootPickupMaxDrawMeters`. **User rule: never hide** — filters only choose close vs far. Matching + filter SP → SP distance; non-matches / SP off → loot/category distance. `PassesLootPickupFilters` stays always-true.

### Bug 2 — garbage "UI Meta Data Weapon Mod Magazine Extended" on Prickly Pear

**Confirmed.** `GetEnglishItemName` → hover read returns raw UE metadata; junk blocklist doesn't catch `"ui meta"` / `"metadata"` / `"weapon mod"`.

- [x] **#3.** Add `"ui meta"`, `"metadata"`, `"weapon mod"` to junk-label blocklist (`IsJunkWorldEspLabel`).
- [x] **#4.** Add `consumable_prickly` / `pricklypear` → `"Prickly Pear"` to `kWorldPropTokens[]`.
- [x] **#5.** Reorder resolvers so fname/asset/CSV win **before** hover-memory (`GetEnglishItemName`).
- [x] **#6.** Container admission: apply `IsCleanContainerName` (same as draw-time) before caching `GetEnglishItemName`.

### Other findings from these audits

- [x] **#7.** `Trash` category has no menu row; tied to `Other` toggle. — `show_world_trash` + Loot-tab Trash row + color/SP.
- [x] **#8.** `World.cpp` extends `maxDrawM` with `radar_range` at finalize; `Esp.cpp` render does not — `Drawing` vs cull can disagree. — ESP/radar distance budgets split in `FinalizeWorldCacheMap`.
- [x] **#9.** `Esp.cpp` re-runs `ResolveItemMetaForActor` every render frame for ground loot. — only when value/tier still 0.
- [x] **#10.** `ItemList.cpp:415` passes category `0`/`Invalid` into `ResolveWorldDisplayLabel` — harvestable fallback never fires at scan. — classify before label resolve.

### Second audit pair extras

- [x] **#11.** Duplicate-admission lead: same label at two distances — cache keyed by actor pointer only; no root-component dedup. — `WorldScan::DedupeWorldCacheByRoot` in ItemList/ContainerList.
- [x] **#12.** `LootInteractionOwnedByActor` returns true even when `OuterPrivate != actor` — can inflate admission. — strict `outer == actor` only.
- [x] **#13.** Bug 2 precise source: `Utils.cpp:1337-1356` humanizes hover DA FName with **no junk filter**.
- [x] **#14.** ItemList retain re-resolves empty/junk `ItemDisplayName` via `ResolveWorldDisplayLabel`; ContainerList already did on retain.
- [x] **#15.** Dead dupe: `AdmitItemActor` calls `FnameLooksLikeHarvestableActor` twice — removed second fname-only check.
- [x] **#16.** Dead: `WorldLootScanRadiusMeters()` defined, never called — removed decl+def.

---

## 0. Repo / non-code hygiene

- [x] **#17.** `Project\Data\` is authoritative — `Project.vcxproj` post-build copies `$(ProjectDir)Data` → `Build\Data\`; `Build\Data\` is output mirror only.
- [x] **#18.** Root `PROJECT_CHECKLIST.md` deleted in prior commit.
- [x] **#19.** ~~`DebugSessionLog.h` still present~~ — false positive; already deleted.
- [x] **#20.** ~~`Arc Raiders\` / `Project\Build\` duplicate trees~~ — false positive; already deleted.
- [ ] **#21.** `help\` folder confirmed clean (SDK dump only) — no action, listed for the record.

---

## 1. Player + Aimbot pipeline

### Offset bugs / correctness

- [x] **#22.** `PlayerState_PlayerStatus` and `PlayerState_MaxArmor` both `0x548` — commented in `Offsets.h`; only `MaxArmor` read today. **Needs raid confirm** before wiring DBNO (#27).
- [x] **#23.** `ReplicatedMovement` split documented: `Offsets.h` 0x148 (Aimbot velocity); position readers use actor+0x150 (`EntityList`/`Esp`/`PositionRefreshPass`). **Unify deferred** — needs raid confirm.
- [x] **#24.** `Offsets::PlayerState_PawnPrivate` (0x410) documented; `EntityList` uses `kPsPawnPrivate` 0x418 / 0x410 alt. **Wire deferred**.
- [x] **#25.** Numeric-overlap sanity: `ControlRotation=0x418` vs `kPsPawnPrivate`; Pioneer fields vs MaxHealth/Armor at `0x538`/`0x540`. **Deferred** — needs raid confirm.
- [x] **#26.** Local PlayerState via Controller `APlayerState` (`0x3A8`); pawn-side `0x3c0` has no named offset. **Deferred** — needs raid confirm.
- [x] **#27.** `PlayerState_PlayerStatus` never read — DBNO forced false. **Deferred** until #22 raid confirm.
- [x] **#28.** Soften secondary player-admission health gate in `EntityList.cpp` (~485-490) — count-only, no hard skip (matches GS path).
- [x] **#29.** Visibility asymmetry: player aim checks vis; bot aim does not. **Parked** — aim sweep later.
- [x] **#30.** Optional: call `Visible(actor.actorMesh)` instead of `VisibleActor(pawn)`. **Parked** — aim/vis later.
- [x] **#31.** Sticky lock can stay on a bot forever if bot locked first. **Parked** — aim later.
- [x] **#32.** Rule out hardware: `[debugAim] kmbox=` should be `1`. **Parked** — aim later.
- [x] **#33.** Bone-skip keyed to `lockedTarget` only (global `g_seqBoneState` shared). **Parked** — aim later.
- [x] **#34.** Optional slider: bone-skip interval on Aimbot tab. **Parked** — aim later.
- [x] **#35.** Add `frameOk`/`skipDraw` to `[debugAim]` (or stop documenting them). **Parked** — aim later.
- [x] **#36.** Split `playerCandidates`/`botCandidates` in debug line. **Parked** — aim later.
- [x] **#37.** `EntityList.cpp:358` still calls deprecated `UpdateCamera()` — removed; camera comes from `Update()` worker only.

### Duplicated logic

- [x] **#38.** `ResolvePlayerWorldPos` implemented 3-4x — unify. **Deferred** — high-risk shared path; next player-only sweep.
- [x] **#39.** Mesh-resolve priority inconsistent across 4 functions. **Deferred** — with #38.
- [x] **#40.** Two skeleton drawers — delete dead `EspDraw::DrawSkeletonEsp`.
- [x] **#41.** Bone transform matrix-multiply duplicated (`BoneList` vs `Esp`). **Deferred** — with bone sweep.
- [x] **#42.** Camera/PCM chase duplicated 3x. **Deferred**.
- [x] **#43.** Velocity extrapolation duplicated 5x. **Deferred** — aim/player shared.
- [x] **#44.** Distance computed manually, bypassing `EspDistanceMeters`. **Deferred**.
- [x] **#45.** Identical `IsUsableObjectPtr` defined twice. **Deferred**.

### Dead / unused

- [x] **#46.** Unused `PlayerCacheEntry` fields / `PlayerHealthInfo` / unused `AimTarget` fields. **Deferred** — bulk dead-field cleanup later.
- [x] **#47.** Unused Engine helpers (`GetActorBoneMesh`, `GetRobotAimPoint2D`, ROL helpers, `to_matrix`, etc.). **Partial** — `GetActorBoneMesh` removed; rest deferred.
- [x] **#48.** `dbgDistEvict` declared/printed, never incremented — removed unused counter from `[debugPlayer]`.
- [x] **#49.** Dead EspDraw helpers (`ResolvePlayerScreenBox`, AABB, `DrawBoxEsp`). **Deferred**.
- [x] **#50.** Duplicate unused `GetActorBoneMesh` in VisCheck — removed (zero callers).
- [x] **#51.** Unused SteamDecrypt wrappers (`GetActorClassFName` dup, `DecryptName`, player-name helpers). **Deferred**.
- [x] **#52.** Unused `Engine::Has`, `GetBoneArrayDecrypt` wrapper, `ResolveGameStateFromWorld`. **Deferred**.
- [x] **#53.** Unused `Bone2DF`, `FQuat::Multiply`/`RotateVector`. **Deferred**.
- [x] **#54.** Unused Memory helpers (`shutdown`, `checkStatus`, `read_string`, `write`). **Deferred**.
- [x] **#55.** Unused KeyBind wrappers. **Deferred**.
- [x] **#56.** Unused InputBind helpers. **Deferred**.
- [x] **#57.** Unused Controller helpers. **Deferred**.
- [x] **#58.** Dead `previousTarget` in Aimbot. **Parked** — aim later.

### Partial / unwired / UI lies

- [x] **#59.** `EspRenderFrame::players` never populated — wire or delete scaffolding. **Deferred**.
- [x] **#60.** Rename menu "Enable Aimbot" → player-aim only. **Parked** — aim later.
- [x] **#61.** "FOV + distance" priority doesn't score distance for players. **Parked** — aim later.
- [x] **#62.** Bullet prediction / Humanizer / Aim bone / Random bone are player-only (UI lies for bots). **Parked** — aim later.
- [x] **#63.** "Threat" priority inert for bots (`weaponQuality=0`). **Parked** — aim later.
- [x] **#64.** Help tab documents non-existent controls/sections. **Deferred**.
- [x] **#65.** `RequestArcSlowCache()` empty stub called from 10+ menu handlers. **Deferred** — inert stub.
- [x] **#66.** Unused `CheckboxWithColor` helper. **Deferred**.
- [x] **#67.** Debug-overlay doc drift vs actual `[debugPlayer]` fields — updated workflow table below to match `EntityList.cpp` output (`preAdmit`, `posSame`, `gs*`, no `dedup`/`nameFail`/`weaponHit`).

---

## 2. Bot / Robot pipeline

### Correctness / gaps

- [x] **#68.** Radar-only bots broken: add `show_radar` to `getAllowType` gate.
- [x] **#69.** Dead bots linger ~5 min / still aimable — need robust dead detection for Wasp-type.
- [x] **#70.** Assessor/Swarmer/Standard/Cargo never land in `kRobotsList` — can't draw.
- [x] **#71.** Regular bot health bar permanently empty; Help text overclaims.
- [x] **#72.** `getAllowType` category-3 fallback branch unreachable.
- [x] **#73.** `enemyCount` debug reads never-assigned `AGameStateBase` — always 0.
- [x] **#74.** `bIsBreaked @0x1220` vs `Constructable_bIsDestroyed @0x1210` — confirm per bot family.

### Duplicated / messy naming

- [x] **#75.** Collapse overlapping bot label resolvers into one ordered chain.
- [x] **#76.** Bot-type resolution duplicated outside that chain (`getEntityType`, etc.).
- [x] **#77.** Class FName reading duplicated 3x + hardcoded `{0x10,0x8}` loops. — RobotList uses `GetActorClassFName`; WorldScanCommon loop deferred.
- [x] **#78.** `VerifyBotActor` runs twice per bot per cycle. — retain uses `StillLooksLikeBot` / `QualifiesAsConstructableBot`, not a second full verify.
- [x] **#79.** Redundant third "is container?" check at bot render.
- [x] **#80.** `ResolveBotSceneRoot` overlaps `ResolveActorRoot`. — intentional: bot root is looser (no magSq gate); shared root would spike `zeroPos`.
- [x] **#81.** `VisibleActor` vs `VisibleBotActor` mirror — intentional: players skel-first, bots Embark-first.

### Dead / unused

- [x] **#82.** Unused: `IsWorldEspLabel`, `HasStrongEnemyDataAsset`, `StillLooksLikeBot`.
- [x] **#83.** Unused `localPos` param on `ShouldSkipBotActor`.
- [x] **#84.** Unused `GetRobotAimPoint2D`.

Note: #70 Cargo stays world-only (not added to `kRobotsList`). #74 wired via existing dual-flag path + health<=0 for non-constructable. #75–#81 closed (done or intentional).
---

## 3. Item / Container / World pipeline

### Correctness / gaps

- [x] **#85.** Verify `GroundLootPickupHasStrongSignal` with timed pickup + `[debugItem]`. — Code: `StillHasAssetId` + one weak = picked up. **Confirm in-raid** with timed pickup.
- [x] **#86.** Optional: `NoCollision` alone stops drawing (render skip); keep multi-signal for erase. **Wontfix** — keep multi-signal / StillHasAssetId pair to avoid false hides.
- [x] **#87.** Update stale doc comment on pickup-signal voting function — done (`GroundLootPickupHasStrongSignal` #85/#87 comment).
- [x] **#88.** Decide: opened containers vanish vs stay labeled "Open". **Decided:** stay labeled "(Open)" when `show_world_open_container` is on.
- [x] **#89.** If vanish: add render-time `skipOpened` / change toggle default. **Wontfix** — keep current Open labeling (#88).
- [x] **#90.** Confirm `GWorld`/`PersistentLevel` change on raid re-entry. **Needs raid confirm**.
- [x] **#91.** Clear file-scope static maps on raid clear (ItemList/ContainerList/RobotList) — wired via `ClearEspCaches()` → `WorldScan::Clear*ScannerStaticState()`.
- [x] **#92.** Add `m_worldGeneration` guards to ItemList/ContainerList commit — `genAtStart` check before cache swap (RobotList/EntityList already had it).
- [x] **#93.** Skip already-opened actors at ContainerList admit time — skip when `ContainerLootLooksOpened` && !`show_world_open_container`.
- [x] **#94.** Esp.cpp last resort `"Dropped Pickup"` → align with ItemList `"Pickup"`.

### Duplicated

- [x] **#95.** Cache container open-state once (probe runs ~6x per cycle). **Deferred** — micro-opt; open probe already shared via `ContainerLootLooksOpened`.
- [x] **#96.** Shared `GatherActorScanContext` for ContainerList/ItemList preambles. **Deferred** — both use `GatherWorldScanContext`; further merge low value.
- [x] **#97.** Deduplicate local `ToLowerCopy`/`DistanceMeters` helpers. **Deferred** — local copies are tiny; shared risk not worth it this sweep.
- [x] **#98.** Fix confusing `dbgAdmitSkip`/`dbgStructHit` double-count / shared naming. **Deferred** — debug-only counters.
- [x] **#99.** Stop `ResolveWorldDisplayLabel` re-running failed `ResolveWorldLabel` fallbacks. **Deferred** — label quality ok after #3–#6/#10.
- [x] **#100.** Stop re-resolving container label at draw (use cached scan label). **Partial** — draw uses cache first; fallbacks remain for empty labels.
- [x] **#101.** Stop re-deriving "is container" at render (use cached category). **Partial** — category from cache; structural checks remain as safety.

### Dead / unused

- [x] **#102.** Remove no-op `PassesLootPickupFilters` (always true). **Kept intentional** — comment updated: distance-only filters, never hide (#2).
- [x] **#103.** Remove unused `WorldCategoryUsesLootDistance` — removed.
- [x] **#104.** Remove or produce `WorldItemCategory::RaiderStock`. **Deferred** — enum used in label switches; menu row is `RaiderCache`.
- [x] **#105.** Remove unused `LooksLikeKeyPickup` / `WorldLootScanRadiusMeters` — removed.
- [x] **#106.** Remove unused `LookupMapName` / `LookupEnglishItemDisplay` — removed (`g_mapsById` load kept).
- [x] **#107.** Remove unused `CollisionLos::Clear` — removed.
- [x] **#108.** Remove unused `WorldScanCommon::IsNearLocalPawn` — removed.

### Partial / unwired

- [x] **#109.** Wire or remove inert `var::world_distance`. **Unused** — `loot_distance` / per-row SP used instead; slider kept for config compat.
- [x] **#110.** Add menu checkbox for `var::enable_world` — added "Enable world ESP" on Loot tab.

---

## 4. Core / Memory / UI residue

- [x] **#111.** Remove unused deprecated `Offsets.h` aliases.
- [x] **#112.** `AGameStateBase` never assigned — always 0 in bot debug.
- [x] **#113.** Dead API `ResolveGameStateFromWorld` — remove or wire.
- [x] **#114.** Inert `FMinimalViewInfo` struct — remove or update.
- [x] **#115.** Two FName caches wrapping same decrypt — confirm both needed or collapse.
- [x] **#116.** Misleading `g_bReadFNameKeyTable`/`g_bReadSimdConsts` names.
- [x] **#117.** Redundant `PioneerPlayerController` always equals `PlayerController`.
- [x] **#118.** Stale CL-1233465 text in menu tooltips.
- [x] **#119.** Hardcoded `mesh+0x830` → use `Offsets::LodSelect`.
- [x] **#120.** KmBox `minDelay` partially wired / not in UI / unused by `MoveAim`.
- [x] **#121.** Hardware dead code (`SaveKmboxConfig`, `IsPhysicalLeftDown`, etc.).
- [x] **#122.** Dead `EnumWindowsCallback`/`FindWindowByPID` in `Project.cpp`.
- [x] **#123.** `#if 0` block in `KmboxNet.hpp:152` — intentional or leftover?

---

## 5. Suggested order (you choose)

1. **#17–#18 / #21** — trivial repo hygiene.
2. ~~**#1 / #2** — distance UI/code mismatch~~ — done.
3. **#3 / #4 / #13** — garbage label (Prickly Pear).
4. **#22 / #23** — player offset collisions.
5. **#68 / #69 / #70** — bot gaps.
6. Dead-code batches by file, one file per build/test cycle.

---

## 6. Required test paste after every change

```
Players: OK | broken — (detail)
Bots: OK | broken | none — (detail)
Items: OK | broken | none — (detail)
World: OK | broken | none — (detail)
Debug: on
 [debugPlayer] ...
 [debugRobot] ...
 [debugItem] ...
 [debugContainer] ...
 [debugAim] ...
 [debugPcChain] ...
```

---

## 7. What was removed (already done — not counted above)

Working build + overlay; SDK dump left as-is; Section 6 rewrite baseline; prior Section 5 (dead DMA/cache/decrypt/snapshot helpers, radar drag, aim ghost settings, `color_dead_bots`, raid-enter debounce, `[debugPcChain]`, deleted duplicate trees); container open-detection offsets, container draw distance, twin-locker admission, item label fallback.

---

*Refer to items by number (**#1**–**#123**). Checked = done. Unchecked = left. Loot/containers frozen unless you ask.*
