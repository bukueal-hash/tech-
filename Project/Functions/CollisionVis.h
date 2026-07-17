#pragma once

#include "../Core/Vector.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class Engine;

namespace CollisionVis {

enum class ProbeStatus : int { Red = 0, Yellow = 1, Green = 2 };

struct RayHit {
    bool clear = true; // fail-open default
    float hitT = 1.f;
    Vector3 hitPos{};
    bool valid = false;
};

struct DebugRay {
    Vector3 from{};
    Vector3 to{};
    bool blocked = false;
    Vector3 hitPos{};
};

struct Stats {
    ProbeStatus probe = ProbeStatus::Red;
    int smc = 0;
    int tris = 0;
    int rebuilding = 0;
    int rebuildMs = 0;
    int queries = 0;
    int failOpen = 0;
    int hits = 0;
    int clears = 0;
};

void TickRebuild(Engine& eng);
RayHit RaycastLos(const Vector3& from, const Vector3& to);
RayHit RaycastLosMulti(const Vector3& from, const Vector3* ends, int endCount);
float EffectivePlayerRangeM();
float EffectiveBotRangeM();
float RebuildRadiusM();
Stats GetStats();
void CopyDebugRays(std::vector<DebugRay>& out);
void RecordDebugRay(const DebugRay& ray);
void ApplyVisToEspCaches(Engine& eng);
bool AimLosAllows(const Vector3& from, const Vector3& aimBoneWorld);

} // namespace CollisionVis
