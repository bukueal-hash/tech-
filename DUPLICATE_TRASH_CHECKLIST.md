# Duplicate & trash cleanup checklist

Scan date: 2026-07-17. Updated after slow safe cleanup pass.

---

## P0 — Safe file trash (delete anytime)

- [x] `imgui.ini` (repo root)
- [x] `Build/imgui.ini`
- [x] `Build/Intermediate/**` (objs, tlogs, iobj/ipdb, CppClean logs)
- [x] `Build/build.log`
- [x] `Build/last_build.log`
- [x] `Build/msbuild_last.log`
- [x] `Build/ArcRaiders.exe.bak_*` (dated exe backups)
- [x] `Build/_link_probe.txt` (if present)
- [x] Root `debug-*.log` / `debug-flicker.log` when they reappear

**Keep:** `Build/ArcRaiders.exe`, `Build/auto_config.ini`, `Project/Data/**`

---

## P1 — Optional deploy mirrors (rebuild restores)

- [ ] Wipe `Build/Data/**` — **skipped** (not needed; post-build restores)
- [ ] Confirm `Project/x64` has DMA DLLs before deleting `Build/vmm.dll` / `Build/leechcore.dll` — **skipped** (never delete Build DMA DLLs)

---

## P2 — Docs / project metadata

- [x] Decide on `help/sdk.txt` — **kept** (reference archive)
- [x] Decide on `help/esp.txt` — **kept** (historical guide)
- [x] Create `help/README.md` + `help/SHARED_GATES.md`
- [x] Refresh stale `DUPLICATE_CODE_AUDIT.md` → pointer to this checklist
- [x] Clean ghost entries in `Project/Project.vcxproj.filters`

---

## P3 — NDJSON / debug I/O dupes (code)

- [x] Migrate all `std::ofstream` → `debug-c190fb.log` / `debug-5681af.log` to `ArcAgentLog` / `ArcAgentLog5681af`
- [x] Remove RobotList dual path (overlay `ArcAgentLog` **plus** NDJSON ofstream)
- [x] Aimbot: `WriteAimTrackNdjson` + select log → async only
- [x] Esp: frame-collect ofstream → `ArcAgentLog5681af`
- [x] Files touched: Aimbot, Esp, RobotList, EntityList, Update, Memory, ContainerList, PositionRefreshPass

---

## P4 — Exact / near helper dupes (code)

- [x] Unify `ToLowerCopy` → `AssetNames.h` / `AssetNames.cpp` (ItemList/ContainerList call it)
- [x] Unify miss-evict helpers → `WorldScan::MissCounterShouldEvict` (+ clear)
  - [x] Item / Container / Bot wrappers
- [x] `WeaponTierColor` (Esp) → `RarityTierColor`
- [x] Drop `ColorFromPicker` → `EspDraw::ColorFromRGBA`
- [x] Shared PCM resolve → `Engine::ResolvePlayerCameraManagerLadder`
- [x] Shared NOCACHE world-pos → `Engine::ReadWorldLocationNocache` (relative flag)
- [x] UTF-16 garbage: `Memory.cpp` → `Engine::LooksLikeUtf16Garbage`
- [x] Remove orphan `PovMatchesPawnView`

---

## P5 — Medium / optional consolidations

- [ ] Ground-loot category helpers — **skipped** (intentional; out of scope)
- [ ] Scanner scaffolding skeleton — **skipped**
- [ ] Weapon sanitize once at EntityList — **skipped**
- [ ] Shared `IsZeroVec3` — **skipped**

**Do not merge (intentional — leave alone)**

- [x] Esp vs Aim velocity lead — **kept separate**
- [x] Bot-only naming resolvers — **not folded into shared gates**
- [x] `WorldScan::BlendCachedVelocity` — left alone
- [x] `EspDraw::ColorFromRGBA` — left alone

---

## Suggested cleanup order

1. [x] P0 file trash
2. [x] P2 filters ghosts + docs
3. [x] P3 NDJSON ofstream migration
4. [x] P4 small helpers
5. [x] P4 miss-evict
6. [x] P4 PCM / NOCACHE shared reads
7. [x] P5 marked skipped

---

## Done criteria

- [x] No hot-path `ofstream` for debug NDJSON
- [x] No triple-copied `ToLowerCopy` / miss-evict
- [x] `PovMatchesPawnView` gone
- [ ] ESP 4-area report green after shared PCM/NOCACHE merges — **user verify**
