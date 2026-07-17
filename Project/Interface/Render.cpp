#include <Windows.h>
#include <chrono>
#include <cstdarg>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <fstream>
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
#include "../Core/AgentLog.h"

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
    // G1/H9: blocking shared_lock on paint while workers hold caches (or wait
    // for unique) stalled Present 595-849ms (overlayMs==paint_gap). Ghost flash
    // = delayed Present after Clear. Never block: try_lock + keep last snap.
    struct OverlaySnap {
        uintptr_t base = 0;
        Engine::EngineStateSnapshot state{};
        size_t playerCacheSz = 0;
        size_t drawTargets = 0;
        size_t worldCacheSz = 0;
        size_t worldDrawSz = 0;
        size_t robotDrawSz = 0;
        Engine::VisCheckDebugStats visDbg{};
        int actorCount = 0;
        float camFov = 0.f;
        uintptr_t playerState = 0;
    };
    static OverlaySnap s_snap{};
    static auto s_lastHeavy = std::chrono::steady_clock::time_point{};
    const auto nowHeavy = std::chrono::steady_clock::now();
    if (s_lastHeavy.time_since_epoch().count() == 0
        || nowHeavy - s_lastHeavy >= std::chrono::milliseconds(500)) {
        const auto t0 = std::chrono::steady_clock::now();
        int64_t msState = 0, msFrame = 0, msPlayer = 0, msWorld = 0, msRobot = 0, msCam = 0;
        int gotState = 0, gotFrame = 0, gotPlayer = 0, gotWorld = 0, gotRobot = 0, gotCam = 0;
        OverlaySnap next = s_snap;
        next.base = Memory::getBaseAddress();
        next.actorCount = 0;

        {
            const auto tA = std::chrono::steady_clock::now();
            std::shared_lock<std::shared_mutex> lock(eng.m_stateMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                gotState = 1;
                next.state.gWorld = eng.GWorld;
                next.state.gWorldRaw = eng.m_gWorldRaw.load(std::memory_order_relaxed);
                next.state.gWorldFailStep = eng.m_gWorldFailStep.load(std::memory_order_relaxed);
                next.state.persistentLevel = eng.PersistentLevel;
                next.state.actors = eng.Actors;
                next.state.playerController = eng.PlayerController;
                next.state.acknowledgedPawn = eng.AcknowledgedPawn;
                next.state.rootComponent = eng.RootComponent;
                next.state.playerCameraManager = eng.PlayerCameraManager;
                next.state.owningGameInstance = eng.OwningGameInstance;
                next.state.localPlayer = eng.localplayer;
                next.playerState = eng.PlayerState;
            }
            msState = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tA).count();
        }
        {
            const auto tA = std::chrono::steady_clock::now();
            std::shared_lock<std::shared_mutex> lock(eng.m_espFrameMutex, std::try_to_lock);
            if (lock.owns_lock() && eng.m_lastEspFrame.valid) {
                gotFrame = 1;
                next.drawTargets = eng.m_lastEspFrame.players.size();
                next.robotDrawSz = eng.m_lastEspFrame.robots.size();
                next.worldDrawSz = eng.m_lastEspFrame.world.size();
            }
            msFrame = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tA).count();
        }
        {
            const auto tA = std::chrono::steady_clock::now();
            std::shared_lock<std::shared_mutex> lock(eng.m_playerCacheMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                gotPlayer = 1;
                next.playerCacheSz = eng.playerCache.size();
                next.visDbg.playersTotal = 0;
                next.visDbg.playersMeshVisible = 0;
                size_t drawN = 0;
                for (const auto& [key, actor] : eng.playerCache) {
                    (void)key;
                    if (!actor.Drawing)
                        continue;
                    if (!(actor.isAlly && var::hide_allies))
                        ++drawN;
                    ++next.visDbg.playersTotal;
                    if (actor.isVisible)
                        ++next.visDbg.playersMeshVisible;
                }
                if (!gotFrame)
                    next.drawTargets = drawN;
            }
            msPlayer = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tA).count();
        }
        {
            const auto tA = std::chrono::steady_clock::now();
            size_t contN = 0, itemN = 0, contDraw = 0, itemDraw = 0;
            bool okC = false, okI = false;
            {
                std::shared_lock<std::shared_mutex> lock(eng.m_containerCacheMutex, std::try_to_lock);
                if (lock.owns_lock()) {
                    okC = true;
                    contN = eng.containerCache.size();
                    for (const auto& [key, e] : eng.containerCache) {
                        (void)key;
                        if (e.Drawing)
                            ++contDraw;
                    }
                }
            }
            {
                std::shared_lock<std::shared_mutex> lock(eng.m_itemCacheMutex, std::try_to_lock);
                if (lock.owns_lock()) {
                    okI = true;
                    itemN = eng.itemCache.size();
                    for (const auto& [key, e] : eng.itemCache) {
                        (void)key;
                        if (e.Drawing)
                            ++itemDraw;
                    }
                }
            }
            if (okC || okI) {
                gotWorld = 1;
                if (okC && okI)
                    next.worldCacheSz = contN + itemN;
                else if (okC)
                    next.worldCacheSz = contN;
                else
                    next.worldCacheSz = itemN;
                if (!gotFrame)
                    next.worldDrawSz = contDraw + itemDraw;
            }
            msWorld = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tA).count();
        }
        {
            const auto tA = std::chrono::steady_clock::now();
            std::shared_lock<std::shared_mutex> lock(eng.m_robotCacheMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                gotRobot = 1;
                next.visDbg.botsTotal = 0;
                next.visDbg.botsMeshVisible = 0;
                size_t drawN = 0;
                for (const auto& [key, entry] : eng.robotCache) {
                    (void)key;
                    if (!entry.Drawing)
                        continue;
                    ++drawN;
                    ++next.visDbg.botsTotal;
                    if (entry.isVisible)
                        ++next.visDbg.botsMeshVisible;
                }
                if (!gotFrame)
                    next.robotDrawSz = drawN;
            }
            msRobot = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tA).count();
        }
        {
            const auto tA = std::chrono::steady_clock::now();
            std::shared_lock<std::shared_mutex> lock(eng.m_cameraMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                gotCam = 1;
                next.camFov = eng.g_Camera.FOV;
            }
            msCam = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tA).count();
        }

        s_snap = next;
        // Always advance timer so a busy-lock frame cannot retry-spam every paint.
        s_lastHeavy = nowHeavy;

        const int64_t totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        // #region agent log
