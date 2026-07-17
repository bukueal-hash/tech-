# Shared ESP gates

Before editing any of these, **grep all callers** and consider impact on Players, Bots, Items, and World ESP.

| Shared choke point | Used by |
|---|---|
| `getEntityType()` | Bots, loot, world classification |
| `getAllowType()` / `getAllowWorldEntry()` | Bot/item/world admission |
| `IsAcceptedBotEspLabel()` | Bot admission + render validation |
| `HumanizeActorFName()`, `ResolveWorldLabel()`, `LookupByAssetName()` | Naming everywhere |
| `WorldScanCommon` helpers (`BlendCachedVelocity`, miss-evict, etc.) | All scanners |

**Durable bot fix:** use bot-only resolvers in `RobotList.cpp` / `AssetNames.cpp` — do **not** widen shared item/world naming gates for bot fixes.

See also: `.cursor/rules/esp-fix-workflow.mdc` and `DUPLICATE_TRASH_CHECKLIST.md`.
