#pragma once
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include "../UpgradeOptions.hpp"
#include "../../../ThirdParty/ImGui/imgui.h"
namespace SystemHud {
inline void Draw() {
    if (!UpgradeOptions::systemHud) return;
    static auto last = std::chrono::steady_clock::time_point{};
    static uint64_t previousIdle = 0, previousTotal = 0;
    static float cpu = -1.f;
    const auto now = std::chrono::steady_clock::now();
    if (UpgradeOptions::hudCpu && now-last >= std::chrono::seconds(1)) {
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            auto value = [](FILETIME t) { return (static_cast<uint64_t>(t.dwHighDateTime)<<32) | t.dwLowDateTime; };
            const uint64_t i = value(idle), total = value(kernel)+value(user);
            if (previousTotal && total>previousTotal && i>=previousIdle && i-previousIdle<=total-previousTotal)
                cpu = 100.f*(1.f-static_cast<float>(i-previousIdle)/static_cast<float>(total-previousTotal));
            previousIdle=i; previousTotal=total;
        } else { cpu=-1.f; previousTotal=previousIdle=0; }
        last=now;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x-12.f,12.f),ImGuiCond_Always,ImVec2(1.f,0.f));
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGui::Begin("##system_hud", nullptr, ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoInputs|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing);
    if (UpgradeOptions::hudFps) ImGui::Text("Overlay %.0f FPS",ImGui::GetIO().Framerate);
    if (UpgradeOptions::hudCpu) {
        if (cpu>=0.f) ImGui::Text("System CPU %.0f%%",cpu); else ImGui::TextUnformatted("System CPU unavailable");
    }
    if (UpgradeOptions::hudGpu) ImGui::TextUnformatted("GPU usage unavailable");
    if (UpgradeOptions::hudResolution) ImGui::Text("Overlay %.0f x %.0f",ImGui::GetIO().DisplaySize.x,ImGui::GetIO().DisplaySize.y);
    if (UpgradeOptions::hudClock) {
        std::time_t t=std::time(nullptr); std::tm local{};
        if (localtime_s(&local,&t)==0) ImGui::Text("%02d:%02d:%02d",local.tm_hour,local.tm_min,local.tm_sec);
    }
    ImGui::End();
}
} // namespace SystemHud
