#pragma once
// T4.1 — per-axis aim calibration (advantage 5): independent X and Y constants,
// a response curve and a speed cap.
//
// Three rules shape this unit, and all three are asserted in
// Tests/aim_calibration_tests.cpp rather than promised in prose:
//
//   1. THE DEFAULT PROFILE IS THE IDENTITY. A fresh config must aim exactly as it
//      did before this file existed: X == Y == the existing single calibration
//      value, curve 0 means "no curve" (a multiplier of exactly 1.0), and cap 0
//      means "defer to the existing distance-scaled burst cap" instead of
//      imposing a second, tighter one.
//
//   2. CONSTANTS ARE REFUSED, NOT CLAMPED. Clamping turns a user's 40.0 into
//      10.0 and leaves them calibrating against a number they never chose, with
//      nothing said. A constant outside its range fails Validate() by name, and
//      the caller refuses the tick rather than aiming on it.
//
//   3. THE CAP IS ADVISORY. An unusable cap has a safe reading -- do not tighten
//      -- and the others do not. Refusing a whole tick because an optional
//      tightening was mistyped would turn a finger-slip in a slider into a dead
//      aim assist, so the cap only ever tightens the existing burst cap, and
//      only when it is a number that can be applied.
//
// Pure header: no globals, no timing, no hardware — so the suite can exercise
// the real formulas without a device or a game.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace AimCalibration {

// ── Ranges ────────────────────────────────────────────────────────────────
inline constexpr float kCountsPerPixelMin = 0.05f;
inline constexpr float kCountsPerPixelMax = 10.0f;
inline constexpr float kResponseMinMs = 20.0f;
inline constexpr float kResponseMaxMs = 500.0f;
inline constexpr float kCurveMin = 0.0f;   // 0 == identity, 1 == strongest
inline constexpr float kCurveMax = 1.0f;
inline constexpr float kCapMinCounts = 1.0f;
inline constexpr float kCapMaxCounts = 100.0f;

// Screen error at which the curve is a no-op. Nearer than this the curve slows
// the pull (the final approach is where overshoot lives); further away it speeds
// catch-up. 30 px sits where the existing close-fraction curve already pulls
// hard (>28 px), so the two do not fight each other.
inline constexpr float kCurveRefPx = 30.0f;

// Independent X and Y constants, in one struct so the two cannot drift apart and
// a single-axis setting cannot be forgotten at a call site.
struct Axis {
    float x = 1.0f;
    float y = 1.0f;
};

struct Profile {
    Axis countsPerPixel{};
    float responseMs = 120.0f;
    float curve = 0.0f;            // 0 = identity
    float maxCountsPerTick = 0.0f; // 0 = defer to the distance-scaled burst cap
};

enum class Field : uint8_t {
    None = 0,
    CountsPerPixelX,
    CountsPerPixelY,
    ResponseMs,
    Curve,
    Count
};

inline const char* FieldName(Field f)
{
    switch (f) {
    case Field::None: return "none";
    case Field::CountsPerPixelX: return "counts_per_pixel_x";
    case Field::CountsPerPixelY: return "counts_per_pixel_y";
    case Field::ResponseMs: return "response_ms";
    case Field::Curve: return "response_curve";
    case Field::Count:
    default: return "unknown";
    }
}

// True for the profile a fresh config resolves to. The suite asserts this profile
// reproduces the pre-T4.1 arithmetic, so the identity is checked, not asserted.
inline bool IsIdentity(const Profile& p)
{
    return p.countsPerPixel.x == 1.0f && p.countsPerPixel.y == 1.0f
        && p.curve == 0.0f && p.maxCountsPerTick == 0.0f;
}

struct Check {
    bool ok = true;
    Field field = Field::None;
    float value = 0.0f;
    float lo = 0.0f;
    float hi = 0.0f;
    const char* name() const { return FieldName(field); }
};

