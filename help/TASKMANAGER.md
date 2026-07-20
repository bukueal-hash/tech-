# TaskManager lanes (DMA scheduling)

ESP work no longer uses 9 `SyncedThread`s + a scan mutex gate. Scheduling follows a MeatyEFT-style **3-lane TaskManager**.

## Lanes

| Lane | Thread | Tasks | Notes |
|---|---|---|---|
| **Hot** | `m_hotWorker` | `UpdateCamera`, `PositionRefresh`, `FrameBuilder`, `AimAssistence` | Never heavy. Camera-first. Aim stays ~4 ms. |
| **Main** | `m_mainWorker` | `Update`, `EntityList`, `RobotList` | `RobotList` is **heavy** (one-heavy-per-tick on this lane). |
| **Features** | `m_featuresWorker` | `ContainerList`, `ItemList`, `VisRebuild` | Container/Item are **cold + heavy**; Vis is mild/heavy. |

## Intervals (defaults in `Project/Core/TaskIntervals.h`)

| Task | ms | Tier | Heavy? |
|---|---|---|---|
| AimAssistence | 4 | hot | no |
| UpdateCamera | 8 | hot | no |
| FrameBuilder | 12 | hot | no |
| PositionRefresh | 16 | hot | no |
| Update | 18 | hot/mild | no |
| EntityList | 22 | hot/mild | no |
| RobotList | 80 | mild | **yes** |
| VisRebuild | 500 | mild | yes |
| ContainerList | 3000 | cold | **yes** |
| ItemList | 5000 | cold | **yes** |

Rules: ~1 ms tick, delta clamp 100 ms, skip catch-up, one heavy due task per lane tick, phase offsets on features.

## Memory traffic stats

`PCIMemory` keeps Meaty-style counters (ops, bytes, scatter batches) without replacing CR3/NOCACHE/`FullRefresh`.

```cpp
auto s = PCIMemory::GetTrafficStats();
// or PCIMemory::GetTrafficStatsString()
auto c = PCIMemory::GetConnectionStats(); // DmaConnectionState + PID/base
```

## Files

- `Project/Core/TaskManager.h` / `.cpp`
- `Project/Core/TaskIntervals.h`
- `Project/Functions/EngineThreads.cpp` — lane workers
- `Project/DMA/Memory.h` / `.cpp` — stats + `TryRead` / `ReadChain` / connection state
