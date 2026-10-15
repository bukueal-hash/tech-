#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../ThirdParty/ImGui/imgui.h"
#include "../ThirdParty/ImGui/imgui_impl_win32.h"
#include "../ThirdParty/ImGui/imgui_impl_dx11.h"
#include "Utils/Variables/index.h"
#include "Utils/UpgradeOptions.hpp"
#include "Utils/Visuals/ThreatWarnings.hpp"
#include "Utils/Visuals/SystemHud.hpp"
#include "Overlay/AimbotDebugPanel.hpp"
#include "../Input/KeyBind.h"
#include "../Core/Engine.h"
#include "../Core/AgentLog.h"
#include "../Core/PlayerEspMissLog.hpp"
#include "../Core/FeaturePolicy.hpp"
#include "../Core/Memory.h"
#include "Overlay/Menu.h"
#include "OverlayHost.h"
#include "Utils/AutoConfig.h"
#include "Utils/Visuals/visuals.hpp"
#include "../Core/Offsets.h"

bool showmenu = false;
bool requestExit = false;
Engine engine = Engine();

static void DrawNearLootHud(Engine& eng);
static void DrawDebugOffsetValidation(Engine& eng);

// Debug panel timer: app-start clock plus a log of when each debug counter
// first went green. Entries are appended in the order they went green; the
// main line shows the latest one and the log keeps the per-run history.
static auto s_appStartTime = std::chrono::steady_clock::now();
// Max log entries kept in the panel (oldest dropped when full).
static constexpr int kGreenLogMax = 24;
struct GreenLogEntry {
    const char* name;
    double greenAt; // seconds since app start
};
static std::vector<GreenLogEntry> s_greenLog;

// Counters tracked for first-green timing, in panel row order. The parallel
// s_firstGreenAt array holds the seconds-since-start of each counter's first
// green frame (0 = not green yet); keep both index-aligned.
static const char* const kGreenNames[] = {
    "PlayerCache", "EspDraw", "PInit", "PDraw", "PFrame",
    "WorldCache", "WorldDraw", "BotCache", "BotDraw", "ActorCount",
};
static constexpr int kGreenCount = 10;
static_assert(sizeof(kGreenNames) / sizeof(kGreenNames[0]) ==
    static_cast<size_t>(kGreenCount), "kGreenNames/kGreenCount out of sync");
static double s_firstGreenAt[kGreenCount] = {};
// Seconds of the all-green moment; <0 = not all-green yet (file scope so the
// per-raid reset below can clear it).
static double s_allGreenAt = -1.0;

// Record that a counter just went green (first time only, in green order).
// Also writes a green_time event to the debug log so timings can be compared
// across runs (same JSON envelope as red_row below).
static void RecordGreen(const char* name, double appSec) {
    // Avoid duplicates: only add if the last entry isn't this same counter.
    if (!s_greenLog.empty() && s_greenLog.back().name == name)
        return;
    s_greenLog.push_back({name, appSec});
    if (static_cast<int>(s_greenLog.size()) > kGreenLogMax)
        s_greenLog.erase(s_greenLog.begin()); // drop oldest
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::ostringstream f;
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"green\","
      << "\"location\":\"Render.cpp\",\"message\":\"green_time\","
      << "\"data\":{\"name\":\"" << name
      << "\",\"appSec\":" << appSec
      << "},\"timestamp\":" << nowMs << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}

