# Extraction Hatch Separation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove extraction hatches from the bot ESP list and route them into their own dedicated cache, drawn on the world ESP pass with their own color/toggle.

**Architecture:** Hatches (`SalvageExtractionPoint_Hatch`) are currently admitted to `robotCache` because the game's actor type system classifies them as `EACTOR_TARGET` (same as bots). The fix intercepts hatch actors during bot admission, stores them in a new `hatchCache`, and draws them through the existing world ESP path — eliminating the string-matching hack in the bot draw loop.

**Tech Stack:** C++17, DMA memory reads, ImGui overlay

---

## File Map

| File | Change |
|------|--------|
| `Project/Core/Engine.h:546` | Add `hatchCache` + `m_hatchCacheMutex` + `HatchCacheCount()` |
| `Project/Core/Engine.h:560-567` | Add `hatches` vector to `EspRenderFrame` |
| `Project/Functions/RobotList.cpp:1792` | Intercept hatch labels, route to `hatchCache` instead of `robotCache` |
| `Project/Functions/RobotList.cpp:1992+` | Exclude hatch keys from bot retention/eviction/prune loops |
| `Project/Functions/Esp.cpp:758-798` | Collect `hatchCache` into `out.world` frames |
| `Project/Functions/Esp.cpp:1376-1390` | Remove `isHatch` string-match hack from bot draw loop |
| `Project/Functions/Esp.cpp:1492` | (No change needed — hatches go through `RenderWorldEspFromFrame`) |
| `Project/Functions/Update.cpp:1072` | Clear `hatchCache` in `ClearEspCaches()` |
| `Project/Interface/Render.cpp:297-313` | Add hatch count to debug overlay |
| `Project/Interface/Overlay/MenuSidebar.cpp:399` | Update tooltip (cosmetic) |

---

## Task 1: Add hatchCache to Engine.h

**Files:**
- Modify: `Project/Core/Engine.h:104-106,440-451,546-548,560-567`

**Interfaces:**
- Produces: `hatchCache`, `m_hatchCacheMutex`, `HatchCacheCount()` — consumed by Tasks 2-5

- [ ] **Step 1: Add mutex after m_robotCacheMutex**

At `Engine.h:106`, after `mutable std::shared_mutex m_robotCacheMutex;`, add:

```cpp
mutable std::shared_mutex m_hatchCacheMutex;  // Protects hatchCache
```

- [ ] **Step 2: Add HatchCacheCount() after RobotCacheCount()**

At `Engine.h:448-451`, after `RobotCacheCount()`, add:

```cpp
size_t HatchCacheCount() const {
    std::shared_lock<std::shared_mutex> lock(m_hatchCacheMutex);
    return hatchCache.size();
}
```

- [ ] **Step 3: Add hatchCache declaration after robotCache**

At `Engine.h:548`, after `std::unordered_map<uintptr_t, WorldCacheEntry> robotCache;`, add:

```cpp
std::unordered_map<uintptr_t, WorldCacheEntry> hatchCache;
```

- [ ] **Step 4: Add hatches vector to EspRenderFrame**

At `Engine.h:564`, inside `EspRenderFrame`, after the `robots` vector, add:

```cpp
std::vector<EspFrameWorld> hatches;
```

- [ ] **Step 5: Build and verify no compile errors**

Run build. Expect clean compile — new fields are declared but unused so far.

---

## Task 2: Route hatches from RobotList admission to hatchCache

**Files:**
- Modify: `Project/Functions/RobotList.cpp:1792-1854`

**Interfaces:**
- Consumes: `hatchCache`, `m_hatchCacheMutex` from Task 1
- Produces: Hatch actors stored in `hatchCache` instead of `robotCache`

- [ ] **Step 1: Add hatch detection after label resolution**

At `RobotList.cpp:1792`, the admission block starts with `std::string itemName = ResolveStructBotAdmissionLabel(actor, fname);`. After the full label resolution chain (lines 1792-1826), before the `broken` check at line 1828, add hatch interception:

```cpp
// Hatch extraction points: route to hatchCache, not robotCache.
{
    std::string lower = itemName;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("hatch") != std::string::npos) {
        const uintptr_t root = ResolveBotSceneRoot(actor);
        if (!root) {
            continue;
        }
        const Vector3 worldPos = ReadSceneWorldPos(root);
        if (!IsPlausibleWorldPos(worldPos)) {
            continue;
        }
        auto& entry = hatchCache.emplace(
            actor,
            WorldCacheEntry(itemName, root, actor, 0)
        ).first->second;
        entry.WorldPos = worldPos;
        entry.ItemType = itemName;
        entry.ActorName = itemName;
        entry.category = 3;
        entry.Drawing = true;
        continue;
    }
}
```

- [ ] **Step 2: Build and verify**

Run build. Hatches should now appear in `hatchCache` and no longer in `robotCache`. Bot ESP should show fewer entries (hatches gone). World ESP won't show them yet (Task 3).

---

## Task 3: Collect hatchCache into ESP frame

**Files:**
- Modify: `Project/Functions/Esp.cpp:758-798`

**Interfaces:**
- Consumes: `hatchCache`, `m_hatchCacheMutex` from Task 1
- Produces: Hatch entries in `out.world` (drawn by existing `RenderWorldEspFromFrame`)

- [ ] **Step 1: Add hatchCache collection after containerCache**

At `Esp.cpp:796-798`, after the `appendWorld(containerCache)` block, add:

```cpp
{
    std::shared_lock<std::shared_mutex> lock(m_hatchCacheMutex);
    appendWorld(hatchCache);
}
```

