# Aimbot clean-shell design

## Goal

Remove the current aim and trigger behavior and leave a small, clear shell for a later rebuild.

## Preserve

- The crosshair renderer and its styles/settings. It lives outside the aim worker.
- `Enable Aimbot` and the aim hotkey as saved placeholders. They do not move the mouse in this clean-shell version.
- The existing ESP visibility-colour toggle (`visCheckEsp`), as requested. It does not gate aim or firing.
- KMBox/MAKCU setup and low-level device support. No aim or trigger commands are sent by this clean shell.
- The Crosshair and hardware setup UI.

## Remove

- Triggerbot behavior and its tab/settings/hotkeys.
- Aim target gathering/selection, bone picking, prediction, FOV aim ring, mouse movement, calibration/response controls, humanizer, sticky/grace state, shot tracking, and aim debug monitor.
- Aim-only scheduling, telemetry, variables, config keys, and project references that become unused. Keep ESP/radar collection paths that serve those features independently.

## Boundaries

- No new executable or application target.
- Do not alter crosshair rendering or general ESP visibility colouring.
- Keep KMBox/MAKCU implementation files, but remove aim/trigger call sites from the engine.
- Old aim/trigger config keys may be ignored after the cleanup; preserved keys are `enable_aimbot`, the aim hotkey, `vischeck_esp`, crosshair settings, and device settings.
- The retained Aimbot checkbox and hotkey are intentionally inactive until a future controller is added.

## Validation

Use source searches and project-reference checks to ensure no removed trigger/aim code remains connected and that crosshair, ESP visibility colouring, and hardware setup still have callers. Do not build or run while another agent is editing/building; otherwise follow the repository's requested post-edit build workflow.
