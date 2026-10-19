# Aim controller baseline design

## Goal

Make the aim controller's response predictable enough to diagnose and tune. The latest telemetry shows large screen errors and repeated direction flips for players. Bot prediction error is a separate issue and is not addressed by this controller change.

## Research basis

- [PX4 Autopilot rate controller](https://github.com/PX4/PX4-Autopilot/blob/main/src/lib/rate_control/rate_control.cpp) is a working, open-source feedback controller. Its implementation uses measured time, bounded controller state, and filtered measured-rate feedback.
- [PX4 PID tuning guide](https://docs.px4.io/main/en/config_mc/pid_tuning_guide_multicopter) recommends tuning controller gains from observed response and warns that excessive proportional gain causes oscillation.
- [ROS 2 control PID docs](https://control.ros.org/rolling/doc/ros2_controllers/pid_controller/doc/userdoc.html) document output limits, state reporting, and anti-windup.

These projects control physical hardware, not a game camera. Use their control and measurement practices as reference, not their code or gain values.

## Design

1. Keep target selection, screen projection, and target-motion prediction unchanged.
2. Replace the randomized/adaptive mouse-pull stack with a fixed proportional baseline: start at 0.20 of the error per nominal 4 ms tick, retimed from measured tick time. Keep the existing 0.70 px/count starting estimate and hard output limit; the estimate still needs calibration from observed response.
3. Disable aim-point wander, randomized pull profiles, and online gain/scale learning while establishing the baseline. These make repeated tests non-repeatable; aim-point wander is explicitly designed to move the crosshair away from the exact target point.
4. Start without a derivative term. The current screen-error delta includes both target motion and camera response, so it is not a clean measurement for derivative damping. Add damping only if new telemetry can separate those effects.
5. Log screen error, proportional gain, tick duration, and mouse command together. Tune one gain at a time from repeatable practice-mode observations; do not claim success until new logs show reduced error and fewer sign flips.
6. Apply a separate controller-delay lead only to the mouse movement point: project the existing ballistic aim point forward by one measured aim tick using the target velocity, capped at 16 ms and 12 screen pixels. Keep target selection, projectile prediction, and trigger checks on the original ballistic point to prevent double-counting bullet travel.

## Scope and validation

Change only the mouse-control step and the minimum supporting settings/logging in `Aimbot.cpp` / `AimMath.hpp`. Do not change prediction or add memory reads. Verification is source review only until the user builds and supplies a fresh practice run; do not build or run the application here.

## Known correction

The earlier change from `kMaxCloseFracPerTick = 0.55` to `0.35` was justified by incorrect unit reasoning: `1 / pxPerMouse` converts pixel error into mouse counts, so multiplying those values does not establish overshoot. Do not treat that cap change as a confirmed fix; review it during implementation against the simpler controller design.