- [ ] **Step 2: Build and verify**

Run build. Hatches should now appear on the world ESP pass with whatever color the world ESP path uses. They will use the container/world draw style (box + label).

---

## Task 4: Exclude hatches from bot retention/eviction loops

**Files:**
- Modify: `Project/Functions/RobotList.cpp:1992-2300+`

**Interfaces:**
- Consumes: `hatchCache` to check if a key belongs to a hatch
- Produces: Hatch keys skipped in bot retention logic

- [ ] **Step 1: Skip hatch keys in the bot retain loop**

At `RobotList.cpp:1992`, the retain loop starts with `for (auto it = localCache.begin(); ...)`. Early in the loop body (after `const uintptr_t key = it->first;` around line 1995), add:

```cpp
// Hatches live in hatchCache — never retain or evict from bot cache.
if (hatchCache.contains(key)) {
    it = localCache.erase(it);
    continue;
}
```

- [ ] **Step 2: Build and verify**

Run build. Hatches should no longer appear in bot retention/eviction logging. No visual change expected — they're already in `hatchCache` from Task 2 and never enter `robotCache`.

---

## Task 5: Remove isHatch hack from bot draw loop

**Files:**
- Modify: `Project/Functions/Esp.cpp:1376-1390`

**Interfaces:**
- Consumes: (nothing — removing code)
- Produces: Cleaner bot draw loop

- [ ] **Step 1: Remove hatch detection and special color from bot draw**

At `Esp.cpp:1376-1390`, delete the entire hatch detection block:

```cpp
// DELETE these lines:
// Hatch (extraction point) — separate from bots, own color
bool isHatch = false;
{
    std::string lower = botLabel;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    isHatch = lower.find("hatch") != std::string::npos;
}
if (isHatch && !var::showHatches)
    continue;

const ImU32 color = isHatch
    ? ImGui::ColorConvertFloat4ToU32(ImVec4(
        var::color_hatches[0], var::color_hatches[1],
        var::color_hatches[2], var::color_hatches[3]))
    : color_base;
```

Replace with just:

```cpp
const ImU32 color = color_base;
```

- [ ] **Step 2: Build and verify**

Run build. Bot ESP should work identically but without hatch entries. Hatches draw through world ESP (Task 3).

---

## Task 6: Wire hatchCache into ClearEspCaches and debug overlay

**Files:**
- Modify: `Project/Functions/Update.cpp:1072`
- Modify: `Project/Interface/Render.cpp:297-313`

**Interfaces:**
- Consumes: `hatchCache`, `m_hatchCacheMutex` from Task 1
- Produces: Cache cleared on raid transition, count shown in debug overlay

- [ ] **Step 1: Clear hatchCache in ClearEspCaches**

At `Update.cpp:1072`, after the `robotCache.clear()` block, add:

```cpp
{
    std::unique_lock<std::shared_mutex> lk(m_hatchCacheMutex);
    hatchCache.clear();
}
```

- [ ] **Step 2: Add hatch count to debug overlay**

At `Render.cpp:297-313`, after the robot cache count block, add a hatch count block:

```cpp
{
    const auto tA = std::chrono::steady_clock::now();
    std::shared_lock<std::shared_mutex> lock(eng.m_hatchCacheMutex, std::try_to_lock);
    if (lock.owns_lock()) {
        next.hatchCacheSz = eng.hatchCache.size();
    }
    (void)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tA).count();
}
```

Note: `hatchCacheSz` needs to be added to the `OverlaySnap` struct if it doesn't exist. Check `Render.cpp` for the snap struct definition and add the field.

- [ ] **Step 3: Build and verify**

Run build. Debug overlay should show hatch count. Cache clears on raid transition.

---

## Task 7: Wire showHatches toggle to hatchCache draw gate

**Files:**
- Modify: `Project/Functions/Esp.cpp:791-798`

**Interfaces:**
- Consumes: `var::showHatches` (existing)
- Produces: Hatches only drawn when toggle is on

- [ ] **Step 1: Gate hatchCache collection on showHatches**

At `Esp.cpp:791-798`, wrap the hatch collection:

```cpp
{
    std::shared_lock<std::shared_mutex> lock(m_hatchCacheMutex);
    if (var::showHatches)
        appendWorld(hatchCache);
}
```

- [ ] **Step 2: Build and verify**

Run build. Toggle `showHatches` on/off — hatches should appear/disappear from world ESP.

---

## Task 8: Update MenuSidebar tooltip

**Files:**
- Modify: `Project/Interface/Overlay/MenuSidebar.cpp:399`

**Interfaces:**
- Consumes: (none)
- Produces: Accurate tooltip text

- [ ] **Step 1: Update tooltip text**

At `MenuSidebar.cpp:399`, change:

```cpp
ArcMenuHoverTooltip("Extraction point hatches — not bots, separate toggle and color.");
```

To:

```cpp
ArcMenuHoverTooltip("Extraction point hatches — separate from bots, own cache and draw pass.");
```

- [ ] **Step 2: Build and verify**

Run build. Tooltip reads correctly.

---

## Verification Checklist

After all tasks:

1. **Bot ESP** — bots draw normally, no hatch entries in bot list
2. **World ESP** — hatches draw with world ESP style (box + label)
3. **showHatches toggle** — hatches appear/disappear when toggled
4. **color_hatches** — (optional) wire `var::color_hatches` into the world draw path for hatches if you want custom color; otherwise they use the default world color
5. **Debug overlay** — hatch count visible
6. **Raid transition** — `hatchCache` clears properly
7. **No flicker** — hatches are static, no PositionRefreshPass needed (same as containers)
