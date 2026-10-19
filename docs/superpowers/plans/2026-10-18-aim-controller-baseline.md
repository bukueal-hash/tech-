# Aim Controller Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make aim movement repeatable so logs can show whether screen error shrinks without oscillation.

**Architecture:** Preserve target selection and target prediction. Replace the layered mouse-pull calculation with a fixed proportional response retimed by actual tick duration, then convert pixels to mouse counts and retain a hard output cap. Disable random target offsets and adaptive/random gain changes for the baseline.

**Tech Stack:** Existing C++ controller (`Aimbot.cpp`, `AimMath.hpp`), current telemetry logs. No new libraries.

## Global Constraints

- Keep the single executable rule: no new executable or application target.
- Do not build or run; the user will build and test.
- Do not add DMA reads or change target prediction.
- Do not commit; the user did not request a commit.
- Avoid unrelated existing workspace changes.

---

## File Structure

- Modify `Project/Functions/Aimbot.cpp`: remove active random/adaptive aim offsets and simplify the screen-error-to-mouse-command path.
- Modify `Project/Core/AimMath.hpp`: use a single conservative proportional close fraction retimed by measured tick duration; remove the unsupported cap justification.
- Do not change robot/player target selection, prediction, or telemetry file formats.

### Task 1: Replace the layered mouse-pull step

**Files:**
- Modify: `Project/Functions/Aimbot.cpp` near the screen-error setup (~3141), adaptive scale learning (~2008), and mouse command calculation (~2099–2332).
- Modify: `Project/Core/AimMath.hpp` near `kMaxCloseFracPerTick` and the existing `RetimeCloseFrac` helper.

**Implementation:**

- [x] Remove the active aim-point wander offset from `dx`/`dy`.
- [x] Stop applying auto gain, randomized `PullProfile`, situational gain, online px-per-mouse learning, per-lock asymmetry, oscillation damping, derivative correction, and the extra EMA in the baseline output path.
- [x] Calculate one fixed 0.20 close fraction, retimed with `s_lastAimTickMs` through `AimMath::RetimeCloseFrac` and capped at 0.35.
- [x] Convert the proportional pixel correction with the fixed 0.70 px-per-mouse starting estimate, keep deadzone and final mouse-burst clamp, then round to integer mouse counts.
- [x] Keep target prediction and target selection unchanged.
- [x] Preserve telemetry for the actual fixed close fraction and final command so the user's next run can be evaluated.
- [x] Add a separate one-tick controller lead to mouse movement only, capped at 16 ms / 12 px; retain the ballistic point for target selection and firing, and use the stabilized bot velocity when available.

**Verification (source only):**

- [x] Search for any remaining active `HumanVariationStep`, `AutoTuneStep`, `s_pullProf`, online scale-learning, or oscillation/EMA stages in the mouse command path.
- [x] Review source and run `git diff --check` to confirm there are no whitespace errors and that the final command still has a hard cap.
- [x] Do not build, run, or execute test binaries. Runtime behavior remains unverified until the user builds and supplies fresh logs.

---

## Review note

The earlier `0.55` → `0.35` cap edit was not justified by the `1 / pxPerMouse` conversion; that is a unit conversion, not proof of overshoot. Replace that reasoning with the explicit fixed proportional baseline and measured-tick retiming. Do not claim the new value is proven until fresh logs are available.
