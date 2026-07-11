#pragma once

#include "../../ThirdParty/ImGui/imgui.h"

struct ArcMenuUi {
    ImVec4 menuAccentColor{ 0.95f, 0.2f, 0.2f, 1.0f };
    ImVec4 headerColor{ 0.4f, 0.08f, 0.08f, 1.0f };

    ImFont* logoFont = nullptr;
    ImFont* headerFont = nullptr;
    ImFont* regularFont = nullptr;

    ImTextureID logoTexture = 0;
    int logoWidth = 0;
    int logoHeight = 0;
};

ArcMenuUi& ArcMenuTheme();