// Raid hotkeys (Block B). The overlay sets WS_EX_TRANSPARENT while the menu is
// closed, so it never receives keyboard messages — poll the physical state with
// GetAsyncKeyState instead. We cannot swallow the keystroke from the game, so
// every binding is Alt+letter: a plain letter would land in the game's own bind
// table mid-raid. Edge-detect on the letter so holding Alt and tapping N then M
// toggles both, while pressing N first and then Alt toggles nothing.
static void HandleRaidHotkeys()
{
    const bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

    static const struct { int vk; bool* flag; } kToggles[] = {
        { 'N', &var::show_near_loot_hud },
        { 'C', &var::show_crosshair },
        { 'M', &var::radar_map_mode },
        { 'G', &var::enable_world },
        { 'T', &var::enable_triggerbot },
        { 'H', &var::show_raid_hud },
    };
    static bool wasDown[IM_ARRAYSIZE(kToggles)] = {};
    for (size_t i = 0; i < IM_ARRAYSIZE(kToggles); ++i) {
        const bool down = (GetAsyncKeyState(kToggles[i].vk) & 0x8000) != 0;
        if (down && !wasDown[i] && altHeld)
            *kToggles[i].flag = !*kToggles[i].flag;
        wasDown[i] = down;
    }

    // Offset debug overlay (green timers): PICKABLE key — Settings -> Debug ->
    // "Debug overlay hotkey", directly under "Show offset validation".
    // Edge-detected, no Alt, saved in auto_config.ini (default F8).
    static bool wasDbgOverlayDown = false;
    const bool dbgOverlayDown = var::debug_overlay_key != 0
        && (GetAsyncKeyState(var::debug_overlay_key) & 0x8000) != 0;
    if (dbgOverlayDown && !wasDbgOverlayDown)
        var::show_debug_overlay = !var::show_debug_overlay;
    wasDbgOverlayDown = dbgOverlayDown;

    // Live zoom (B1): Alt+- / Alt+= step the same "World range" slider the menu
    // edits, so the menu and the hotkey can never disagree about range.
    if (var::show_radar) {
        static const struct { int vk; float dir; } kZoom[] = {
            { VK_OEM_MINUS, -1.f },
            { VK_OEM_PLUS,  1.f },
        };
        static bool zoomWasDown[IM_ARRAYSIZE(kZoom)] = {};
        for (size_t i = 0; i < IM_ARRAYSIZE(kZoom); ++i) {
            const bool down = (GetAsyncKeyState(kZoom[i].vk) & 0x8000) != 0;
            if (down && !zoomWasDown[i] && altHeld) {
                const float r = var::radar_range > 0.f ? var::radar_range : 100.f;
                var::radar_range = std::clamp(
                    r + kZoom[i].dir * (r * 0.15f),
                    20.f, var::kMaxDistanceSliderM);
            }
            zoomWasDown[i] = down;
        }
    }

    // Force rescan (B4): Alt+R raises a flag; Engine applies it on its own
    // thread with a cooldown so a held key cannot wipe the caches in a loop.
    static bool rescanWasDown = false;
    const bool rescanDown = (GetAsyncKeyState('R') & 0x8000) != 0;
    if (rescanDown && !rescanWasDown && altHeld)
        engine.RequestForceRescan();
    rescanWasDown = rescanDown;
}

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
                AutoConfig_SaveNowAsync();
        }
        insertWasDown = insertDown;
    }

    if (showmenu) {
        ApplyOverlayMode(hwnd, true);
        DrawArcSidebar(showmenu, requestExit);
    } else {
        ApplyOverlayMode(hwnd, false);
    }

    const bool raidActive = engine.IsEspRaidActive();
    if (!raidActive || showmenu || !UpgradeOptions::warnings) ThreatWarnings::Clear();
    if (raidActive && !showmenu)
        HandleRaidHotkeys();
    {
        static bool previousHeld = false;
        const bool held = UpgradeOptions::combatKey != 0 && KeyBindIsHeld(UpgradeOptions::combatKey);
        if (!raidActive || !UpgradeOptions::combatEnabled || showmenu)
            UpgradeOptions::combatActive = false;
        else if (UpgradeOptions::combatMode == 0)
            UpgradeOptions::combatActive = held;
        else if (held && !previousHeld)
            UpgradeOptions::combatActive = !UpgradeOptions::combatActive;
        previousHeld = held;
        if (UpgradeOptions::combatActive)
            ImGui::GetForegroundDrawList()->AddText(ImVec2(12.f, 12.f), IM_COL32(255, 190, 80, 255), "COMBAT MODE");
    }
    // ESP/FOV/crosshair hide while the menu is open (menu readability). T4.4's
    // practice reference is drawn here too, and returns immediately when the mode
    // is off -- one call site, so the mode cannot leak into an unrelated path.
    engine.RenderPracticeReference();
    if (raidActive && !showmenu) {
        engine.RenderFovCircle();
        engine.RenderOverlayCrosshair();
        engine.RenderEsp();
    }

    if (raidActive) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (ds.x > 0.f && ds.y > 0.f)
            engine.SetProjectionViewport(ds.x, ds.y);
    }

    // The objective brief shares this panel but has its own toggle, so the
    // panel must render when either one is enabled.
    if (raidActive && !showmenu && (var::show_raid_hud || var::show_objectives))
        engine.RenderRaidHud();

    if (raidActive && !showmenu && var::show_activity_feed)
        engine.RenderActivityFeed();

    if (raidActive && var::show_radar)
        engine.RenderRadar(showmenu);
    
    if (raidActive)
        engine.RenderSearch();
    
    if (raidActive && !showmenu && var::show_near_loot_hud && !UpgradeOptions::combatActive)
        DrawNearLootHud(engine);


    if (!showmenu) SystemHud::Draw();

    if (var::show_debug_overlay && !showmenu)
        DrawDebugOffsetValidation(engine);
    
    // Aimbot debug monitor panel (renders when visible)
    if (OverlayUI::AimbotDebugPanel::IsVisible())
        OverlayUI::AimbotDebugPanel::Render();

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
        // World generation the snapshot's counters belong to. Caches can
        // still hold the PREVIOUS raid's counts for a frame after the gen
        // bumps — stamping greens then produced a false "all green: 0.0s".
        uint64_t worldGen = 0;
        size_t drawTargets = 0;
        size_t worldCacheSz = 0;
        size_t worldDrawSz = 0;
        size_t robotDrawSz = 0;
        size_t robotCacheSz = 0;
        Engine::PlayerPipelineStats playerDiag{};
        int actorCount = 0;
        float camFov = 0.f;
        uintptr_t playerState = 0;
        std::string playerMissSummary;
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
                next.state.chain = eng.m_chain;   // same lock: the ladder's state
                next.state.gWorldRaw = eng.m_chain.worldRaw;
                next.state.gWorldFailStep = eng.m_chain.worldFailStep;
                next.state.persistentLevel = eng.PersistentLevel;
                next.state.actors = eng.Actors;
                next.actorCount = eng.ActorsCount;
                next.state.playerController = eng.PlayerController;
                next.state.acknowledgedPawn = eng.AcknowledgedPawn;
                next.state.rootComponent = eng.RootComponent;
                next.state.playerCameraManager = eng.PlayerCameraManager;
                next.state.owningGameInstance = eng.OwningGameInstance;
                next.state.localPlayer = eng.localplayer;
                next.playerState = eng.PlayerState;
                // SDK reflection walker + FString self-check (Core/Reflection.hpp).
                next.state.reflectOk = eng.m_reflectOk;
                next.state.reflectDepth = eng.m_reflectDepth;
                next.state.reflectProps = eng.m_reflectProps;
                next.state.reflectSample = eng.m_reflectSample;
                next.state.fstrOk = eng.m_fstrOk;
                next.state.fstrKey = eng.m_fstrKey;
                next.state.fstrText = eng.m_fstrText;
            }
            msState = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tA).count();
        }
        {
            const auto tA = std::chrono::steady_clock::now();
            std::shared_lock<std::shared_mutex> lock(eng.m_espFrameMutex, std::try_to_lock);
            if (lock.owns_lock() && eng.m_espFrameShared && eng.m_espFrameShared->valid) {
                gotFrame = 1;
                next.drawTargets = eng.m_espFrameShared->players.size()
                    + eng.m_espFrameShared->world.size()
                    + eng.m_espFrameShared->robots.size();
                next.robotDrawSz = eng.m_espFrameShared->robots.size();
                next.worldDrawSz = eng.m_espFrameShared->world.size();
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
                size_t drawN = 0;
                for (const auto& [key, actor] : eng.playerCache) {
                    (void)key;
                    if (!actor.Drawing)
                        continue;
                    if (!(actor.isAlly && var::hide_allies))
                        ++drawN;
                }
                if (!gotFrame)
                    next.drawTargets = drawN;
                {
                    std::shared_lock<std::shared_mutex> dl(eng.m_playerDiagMutex,
                        std::try_to_lock);
                    if (dl.owns_lock())
                        next.playerDiag = eng.m_playerDiag;
                }
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
                if (!gotFrame) {
                    next.worldDrawSz = contDraw + itemDraw;
                    next.drawTargets += next.worldDrawSz;
                }
            }
            msWorld = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tA).count();
        }
        {
            const auto tA = std::chrono::steady_clock::now();
            std::shared_lock<std::shared_mutex> lock(eng.m_robotCacheMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                gotRobot = 1;                    next.robotCacheSz = eng.robotCache.size();
                    next.worldGen = eng.m_worldGeneration.load(
                        std::memory_order_acquire);
                size_t drawN = 0;
                for (const auto& [key, entry] : eng.robotCache) {
                    (void)key;
                    if (!entry.Drawing)
                        continue;
                    ++drawN;
                }
                if (!gotFrame) {
                    next.robotDrawSz = drawN;
                    next.drawTargets += next.robotDrawSz;
                }
            }
            msRobot = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tA).count();
        }
        {
            // PlayerEspMissLog: who is missing and which gate dropped them.
            // try_lock only — the ledger is written by workers; a contended
            // paint frame keeps the previous summary (same policy as above).
            std::string missSummary;
            if (PlayerEspMiss::Global().SummaryTryLock(
                    missSummary, PlayerEspMiss::NowMs(), 4))
                next.playerMissSummary = std::move(missSummary);
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
    // Keep the longest first-column chain trace inside its column and leave
    // room for every diagnostic row (including CameraRead).
    const int rowsPerCol = 40;
    const float h = lineH * rowsPerCol + pad * 2.f;
    const float x1 = 20.f;
    const float x2 = x1 + colW + spacing;
    const float x3 = x2 + colW + spacing;
    const float y = 10.f + (ImGui::GetFontSize() + 8) * 2 + 10.f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    for (float xPos : {x1, x2, x3}) {
        dl->AddRectFilled(ImVec2(xPos, y), ImVec2(xPos + colW, y + h), IM_COL32(0, 0, 0, 220));
    }

    ImFont* font = ImGui::GetFont();
    const float fontSize = 14.f;
    const float headerSize = 16.f;

    auto logRedRow = [](const char* label, const char* text) {
        static std::unordered_map<std::string, uint64_t> s_seen;
        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        uint64_t& last = s_seen[label];
        if (nowMs - last < 5000)
            return;
        last = nowMs;
        std::ostringstream f;
        f << "{\"sessionId\":\"c190fb\",\"runId\":\"verify\","
          << "\"location\":\"Render.cpp\",\"message\":\"red_row\","
          << "\"data\":{\"label\":\"" << label << "\",\"text\":\"" << text
          << "\"},\"timestamp\":" << nowMs << "}\n";
        EntityDiagnostics::LogRaw(f.str());
    };
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
        if (!ok)
            logRedRow(label, buf);
    };
    auto drawFittedRow = [&](float xPos, int row, const char* label, bool ok,
        const char* text) {
        const float left = xPos + pad;
        const float right = xPos + colW - pad;
        const ImU32 col = ok ? IM_COL32(60, 255, 100, 255) : IM_COL32(255, 80, 80, 255);
        char prefix[48];
        std::snprintf(prefix, sizeof(prefix), "%s: ", label);
        const ImVec2 prefixSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, prefix);
        const char* value = text ? text : "";
        const char* remaining = value;
        const char* suffix = "...";
        const ImVec2 suffixSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, suffix);
        const float valueWidth = (std::max)(right - left - prefixSize.x - suffixSize.x, 1.f);
        // CalcTextSizeA reports the next byte when max_width is reached. Keep
        // a UTF-8 codepoint intact if that byte belongs to a continuation.
        font->CalcTextSizeA(fontSize, valueWidth, 0.f, value, nullptr, &remaining);
        if (remaining > value && *remaining &&
            (static_cast<unsigned char>(*remaining) & 0xC0) == 0x80) {
            while (remaining > value &&
                (static_cast<unsigned char>(*remaining) & 0xC0) == 0x80)
                --remaining;
        }

        const float ry = y + pad + row * lineH;
        dl->AddText(font, fontSize, ImVec2(left, ry), col, prefix);
        dl->AddText(font, fontSize, ImVec2(left + prefixSize.x, ry), col, value, remaining);
        if (*remaining)
            dl->AddText(font, fontSize, ImVec2(right - suffixSize.x, ry), col, suffix);
        if (!ok)
            logRedRow(label, value);
    };

    const uintptr_t base = snap.base;
    const Engine::EngineStateSnapshot& state = snap.state;
    bool ok;

    // Phase 2: a zero row names the gate that produced it. ordered the same way
    // the collect pass gates players in Functions/Esp.cpp, and read from the same
    // counters the PDrop row shows, so the name matches the ledger reason.
    auto dominantPlayerGate = [](const Engine::PlayerPipelineStats& pd, int& count) {
        const FeaturePolicy::FrameGateCounts counts{
            pd.frameNotDrawing, pd.frameNotInitialized, pd.frameAlly,
            pd.frameDistance, pd.framePosition };
        return FeaturePolicy::DominantFrameGate(counts, count);
    };
    // Indented reason line under a zero value. Amber so it reads as an
    // explanation rather than as another reading.
    auto drawWhy = [&](float xPos, int row, const char* text) {
        if (!text || !*text)
            return;
        char line[320];
        std::snprintf(line, sizeof(line), "-> %s", text);
        dl->AddText(font, 12.f, ImVec2(xPos + pad + 12.f, y + pad + row * lineH),
            IM_COL32(255, 190, 90, 255), line);
    };

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

        // Which rung won the world hop (Core/PlayerChain.hpp): slotDirect is the
        // dump-backed read, slotInner a GWorld** slot, gameStateGlobal the
        // GameState-owning-world fallback.
        const PlayerChain::Rung worldRung =
            state.chain.RungOf(PlayerChain::Hop::World);
        drawRow(xPos, row++, "WorldSrc", state.chain.Ok(PlayerChain::Hop::World),
            "%s", PlayerChain::RungName(worldRung));

        ok = state.gWorld != 0;
        drawRow(xPos, row++, "UWorld", ok, "0x%llX", (unsigned long long)state.gWorld);

        ok = state.owningGameInstance != 0;
        drawRow(xPos, row++, "OwningGI", ok, "0x%llX", (unsigned long long)state.owningGameInstance);

        // GameInstance rung: owning slot / its world back-ref / the LP outer hop /
        // the retired SIMD decrypt / legacy slot / world scan.
        drawRow(xPos, row++, "GISrc", state.chain.Ok(PlayerChain::Hop::GameInstance),
            "%s", PlayerChain::RungName(
                state.chain.RungOf(PlayerChain::Hop::GameInstance)));

        ok = eng.IsValidPointer(state.localPlayer);
        drawRow(xPos, row++, "LocalPlayer", ok, "0x%llX", (unsigned long long)state.localPlayer);

        // LocalPlayer rung: GI array with the PC back-ref, the PC+0x4B0 decrypt,
        // the retirement slot scan, GI array slot 0, or the cached value.
        drawRow(xPos, row++, "LPSrc", state.chain.Ok(PlayerChain::Hop::LocalPlayer),
            "%s", PlayerChain::RungName(
                state.chain.RungOf(PlayerChain::Hop::LocalPlayer)));

        // The controller rung (flag / pair cache / camera manager / actor scan /
        // GI array) and the pawn rung, then the whole chain as one line: hop=rung
        // and how many rungs each hop tried. This is the single source of truth.
        drawRow(xPos, row++, "PCSrc", state.chain.Ok(PlayerChain::Hop::Controller),
            "%s", PlayerChain::RungName(
                state.chain.RungOf(PlayerChain::Hop::Controller)));
        drawRow(xPos, row++, "PawnSrc", state.chain.Ok(PlayerChain::Hop::Pawn),
            "%s", PlayerChain::RungName(
                state.chain.RungOf(PlayerChain::Hop::Pawn)));
        drawFittedRow(xPos, row++, "Chain",
            state.chain.Ok(PlayerChain::Hop::Controller),
            PlayerChain::Trace(state.chain).c_str());

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
        if (snap.playerCacheSz == 0) {
            char why[192];
            std::snprintf(why, sizeof(why),
                "nothing passed admission (scanned=%d candidates=%d admitted=%d)",
                snap.playerDiag.scanned, snap.playerDiag.gsCandidates,
                snap.playerDiag.admitted);
            drawWhy(xPos, row++, why);
        }

        ok = snap.drawTargets > 0;
        // Total published frame entries. The old row only counted players,
        // so a frame drawing world/bot entries reported EspDraw: 0.
        drawRow(xPos, row++, "EspDraw", ok, "%zu", snap.drawTargets);
        if (snap.drawTargets == 0) {
            // The cull has a name: the dominant frame-collect gate, then the
            // ledger's own per-actor reasons (top rows, bounded by summary size).
            int gateCount = 0;
            const char* gate = dominantPlayerGate(snap.playerDiag, gateCount);
            char why[192];
            if (gate) {
                std::snprintf(why, sizeof(why),
                    "culled by %s x%d (player frame gates)", gate, gateCount);
            } else {
                std::snprintf(why, sizeof(why),
                    "no_candidates: nothing eligible was selected into the frame");
            }
            drawWhy(xPos, row++, why);
            if (!snap.playerMissSummary.empty())
                drawWhy(xPos, row++, snap.playerMissSummary.c_str());
        }
        drawRow(xPos, row++, "PInit", snap.playerDiag.initialized > 0,
            "%d/%d", snap.playerDiag.initialized, snap.playerDiag.uninitialized);
        drawRow(xPos, row++, "PDraw", snap.playerDiag.drawing > 0,
            "%d", snap.playerDiag.drawing);
        drawRow(xPos, row++, "PFrame", snap.playerDiag.frameSelected > 0,
            "%d", snap.playerDiag.frameSelected);
        drawRow(xPos, row++, "PDrop", true,
            "nd=%d ni=%d ally=%d dist=%d pos=%d",
            snap.playerDiag.frameNotDrawing,
            snap.playerDiag.frameNotInitialized,
            snap.playerDiag.frameAlly,
            snap.playerDiag.frameDistance,
            snap.playerDiag.framePosition);
        drawFittedRow(xPos, row++, "PMiss", true,
            snap.playerMissSummary.empty()
                ? "none" : snap.playerMissSummary.c_str());

        ok = snap.worldCacheSz > 0;
        drawRow(xPos, row++, "WorldCache", ok, "%zu", snap.worldCacheSz);

        ok = snap.worldDrawSz > 0;
        drawRow(xPos, row++, "WorldDraw", ok, "%zu", snap.worldDrawSz);
        if (snap.worldDrawSz == 0) {
            char why[192];
            std::snprintf(why, sizeof(why),
                "no world entry passed its draw gate (cache=%zu)",
                snap.worldCacheSz);
            drawWhy(xPos, row++, why);
        }

        ok = snap.robotDrawSz > 0;
        drawRow(xPos, row++, "BotCache", snap.robotCacheSz > 0, "%zu", snap.robotCacheSz);
        drawRow(xPos, row++, "BotDraw", ok, "%zu", snap.robotDrawSz);
        if (snap.robotDrawSz == 0) {
            // Two very different failures — name the real one instead of the
            // old "cache empty or filtered" guess (bots can be 10m away while
            // admission is still rebuilding the cache after a world-gen reset).
            if (snap.robotCacheSz == 0)
                drawWhy(xPos, row++,
                    "bot cache EMPTY: admission backlog, bots exist but none verified yet");
            else {
                char why[192];
                std::snprintf(why, sizeof(why),
                    "%zu cached bot(s) but all filtered (distance/projection gate)",
                    snap.robotCacheSz);
                drawWhy(xPos, row++, why);
            }
        }

        ok = snap.actorCount > 0 && snap.actorCount < 10000;
        drawRow(xPos, row++, "ActorCount", ok, "%d", snap.actorCount);

        ok = IsUsableCameraFov(snap.camFov);
        drawRow(xPos, row++, "CameraRead", ok, "FOV:%.1f", snap.camFov);
        if (!IsUsableCameraFov(snap.camFov))
            drawWhy(xPos, row++, "camera_fov_unusable: live g_Cam FOV rejected");

        // Bottom: EXACTLY two rows — app clock, then the newest complete
        // green event. The full green-order history goes to the debug log
        // only (green_time events), never on screen.
        // Raid-scoped timers: a new world generation restarts the clock and
        // every green stamp, so each raid measures its own all-green time
        // instead of accumulating across raids.
        static uint64_t s_raidTimerGen = UINT64_MAX;
        const uint64_t raidGen =
            eng.m_worldGeneration.load(std::memory_order_acquire);
        if (raidGen != s_raidTimerGen) {
            if (s_raidTimerGen != UINT64_MAX) {
                std::ostringstream rr;
                rr << "{\"sessionId\":\"c190fb\",\"runId\":\"green\","
                   << "\"location\":\"Render.cpp\",\"message\":\"green_time\","
                   << "\"data\":{\"name\":\"RAID_RESET\",\"appSec\":0"
                   << ",\"gen\":" << raidGen << "},\"timestamp\":"
                   << static_cast<uint64_t>(std::chrono::duration_cast<
                          std::chrono::milliseconds>(
                              std::chrono::steady_clock::now()
                                  .time_since_epoch()).count())
                   << "}\n";
                EntityDiagnostics::LogRaw(rr.str());
            }
            s_raidTimerGen = raidGen;
            s_appStartTime = std::chrono::steady_clock::now();
            for (int gi = 0; gi < kGreenCount; ++gi)
                s_firstGreenAt[gi] = 0.0;
            s_greenLog.clear();
            s_allGreenAt = -1.0;
        }
        const double appSec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - s_appStartTime).count();
        // First-green stamp for every tracked counter, in panel row order.
        const bool greenNow[kGreenCount] = {
            snap.playerCacheSz > 0,
            snap.drawTargets > 0,
            snap.playerDiag.initialized > 0,
            snap.playerDiag.drawing > 0,
            snap.playerDiag.frameSelected > 0,
            snap.worldCacheSz > 0,
            snap.worldDrawSz > 0,
            snap.robotCacheSz > 0,
            snap.robotDrawSz > 0,
            snap.actorCount > 0 && snap.actorCount < 10000,
        };
        // Only stamp greens when the snapshot is from the CURRENT world gen —
        // otherwise the previous raid's surviving counts stamp everything
        // green on the first frame after a gen bump ("all green 0.0s, no esp").
        const bool snapGenFresh = snap.worldGen == raidGen;
        for (int gi = 0; gi < kGreenCount; ++gi) {
            if (greenNow[gi] && snapGenFresh && s_firstGreenAt[gi] <= 0.0) {
                s_firstGreenAt[gi] = appSec;
                RecordGreen(kGreenNames[gi], appSec);
            }
        }
        // Row 1: clock FREEZES once every tracked counter has completed
        // green — the frozen value is the run's all-green time.
        if (s_allGreenAt < 0.0) {
            bool all = true;
            for (int gi = 0; gi < kGreenCount; ++gi) {
                if (s_firstGreenAt[gi] <= 0.0) { all = false; break; }
            }
            if (all) {
                s_allGreenAt = appSec;
                RecordGreen("ALL_GREEN", appSec);
            }
        }
        char appLine[64];
        if (s_allGreenAt > 0.0)
            std::snprintf(appLine, sizeof(appLine),
                "All green: %.1fs [stopped]", s_allGreenAt);
        else
            std::snprintf(appLine, sizeof(appLine), "App time: %.1fs", appSec);
        dl->AddText(font, fontSize, ImVec2(xPos + pad, y + pad + 37 * lineH),
            IM_COL32(120, 200, 255, 255), appLine);

        // Row 2: newest counter to complete green (replaces as each fires).
        const float latestY = y + pad + 38 * lineH;
        if (!s_greenLog.empty()) {
            const GreenLogEntry& latest = s_greenLog.back();
            char latestLine[64];
            std::snprintf(latestLine, sizeof(latestLine), "%s: %.1fs",
                latest.name, latest.greenAt);
            dl->AddText(font, 12.f, ImVec2(xPos + pad, latestY),
                IM_COL32(180, 230, 255, 255), latestLine);
        } else {
            dl->AddText(font, 12.f, ImVec2(xPos + pad, latestY),
                IM_COL32(180, 200, 255, 255), "-- waiting --");
        }

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
        drawRow(xPos, row++, "C2WAlt", true, "0x%llX", (unsigned long long)Offsets::ComponentToWorld_Alt);
        drawRow(xPos, row++, "C2WUsed", true, "0x%llX", (unsigned long long)(
            Engine::ProbeComponentToWorldOffset(state.rootComponent)));
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

        const Engine::HumanizerDebugState humanizer = eng.GetHumanizerDebug();
        const char* phase = !humanizer.valid ? "retired"
            : humanizer.phase == 1 ? "react"
            : humanizer.phase == 2 ? "settle"
            : humanizer.phase == 3 ? "held" : "retired";
        drawRow(xPos, row++, "Humanizer", true,
            "phase=%s offset=(%.2f,%.2f) limiter=%s",
            phase, humanizer.injectedX, humanizer.injectedY,
            humanizer.limiterActive ? "ACTIVE" : "off");
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
