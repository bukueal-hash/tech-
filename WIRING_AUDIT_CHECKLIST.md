# Wiring audit checklist

Scan date: 2026-07-17. Whole-project scan for not-wired / partially-wired code.
Core pipeline (ESP, aim, radar, 7 worker threads, DMA keyboard, MAKCU/Net mouse) is fully wired — items below are gaps, dead code, or cosmetics.

Prefer **one change -> `.\rebuild-run.ps1` -> 4-area report** for anything touching ESP/aim.

## Progress

| Tier | Done | Left |
|------|------|------|
| **P0** | 6 | 0 |
| **P1** | 23 | 0 |
| **P2** | 2 | 2 |
| **Confirmed fine** | 6 | — |

**Next up:** P2 #3 deleted + #4 fixed (await keep/revert). #1–#2 skipped.

---

## P0 — Misleading / partially wired (user-visible; fix first)

- [x] `var::world_distance` — **DELETED** (var + save/snapshot; old ini key ignored on load). Real caps remain `loot_distance` / `container_distance_sp`.
- [x] KmBox `"BPro"` type — **DELETED** from load paths (maps legacy BPro / type=0 → MAKCU). Menu already only listed MAKCU/Net. Unsupported type now remaps to MAKCU on init.
- [x] `showLoot` overrides per-category world toggles — **WIRED**: `showLoot` is master draw gate; each category flag filters while on. Menu container rows disabled when Show loot off; tooltip updated.
  - `WorldItemCategory.cpp` `WorldCategoryEnabled`; `MenuSidebar.cpp` Loot tab
- [x] `visiblecheck` — **REMOVED** (broken; menu + aim gate + AutoConfig save gone; actors always `isVisible=true`). Legacy ini key ignored on load.
- [x] Aim tooltip "Obstruction LOS (ESP tab)" — **GONE** with vis-check menu removal.
- [x] `TryCaptureDebugOverlaySnap` / `DebugOverlaySnap` — **DELETED** (unused; live overlay stays in `Render.cpp`).
- [x] **`VisCheck.cpp` removed entirely** — mesh render-time vis (`Visible`/`VisibleActor`/`VisibleBotActor`/`VisCheckDebugStats`/`PrintVisCheckDebugConsole`) gone. Mesh lookup helpers moved to `BoneList.cpp`. Fresh vis-check later when we have a working one.

---

## P1 — Dead code: Engine.h (never called / write-only)

- [x] `RenderPlayerEspFromCache` — **DELETED** (superseded by `RenderPlayerEspFromFrame`)
- [x] Ground-pickup HUD trio — **WIRED**: menu **Near loot HUD** + on-screen list via `CollectDrawingGroundPickups`; **F7** calls `UserConfirmGroundItemPicked` on nearest.
- [x] `GetCurrentWeaponActor`, `GetWeaponQuality` — **DELETED** (only `GetWeaponQualityFromActor` remains; live weapon path)
- [x] Write-only fields: `PlayerPosition`, `LocalPlayerTeam`, `CurrentGun` — **DELETED** (never read/written)
- [x] Write-only `PlayerCacheEntry` fields: `shieldLevel`, `bIsABot`, `lastWeaponPtr`, `bIsDeathVerge` — **DELETED** (+ dead `bIsDeathVerge=false` write in EntityList)
- [x] `WorldCacheEntry::ItemRarity` + `EItemRarity` enum — **DELETED** (live loot uses `lootRarityTier`)
- [x] `PlayerHealthInfo` struct — **DELETED** (never instantiated)
- [x] `itemImColors` map — **DELETED** (never indexed; live colors use `var::color_*`)
- [x] Crypto/rotate leftovers — **DELETED** (`RadToDeg`, `ROL4`/`ROL8`, `rotl32`/`rotl64`, `u64_lo`, `to_matrix`, `rol32`). Kept live: `DegToRad`, `toLower`, `MatrixMultiplication`.
- [x] `Has` wrapper — **DELETED** (never called)
- [x] `GetBoneArrayDecrypt` wrapper — **DELETED** (callers use `steam_decrypt::` directly)
- [x] Bot HP speculative readers + NDJSON agent log I/O — **DELETED** (`RefreshBotHealth`/ASC/HealthGroup probes; `AgentLog.h` stubbed no-op)
- [x] `ActorTypeProbeState` / `m_actorTypeProbe` / `GetActorTypeProbe` — **DELETED**
- [x] `CountWorldDrawable` / `CountContainerDrawable` / `CountItemDrawable` / `CountRobotDrawable` — **DELETED** (kept `CountEspDrawablePlayers` for Debug tab)
- [x] `FNameCache::{TryGet,Add,Contains,Size}` — **DELETED** (kept `Clear()` / `Instance()`)

