#include "../Interface/Utils/Helper/helper.hpp"
#include "../Core/Cache.hpp"
#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Interface/Utils/Variables/index.h"
#include "../Interface/Utils/Visuals/visuals.hpp"

void Engine::RenderEsp() {
    if (!var::enableesp)
        return;

    if (!entityStarted) return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    for (const auto& [key, actor] : playerCache) {
        if (!actor.Drawing) continue;
        if (actor.health < 1.0f || actor.health < 1) continue;

        if (var::box) {
            auto color = actor.isVisible ? ImColor(255, 255, 255, 255) : ImColor(255, 0, 0, 255);
			auto colortwo = actor.bIsDeathVerge ? ImColor(209, 92, 255, 255) : color;
            Visuals::Box(actor.ScreenTop, actor.ScreenBottom, actor.isVisible, colortwo, 1);
        }

        if (var::health)
        {
            auto BottomPos = actor.ScreenBottom;
            auto TopPos = actor.ScreenTop;

            Visuals::HealthBar(
                TopPos.x - (fabsf(TopPos.y - BottomPos.y) * 0.65f) / 2.0f,
                TopPos.y,
                BottomPos.y,
                actor.health
            );
        }

        if (var::names) {
            auto BottomPos = actor.ScreenBottom;
            auto TopPos = actor.ScreenTop;

            float boxHeight = fabsf(TopPos.y - BottomPos.y);
            float boxWidth = boxHeight * 0.65f;

            Visuals::Names(
                actor.ActorName,
                TopPos.x,
                TopPos.y,
                boxWidth,
                ImColor(255, 255, 255, 255)
            );
        }

        if (var::snaplines) {
            ImVec2 screenCenter(ImGui::GetIO().DisplaySize.x / 2.0f,
                ImGui::GetIO().DisplaySize.y);

            auto snapLinesColor = ImColor(255, 255, 255);

            auto color = actor.isVisible ? snapLinesColor : ImColor(255, 0, 0, 255);
            drawList->AddLine(screenCenter,
                ImVec2(actor.ScreenBottom.x, actor.ScreenBottom.y),
                color,
                1.0f);
        }

        if (var::skeleton) {
            auto& boneData = actor.boneData;
            auto color = actor.isVisible ? ImColor(255, 255, 255, 255) : ImColor(255, 0, 0, 255);
            auto colortwo = actor.bIsDeathVerge ? ImColor(209, 92, 255, 255) : color;
            for (auto& [a, b] : SkeletonLinksArcRaiders)
            {
                size_t ia = (size_t)a;
                size_t ib = (size_t)b;

                if (!boneData.valid.test(ia) || !boneData.valid.test(ib))
                    continue;

                Visuals::SnapLinesDouble(
                    boneData.bonesDouble[ia],
                    boneData.bonesDouble[ib],
                    colortwo,
                    1.0f
                );
            }

        }

    }
}