// Refuse rather than clamp: an out-of-range or non-finite constant fails here by
// name, and the caller keeps its last good value / refuses the tick. Nothing in
// this file silently substitutes a different number.
inline Check Validate(const Profile& profile)
{
    const auto inRange = [](float v, float lo, float hi) {
        return std::isfinite(v) && v >= lo && v <= hi;
    };
    const auto refuse = [](Field f, float v, float lo, float hi) {
        Check c;
        c.ok = false;
        c.field = f;
        c.value = v;
        c.lo = lo;
        c.hi = hi;
        return c;
    };

    if (!inRange(profile.countsPerPixel.x, kCountsPerPixelMin, kCountsPerPixelMax))
        return refuse(Field::CountsPerPixelX, profile.countsPerPixel.x,
            kCountsPerPixelMin, kCountsPerPixelMax);
    if (!inRange(profile.countsPerPixel.y, kCountsPerPixelMin, kCountsPerPixelMax))
        return refuse(Field::CountsPerPixelY, profile.countsPerPixel.y,
            kCountsPerPixelMin, kCountsPerPixelMax);
    if (!inRange(profile.responseMs, kResponseMinMs, kResponseMaxMs))
        return refuse(Field::ResponseMs, profile.responseMs,
            kResponseMinMs, kResponseMaxMs);
    if (!inRange(profile.curve, kCurveMin, kCurveMax))
        return refuse(Field::Curve, profile.curve, kCurveMin, kCurveMax);

    return Check{};
}

// Rule 3: 0 (deferred) or a cap that can actually be applied. A sub-count cap is
// not usable -- it would round the command to nothing -- and is skipped rather
// than obeyed or clamped.
inline bool CapUsable(const Profile& p)
{
    if (p.maxCountsPerTick == 0.0f)
        return true;
    return std::isfinite(p.maxCountsPerTick)
        && p.maxCountsPerTick >= kCapMinCounts
        && p.maxCountsPerTick <= kCapMaxCounts;
}

// ── Response curve ────────────────────────────────────────────────────────
// Applied per axis to the screen error, before that axis's calibration.
//   curve == 0            -> exactly 1.0 for every error (the identity)
//   |error| == 0          -> 0.0 (a zero error stays zero, never NaN)
//   |error| <  ref (30px) -> < 1.0  (slower final approach: less overshoot)
//   |error| >  ref        -> > 1.0  (faster catch-up)
// Monotone in curve, continuous at the reference, and clamped at kCurveMax so a
// hand-edited value cannot run away.
inline float CurveMultiplier(float absErrorPx, float curve)
{
    if (!std::isfinite(absErrorPx) || !std::isfinite(curve) || curve <= 0.0f)
        return 1.0f;
    if (absErrorPx <= 0.0f)
        return 0.0f;
    const float k = (curve > kCurveMax) ? kCurveMax : curve;
    return std::pow(absErrorPx / kCurveRefPx, k);
}

// The per-axis screen-error mapping: curve, then that axis's calibration.
//
// commonScale is everything the two axes share (close fraction, user gain, the
// learned px-per-mouse scale). Keeping it as ONE argument is deliberate: it is
// what previously let both axes share one calibration constant by accident, so
// the only way to calibrate an axis now is to pass X and Y separately. Returned
// x/y are mouse counts for that axis.
inline Axis ApplyCalibration(
    const Profile& profile, float errorX, float errorY, float commonScale)
{
    if (!std::isfinite(errorX) || !std::isfinite(errorY) || !std::isfinite(commonScale))
        return {};

    const float curveX = CurveMultiplier(std::fabs(errorX), profile.curve);
    const float curveY = CurveMultiplier(std::fabs(errorY), profile.curve);

    return {
        errorX * curveX * profile.countsPerPixel.x * commonScale,
        errorY * curveY * profile.countsPerPixel.y * commonScale
    };
}

// The speed cap. Returns the magnitude cap the caller should apply, given the cap
// it already applies on its own. A configured cap only ever tightens the existing
// one; an unset or unusable cap changes nothing.
inline float EffectiveCapCounts(const Profile& profile, float existingCap)
{
    if (!(profile.maxCountsPerTick > 0.0f) || !CapUsable(profile))
        return existingCap;
    if (!std::isfinite(existingCap) || existingCap <= 0.0f)
        return profile.maxCountsPerTick;
    return (std::min)(existingCap, profile.maxCountsPerTick);
}

} // namespace AimCalibration
