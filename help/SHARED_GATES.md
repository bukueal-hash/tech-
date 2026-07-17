# Shared admission / naming gates

Before editing any shared helper, **grep all callers**. Widening a gate for bots often breaks items/world (and vice versa).

| Shared choke point | Used by |
|---|---|
| `getEntityType()` | Bots, loot, world classification |
| `getAllowType()` | Bot/item/world admission |
| `IsAcceptedBotEspLabel()` | Bot admission + render validation |
| `HumanizeActorFName()`, `ResolveWorldLabel()`, `LookupByAssetName()` | Naming everywhere |
| `WorldScanCommon` helpers | All scanners |
| `PassesLootPickupFilters()` | Item + world draw filters |

**Durable bot fix:** use bot-only resolvers in `RobotList.cpp` / `AssetNames.cpp` — do **not** widen shared item/world naming gates.

See also `.cursor/rules/esp-fix-workflow.mdc`.
