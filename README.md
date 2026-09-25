# Arc Raiders Overlay

External, DMA-based overlay for ARC Raiders (Windows x64). It reads the game through
DMA hardware instead of touching the target process, then draws ESP, loot and world
markers in an ImGui DirectX overlay.

Features: player ESP (custom segmented health bar + hex armor bar, names, distance,
squad index), nearby-bot ESP, dropped/world item and container ESP with crate contents,
radar, loadout and activity-feed panels, and aim-assist helpers.

## Run it (no build required)

The `Build/` folder is committed as a ready-to-run build, so a fresh clone does not
need a compiler:

1. Clone or download this repository.
2. Make sure your DMA hardware is connected and its driver is installed.
3. Run `Build/ArcRaiders.exe`.

Requirements: 64-bit Windows, and the bundled runtime DLLs (LeechCore / MemProcFS
`vmm.dll`, `leechcore*.dll`, `FTD3XX*.dll`, plus `vcruntime140.dll`) which live next to
the exe in `Build/`. The overlay's data files (item tables, localisations) are in
`Build/Data/`.

Settings are autosaved to `Build/auto_config.ini`, and the overlay window layout to
`Build/imgui.ini`. Deleting either just restores defaults on the next launch.

## Build from source

Requirements: Visual Studio with the C++ toolchain (v143+ / C++20), Windows x64.

```powershell
# build Release|x64
scripts/build.ps1

# build, then launch
scripts/build.ps1 -Run

# build and run the unit test suite
scripts/build.ps1 -Test

# full rebuild
scripts/build.ps1 -FullRebuild
```

Or drive MSBuild directly:

```powershell
MSBuild.exe Project/Project.vcxproj -t:Rebuild -m -p:Configuration=Release -p:Platform=x64
MSBuild.exe Project/Project.Tests.vcxproj -t:Rebuild -m -p:Configuration=Release -p:Platform=x64
./Build/Tests/ArcRaiders.Tests.exe
```

Output lands in `Build/` (`Build/ArcRaiders.exe`) and `Build/Tests/`. If the build fails
with `LNK1104: cannot open file 'Build\ArcRaiders.exe'`, an old instance is still
running — close it before rebuilding.

## Layout

| Path | Contents |
| --- | --- |
| `Project/Core` | Platform-independent logic and policy headers (math, offsets, caches, diagnostics), heavily unit-tested |
| `Project/Functions` | Per-frame runtime passes: entity list, world scan, container list, ESP collection, aim |
| `Project/Interface` | Overlay UI, ImGui/DirectX rendering, menu, config persistence |
| `Project/DMA` | DMA read path and caching |
| `Project/Hardware` | Mouse/input hardware bridges (kmbox) |
| `Project/Tests` | doctest unit tests |
| `Project/x64`, `Project/lib` | Runtime DLLs and symbol data copied into `Build/` |

## Diagnostics

Debug output is opt-in through the config (`show_debug_overlay=1`) plus a few
`debug_*` keys such as `debug_ghost_bots`. Structured traces (player-ESP miss ledger,
crate-content reads, container scans) are appended as NDJSON to the log path configured
in `Project/Core/AgentLog.h`.

## Notes

This repository intentionally excludes the local SDK/dump corpora, symbol caches and
generated reports used during development — they are large and not needed to build or
run. Only the source, the unit tests, the small dev scripts in `tools/`, and the
runnable `Build/` runtime are tracked.
