#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cfloat>
#include <cstdio>
#include <cstring>
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
#include "../Functions/CollisionVis.h"

bool showmenu = false;
bool requestExit = false;
Engine engine = Engine();

static void DrawNearLootHud(Engine& eng);
static void DrawDebugOffsetValidation(Engine& eng);

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
    }

    // ESP/FOV/crosshair stay live under the open menu (raid gate only).
    if (engine.IsEspRaidActive()) {
        engine.RenderFovCircle();
        engine.RenderOverlayCrosshair();
        engine.RenderEsp();
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

    if (engine.IsEspRaidActive() && !showmenu && var::vis_debug && var::vis_debug_rays) {
        std::vector<CollisionVis::DebugRay> rays;
        CollisionVis::CopyDebugRays(rays);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        if (dl) {
            for (const auto& r : rays) {
                Vector3 sa{}, sb{};
                if (!engine.ProjectWorldLocationToScreen(r.from, sa))
                    continue;
                if (!engine.ProjectWorldLocationToScreen(r.blocked ? r.hitPos : r.to, sb))
                    continue;
                const ImU32 col = r.blocked ? IM_COL32(255, 60, 60, 220) : IM_COL32(60, 255, 90, 200);
                dl->AddLine(ImVec2((float)sa.x, (float)sa.y), ImVec2((float)sb.x, (float)sb.y), col, 1.5f);
            }
        }
    }

    if (var::show_debug_overlay)
        DrawDebugOffsetValidation(engine);

    if (requestExit) {
        AutoConfig_SaveNow();
        requestExit = false;
        SendMessage(hwnd, WM_CLOSE, 0, 0);
        return;
    }
}

static ImU32 HealthColor(int grade)
{
    // 2=G, 1=Y, 0=R
    if (grade >= 2)
        return IM_COL32(80, 220, 100, 255);
    if (grade == 1)
        return IM_COL32(230, 200, 60, 255);
    return IM_COL32(230, 70, 70, 255);
}

static const char* HealthLetter(int grade)
{
    if (grade >= 2)
        return "G";
    if (grade == 1)
        return "Y";
    return "R";
}

static void DrawDebugOffsetValidation(Engine& eng)
{
    const Engine::EngineStateSnapshot snap = eng.GetStateSnapshot();
    Engine::CameraCache cam{};
    {
        std::shared_lock<std::shared_mutex> lock(eng.m_cameraMutex, std::try_to_lock);
        if (lock.owns_lock())
            cam = eng.g_Camera;
    }

    const bool raid = eng.IsEspRaidActive();
    const size_t players = eng.PlayerCacheCount();
    const size_t bots = eng.RobotCacheCount();
    const size_t items = eng.ItemCacheCount();
    const size_t world = eng.WorldCacheCount();
    const CollisionVis::Stats vis = CollisionVis::GetStats();

    uint64_t dmaExec = 0, dmaPrep = 0, dmaLast = 0;
    DmaScatterStats_Get(dmaExec, dmaPrep, dmaLast);

    auto gradePtr = [](uintptr_t p) -> int { return p ? 2 : 0; };
    const int gWorldG = snap.gWorldFailStep == 0 && snap.gWorld ? 2
        : (snap.gWorldRaw ? 1 : 0);
    const int pcG = gradePtr(snap.playerController);
    const int pawnG = gradePtr(snap.acknowledgedPawn);
    const int pcmG = gradePtr(snap.playerCameraManager);
    const int camG = (cam.FOV > 5.f && cam.FOV < 170.f
        && (std::abs(cam.Location.x) + std::abs(cam.Location.y) + std::abs(cam.Location.z)) > 10.0)
        ? 2
        : (raid ? 1 : 0);
    const int playerG = !raid ? 0 : (players > 0 ? 2 : 1);
    const int botG = !raid ? 0 : (bots > 0 ? 2 : 1);
    const int itemG = !raid ? 0 : (items > 0 ? 2 : 1);
    const int worldG = !raid ? 0 : (world > 0 ? 2 : 1);
    const int visG = static_cast<int>(vis.probe);

    if (ImDrawList* dl = ImGui::GetForegroundDrawList()) {
        const float x = 10.f;
        float y = 48.f;
        char line[192];
        auto row = [&](const char* name, int grade, const char* detail) {
            std::snprintf(line, sizeof(line), "[%s] %-8s %s",
                HealthLetter(grade), name, detail ? detail : "");
            dl->AddText(ImVec2(x, y), HealthColor(grade), line);
            y += 16.f;
        };

        dl->AddText(ImVec2(x, y), IM_COL32(200, 200, 200, 255),
            "Offset health (G/Y/R)");
        y += 18.f;

        char detail[96];
        std::snprintf(detail, sizeof(detail), "step=%d", snap.gWorldFailStep);
        row("GWorld", gWorldG, detail);
        row("PC", pcG, nullptr);
        row("Pawn", pawnG, nullptr);
        row("PCM", pcmG, nullptr);
        std::snprintf(detail, sizeof(detail), "fov=%.1f", cam.FOV);
        row("Camera", camG, detail);
        std::snprintf(detail, sizeof(detail), "n=%zu", players);
        row("Players", playerG, detail);
        std::snprintf(detail, sizeof(detail), "n=%zu", bots);
        row("Bots", botG, detail);
        std::snprintf(detail, sizeof(detail), "n=%zu", items);
        row("Items", itemG, detail);
        std::snprintf(detail, sizeof(detail), "n=%zu", world);
        row("World", worldG, detail);
        std::snprintf(detail, sizeof(detail), "smc=%d tris=%d %dms",
            vis.smc, vis.tris, vis.rebuildMs);
        row("Vis", visG, detail);
        std::snprintf(detail, sizeof(detail), "exec=%llu prep=%llu last=%llu",
            (unsigned long long)dmaExec,
            (unsigned long long)dmaPrep,
            (unsigned long long)dmaLast);
        row("DMA", dmaExec > 0 ? 2 : (raid ? 1 : 0), detail);
    }

    static auto s_lastConsole = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (s_lastConsole.time_since_epoch().count() == 0
        || now - s_lastConsole >= std::chrono::seconds(1)) {
        s_lastConsole = now;
        std::printf(
            "[debugOffset] raid=%d GWorld=%s PC=%s Pawn=%s PCM=%s Cam=%s "
            "Players=%s(%zu) Bots=%s(%zu) Items=%s(%zu) World=%s(%zu) Vis=%s "
            "dmaExec=%llu\n",
            raid ? 1 : 0,
            HealthLetter(gWorldG), HealthLetter(pcG), HealthLetter(pawnG),
            HealthLetter(pcmG), HealthLetter(camG),
            HealthLetter(playerG), players,
            HealthLetter(botG), bots,
            HealthLetter(itemG), items,
            HealthLetter(worldG), world,
            HealthLetter(visG),
            (unsigned long long)dmaExec);
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
