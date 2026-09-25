#pragma once
// Bot vision cones (feature #7).
//
// Pure geometry + gates between the DMA reads (AIStateService::SightRange /
// SightHalfAngle, AISenseConfigSight::SightRadius / PeripheralVisionDeg) and
// the cone polygon drawn on the ESP. A cone only renders when the sight pair
// passes the plausibility gate - a flaky read leaves no cone, never a giant
// garbage fan.

#include <cmath>

namespace VisionCone {

struct Vec2 {
    float x = 0.f;
    float y = 0.f;
};

// Alertness label (AIStateService::ALERTNESS: 0=Idle .. 3=Combat). Empty for
// out-of-range bytes.
inline const char* AlertnessTag(int alertness)
{
    switch (alertness) {
    case 0: return "Idle";
    case 1: return "Alert";
    case 2: return "Searching";
    case 3: return "Combat";
    default: return "";
    }
}

// Raw sight values only render a cone inside these bounds (cm / degrees).
inline bool PlausibleSight(float radiusCm, float halfAngleDeg)
{
    return radiusCm > 100.f && radiusCm < 200000.f
        && halfAngleDeg >= 5.f && halfAngleDeg <= 180.f;
}

// Fan polygon in world XY (cm): apex at (xCm, yCm), forward = yawDeg (UE yaw
// convention: 0 = +X, +90 = +Y), arc spanning +-halfAngleDeg at radiusCm.
// Writes apex + arc samples into out (cap >= 3) and returns the count written.
inline int FanPoints(
    float xCm, float yCm, float yawDeg, float halfAngleDeg, float radiusCm,
    Vec2* out, int cap, int segmentsPerSide = 4)
{
    if (!out || cap < 3 || radiusCm <= 0.f)
        return 0;
    if (halfAngleDeg < 1.f)
        halfAngleDeg = 1.f;
    if (halfAngleDeg > 180.f)
        halfAngleDeg = 180.f;
    out[0].x = xCm;
    out[0].y = yCm;
    int want = segmentsPerSide * 2 + 1;
    if (want > cap - 1)
        want = cap - 1;
    if (want < 2)
        want = 2;
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    for (int i = 0; i < want; ++i) {
        const double t = static_cast<double>(i) / (want - 1);
        const double angle = (yawDeg - halfAngleDeg) + t * (2.0 * halfAngleDeg);
        out[1 + i].x = xCm + static_cast<float>(std::cos(angle * kDeg2Rad) * radiusCm);
        out[1 + i].y = yCm + static_cast<float>(std::sin(angle * kDeg2Rad) * radiusCm);
    }
    return 1 + want;
}

} // namespace VisionCone
