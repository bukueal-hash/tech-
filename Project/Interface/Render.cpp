#include <Windows.h>
#include <cstdarg>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include "../ThirdParty/ImGui/imgui.h"
#include "../ThirdParty/ImGui/imgui_impl_win32.h"
#include "../ThirdParty/ImGui/imgui_impl_dx11.h"
#include "Utils/Variables/index.h"
#include "../Core/Engine.h"
#include "Overlay/Menu.h"
#include "OverlayHost.h"
#include "Utils/AutoConfig.h"
#include "Utils/Visuals/visuals.hpp"

bool showmenu = false;
bool requestExit = false;
Engine engine = Engine();

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

    if (var::show_debug_overlay && !showmenu) {
        DrawDebugOffsetValidation(engine);
        engine.PrintVisCheckDebugConsole();

        // Top-center flicker flash banner + last events (readable when ESP blanks).
        const Engine::FlickerDebugSnapshot flick = engine.GetFlickerDebug();
        ImDrawList* fdl = ImGui::GetForegroundDrawList();
        ImFont* font = ImGui::GetFont();
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (flick.lastReason[0] && flick.lastAgeMs >= 0 && flick.lastAgeMs < 5000
            && engine.IsEspRaidActive()) {
            const bool isHitch = std::strncmp(flick.lastReason, "hitch_", 6) == 0
                || std::strncmp(flick.lastReason, "pc_drop", 7) == 0;
            char banner[160];
            snprintf(banner, sizeof(banner), "%s: %s  (%lld ms ago)  total=%u",
                isHitch ? "FREEZE/HITCH" : "FLICKER",
                flick.lastReason,
                static_cast<long long>(flick.lastAgeMs),
                flick.total);
            const ImVec2 sz = font->CalcTextSizeA(22.f, FLT_MAX, 0.f, banner);
            const float bx = (ds.x - sz.x) * 0.5f;
            const float by = 8.f;
            fdl->AddRectFilled(
                ImVec2(bx - 10.f, by - 4.f),
                ImVec2(bx + sz.x + 10.f, by + sz.y + 6.f),
                isHitch ? IM_COL32(200, 80, 0, 230) : IM_COL32(180, 20, 20, 230));
            fdl->AddText(font, 22.f, ImVec2(bx, by), IM_COL32(255, 255, 80, 255), banner);
        }
    }

    if (requestExit) {
        AutoConfig_SaveNow();
        requestExit = false;
        SendMessage(hwnd, WM_CLOSE, 0, 0);
        return;
    }
}

