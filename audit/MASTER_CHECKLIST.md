# MASTER CHECKLIST v2 — ARC ESP / Aimbot

**Item count: 124 total | 41 done | 83 left**

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

- [ ] **#7.** `Trash` category has no menu row; tied to `Other` toggle.
- [ ] **#8.** `World.cpp` extends `maxDrawM` with `radar_range` at finalize; `Esp.cpp` render does not — `Drawing` vs cull can disagree.
- [ ] **#9.** `Esp.cpp` re-runs `ResolveItemMetaForActor` every render frame for ground loot.
- [ ] **#10.** `ItemList.cpp:415` passes category `0`/`Invalid` into `ResolveWorldDisplayLabel` — harvestable fallback never fires at scan.

### Second audit pair extras

- [ ] **#11.** Duplicate-admission lead: same label at two distances — cache keyed by actor pointer only; no root-component dedup. Needs debug log of actorKey/root/label/distance to confirm.
- [ ] **#12.** `LootInteractionOwnedByActor` returns true even when `OuterPrivate != actor` — can inflate admission.
- [x] **#13.** Bug 2 precise source: `Utils.cpp:1337-1356` humanizes hover DA FName with **no junk filter**.
- [ ] **#14.** Neither ItemList nor ContainerList re-resolves `ItemDisplayName` on retain — bad labels stick until eviction.
- [ ] **#15.** Dead dupe: `AdmitItemActor` calls `FnameLooksLikeHarvestableActor` twice.
- [ ] **#16.** Dead: `WorldLootScanRadiusMeters()` defined, never called.

---

## 0. Repo / non-code hygiene

- [ ] **#17.** `Project\Data\` duplicates `Build\Data\` — confirm which is authoritative and delete the stale copy.
- [ ] **#18.** Root `PROJECT_CHECKLIST.md` is a stale pre-rewrite checklist — safe to delete.
- [x] **#19.** ~~`DebugSessionLog.h` still present~~ — false positive; already deleted.
- [x] **#20.** ~~`Arc Raiders\` / `Project\Build\` duplicate trees~~ — false positive; already deleted.
- [ ] **#21.** `help\` folder confirmed clean (SDK dump only) — no action, listed for the record.

---

## 1. Player + Aimbot pipeline

### Offset bugs / correctness

- [ ] **#22.** `PlayerState_PlayerStatus` and `PlayerState_MaxArmor` both `0x548` — pick the correct one.
- [ ] **#23.** `ReplicatedMovement` split: named `0x148` vs local `0x150` in position code vs aim velocity — unify.
- [ ] **#24.** `Offsets::PlayerState_PawnPrivate` orphaned; EntityList hardcodes `0x418`/`0x410`.
- [ ] **#25.** Numeric-overlap sanity: `ControlRotation=0x418` vs `kPsPawnPrivate`; Pioneer fields vs MaxHealth/Armor at `0x538`/`0x540`.
- [ ] **#26.** Local PlayerState via Controller `APlayerState` (`0x3A8`); pawn-side `0x3c0` has no named offset.
- [ ] **#27.** `PlayerState_PlayerStatus` never read — DBNO forced false.
- [ ] **#28.** Soften secondary player-admission health gate in `EntityList.cpp` (~485-490).
- [ ] **#29.** Visibility asymmetry: player aim checks vis; bot aim does not.
- [ ] **#30.** Optional: call `Visible(actor.actorMesh)` instead of `VisibleActor(pawn)`.
- [ ] **#31.** Sticky lock can stay on a bot forever if bot locked first.
- [ ] **#32.** Rule out hardware: `[debugAim] kmbox=` should be `1`.
- [ ] **#33.** Bone-skip keyed to `lockedTarget` only (global `g_seqBoneState` shared).
- [ ] **#34.** Optional slider: bone-skip interval on Aimbot tab.
- [ ] **#35.** Add `frameOk`/`skipDraw` to `[debugAim]` (or stop documenting them).
- [ ] **#36.** Split `playerCandidates`/`botCandidates` in debug line.
- [ ] **#37.** `EntityList.cpp:358` still calls deprecated `UpdateCamera()`.

### Duplicated logic

- [ ] **#38.** `ResolvePlayerWorldPos` implemented 3-4x — unify.
- [ ] **#39.** Mesh-resolve priority inconsistent across 4 functions.
- [x] **#40.** Two skeleton drawers — delete dead `EspDraw::DrawSkeletonEsp`.
- [ ] **#41.** Bone transform matrix-multiply duplicated (`BoneList` vs `Esp`).
- [ ] **#42.** Camera/PCM chase duplicated 3x.
- [ ] **#43.** Velocity extrapolation duplicated 5x.
- [ ] **#44.** Distance computed manually, bypassing `EspDistanceMeters`.
- [ ] **#45.** Identical `IsUsableObjectPtr` defined twice.

