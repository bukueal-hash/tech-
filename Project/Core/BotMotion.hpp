#pragma once

// Render-time motion for entity boxes.
//
// Position samples do not arrive on the paint clock: the entity scans publish
// them on their own cadence (measured: p50 2.4s per RobotList pass, bot samples
// 404ms old p50 / 919ms p90 while the 16ms sampler was blocked). Painting the
// newest sample directly therefore teleports the box once per sample and jumps
// it BACKWARD whenever a sample lands late — that is the "choppy" look, and no
// amount of velocity smoothing fixes it, because the input itself steps.
//
// The fix is to render from the two newest samples instead of from one, in
// exactly one of two modes — never a blend, because a delay smaller than the
// sampling interval renders a spot from inside the previous interval and then
// has to glide forward again on arrival, which is itself a backward jump:
//
//   dense samples (interval <= kInterpMaxIntervalMs)
//     renderTime = now - interval, then lerp(prev, cur)
//     exact, needs no velocity, costs one (small) interval of lag
//
//   sparse samples (the measured 400ms-2.4s bot path)
//     cur + velocity * lead, lead capped at one interval + slack
//     a full-interval delay would leave the box metres behind the body, so this
//     mode extrapolates across the gap and leaves the residual prediction error
//     to SmoothCorrection, which glides it in instead of snapping.
//
// Both modes place the box where it was at paint time when the velocity is
// right, so the rendered position is continuous across a sample arrival.

#include "Vector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace BotMotion {

struct Sample {
    Vector3 pos{};
    uint64_t ms = 0;
};

struct RenderState {
    Vector3 pos{};
    uint64_t intervalMs = 0;  // spacing of the two samples used
    uint64_t delayMs = 0;     // how far behind now the rendered sample is
    uint64_t leadMs = 0;      // time extrapolated past the newest sample
    bool valid = false;
};

// Sampling interval at or below which the two newest samples straddle the paint
// tick: interpolate (exact, needs no velocity, costs one interval of lag).
inline constexpr uint64_t kInterpMaxIntervalMs = 50;
// Smallest render delay inside interpolation: below this the render time would
// sit on the newest sample itself and step with the next arrival.
inline constexpr uint64_t kMinDelayMs = 8;
// Extrapolation budget. The lead has to cover the sampling gap itself, or the
// box is systematically behind the body by (interval - lead) — 140cm at run
// speed for the measured 400ms bot samples. It is capped at one interval plus
// slack so a stale sample can never send the box a second into the future.
inline constexpr uint64_t kLeadSlackMs = 100;
inline constexpr uint64_t kMinLeadMs = 120;
inline constexpr uint64_t kMaxLeadMs = 600;

inline Vector3 Advance(const Vector3& pos, const Vector3& velocity, uint64_t ms)
{
    if (ms == 0)
        return pos;
    const double sec = static_cast<double>(ms) * 0.001;

    return Vector3{
        pos.x + velocity.x * sec,
        pos.y + velocity.y * sec,
        pos.z + velocity.z * sec};
}

inline Vector3 Lerp(const Vector3& a, const Vector3& b, double t)
{
    if (t <= 0.0)
        return a;
    if (t >= 1.0)
        return b;
    return Vector3{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t};
}

// Correction budget on top of the bot's own motion, cm/s. A sample that lands
// late corrects the extrapolated target by the prediction error accumulated
// while it was missing — tens of cm for a 100-400ms gap at run speed. Snapping
// to that correction is the visible teleport; gliding to it at the bot's speed
// plus this budget absorbs it in ~0.1s, below perception.
inline constexpr double kCorrectionCmPerSec = 800.0;

inline double AllowedStepCm(double speedCmPerSec, uint64_t dtMs)
{
    const double sec = static_cast<double>(dtMs) * 0.001;
    return (speedCmPerSec + kCorrectionCmPerSec) * sec;
}

// Move prev toward target by at most AllowedStepCm; return target when the
// error already fits inside the budget. Normal motion fits (the bot's own speed
// is in the budget), so this only bounds corrections.
inline Vector3 SmoothCorrection(
    const Vector3& prev,
    const Vector3& target,
    uint64_t dtMs,
    double speedCmPerSec)
{
    if (dtMs == 0)
        return prev;
    const double ex = target.x - prev.x;
    const double ey = target.y - prev.y;
    const double ez = target.z - prev.z;
    const double err = std::sqrt(ex * ex + ey * ey + ez * ez);
    const double allowed = AllowedStepCm(speedCmPerSec, dtMs);
    if (err <= allowed || !(err > 0.0))
        return target;
    const double k = allowed / err;
    return Vector3{prev.x + ex * k, prev.y + ey * k, prev.z + ez * k};
}

inline double SpeedOf(const Vector3& velocity)
{
    return std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y
        + velocity.z * velocity.z);
}

inline RenderState ResolveRenderPos(
    const Sample& prev,
    const Sample& cur,
    const Vector3& velocity,
    uint64_t nowMs)
{
    RenderState out{};
    out.pos = cur.pos;
    out.valid = true;

    // No sample at all: nothing to render (caller keeps its own fallback).
    if (cur.ms == 0) {
        out.valid = prev.ms != 0;
        return out;
    }

    // Only one sample so far: no interval to interpolate across, so this is the
    // one case where the box has to be extrapolated from the newest sample.
    if (prev.ms == 0 || prev.ms >= cur.ms) {
        if (nowMs > cur.ms) {
            out.leadMs = (std::min<uint64_t>)(nowMs - cur.ms, kMaxLeadMs);
            out.pos = Advance(cur.pos, velocity, out.leadMs);
        }
        return out;
    }

    out.intervalMs = cur.ms - prev.ms;

    // Deliberately one mode or the other, never a blend. A delay smaller than
    // the interval puts the render time INSIDE the newest interval after a
    // sample lands, which renders the spot the bot occupied 400ms ago and then
    // has to glide forward again — a backward jump, the very artefact this
    // function exists to remove.
    if (out.intervalMs <= kInterpMaxIntervalMs) {
        // Dense samples: interpolate. Exact, and independent of velocity.
        const uint64_t delay = (std::max<uint64_t>)(out.intervalMs, kMinDelayMs);
        const uint64_t renderTime = nowMs > delay ? nowMs - delay : 0;
        out.delayMs = nowMs > renderTime ? nowMs - renderTime : 0;

        if (renderTime <= prev.ms) {
            out.pos = prev.pos;
            return out;
        }
        if (renderTime <= cur.ms) {
            const double t = static_cast<double>(renderTime - prev.ms)
                / static_cast<double>(out.intervalMs);
            out.pos = Lerp(prev.pos, cur.pos, t);
            return out;
        }
        const uint64_t tail = (std::min<uint64_t>)(renderTime - cur.ms, kMinLeadMs);
        out.leadMs = tail;
        out.pos = Advance(cur.pos, velocity, tail);
        return out;
    }

    // Sparse samples (the measured 400ms-2.4s bot path): a full-interval delay
    // would leave the box visibly behind the body, so extrapolate across the
    // gap instead and let the caller's correction budget absorb the error.
    const uint64_t leadCap = (std::min<uint64_t>)(
        out.intervalMs + kLeadSlackMs,
        (std::max<uint64_t>)(kMinLeadMs, kMaxLeadMs));
    if (nowMs > cur.ms)
        out.leadMs = (std::min<uint64_t>)(nowMs - cur.ms, leadCap);
    out.pos = Advance(cur.pos, velocity, out.leadMs);
    return out;
}

} // namespace BotMotion