static void DrawDebugOffsetValidation(Engine& eng)
{
    const float pad = 12.f;
    const float lineH = 20.f;
    const float colW = 360.f;
    const float spacing = 20.f;
    // Col1 grew (GWorldRaw/Step + vis rows); size panels to content — no clipped border box.
    const int rowsPerCol = 34;
    const float h = lineH * rowsPerCol + pad * 2.f;
    const float x1 = 20.f;
    const float x2 = x1 + colW + spacing;
    const float x3 = x2 + colW + spacing;
    const float y = 20.f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    for (float xPos : {x1, x2, x3}) {
        dl->AddRectFilled(ImVec2(xPos, y), ImVec2(xPos + colW, y + h), IM_COL32(0, 0, 0, 220));
    }

    ImFont* font = ImGui::GetFont();
    const float fontSize = 14.f;
    const float headerSize = 16.f;

    auto drawRow = [&](float xPos, int row, const char* label, bool ok, const char* fmt, ...) {
        float ry = y + pad + row * lineH;
        ImU32 col = ok ? IM_COL32(60, 255, 100, 255) : IM_COL32(255, 80, 80, 255);
        char buf[128];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        char line[256];
        snprintf(line, sizeof(line), "%s: %s", label, buf);
        dl->AddText(font, fontSize, ImVec2(xPos + pad, ry), col, line);
    };

    const uintptr_t base = Memory::getBaseAddress();
    const Engine::EngineStateSnapshot state = eng.GetStateSnapshot();
    bool ok;

    {
        float xPos = x1;
        int row = 0;
        dl->AddText(font, headerSize, ImVec2(xPos + pad, y + pad), IM_COL32(255, 200, 100, 255), "CORE POINTERS");
        row++;

        ok = base != 0;
        drawRow(xPos, row++, "Base", ok, "0x%llX", (unsigned long long)base);

        ok = state.gWorldRaw != 0;
        drawRow(xPos, row++, "GWorldRaw", ok, "0x%llX", (unsigned long long)state.gWorldRaw);

        static const char* kFail[] = {
            "ok", "null_slot", "bad_PL", "bad_actors", "soft_ok"
        };
        const int fs = state.gWorldFailStep;
        const char* failName = (fs >= 0 && fs <= 4) ? kFail[fs] : "?";
        ok = (fs == 0 || fs == 4);
        drawRow(xPos, row++, "GWorldStep", ok, "%s", failName);

        ok = state.gWorld != 0;
        drawRow(xPos, row++, "UWorld", ok, "0x%llX", (unsigned long long)state.gWorld);

        ok = state.owningGameInstance != 0;
        drawRow(xPos, row++, "OwningGI", ok, "0x%llX", (unsigned long long)state.owningGameInstance);

        ok = eng.IsValidPointer(state.localPlayer);
        drawRow(xPos, row++, "LocalPlayer", ok, "0x%llX", (unsigned long long)state.localPlayer);

        ok = eng.IsValidPointer(state.playerController);
        drawRow(xPos, row++, "PlayerCtrl", ok, "0x%llX", (unsigned long long)state.playerController);

        ok = eng.IsValidPointer(state.playerCameraManager);
        drawRow(xPos, row++, "CameraMgr", ok, "0x%llX", (unsigned long long)state.playerCameraManager);

        ok = eng.IsValidPointer(state.acknowledgedPawn);
        drawRow(xPos, row++, "AckPawn", ok, "0x%llX", (unsigned long long)state.acknowledgedPawn);

        ok = eng.IsValidPointer(state.rootComponent);
        drawRow(xPos, row++, "RootComp", ok, "0x%llX", (unsigned long long)state.rootComponent);

        ok = state.persistentLevel != 0;
        drawRow(xPos, row++, "PersistLvl", ok, "0x%llX", (unsigned long long)state.persistentLevel);

        ok = state.actors != 0;
        drawRow(xPos, row++, "Actors", ok, "0x%llX", (unsigned long long)state.actors);

        ok = eng.IsEntityStarted();
        drawRow(xPos, row++, "EntityStart", ok, "%s", ok ? "YES" : "NO");

        ok = eng.IsInRaidRaw();
        drawRow(xPos, row++, "RaidRaw", ok, "%s", ok ? "YES" : "NO");

        ok = eng.IsEspRaidActive();
        drawRow(xPos, row++, "EspRaid", ok, "%s", ok ? "YES" : "NO");

        const size_t playerCacheSz = eng.PlayerCacheCount();
        ok = playerCacheSz > 0;
        drawRow(xPos, row++, "PlayerCache", ok, "%zu", playerCacheSz);

        const size_t drawTargets = eng.CountEspDrawablePlayers();
        ok = drawTargets > 0;
        drawRow(xPos, row++, "EspDraw", ok, "%zu", drawTargets);

        const size_t worldCacheSz = eng.WorldCacheCount();
        ok = worldCacheSz > 0;
        drawRow(xPos, row++, "WorldCache", ok, "%zu", worldCacheSz);

        const size_t worldDrawSz = eng.CountWorldDrawable();
        ok = worldDrawSz > 0;
        drawRow(xPos, row++, "WorldDraw", ok, "%zu", worldDrawSz);

        const size_t robotDrawSz = eng.CountRobotDrawable();
        ok = robotDrawSz > 0;
        drawRow(xPos, row++, "BotDraw", ok, "%zu", robotDrawSz);

        const Engine::VisCheckDebugStats visDbg = eng.CollectVisCheckDebugStats();
        drawRow(xPos, row++, "MeshVisMode", true, "%s",
            var::visiblecheck ? "render_time" : "off");
        drawRow(xPos, row++, "PlrMeshVis", visDbg.playersTotal > 0,
            "%d/%d", visDbg.playersMeshVisible, visDbg.playersTotal);
        drawRow(xPos, row++, "BotMeshVis", visDbg.botsTotal > 0,
            "%d/%d", visDbg.botsMeshVisible, visDbg.botsTotal);
        if (visDbg.hasSample) {
            drawRow(xPos, row++, "VisSample", visDbg.sampleVisible,
                "on=%d rec=%d s=%.2f",
                visDbg.sampleOnScreen ? 1 : 0,
                visDbg.sampleRecent ? 1 : 0,
                visDbg.sampleSubmit);
        }

        int actorCount = 0;
        if (state.persistentLevel) {
            actorCount = Memory::read<int>(state.persistentLevel + Offsets::AActors + 8);
        }
        ok = actorCount > 0 && actorCount < 10000;
        drawRow(xPos, row++, "ActorCount", ok, "%d", actorCount);

        Engine::CameraCache cam{};
        {
            std::shared_lock<std::shared_mutex> lock(eng.m_cameraMutex);
            cam = eng.g_Camera;
        }
        ok = IsUsableCameraFov(cam.FOV);
        drawRow(xPos, row++, "CameraRead", ok, "FOV:%.1f", cam.FOV);

        const Engine::FlickerDebugSnapshot flick = eng.GetFlickerDebug();
        ok = flick.lastAgeMs < 0 || flick.lastAgeMs > 2000;
        drawRow(xPos, row++, "FlickerN", flick.total == 0, "%u", flick.total);
        if (flick.lastReason[0]) {
            drawRow(xPos, row++, "FlickerLast", ok, "%s", flick.lastReason);
            drawRow(xPos, row++, "FlickerAge", ok, "%lldms",
                static_cast<long long>(flick.lastAgeMs));
        }
        // Show up to 3 newest events (newest last in list).
        const int showN = flick.recentCount < 3 ? flick.recentCount : 3;
        for (int i = 0; i < showN; ++i) {
            const int idx = flick.recentCount - showN + i;
            const Engine::FlickerEvent& e = flick.recent[idx];
            char tag[32];
            snprintf(tag, sizeof(tag), "Flick#%d", i + 1);
            drawRow(xPos, row++, tag, true, "%s", e.reason);
        }
    }

    {
        float xPos = x2;
        int row = 0;
        dl->AddText(font, headerSize, ImVec2(xPos + pad, y + pad), IM_COL32(255, 200, 100, 255), "PLAYER OFFSETS");
        row++;

        ok = true;
        drawRow(xPos, row++, "ControlRot", ok, "0x%llX", (unsigned long long)Offsets::ControlRotation);
        ok = eng.IsUsermodePtr(eng.PlayerState);
        drawRow(xPos, row++, "PlayerState", ok, "0x%llX", (unsigned long long)eng.PlayerState);
        drawRow(xPos, row++, "PlayerNameOff", true, "0x%llX", (unsigned long long)Offsets::PlayerNamePrivate);
        drawRow(xPos, row++, "CharMovement", true, "0x%llX", (unsigned long long)Offsets::CharacterMovement);
        drawRow(xPos, row++, "HealthComp", true, "0x%llX", (unsigned long long)Offsets::HealthComponent);
        drawRow(xPos, row++, "Health", true, "0x%llX", (unsigned long long)Offsets::Health);
        drawRow(xPos, row++, "MaxHealth", true, "0x%llX", (unsigned long long)Offsets::MaxHealth);
        drawRow(xPos, row++, "Shield", true, "0x%llX", (unsigned long long)Offsets::Shield);
        drawRow(xPos, row++, "TeamID", true, "0x%llX", (unsigned long long)Offsets::TeamID);
        drawRow(xPos, row++, "RepMovement", true, "0x%llX", (unsigned long long)Offsets::ReplicatedMovement);
        drawRow(xPos, row++, "Velocity", true, "0x%llX", (unsigned long long)Offsets::Velocity);
        drawRow(xPos, row++, "CompToWorld", true, "0x%llX", (unsigned long long)Offsets::ComponentToWorld);
        drawRow(xPos, row++, "RelLocation", true, "0x%llX", (unsigned long long)Offsets::RelativeLocation);
    }

    {
        float xPos = x3;
        int row = 0;
        dl->AddText(font, headerSize, ImVec2(xPos + pad, y + pad), IM_COL32(255, 200, 100, 255), "COMPONENT OFFSETS");
        row++;

        ok = true;
        drawRow(xPos, row++, "SkeletalMesh", ok, "0x%llX", (unsigned long long)Offsets::USkeletalMeshComponent);
        drawRow(xPos, row++, "MeshAsset", ok, "0x%llX", (unsigned long long)Offsets::SkeletalMeshAsset);
        drawRow(xPos, row++, "BoundsScale", ok, "0x%llX", (unsigned long long)Offsets::BoundsScale);
        drawRow(xPos, row++, "LastRender", ok, "0x%llX", (unsigned long long)Offsets::LastRenderTime);
        drawRow(xPos, row++, "LastRenderScr", ok, "0x%llX", (unsigned long long)Offsets::LastRenderTimeOnScreen);
        drawRow(xPos, row++, "IsRendered", ok, "0x%llX", (unsigned long long)Offsets::IsRenderedTime);
        drawRow(xPos, row++, "ActorID", ok, "0x%llX", (unsigned long long)Offsets::ActorID);
        drawRow(xPos, row++, "LootInteract", ok, "0x%llX", (unsigned long long)Offsets::LootInteractionComponent);
        drawRow(xPos, row++, "LootSearched", ok, "0x%llX", (unsigned long long)Offsets::LootInteraction_Searched);
        drawRow(xPos, row++, "SimpleLootInteract", ok, "0x%llX", (unsigned long long)Offsets::SimpleLootActivity_LootInteraction);
        drawRow(xPos, row++, "ChosenMesh", ok, "0x%llX", (unsigned long long)Offsets::SalvageContainer_ChosenMesh);
        drawRow(xPos, row++, "UIHoverData", ok, "0x%llX", (unsigned long long)Offsets::UIHoverData);
        drawRow(xPos, row++, "IsBreaked", ok, "0x%llX", (unsigned long long)Offsets::bIsBreaked);
        drawRow(xPos, row++, "Inventory", ok, "0x%llX", (unsigned long long)Offsets::InventoryComponent);
    }
}
