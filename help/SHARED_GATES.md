# Shared ESP gates

Functions in this table affect **multiple scanners** (bots, items, world, players). Before editing any of them, **grep all callers** and note impact on all four ESP areas.

| Function | File | Touches |
|----------|------|---------|
| `getEntityType()` | `Project/Functions/Utils.cpp` | Bot, item, and world classification buckets |
| `getAllowType()` | `Project/Functions/Utils.cpp` | Bot admission and menu category filters |
| `IsAcceptedBotEspLabel()` | `Project/Core/AssetNames.cpp` | Bot admission + render validation |
| `LookupByAssetName()` | `Project/Core/AssetNames.cpp` | All naming / display lookups |
| `HumanizeActorFName()` | `Project/Core/AssetNames.cpp` | All display labels when tables miss |
| `LooksLikeBotPawn()` | `Project/Functions/WorldScanCommon.cpp` | Cross-scanner pawn exclusion |
| `LooksLikeContainerActor()` | `Project/Functions/WorldScanCommon.cpp` | Item vs container classification |

## Rules

1. **Never widen shared gates to fix bots only** — use bot-only paths (`RobotList.cpp`, `ResolveRobotTypeFromFName`, `LookupBotClassToken`, `ResolveBotTypeLabel`).
2. Each function has a `// SHARED GATE — grep callers before edit` comment at its definition.
3. After any shared-gate change, test **all four** ESP areas (players, bots, items, world) with debug overlay on.

## Bot-specific notes (post audit hardening)

### `getAllowType(..., category == 3)`

- **Only caller:** `RobotList.cpp` (bot cache retention).
- **Allow list:** `kBotStructAdmissionToken` (`"Constructable"`) or `IsAcceptedBotEspLabel` — not any non-empty string.

### `IsAcceptedBotEspLabel`

- Accepts: `robotsList`, `LookupEnemyBotByFName`, `LookupEnemyBotDisplayLabel`, `getEntityType(fnameHint)` when result is in `robotsList`, `NormalizeBotDisplayName` aliases.
- **Removed:** blanket `"ARC "` prefix accept (too broad).

### `LooksLikeBotPawn`

- Excludes actors from item/container scans when they show **bot-positive** signals:
  - `IsAnyBotActor()` (includes `EACTOR_TARGET` with flag @ 0x1D0)
  - `HasArcEnemyAssetPointer()`
  - cached class/fname resolves via `ResolveRobotTypeFromFName` / `LookupEnemyBotByFName`
- **Removed:** loose mesh + root-only heuristic.

### JSON bot token registration

- `RegisterBotClassToken` in `AssetNames.cpp` must never register `NormalizeBotDisplayName` aliases (`Heavy`, `Elite`, `Drone`, bare `Husk`, etc.) — causes scanner floods.

See also: `.cursor/rules/esp-fix-workflow.mdc`

## Container-specific notes (post A+ hardening)

### `QuickContainerCandidate` (container-only)

- **File:** `Project/Functions/ContainerList.cpp` — **not** a shared gate.
- Cheap pre-filter before full admission: chest class id, loot-interaction component pointers, cached fname/class container tokens, raider cache / cargoship fname tokens.
- Does **not** call `LooksLikeContainerActor()` (shared + expensive).

### `LooksLikeContainerActor`

- Still shared — used after pre-filter in container admission when the `looksLikeContainer` shortcut does not apply.

### Opened-container render

- `ProbeContainerOpenSignals()` / `ContainerLootLooksOpened()` in `WorldItemCategory.cpp` — container-only open detection (salvage mesh, item container open time, loot searched flag).
- `ResolveContainerEspDrawLabel()` in `Esp.cpp` appends `" (Open)"` when opened and **Show open container** is enabled; gray color path unchanged.
- `FinalizeWorldCacheMap()` in `World.cpp` still promotes category to `OpenedContainer` and caches `"… (Open)"` in `ItemDisplayName`.

## Item / loot-specific notes (post A+ hardening)

### `ProbeGroundLootPickupSignals` (item-only)

- **File:** `Project/Core/WorldItemCategory.cpp` — **not** a shared gate.
- Strong depletion signals: hidden/destroyed, loot searched, item DA gone, collision off, spawn-items empty (with lingering DA).
- `StillHasAssetId` is diagnostic only — must **not** keep loot alive alone.

### `GroundLootLooksPickedUp`

- Requires pickup-like actor; returns true on any strong probe signal (no asset-ID-only keep-alive).

### Salvage display strip

- `FormatEspDisplayLabel()` strips leading `"Salvage "` from display strings (display-only).
- `StripEspFnamePrefixes()` also strips `item_salvage_`, `salvage_`, `da_item_salvage_` before humanize.
- Generic socket/salvage container fallback label is `"Container"` (not `"Salvage Container"`).

### Item cache retention (`ItemList.cpp`)

- **5s TTL:** `s_pickupEvictAfter` force-evicts after strong pickup signal.
- **5s denylist:** `s_depletedDeny` blocks re-admission blink during actor teardown.
- Item thread @ **50ms** via `m_worldEspThread`.

## Player-specific notes (post A+ hardening)

### Live root / mesh refresh (player-only)

- **File:** `Project/Functions/EntityList.cpp` — **not** a shared gate.
- Every retention tick re-reads `RootComponent` and skeletal mesh from the live pawn before scatter position reads.
- Invalid root after refresh → evict (no spawn ghost).
- `CollectEspRenderFrame` (`Esp.cpp`) and `PositionRefreshPass` scatter from live pawn root, falling back to cache only when the live read fails.

### Remote weapon resolution

- Equipped weapon for **other players** comes from world `BP_WeaponActor_*` / `BP_Weapon_*` actors whose Owner/Instigator points at the pawn — not local inventory replication.
- Stowed weapon actors are excluded. Display uses `GetWeaponName` → `LookupByAssetName` / `HumanizeActorFName` fallback.
- Requires **Show weapon** enabled in Player ESP menu.

### Name decrypt path

- `ResolvePlayerDisplayName()` in `SteamDecrypt.hpp` — pawn @ `0x438`, PlayerState @ `0x440` (+ probe offsets).
- Metadata pass retries every 500ms; display fallback `"Raider"` when decrypt fails (player render only, not shared bot gates).

### Spawn ghost eviction

- Stale `WorldPos` unchanged >2s while pawn still in GameState → evict.
- Same `PlayerState` as local ack pawn → evict duplicate self-ghost.
