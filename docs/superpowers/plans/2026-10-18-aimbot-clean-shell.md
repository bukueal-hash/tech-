# Aimbot Clean-Shell Implementation Plan

> **For agentic workers:** Implement inline task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Strip aim movement and triggerbot from the app while keeping the crosshair, ESP visibility colours, saved Aimbot enable/hotkey placeholders, and KMBox/MAKCU setup.

**Architecture:** Remove the aim worker, target selection, aim prediction, trigger firing, and aim-only diagnostics. Remove their menu/config surface and aim-only cache demand, but preserve ordinary ESP/radar, the independent crosshair, visibility colouring, and hardware setup. The retained Aimbot toggle/hotkey do not move the mouse.

**Tech Stack:** Existing C++ Win32/ImGui main target and existing `Build/ArcRaiders.exe` only.

## Global Constraints

- Never add an executable, application target, helper, launcher, or test binary.
- Keep edits local; do not commit or upload.
- Do not build/run test executables; repository policy forbids that unless tests are requested by name.
- Preserve `RenderOverlayCrosshair`, crosshair configuration, `visCheckEsp`, Aimbot enable/hotkey placeholders, and KMBox/MAKCU setup.
- Before the post-edit build/run, verify no other agent is editing/building; do not interfere with an active overlay session.

---

## Task 1: Remove aim and trigger runtime

**Files:**
- Delete `Project/Functions/Aimbot.cpp`.
- Modify `Project/Core/Engine.h`, `Project/Functions/EngineThreads.cpp`, and `Project/Project.cpp`.
- Remove aim-only `FeaturePolicy.hpp` helpers/self-test and corresponding `RunReport.hpp` fields.

- [x] Remove `Engine::AimAssistence`, `ReleaseAimOutputs`, the aim worker, aim cadence, and the aim-profile self-test.
- [x] Remove `AimTarget`, aim-only projection/target APIs, humanizer state, and aim-thread members from `Engine.h`.
- [x] Remove trigger hold/click logic, target selection, prediction, mouse movement, humanizer, shot tracking, and aim diagnostics by removing `Aimbot.cpp`.
- [x] Keep other `FeaturePolicy` rules and `RunReport` fields unrelated to the aim pass.

## Task 2: Remove aim/trigger UI, variables, config, and debug panel

**Files:**
- Modify `Project/Interface/Overlay/MenuSidebar.cpp`, `Project/Interface/Render.cpp`, and `Project/Interface/Utils/AutoConfig.cpp`.
- Modify `Project/Interface/Utils/Variables/index.h/.cpp` and `Project/Interface/Utils/UpgradeOptions.hpp`.
- Delete `Project/Functions/AimbotMonitor.cpp/.hpp` and `Project/Interface/Overlay/AimbotDebugPanel.cpp/.hpp`.
- Modify `Project/Project.vcxproj` and `Project/Project.vcxproj.filters`.

- [x] Keep the Aimbot tab limited to `Enable Aimbot`, the existing ESP visibility-colour toggle (`visCheckEsp`), the aim hotkey placeholder, and KMBox/MAKCU status.
- [x] Remove the Triggerbot tab, activation/hotkey/deadzone/delay/hold settings, and the `Alt+T` trigger shortcut.
- [x] Keep the Crosshair tab and `RenderOverlayCrosshair` unchanged.
- [x] Remove aim-debug panel/monitor, humanizer display, and aim-only controls/config keys; keep crosshair, vischeck, placeholder, and device keys.
- [x] Retain KMBox/MAKCU library/config files, but ensure the engine no longer calls `MoveAim`, `Click`, `HoldStart`, or `HoldEnd`.

## Task 3: Remove aim-only demand from ESP/cache workers

**Files:**
- Modify `Project/Functions/Esp.cpp`, `EntityList.cpp`, `RobotList.cpp`, `PositionRefreshPass.cpp`, and `Utils.cpp`.
- Remove obsolete aim-only headers only after searching all project references.

- [x] Remove aim-driven FOV ring, bone capture, distance expansion, inventory reads, target frame collection, and scan gates.
- [x] Remove `robotAimEnabled`, `enable_triggerbot`, and other deleted aim variables from producer/consumer conditions.
- [x] Preserve normal player/bot ESP, radar collection, visible-state colouring, crosshair rendering, and non-aim inventory displays.
- [x] Keep `visCheckEsp` for ESP colouring only; remove the unused `visCheckAim` setting.

## Task 4: Project-reference cleanup and verification

**Files:**
- Modify `Project/direct-read-baseline.txt` if the deleted `Aimbot.cpp` read entry is no longer needed.

- [x] Search for active references to `AimAssistence`, `AimTarget`, `enable_triggerbot`, `robotAimEnabled`, `AimbotMonitor`, and `AimbotDebugPanel`; remove dangling references.
- [x] Check that the Crosshair tab/render path, `visCheckEsp`, placeholder controls, and KMBox/MAKCU settings remain connected.
- [x] Run `git diff --check` and review the local diff; do not build the test executable.
- [x] Follow `AGENTS.md` for the main-target build/run after checking for other builders and the overlay; close only `ArcRaiders.exe` when needed.

---

## Acceptance

- No target-selection, mouse-movement, trigger-fire, aim-prediction, humanizer, or aim-debug path is connected to the main app.
- The Triggerbot tab and triggerbot config keys are gone.
- Crosshair drawing/settings, ESP visibility colours, the Aimbot enable/hotkey placeholders, and KMBox/MAKCU setup remain.
- Ordinary ESP/radar behavior remains independent of the removed aim code.
