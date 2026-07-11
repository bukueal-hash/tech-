# Project checklist

Track progress here. Check off items as each is verified in-game or merged.

**Last updated:** 2026-07-11

---

## Critical — game broken

- [ ] **1. Fix player admission in `EntityList.cpp`** — `playerCache=0`, `classSkip` blocking everyone. Remove/fix `EACTOR_PLAYER` gate; restore Arc Raiders admission (`APlayerState` + health + root + mesh@0x428).
- [ ] **2. Fix player health read** — PS+0x530 may be wrong; try `HealthComponent @ pawn+0xDC8` per qwe900 locked offsets.
- [ ] **3. Verify GameState pawn offset** — `PlayerState_PawnPrivate @ 0x418`; add `[debugPlayer]` skip counters (`gsSkipHealth`, `gsSkipMesh`, etc.).
- [ ] **4. Full 4-area test pass** — Players, Bots, Items, World + debug overlay after player fix.

---

## Aim — verify and finish

- [ ] **5. In-game bot aim test** — Hotkey near bot; `[debugAim] candidates>0`, `kmbox=1`, crosshair tracks via KmBox.
- [ ] **6. Player aim test** — Should work once player cache is fixed (same `playerCache` loop).
- [ ] **7. Resolve ghost aim config** — `sticky_target_lock`, `aimbot_priority`, `aim_bone_mode`, grace vars still in `.ini` but unused. **Decide:** delete from config **or** wire into `Aimbot.cpp`.

---

## Dead / unwired code — safe cleanup

- [ ] **8. Remove ESP snapshot dead path** — `PublishEspSnapshot()`, `EspRenderSnapshot`, `m_espSnapshots[]` (never called).
- [ ] **9. Remove or finish `EspFramePlayer`** — `CollectEspRenderFrame` never fills `frame.players`.
- [ ] **10. Delete `Core/DebugSessionLog.h`** — empty stub, zero includes.
- [ ] **11. Delete `#if 0` CL-1233465 block in `SteamDecrypt.hpp`** (~100 lines).
- [ ] **12. Remove dead ESP vars** — `esp_smoothing_mode`, `esp_disable_box_cache`, `esp_smooth_alpha`.
- [ ] **13. Remove dead `Engine.h` cruft** — `ActorTypeProbeState`, `GetActorTypeProbe()`, `PlayerPosition`, `CurrentGun`, `decode_in_place()`, unused `PlayerCacheEntry` fields (`shieldLevel`, `bIsABot`).
- [ ] **14. Remove dead VisCheck symbols** — `EvaluateTargetVisibility()`, `VisibleActor()` (if confirmed unused), `GetActorBoneMesh()`.
- [ ] **15. Remove `FiringProxyAvailable()`** in KmBox — never called.
- [ ] **16. Remove `IsButtonPressed()`** in Controller if only `IsButtonDown` is used.
- [ ] **17. Clean `Project.vcxproj.filters`** — ghost entries for missing `motorsynergy.h`, `helper.hpp`, `helper.cpp`.

---

## Partially wired — menu vs runtime

- [ ] **18. Wire or remove `color_dead_bots`** — menu + AutoConfig exist; bot render never uses it.
- [ ] **19. Wire or remove unwired aim settings** — sticky FOV bias, grace period, priority enum, bone mode (if not handled in #7).

---

## Redundant / duplicate — after ESP stable

- [ ] **20. Extract shared Item/Container scan preamble** — ~40 lines duplicated; grep shared-gate callers first.
- [ ] **21. Consolidate PC/PCM/camera resolution** — duplicated across `Utils.cpp`, `Update.cpp`, `Esp.cpp`.
- [ ] **22. Clarify world category gating** — overlap in `WorldCategoryEnabled`, `PassesLootPickupFilters`, `getAllowWorldEntry`.

---

## Repo / tree hygiene

- [ ] **23. Archive or delete `Arc Raiders/Project/`** — stale fork; not active build (keep as reference elsewhere if needed).
- [ ] **24. Stop tracking `Project/Build/` in git** — logs, `auto_config.ini`, duplicate `Data/`.
- [ ] **25. Exclude `imgui_demo.cpp` from Release build** — compiled but never used.

---

## Optional — when stable

- [ ] **26. GameState roster fallback** — only if level scan still misses players after #1–3.
- [ ] **27. Richer `[debugPlayer]` skip counters** — per-reason admission breakdown.
- [ ] **28. Docs update** — note KmBox-only aim, baseline cache path, current blockers in workflow doc.

---

## Verification notes

Use this block when checking off test items (#4–6):

```
Players: OK | broken | none — (detail)
Bots: OK | broken | none — (detail)
Items: OK | broken | none — (detail)
World: OK | broken | none — (detail)
Debug: on | off
  [debugPlayer] ...
  [debugRobot] ...
  [debugItem] ...
  [debugContainer] ...
  [debugAim] ...
```

| Item | Verified by | Date |
|------|-------------|------|
| | | |
