# ARCs — Full Project Audit Checklist

Five-pass sweep (dupes/trash, wiring, structure/risks).  
**Rule:** one change → kill → build → run → 4-area report. Do not stack fixes.

---

## P0 — Fix first (correctness / between-raid / ESP thrash)

### Raid & cache lifecycle
- [ ] Bump `m_worldGeneration` on **every** `ClearEspCaches()` path (leave + `HandleWorldLost`, not only enter/transition)
- [ ] Decide `IsEspDrawReady`: **wire** into `RenderEsp` / aim / overlay **or delete** `TickEspCacheReadiness` + flag
- [ ] Soften between-raid re-arm: avoid full disarm on transient `PersistentLevel` flicker when still raid-like
- [ ] Don't `ClearEspCaches()` again on raid arm if caches already empty (or document why double-clear is needed)
- [ ] Surface raid phase on debug overlay: `party_wait`, `enter_wait`, `EspRaid`, `EspDrawReady`, `PlayerArray` count
- [ ] Fix `PawnHasRaidCombatComponents`: use pawn PS **0x3C0** (not `APlayerState` 0x3A8) for enter-path raid probe
- [ ] Soften `TickEspCacheReadiness` hard gate (`players && bots && world`) — solo / empty pocket shouldn't wait 12s soft path only

### Offsets (PioneerPS collisions — comment/name first, value flips one at a time)
- [ ] Reconcile `Offsets.h` vs `EntityList.cpp`: `0x530` is **not** HealthInfo (`bIsInEncounter`)
- [ ] Fix naming collision: `PlayerStatus` and `MaxArmor` both at **0x548**
- [ ] Unify `ReplicatedMovement`: `Offsets.h` **0x148** vs EntityList **0x150**
- [ ] Unify `PawnPrivate`: `Offsets.h` **0x410** vs EntityList **0x418**
- [ ] Mark `HELP_FILE_PROJECT_COMPARISON.md` HealthInfo@0x530 section as **wrong** (do not "align" to it)

### Shared gates (never widen for bot fixes)
- [ ] Audit callers before any edit to `getEntityType`, `getAllowType`, `IsAcceptedBotEspLabel`, `FnameAdmitsWorldActor`
- [ ] Keep bot naming in `RobotList.cpp` / `AssetNames.cpp` bot-only resolvers only

### DMA / scan load
- [ ] One shared level-actor snapshot per `m_worldGeneration` (stop Robot + Container + Item each re-walking all levels)

---

## P1 — Structure / redundancy / dead wiring

### Duplicate scanners & passes
- [ ] Factor shared skeleton: ContainerList ↔ ItemList (gather → prune → occupy → scan → dedupe → retain → finalize)
- [ ] Merge Container + Item into one world pass with two admit paths (optional, after snapshot)
- [ ] Unify container/bot identity helpers (`LooksLikeContainerActor`, `LooksLikeBotPawn`, `Quick*Candidate` — one source each)
- [ ] Stop double world label resolve at admit **and** draw — trust cached `ItemDisplayName` / meta unless empty

### Bot naming stack
- [ ] Delete unused `ResolveRobotTypeForActor` (`AssetNames.cpp`)
- [ ] Delete unused `ResolveBotLabelFromFName` (`RobotList.cpp`)
- [ ] Collapse bot naming layers: admission + draw + `getEntityType` overlap

### Position / validity helpers
- [ ] Unify `IsPlausibleWorldPos`, `IsOldStyleInvalidXY`, `IsZeroWorldPos`, `IsNearZero` → one shared API

