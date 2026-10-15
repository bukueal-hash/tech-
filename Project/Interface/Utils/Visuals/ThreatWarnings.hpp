#pragma once
#include "../../../Core/Engine.h"
#include "../UpgradeOptions.hpp"
#include "../../../ThirdParty/ImGui/imgui.h"
#include <chrono>
#include <cmath>
#include <cstdio>

namespace ThreatWarnings {
inline uint64_t generation = UINT64_MAX, since[3]{};
inline void Clear() { for (auto& s : since) s = 0; generation = UINT64_MAX; }
inline void Draw(const Engine::EspRenderFrame& frame, const Engine::CameraCache& cam) {
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (generation != frame.worldGeneration) { for (auto& s : since) s = 0; generation = frame.worldGeneration; }
    if (!UpgradeOptions::warnings) { for (auto& s : since) s = 0; return; }
    int nearby = 0, behind = 0, closing = 0;
    const double yaw = cam.Rotation.y * 3.141592653589793 / 180.0;
    for (const auto& item : frame.players) {
        const auto& p = item.entry;
        if (!p.Drawing || p.isAlly || p.bIsDead || !p.positionSampleMs || now < p.positionSampleMs || now-p.positionSampleMs > 500) continue;
        const double x = p.WorldPos.x-cam.Location.x, y = p.WorldPos.y-cam.Location.y, z = p.WorldPos.z-cam.Location.z;
        const double cm = std::sqrt(x*x+y*y+z*z);
        if (!std::isfinite(cm) || cm < 1 || cm > UpgradeOptions::warningRange*100.0) continue;
        ++nearby;
        if (x*std::cos(yaw)+y*std::sin(yaw) < 0) ++behind;
        // Target radial velocity, not relative velocity: local motion is not cached here.
        const double approach = -(x*p.cachedVelocity.x+y*p.cachedVelocity.y+z*p.cachedVelocity.z)/cm/100.0;
        if (p.lastVelocityUpdate && now >= p.lastVelocityUpdate && now-p.lastVelocityUpdate <= 500
            && std::isfinite(approach) && approach >= UpgradeOptions::warningClosing) ++closing;
    }
    const bool on[] = { nearby > 0, UpgradeOptions::warningBehind && behind > 0, UpgradeOptions::warningApproach && closing > 0 };
    const int counts[] = { nearby, behind, closing };
    const char* labels[] = {"Nearby enemies", "Enemies behind", "Moving toward your position"};
    float y = 40.f;
    for (int i=0; i<3; ++i) {
        if (!on[i]) { since[i] = 0; continue; }
        if (!since[i]) since[i] = now;
        if (now-since[i] < UpgradeOptions::warningDebounce*1000.f) continue;
        char text[96]; std::snprintf(text, sizeof(text), "%s: %d", labels[i], counts[i]);
        ImGui::GetForegroundDrawList()->AddText(ImVec2(12.f,y), IM_COL32(255,170,80,240), text);
        y += ImGui::GetFontSize()+4.f;
    }
}
} // namespace ThreatWarnings