#if ARC_AGENT_NDJSON
        // Never ofstream on paint — async queue only (CreateFile stalled Present).
        if (totalMs > 20) {
            ArcAgentLog(
                "ghost-flicker",
                "G1",
                "Render.cpp:DrawDebugOffsetValidation",
                "overlay_try_refresh",
                std::string("{\"totalMs\":") + std::to_string(totalMs)
                    + ",\"msState\":" + std::to_string(msState)
                    + ",\"msFrame\":" + std::to_string(msFrame)
                    + ",\"msPlayer\":" + std::to_string(msPlayer)
                    + ",\"msWorld\":" + std::to_string(msWorld)
                    + ",\"msRobot\":" + std::to_string(msRobot)
                    + ",\"msCam\":" + std::to_string(msCam)
                    + ",\"gotState\":" + std::to_string(gotState)
                    + ",\"gotFrame\":" + std::to_string(gotFrame)
                    + ",\"gotPlayer\":" + std::to_string(gotPlayer)
                    + ",\"gotWorld\":" + std::to_string(gotWorld)
                    + ",\"gotRobot\":" + std::to_string(gotRobot)
                    + ",\"gotCam\":" + std::to_string(gotCam)
                    + "}");
        }
#endif // ARC_AGENT_NDJSON
        // #endregion
        (void)totalMs;
        (void)msState;
        (void)msFrame;
        (void)msPlayer;
        (void)msWorld;
        (void)msRobot;
        (void)msCam;
        (void)gotState;
        (void)gotFrame;
        (void)gotPlayer;
        (void)gotWorld;
        (void)gotRobot;
        (void)gotCam;
    }
    const OverlaySnap& snap = s_snap;

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
    const float y = 55.f;

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

    const uintptr_t base = snap.base;
    const Engine::EngineStateSnapshot& state = snap.state;
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

        ok = snap.playerCacheSz > 0;
        drawRow(xPos, row++, "PlayerCache", ok, "%zu", snap.playerCacheSz);

        ok = snap.drawTargets > 0;
        drawRow(xPos, row++, "EspDraw", ok, "%zu", snap.drawTargets);

        ok = snap.worldCacheSz > 0;
        drawRow(xPos, row++, "WorldCache", ok, "%zu", snap.worldCacheSz);

        ok = snap.worldDrawSz > 0;
        drawRow(xPos, row++, "WorldDraw", ok, "%zu", snap.worldDrawSz);

        ok = snap.robotDrawSz > 0;
        drawRow(xPos, row++, "BotDraw", ok, "%zu", snap.robotDrawSz);

        const Engine::VisCheckDebugStats& visDbg = snap.visDbg;
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

        ok = snap.actorCount > 0 && snap.actorCount < 10000;
        drawRow(xPos, row++, "ActorCount", ok, "%d", snap.actorCount);

        ok = IsUsableCameraFov(snap.camFov);
        drawRow(xPos, row++, "CameraRead", ok, "FOV:%.1f", snap.camFov);
    }

    {
        float xPos = x2;
        int row = 0;
        dl->AddText(font, headerSize, ImVec2(xPos + pad, y + pad), IM_COL32(255, 200, 100, 255), "PLAYER OFFSETS");
        row++;

        ok = true;
        drawRow(xPos, row++, "ControlRot", ok, "0x%llX", (unsigned long long)Offsets::ControlRotation);
        ok = eng.IsUsermodePtr(snap.playerState);
        drawRow(xPos, row++, "PlayerState", ok, "0x%llX", (unsigned long long)snap.playerState);
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
