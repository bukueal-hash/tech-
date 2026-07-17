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
void DrawArcLootTab();
void DrawArcRadarTab();
void DrawArcAimbotTab();
void DrawArcVisTab();
void DrawArcSettingsTab();
void DrawArcDebugTab();
void DrawArcHelpTab();
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