### Dead / unused

- [ ] **#46.** Unused `PlayerCacheEntry` fields / `PlayerHealthInfo` / unused `AimTarget` fields.
- [ ] **#47.** Unused Engine helpers (`GetActorBoneMesh`, `GetRobotAimPoint2D`, ROL helpers, `to_matrix`, etc.).
- [ ] **#48.** `dbgDistEvict` declared/printed, never incremented.
- [ ] **#49.** Dead EspDraw helpers (`ResolvePlayerScreenBox`, AABB, `DrawBoxEsp`).
- [ ] **#50.** Duplicate unused `GetActorBoneMesh` in VisCheck.
- [ ] **#51.** Unused SteamDecrypt wrappers (`GetActorClassFName` dup, `DecryptName`, player-name helpers).
- [ ] **#52.** Unused `Engine::Has`, `GetBoneArrayDecrypt` wrapper, `ResolveGameStateFromWorld`.
- [ ] **#53.** Unused `Bone2DF`, `FQuat::Multiply`/`RotateVector`.
- [ ] **#54.** Unused Memory helpers (`shutdown`, `checkStatus`, `read_string`, `write`).
- [ ] **#55.** Unused KeyBind wrappers.
- [ ] **#56.** Unused InputBind helpers.
- [ ] **#57.** Unused Controller helpers.
- [ ] **#58.** Dead `previousTarget` in Aimbot.

### Partial / unwired / UI lies

- [ ] **#59.** `EspRenderFrame::players` never populated — wire or delete scaffolding.
- [ ] **#60.** Rename menu "Enable Aimbot" → player-aim only.
- [ ] **#61.** "FOV + distance" priority doesn't score distance for players.
- [ ] **#62.** Bullet prediction / Humanizer / Aim bone / Random bone are player-only (UI lies for bots).
- [ ] **#63.** "Threat" priority inert for bots (`weaponQuality=0`).
- [ ] **#64.** Help tab documents non-existent controls/sections.
- [ ] **#65.** `RequestArcSlowCache()` empty stub called from 10+ menu handlers.
- [ ] **#66.** Unused `CheckboxWithColor` helper.
- [ ] **#67.** Debug-overlay doc drift vs actual `[debugPlayer]` fields.

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

- [ ] **#85.** Verify `GroundLootPickupHasStrongSignal` with timed pickup + `[debugItem]`.
- [ ] **#86.** Optional: `NoCollision` alone stops drawing (render skip); keep multi-signal for erase.
- [ ] **#87.** Update stale doc comment on pickup-signal voting function.
- [ ] **#88.** Decide: opened containers vanish vs stay labeled "Open".
- [ ] **#89.** If vanish: add render-time `skipOpened` / change toggle default.
- [ ] **#90.** Confirm `GWorld`/`PersistentLevel` change on raid re-entry.
- [ ] **#91.** Clear file-scope static maps on raid clear (ItemList/ContainerList/RobotList).
- [ ] **#92.** Add `m_worldGeneration` guards to ItemList/ContainerList commit.
- [ ] **#93.** Skip already-opened actors at ContainerList admit time.
- [ ] **#94.** Esp.cpp last resort `"Dropped Pickup"` → align with ItemList `"Pickup"`.

### Duplicated

- [ ] **#95.** Cache container open-state once (probe runs ~6x per cycle).
- [ ] **#96.** Shared `GatherActorScanContext` for ContainerList/ItemList preambles.
- [ ] **#97.** Deduplicate local `ToLowerCopy`/`DistanceMeters` helpers.
- [ ] **#98.** Fix confusing `dbgAdmitSkip`/`dbgStructHit` double-count / shared naming.
- [ ] **#99.** Stop `ResolveWorldDisplayLabel` re-running failed `ResolveWorldLabel` fallbacks.
- [ ] **#100.** Stop re-resolving container label at draw (use cached scan label).
- [ ] **#101.** Stop re-deriving "is container" at render (use cached category).

### Dead / unused

- [ ] **#102.** Remove no-op `PassesLootPickupFilters` (always true).
- [ ] **#103.** Remove unused `WorldCategoryUsesLootDistance`.
- [ ] **#104.** Remove or produce `WorldItemCategory::RaiderStock`.
- [ ] **#105.** Remove unused `LooksLikeKeyPickup` / `WorldLootScanRadiusMeters`.
- [ ] **#106.** Remove unused `LookupMapName` / `LookupEnglishItemDisplay`.
- [ ] **#107.** Remove unused `CollisionLos::Clear`.
- [ ] **#108.** Remove unused `WorldScanCommon::IsNearLocalPawn`.

### Partial / unwired

- [ ] **#109.** Wire or remove inert `var::world_distance`.
- [ ] **#110.** Add menu checkbox for `var::enable_world` (or remove the hidden gate).

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