### Memory / scatter
- [ ] Document DMA tiers: cached singles vs scatter NOCACHE vs `FullRefresh` (don't NOCACHE everything)
- [ ] Standardize scatter: `PersistentScatter` vs `ScatterSession` — one pattern per thread

### Unwired / partial features
- [ ] `RadarPos`: use in `RenderRadar` **or** stop writing in `EntityList`
- [ ] `DbgStoreCameraProbe`: store + show on overlay **or** remove call
- [ ] `m_actorTypeProbe` / `GetActorTypeProbe`: wire or delete
- [ ] `showRobots` vs `robotAimEnabled`: document that draw requires `showRobots` (aim-only bots invisible)
- [ ] Fix `silhouette` comment vs default (comment says esp_distance; default is 25m)

### Item pickup / vanish
- [ ] Harden `WorldLootCacheEntryDepleted` — strong signals only (already partially fixed)
- [ ] Add `denyReAdmit` map + debug counter if blink returns
- [ ] Optional render safety net: `skipPickedUp` in world draw + `[debugWorldEsp]`

### AutoConfig drift
- [ ] Fix one-way keys: `hideOpenedLoot`, `container_distance_long`, `container_range_long_*`
- [ ] Table-drive AutoConfig (kill Snapshot ×4 duplication) or codegen
- [ ] Audit `index.cpp` defaults vs `Build/auto_config.ini` drift

### Debug overlay / docs
- [ ] Align `.cursor/rules/esp-fix-workflow.mdc` debug fields to **actual** runtime tags
- [ ] Restore or rewrite `help/README.md` + `help/SHARED_GATES.md` (currently missing)
- [ ] Add overlay row: `EspDrawReady`
- [ ] Remove doc references to: `denyReAdmit`, `ttlEvict`, `skipPickedUp`, `posFail`, `nameFail`, `weaponHit`, `frameOk`, `skipDraw` (unless re-implemented)

### Health / armor display
- [ ] Verify HC-first health path end-to-end (living + dead + DBNO)
- [ ] Validate `get_maxarmor` `Shield + 0x8` offset
- [ ] Wire or remove `bIsDeathVerge` (always false)

---

## P2 — Hygiene / cleanup / optional upgrades

### Trash & orphans (safe deletes after confirm)
- [ ] Delete or archive `Arc Raiders/` stale tree (outside repo or zip once)
- [ ] Delete nested `Project/Build/` (live out dir is repo `Build/`)
- [ ] Delete orphan `CollisionLos.cpp` / `CollisionLos.h` (not in vcxproj; references dead `vischeck_auto_thin`)
- [ ] Delete `DebugSessionLog.h` (no callers)
- [ ] Delete dead `RenderPlayerEspFromCache` (`Esp.cpp` / `Engine.h`)
- [ ] Shrink or delete `KeyBind.*` wrappers — aim uses `InputBind` directly
- [ ] Remove unused `InputBindIsEitherDown`, `InputBindNormalizeBindCode`
- [ ] Remove or wire `var::world_distance` (debug print only; menu uses `loot_distance` / SP)

### Dead Engine API surface
- [ ] `Engine::Has()` — delete or use
- [ ] `GetBoneArrayDecrypt()` wrapper — delete (BoneList calls steam_decrypt directly)
- [ ] `itemImColors` map — delete or use for world colors
- [ ] `PlayerHealthInfo` struct — delete
- [ ] `bIsABot`, `shieldLevel` fields — delete or wire
- [ ] Trim unused crypto helpers: `ROL4`, `ROL8`, `rotl*`, `u64_lo`, `RadToDeg`

### Offsets.h bloat
- [ ] Mark header-only / future offsets with `// unused` or prune never-read entries (PhysX, Lighting, unused GameState fields, etc.)

### God-file splits (mechanical, no behavior change — do last)
- [ ] Split `AssetNames.cpp` (~2665 lines)
- [ ] Split `WorldItemCategory.cpp` (~2627 lines)
- [ ] Split `Esp.cpp` (~1902 lines) — player collect/render **last**
- [ ] Split `Utils.cpp` (~1627 lines) — grep all callers every time
- [ ] Move fat inline / color maps / health helpers out of `Engine.h`

### Misc format
- [ ] Normalize include paths (`Core\Engine.h` vs `../Core/Engine.h`)
- [ ] Fix `AssetNames.cpp` including `../Core/Engine.h` from inside `Core/`
- [ ] `Cache.hpp`: replace `d3d9.h` `D3DMATRIX` with plain matrix or D3D11 types
- [ ] Inline `CollectLevelActors` one-liner or rename
- [ ] Dedupe API aliases: `EntityListReady` ≡ `IsEntityStarted`, `IsInRaid` ≡ `IsEspRaidActive`
- [ ] Add `GetActorFNameOrDecrypt(actor)` helper (stop copy-paste cached→live fallback)
- [ ] Data-drive repeated keyword/token lists in `WorldItemCategory` / `AssetNames`
- [ ] Split `WorldCacheEntry` → `RobotCacheEntry` vs `WorldLootEntry` (when safe)
- [ ] Unify SteamDecrypt RVAs with `Offsets.h` (single `Rvas.h` or include)

### Config / version
- [ ] `configVersion=1` in AutoConfig — version-gate migrations or stop writing

---

## Frozen — do not edit unless fixing that area

| Area | Files |
|------|--------|
| **Players** | `EntityList.cpp`, player collect/render in `Esp.cpp` |
| **Shared gates** | `Utils.cpp` `getAllowType` / `getEntityType` — grep callers first |
| **Blind offset "fixes"** | Don't align `0x530/0x538/0x548` to stale comparison doc |

---

## Suggested fix order (one cycle each)

1. [ ] Docs-only: mark stale HealthInfo + missing help links
2. [ ] Generation bump on leave/world_lost
3. [ ] `IsEspDrawReady` wire-or-delete + overlay row
4. [ ] Raid enter probe PS offset (`PawnHasRaidCombatComponents`)
5. [ ] Between-raid re-arm soften (party/enter waits, transition flicker)
6. [ ] Delete orphans: `Arc Raiders/`, `Project/Build/`, `CollisionLos`, dead APIs
7. [ ] Shared actor snapshot (DMA load)
8. [ ] Offset comment/name reconciliation (no value flips in same build)
9. [ ] Bot-only naming cleanup (no shared gate edits)
10. [ ] Item/container deplete hardening + debug alignment
11. [ ] God-file splits (Esp player path last)

---

## Test every cycle (all four)

```
Players: OK | broken — (detail)
Bots: OK | broken — (detail)
Items: OK | broken | none — (detail)
World: OK | broken | none — (detail)
Debug: on | off
 [debugPlayer] ...
 [debugRobot] ...
 [debugItem] ...
 [debugContainer] ...
```

### Between-raid specific checks
- [ ] Hub: ESP off, caches empty, overlay shows `EspRaid=NO`
- [ ] Party (3+): console shows `[raid] party_wait` then `[raid] enter_wait` then `[raid] entered`
- [ ] In raid: all four ESP areas populate
- [ ] Leave raid: `[raid] left`, caches clear, no ghosts into next raid
- [ ] Re-enter raid without exe restart: ESP recovers

---

*Generated from audit passes: dupes/trash, wiring, structure/risks — Jul 2026.*