---

## P1 — Dead code: Visuals / RenderQueue

- [x] `Visuals::{Headline, SnapLinesDouble, BoxScreenRect, ComputeEspScaleFromLootMarker, TextScaleFromDistance, HealthColorFromPct}` — **DELETED**
- [x] `RenderQueue::{addRect, addRectFilled, addCircle, addCircleFilled, addText}` — **DELETED** (kept `addLine` + frame plumbing; command struct slimmed to lines)

---

## P1 — Dead code: Input / KmBox

- [x] Extra `KeyBind*` helpers — **DELETED** (kept `KeyBindIsHeld`)
- [x] `InputBindIsEitherDown` / `NormalizeBindCode` / `CodeToControllerMask` — **DELETED**
- [x] `Controller::IsButtonPressed` (+ unused `UpdatePressedState`) — **DELETED** (kept `IsButtonDown`)
- [x] `DmaGamepad::ReadState` (+ unused `XINPUT_STATE`) — **DELETED** (kept `ReadRaw`)
- [x] `MoveSmooth` / `MoveBezier` (KmBox + MyMakcu wrappers) — **DELETED** (kept `MoveAim`; vendor lib untouched)
- [x] `GetRememberConfig` — **DELETED** (kept `SetRememberConfig`)
- [x] `ImGui::Keybind` mode/enablemode UI — **DELETED** (signature is label+key only)

---

## P2 — Low priority / leave unless slimming

- [ ] KmBoxNet vendor surface unused except `init` / `mouse_move*` / `mouse_left` / `monitor` / `mask_mouse_left` / `unmask_all` / `lcd_color` / `reboot` — **SKIPPED** (vendor completeness)
- [ ] `PCIMemory::{InitializeVmmOnly, SetVmmHandle, Write, WriteEx}` / `Memory::write` — **SKIPPED** (keep write surface)
- [x] AutoConfig cosmetics — **DELETED** (`configVersion` write; legacy load aliases: `hideOpenedLoot`, `container_distance_long`, `container_range_long_*`, `ui_text_scale`, `Prediction`, `visiblecheck` ignore, `world_distance` ignore, bare KmBox keys / numeric `type`)
- [x] `silhouette_max_distance_m` — **FIXED** comment to match helper + menu (`0` → 25 m, not `esp_distance`)

---

## Confirmed fine (no action)

- [x] 7 worker threads all started (`Project.cpp` L68) and consumed
- [x] Radar end-to-end (`show_radar` -> scanners -> `RenderRadar`)
- [x] DMA keyboard + gamepad aim-key bind path
- [x] MAKCU + Net mouse aim move path; `EnsureReady` MAKCU->Net fallback
- [x] AutoConfig snapshot symmetric with current `var::` set
- [x] `radar_pos_*` drag-only (intentional, no slider)

---

## Suggested order

1. [x] P0 misleading items — **6/6 done**
2. [x] P1 dead-code sweep (Engine.h -> Visuals/RenderQueue -> Input/KmBox) — **done**
3. [x] P2 optional slim — #1–#2 skipped; #3 deleted; #4 comment fixed
4. [ ] 4-area ESP report after any change touching ESP/aim/scanners

