# Loot / Item / Container Distance System

## Two Sliders Only

| Slider | Variable | Range | Purpose |
|---|---|---|---|
| **"Loot distance"** | `var::loot_distance` | 20–500m | Default draw range when SP is OFF on a category row |
| **"SP"** | `var::container_distance_sp` | 20–500m | Extended draw range when SP is ON on a category row |

`world_distance` exists as a variable but is **dead** — never read by any distance check.

## Per-Category SP Checkbox

In the **Container types** section of the loot tab, every category row has an SP checkbox:

```
Type       Color    SP
[☐] Crate  [■]   [☑]   ← SP checked → uses container_distance_sp
[☐] Safe   [■]   [☐]   ← SP unchecked → uses loot_distance
```

**Tooltip:** *"Checked = SP distance for this container type. Unchecked = loot distance."*

Stored per-category via `SetContainerRangeSp()` → `g_containerRangeSp[cat]`. Persisted as `container_range_sp_<suffix>=1` in config.

## Two "Qualifying Pickup" SP Checkboxes

In the **Loot** section above the category list:

1. **"Min loot value"** slider (`loot_min_value`) + SP checkbox (`loot_min_val_sp`)
2. **"Min rarity"** combo (`loot_min_rarity`) + SP checkbox (`loot_min_rar_sp`)

These are independent of the per-category SP — they control whether a pickup meeting the threshold gets promoted to SP distance or stays on loot distance.

## Core Distance Function

`WorldLootPickupMaxDrawMeters(cat, &loot)` at `WorldItemCategory.cpp:1047`:

```
1. base = CategoryRowUsesSp(cat) → container_distance_sp : loot_distance

2. If CategoryRowUsesSp(cat):
       return container_distance_sp   ← SP rows are unconditional

3. If entry LooksLikeContainer:
       return base                    ← containers never get promotion

4. If !LooksLikePickup:
       return base                    ← non-pickups stay at base

5. Qualifying check:
       meetsValue  = loot_min_value > 0  && lootValue >= minValue
       meetsRarity = minTier > 0 && rarityTier >= minTier

       if neither:  return base

6. Promotion decision:
       if meetsValue  && loot_min_val_sp  → container_distance_sp
       if meetsRarity && loot_min_rar_sp  → container_distance_sp
       else:                              → loot_distance
```

## Value on Labels — Containers vs Pickups

- **Containers** always get `lootValue = 0` (set in `ContainerList.cpp:500-501`)
- **Dropped pickups** may have `lootValue > 0` from their data asset
- Draw code at `Esp.cpp:1447`:
  ```
  if (show_loot_value && lootValue > 0 && isPickup && !isContainerEsp && !looksLikeContainer)
      → "Label [value] [distm]"
  else
      → "Label [distm]"
  ```
- Containers never show a value because `lootValue = 0` and `isPickup = false`

## Three Distance Checks Per Entry

| Stage | Where | What happens |
|---|---|---|
| **1. Cache finalize** | `World.cpp:106-117` | `WorldLootPickupMaxDrawMeters()` → sets `entry.Drawing = false` if too far |
| **2. Frame assembly** | `Esp.cpp:581-588` | Only copies entries with `Drawing == true` |
| **3. Draw time (x2!)** | `Esp.cpp:1277 + 1413` | Distance recomputed from fresh camera. `WorldLootPickupMaxDrawMeters()` called **twice** — once before label resolution, once after (with final value/tier) |

## Radar Override

In `World.cpp:107-111`:
```cpp
if (radarVisible)
    maxDrawM = max(maxDrawM, radar_range);
```
Radar ensures even entries beyond their normal draw distance appear on the radar if `radar_range` is larger.

## Config Persistence

Saved in `AutoConfig.cpp:975-993`:
```
loot_distance=500
container_distance_sp=200
container_range_sp_ammo=0
container_range_sp_crate=1
...
loot_min_val_sp=1
loot_min_rar_sp=0
show_loot_value=1
```
