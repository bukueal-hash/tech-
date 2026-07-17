#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include "../ThirdParty/ImGui/imgui.h"
#include "../ThirdParty/ImGui/imgui_impl_win32.h"
#include "../ThirdParty/ImGui/imgui_impl_dx11.h"
#include "Utils/Variables/index.h"
#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "Overlay/Menu.h"
#include "OverlayHost.h"
#include "Utils/AutoConfig.h"
#include "Utils/Visuals/visuals.hpp"

bool showmenu = false;
bool requestExit = false;
Engine engine = Engine();

static void DrawNearLootHud(Engine& eng);

void Render(HWND hwnd)
{
    (void)hwnd;

    {
        static bool insertWasDown = false;
        const bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (insertDown && !insertWasDown) {
            const bool closingMenu = showmenu;
            showmenu = !showmenu;
            if (closingMenu)
                AutoConfig_SaveNow();
        }
        insertWasDown = insertDown;
    }

    if (showmenu) {
        ApplyOverlayMode(hwnd, true);
        DrawArcSidebar(showmenu, requestExit);
    } else {
        ApplyOverlayMode(hwnd, false);
        if (engine.IsEspRaidActive()) {
            engine.RenderFovCircle();
            engine.RenderEsp();
        }
    }

    if (engine.IsEspRaidActive()) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (ds.x > 0.f && ds.y > 0.f)
            engine.SetProjectionViewport(ds.x, ds.y);
    }

    if (engine.IsEspRaidActive() && var::show_radar)
        engine.RenderRadar(showmenu);

    if (engine.IsEspRaidActive() && !showmenu && var::show_near_loot_hud)
        DrawNearLootHud(engine);

    if (requestExit) {
        AutoConfig_SaveNow();
        requestExit = false;
        SendMessage(hwnd, WM_CLOSE, 0, 0);
        return;
    }
}

static void DrawNearLootHud(Engine& eng)
{
    static std::vector<Engine::GroundPickupHudRow> s_rows;
    static auto s_lastCollect = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (s_lastCollect.time_since_epoch().count() == 0
        || now - s_lastCollect >= std::chrono::milliseconds(200)) {
        s_lastCollect = now;
        eng.CollectDrawingGroundPickups(s_rows);
    }

    {
        static bool f7WasDown = false;
        const bool f7Down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7Down && !f7WasDown && !s_rows.empty())
            eng.UserConfirmGroundItemPicked(s_rows.front().key);
        f7WasDown = f7Down;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl)
        return;

    const float x = 18.f;
    float y = 120.f;
    dl->AddText(ImVec2(x, y), IM_COL32(255, 200, 80, 255), "Near loot (F7 = mark nearest picked)");
    y += 18.f;

    if (s_rows.empty()) {
        dl->AddText(ImVec2(x, y), IM_COL32(180, 180, 180, 200), "(none drawing)");
        return;
    }

    const int maxShow = (std::min)(static_cast<int>(s_rows.size()), 12);
    for (int i = 0; i < maxShow; ++i) {
        const auto& row = s_rows[static_cast<size_t>(i)];
        char line[256];
        const char* name = row.name.empty() ? "?" : row.name.c_str();
        std::snprintf(
            line,
            sizeof(line),
            "%s%2d  %.0fm  %s",
            i == 0 ? "> " : "  ",
            i + 1,
            row.distM,
            name);
        const ImU32 col = i == 0 ? IM_COL32(120, 255, 140, 255) : IM_COL32(230, 230, 230, 220);
        dl->AddText(ImVec2(x, y), col, line);
        y += 16.f;
    }
}
