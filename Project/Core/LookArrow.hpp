#pragma once
// Look-direction arrows (feature: look arrows).
//
// Pure geometry + validation between the DMA reads (AController::
// ControlRotation, PlayerCameraManager view clamps) and the arrow drawn on the
// ESP. The arrow angle is the player's aim yaw measured against the camera
// yaw: 0 deg = up = camera forward, matching the radar's ally arrows.

#include <cmath>

namespace LookArrow {

constexpr double kPi = 3.14159265358979323846;

// Normalize degrees into (-180, 180].
inline double NormalizeDeg(double deg)
{
    while (deg <= -180.0)
        deg += 360.0;
    while (deg > 180.0)
        deg -= 360.0;
    return deg;
}

// Screen angle in radians (0 = up, positive = clockwise) for the aim arrow.
inline double ArrowAngleRad(double aimYawDeg, double camYawDeg)
{
    return NormalizeDeg(aimYawDeg - camYawDeg) * kPi / 180.0;
}

struct Point {
    float x = 0.f;
    float y = 0.f;
};

// Small triangle (tip + two base corners) at (cx, cy). len = tip distance from
// center, baseBack = base distance behind center.
inline void ArrowPoints(
    float cx, float cy, double angleRad, float len,
    Point& tip, Point& left, Point& right)
{
    const float sa = static_cast<float>(std::sin(angleRad));
    const float ca = static_cast<float>(std::cos(angleRad));
    tip.x = cx + sa * len;
    tip.y = cy - ca * len;
    const float baseBack = len * 0.45f;
    const float halfW = len * 0.42f;
    // base center sits behind the tip along the arrow axis
    const float bcx = cx - sa * baseBack;
    const float bcy = cy + ca * baseBack;
    left.x = bcx - ca * halfW;
    left.y = bcy - sa * halfW;
    right.x = bcx + ca * halfW;
    right.y = bcy + sa * halfW;
}

// A ControlRotation-derived aim pitch is accepted only inside the camera's
// view clamps (plus slack) - free-cam/garbage rotations fail here instead of
// spinning the arrow. Non-inverted clamp pairs (max - min < 1) disable the
// check (server did not expose usable clamps).
inline bool AimPitchValid(double pitchDeg, double pitchMinDeg, double pitchMaxDeg)
{
    if (!std::isfinite(pitchDeg) || pitchDeg < -180.0 || pitchDeg > 180.0)
        return false;
    if (pitchMaxDeg - pitchMinDeg < 1.0)
        return true;
    return pitchDeg >= pitchMinDeg - 10.0 && pitchDeg <= pitchMaxDeg + 10.0;
}

} // namespace LookArrow
