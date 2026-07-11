# Help folder index

Reference docs for offsets, decryption, ESP distance logic, and shared-code guardrails.

## Files

| File | Purpose |
|------|---------|
| [CL-1315578_REFERENCE.md](CL-1315578_REFERENCE.md) | FName pipeline, bone decrypt, player name decrypt, full offset dump (CL-1315578) |
| [SAVED_OFFSETS_LOCKED.txt](SAVED_OFFSETS_LOCKED.txt) | Locked offset snapshot mirrored in `Project/Core/Offsets.h` — do not edit unless game CL changes |
| [distance_system.md](distance_system.md) | Loot/container SP distance sliders and `WorldLootPickupMaxDrawMeters` |
| [SHARED_GATES.md](SHARED_GATES.md) | Shared classification/naming choke points — grep callers before edits |
| [UC_NOTES.md](UC_NOTES.md) | UnknownCheats forum notes (pages 184–185) and verification status |

## Data assets (runtime)

**Canonical source:** `Project/Data/` — copied to `Build/Data/` on every Release build.

Do **not** edit `Bots_Items_Maps/en.json` at the repo root; that copy is stale and unused.

## Offset edit rule

Red CORE pointer failures in the debug overlay are usually DMA module-base / DTB issues, not wrong offset numbers. Only update `Offsets.h` when the game build/CL changes and the user explicitly requests it.
