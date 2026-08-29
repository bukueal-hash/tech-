#pragma once

#include "../../ThirdParty/ImGui/imgui.h"
#include <d3d11.h>

struct ID3D11Device;

void DrawArcSidebar(bool& menuOpen, bool& requestExit);

void InitArcMenuAssets(ID3D11Device* device);

void ArcMenuRestartApplication();
void ArcMenuResetController();
void ArcMenuResetKmBox();

namespace arc_ui {
void DrawArcEspTab();
void DrawArcRadarTab();
void DrawArcAimbotTab();
void DrawArcSettingsTab();
void DrawArcHelpTab();
void DrawArcDebugContent();
void DrawArcTriggerbotContent();
void DrawArcLootContent();
}

void ArcMenuAddVerticalSpacing(float spacing);
void ArcMenuHoverTooltip(const char* txt);

class ScopedStyleColor {
public:
    ScopedStyleColor(ImGuiCol idx, const ImVec4& col) : idx_(idx) { ImGui::PushStyleColor(idx_, col); }
    ~ScopedStyleColor() { ImGui::PopStyleColor(); }

private:
    ImGuiCol idx_;
};
