#pragma once
// Map-accurate radar (feature #11).
//
// Pure projection between the DMA reads (AWorldPartitionMiniMap::
// WORLD_UNITS_PER_PIXEL + MINIMAP_WORLD_BOUNDS) and the radar. Map mode is
// geographically correct: north-up, whole-map fit from the minimap bounds,
// falling back to a player-centered north-up range view when the bounds have
// not resolved. Underground floors are detected by Z alone (MapWidgetLevel
// settings live in the UMG widget tree, which DMA cannot walk; the Z test
// works regardless of whether the map HAS_UNDERGROUND flag is reachable).

#include <cmath>

namespace RadarProjection {

struct Bounds {
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
};

// The minimap bounds cover a raid map: 10m .. 40km per side. Anything else is
// a mis-read block and the radar must not calibrate from it.
inline bool BoundsPlausible(const Bounds& b)
{
    const double w = b.maxX - b.minX;
    const double h = b.maxY - b.minY;
    return std::isfinite(w) && std::isfinite(h)
        && w > 1000.0 && w < 4.0e6
        && h > 1000.0 && h < 4.0e6;
}

// Whole-map fit: world XY (cm) -> radar pixel offsets from the radar center.
// North (+Y) is up; points outside the bounds return false (no blip).
inline bool MapFitted(
    double xCm, double yCm, const Bounds& b, float px,
    float& outX, float& outY)
{
    if (!BoundsPlausible(b) || px <= 0.f)
        return false;
    if (xCm < b.minX || xCm > b.maxX || yCm < b.minY || yCm > b.maxY)
        return false;
    const double nx = (xCm - b.minX) / (b.maxX - b.minX);
    const double ny = (yCm - b.minY) / (b.maxY - b.minY);
    outX = static_cast<float>((nx - 0.5) * 2.0 * px);
    outY = static_cast<float>(-(ny - 0.5) * 2.0 * px);
    return true;
}

// Player-centered north-up fallback: same metric scale as the rotating radar,
// but oriented geographically (+Y = up) instead of with the camera yaw.
inline bool NorthUp(
    double xCm, double yCm, double localXCm, double localYCm,
    double rangeCm, float px,
    float& outX, float& outY)
{
    if (!(rangeCm > 0.0) || px <= 0.f)
        return false;
    const double dx = xCm - localXCm;
    const double dy = yCm - localYCm;
    if (dx * dx + dy * dy > rangeCm * rangeCm)
        return false;
    outX = static_cast<float>(dx / rangeCm * px);
    outY = static_cast<float>(-dy / rangeCm * px);
    return true;
}

// Underground floor detection: a blip well below the local floor.
inline bool IsUnderground(double dzCm, double thresholdCm = 400.0)
{
    return std::isfinite(dzCm) && dzCm < -thresholdCm;
}

} // namespace RadarProjection
