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

    // Radar map background (D16). Separate SRV from the logo so the two can
    // never overwrite each other; 0 means no map image was found on disk.
    ImTextureID mapTexture = 0;
    int mapWidth = 0;
    int mapHeight = 0;
};

ArcMenuUi& ArcMenuTheme();
