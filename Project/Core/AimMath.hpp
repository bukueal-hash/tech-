#pragma once
// Pure aim-smoothing / control-loop math, extracted from Aimbot.cpp
// SendKmAimDelta so Project.Tests can exercise the real code. All functions
// are deterministic: no globals, no RNG, no timing side effects — callers
// pass every input (including the current time for windowed state).
//
// Extracted verbatim from Aimbot.cpp (Pillar 1 of docs/aplus-plan.md); do not
// "improve" the formulas here without also changing Aimbot.cpp — the shake
// analysis (tools/analyze_aim_shake.py) is the arbiter of what these should
// do, and these functions must match it.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace AimMath {

// ── Constants (moved from Aimbot.cpp) ───────────────────────────────────────
// Learned screen-px closed per mouse count. post-fix3 rem≈dist but error
// barely closed → mouse≠1:1 screen; start conservative (need more mouse).
inline constexpr float kMousePxPerCountInit = 0.70f;
// Floor 0.50 (was 0.04): a near-zero scale makes one tick's command up to 25x
// the screen error → guaranteed overshoot → sign flip → punish → lower scale
// → the limit-cycle that read as aim-key shake (probe: pxPerMouse med 0.25
// at crossovers). Understeer from a high floor is safe; oversteer shakes.
inline constexpr float kMousePxPerCountMin = 0.50f;
inline constexpr float kMousePxPerCountMax = 1.25f;
// Oscillation damping: per-axis crossover windows. ≥kOscFlipsPerWindow sign
// flips inside kOscWindowMs means the loop is ringing (aim-shake probe:
// flipY 22, overshoot 29 in one 4s lock) — ramp damping, which eases
// closeFrac and smooths the EMA until the flips stop, then recover slowly.
inline constexpr int kOscWindowMs = 500;
inline constexpr int kOscFlipsPerWindow = 3;
inline constexpr float kOscDampStep = 0.15f;
inline constexpr float kOscDampMax = 0.70f;
inline constexpr float kOscRecoverStep = 0.05f;
inline constexpr float kOscPullFactor = 0.5f;     // closeFrac multiplier at full damp
inline constexpr float kOscSmoothFactor = 0.35f;  // EMA-alpha reduction at full damp

// ── closeFrac curve (fraction of remaining screen error closed this tick) ──
inline float CloseFractionForDist(float dist)
{
    float closeFrac = 0.38f;
    if (dist > 12.f)
        closeFrac = 0.48f;
    if (dist > 28.f)
        closeFrac = 0.58f;
    if (dist > 55.f)
        closeFrac = 0.68f;
    if (dist > 100.f)
        closeFrac = 0.78f;
    return closeFrac;
}

// Accelerated algorithm: extra pull proportional to (normalized dist)^2,
// capped at 0.92.
inline float AcceleratedCloseFrac(float closeFrac, float dist)
{
    const float norm = (std::min)(dist / 100.f, 1.f);
    return (std::min)(0.92f, closeFrac + 0.12f * norm * norm);
}

// pullScale clamp: never below 0.90 (keeps pull alive even when the caller
// tries to ease off hard).
inline float ApplyPullScale(float closeFrac, float pullScale)
{
    return closeFrac * (std::max)(pullScale, 0.90f);
}

// Per-lock randomized pull profile ramp: accel<0 = slow start then hurry;
// accel>0 = fast start then ease off. Clamped to [0.55, 1.6].
inline float PullProfileRamp(float accel, float t)
{
    return (std::clamp)(1.f + accel * (t * 2.f - 1.f), 0.55f, 1.6f);
}

// Combined profile application: speed * ramp * sharp * situational bias,
// clamped to [0.05, 0.97].
inline float ApplyPullProfile(float closeFrac, float speed, float ramp,
    float sharp, float situ)
{
    return (std::clamp)(closeFrac * speed * ramp * sharp * situ, 0.05f, 0.97f);
}

// Per-axis oscillation damping factor on the pull: 1 at damp=0 down to
// 1 - factor at full damp.
inline float OscDampPullFactor(float damp, float factor)
{
    return 1.f - damp * factor;
}

// Light EMA — kills dy sign-flip chatter without lagging far catch-up.
inline float EmaStep(float prev, float sample, float alpha)
{
    return prev * (1.f - alpha) + sample * alpha;
}

// Tick-scale for the mouse burst cap: keep a 1x..10x range around 8ms ticks.
inline float TickScaleFromTickMs(float tickMs)
{
    float tickScale = tickMs / 8.f;
    if (tickScale < 1.f)
        tickScale = 1.f;
    if (tickScale > 10.f)
        tickScale = 10.f;
    return tickScale;
}

// Cap mouse burst so one late tick cannot slam (choppy). Scale cap with
// recent tickMs so slow ticks still catch movers.
inline float MaxMouseForDist(float dist, float tickScale)
{
    return (dist > 40.f ? 140.f : 95.f) * tickScale;
}

// Scale (mx,my) down to magnitude <= maxMouse (returns scaled pair).
inline std::pair<float, float> ClampMouseBurst(float mx, float my, float maxMouse)
{
    const float mag = std::hypot(mx, my);
    if (mag > maxMouse && mag > 0.01f) {
        const float s = maxMouse / mag;
        return { mx * s, my * s };
    }
    return { mx, my };
}

// ── Oscillation damping state machine (per axis) ───────────────────────────
// Counts crossovers per axis in a kOscWindowMs window; ≥kOscFlipsPerWindow in
// a window means the loop is ringing — ramp damping, which eases closeFrac
// and smooths the EMA until the flips stop, then recover slowly.
struct OscDampState {
    int flips = 0;            // crossovers in the current window
    uint64_t winStartMs = 0;
    float damp = 0.f;         // 0..kOscDampMax extra damping

    void tick(uint64_t flipNowMs, bool flip)
    {
        if (winStartMs == 0)
            winStartMs = flipNowMs;
        if (flipNowMs - winStartMs >= static_cast<uint64_t>(kOscWindowMs)) {
            if (flips >= kOscFlipsPerWindow)
                damp = (std::min)(kOscDampMax, damp + kOscDampStep);
            else if (flips == 0)
                damp = (std::max)(0.f, damp - kOscRecoverStep);
            flips = 0;
            winStartMs = flipNowMs;
        }
        if (flip)
            ++flips;
    }
};

// EMA alpha reduction under damping (stronger smoothing while ringing).
inline float EmaAlphaWithDamp(float baseAlpha, float damp)
{
    return baseAlpha * (1.f - damp * kOscSmoothFactor);
}

// ── px-per-mouse learning (adaptive mouse↔screen scale) ────────────────────
// Slow EMA (0.92/0.08, was 0.82/0.18): fast adaptation chased per-tick noise
// and swung the scale itself into oscillation.
inline float LearnPxPerMouse(float current, float sample)
{
    return current * 0.92f + sample * 0.08f;
}

inline float ClampPxPerMouseSample(float sample)
{
    if (sample < kMousePxPerCountMin)
        sample = kMousePxPerCountMin;
    if (sample > kMousePxPerCountMax)
        sample = kMousePxPerCountMax;
    return sample;
}

// Overshoot: the error crossed the target and GREW after our command — the
// mouse is STRONGER than learned. Raise the scale so the next tick sends
// FEWER counts.
inline float OvershootRaisePxPerMouse(float current)
{
    return (std::min)(kMousePxPerCountMax, current * 1.05f);
}

} // namespace AimMath