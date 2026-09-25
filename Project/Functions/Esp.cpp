#include "../Core/Engine.h"
#include "../Core/FeaturePolicy.hpp"
#include "../Core/AgentLog.h"
#include "../Core/ActorType.h"
#include "../Core/BotMotion.hpp"
#include "../Core/BotEspExpiry.hpp"
#include "../Core/BotEspPosition.hpp"
#include "../Core/EntityDiagnostics.hpp"
#include "../Core/PlayerEspMissLog.hpp"
#include "../Core/AssetNames.h"
#include "../Core/Memory.h"
#include "../Core/WorldItemCategory.h"
#include "../Functions/WorldScanCommon.h"
#include "../Functions/RobotList.h"
#include "../Functions/CollisionMirror.h"
#include "../Interface/Utils/Variables/index.h"
#include "../Interface/Utils/Visuals/visuals.hpp"
#include "EspDraw.h"
#include "../Interface/Render/RenderQueue.h"

#include "../ThirdParty/ImGui/imgui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <vector>

extern Engine engine;

namespace {

constexpr size_t kMaxEspFrameBoneReads = 24;

struct WorldEspDebugStats {
    int frameEntries = 0;
    int rendered = 0;
    int skipAllow = 0;
    int skipPos = 0;
    int skipDist = 0;
    int skipProj = 0;
    int skipPickedUp = 0;
};

WorldEspDebugStats g_worldEspDbg{};

static void LogGhostBotDecision(
    uintptr_t actorKey, const Engine::WorldCacheEntry& entry,
    const char* reason, float distanceM = -1.f)
{
    if (!var::debug_ghost_bots)
        return;

    // The paint path can evaluate a bot many times per second. Keep one
    // decision per actor/reason per second so the log remains useful instead
    // of flooding the asynchronous diagnostic queue.
    static std::unordered_map<uintptr_t, std::pair<const char*, uint64_t>> last;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    auto& state = last[actorKey];
    if (state.first == reason && now >= state.second && now - state.second < 1000)
        return;
    state = { reason, now };
    if (last.size() > 4096)
        last.clear();

    const bool positionValid = IsPlausibleWorldPos(entry.WorldPos);
    const uint64_t age = entry.positionSampleMs != 0 && now >= entry.positionSampleMs
        ? now - entry.positionSampleMs : 0;
    EntityDiagnostics::LogBotDecision(
        actorKey, entry.ActorName, reason, entry.Drawing,
        entry.botIdentityProven, entry.IsBreaked, positionValid, age,
        distanceM >= 0.f ? distanceM : entry.Distance);
}

static const char* EspFrameResultName(int result)
{
    switch (static_cast<Engine::EspFrameResult>(result)) {
    case Engine::EspFrameResult::Published: return "published";
    case Engine::EspFrameResult::Inactive: return "inactive";
    case Engine::EspFrameResult::ScatterInitFailed: return "scatter-init";
    case Engine::EspFrameResult::ScatterExecuteFailed: return "scatter-exec";
    case Engine::EspFrameResult::CameraFailed: return "camera";
    case Engine::EspFrameResult::GenerationChanged: return "generation";
    case Engine::EspFrameResult::None:
    default: return "none";
    }
}

static uint64_t SteadyNowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct RenderCamDebug {
    const char* src = "none";
    bool leadApplied = false;
    bool leadSkipped = false;
    float leadYaw = 0.f;
    float leadPitch = 0.f;
};

static bool IsCurrentFreshEspFrame(
    const Engine::EspRenderFrame& frame,
    uint64_t nowMs = SteadyNowMs())
{
    return EspFramePolicy::IsAcceptable(
        {frame.valid, frame.worldGeneration, frame.collectStampMs},
        engine.m_worldGeneration.load(std::memory_order_acquire),
        nowMs);
}

static void RenderEspFrameHealthFallback(
    const Engine::EspRenderFrame* frame,
    const RenderCamDebug* camDbg)
{
    if (!var::show_debug_overlay)
        return;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    const auto& diag = engine.m_espFrameDiagnostics;
    const uint64_t nowMs = SteadyNowMs();
    const uint64_t generation =
        engine.m_worldGeneration.load(std::memory_order_acquire);
    const uint64_t publishMs = diag.lastPublishMs.load(std::memory_order_acquire);
    const int publishAge = publishMs != 0 && nowMs >= publishMs
        ? static_cast<int>(nowMs - publishMs) : -1;
    const uint64_t failureMs = diag.lastFailureMs.load(std::memory_order_acquire);
    const int failureAge = failureMs != 0 && nowMs >= failureMs
        ? static_cast<int>(nowMs - failureMs) : -1;
    const int frameAge = frame && frame->collectStampMs != 0
        && nowMs >= frame->collectStampMs
        ? static_cast<int>(nowMs - frame->collectStampMs) : -1;
    const char* frameState = !frame
        ? "none"
        : (IsCurrentFreshEspFrame(*frame, nowMs) ? "fresh" : "stale");
    const char* cameraState = camDbg ? camDbg->src : "unavailable";

    char line[320];
    drawList->AddRectFilled(
        ImVec2(20.f, 60.f), ImVec2(620.f, 150.f), IM_COL32(0, 0, 0, 220));
    drawList->AddText(
        ImGui::GetFont(), 16.f, ImVec2(28.f, 65.f),
        IM_COL32(255, 200, 100, 255), "ESP FRAME HEALTH");
    std::snprintf(line, sizeof(line),
        "state %s | result %s | frameAge %dms | publishAge %dms",
        frameState,
        EspFrameResultName(diag.lastResult.load(std::memory_order_acquire)),
        frameAge, publishAge);
    drawList->AddText(
        ImGui::GetFont(), 14.f, ImVec2(28.f, 88.f),
        IM_COL32(200, 220, 255, 255), line);
    std::snprintf(line, sizeof(line),
        "gen cur %llu | failAge %dms x%d | cam %s | counts p%d b%d w%d",
        static_cast<unsigned long long>(generation),
        failureAge,
        diag.consecutiveFailures.load(std::memory_order_acquire),
        cameraState,
        diag.lastPlayerCount.load(std::memory_order_relaxed),
        diag.lastRobotCount.load(std::memory_order_relaxed),
        diag.lastWorldCount.load(std::memory_order_relaxed));
    drawList->AddText(
        ImGui::GetFont(), 14.f, ImVec2(28.f, 110.f),
        IM_COL32(200, 220, 255, 255), line);
}

// Clean, human-readable category label for a container. Never empty, never a
// raw fname or number — used whenever a specific name can't be resolved so a
// container is ALWAYS labeled rather than drawn blank.
static std::string ContainerCategoryFallbackLabel(WorldItemCategory cat)
{
    return ContainerCategoryFallbackEspLabel(cat);
}

// A "proper" container name: readable, not generic, and not a raw fname
// token (no underscores, not BP_-prefixed, short, few words). This keeps the
// long fname sludge and numeric codes out of the label.
static bool IsCleanContainerName(const std::string& s)
{
    if (s.empty() || s.size() > 28)
        return false;
    if (IsGenericWorldEspLabel(s) || IsJunkWorldEspLabel(s) || !IsPlausibleEspLabel(s))
        return false;
    if (s.find('_') != std::string::npos)
        return false;
    if (s.size() >= 2
        && (s[0] == 'B' || s[0] == 'b')
        && (s[1] == 'P' || s[1] == 'p'))
        return false;
    int words = 1;
    for (char c : s)
        if (c == ' ')
            ++words;
    return words <= 4;
}

static std::string AppendContainerOpenSuffix(std::string label)
{
    if (label.empty())
        return label;
    if (label.find("(Open)") != std::string::npos)
        return FormatEspDisplayLabel(label);
    return FormatEspDisplayLabel(label + " (Open)");
}

bool IsGroundLootEspCategory(WorldItemCategory cat)
{
    switch (cat) {
    case WorldItemCategory::DroppedPickup:
    case WorldItemCategory::Items:
    case WorldItemCategory::Ammo:
    case WorldItemCategory::ArcLoot:
    case WorldItemCategory::Backpack:
    case WorldItemCategory::Grenade:
    case WorldItemCategory::Medical:
    case WorldItemCategory::Keys:
    case WorldItemCategory::Harvestable:
    case WorldItemCategory::QuestItem:
        return true;
    default:
        return false;
    }
}

inline float EffectiveRadarRangeM()
{
    return var::radar_range > 0.f ? var::radar_range : 100.f;
}

inline float PlayerCollectMaxM()
{
    if (var::enableesp)
        return var::esp_distance;
    return 0.f;
}

inline float BotCollectMaxM()
{
    if (var::showRobots || var::robotAimEnabled)
        return var::bot_esp_distance;
    return 0.f;
}

void DrawPulsatingHeart(ImDrawList* dl, ImVec2 center, float baseRadius, ImU32 color)
{
    if (!dl || baseRadius < 0.5f)
        return;

    const float t = static_cast<float>(ImGui::GetTime());
    // Heartbeat: dual thump pattern
    const float cycle = fmodf(t * 1.25f, 1.0f);
    float beat = 0.0f;
    if (cycle < 0.15f) {
        beat = sinf(cycle / 0.15f * 3.14159f); // first thump
    } else if (cycle > 0.25f && cycle < 0.4f) {
        beat = sinf((cycle - 0.25f) / 0.15f * 3.14159f) * 0.7f; // weaker second thump
    }
    
    // Softer scaling down to better retain structure
    const float scale = 0.9f + 0.25f * beat;
    const float s = baseRadius * scale;

    const int alpha = static_cast<int>((color >> IM_COL32_A_SHIFT) & 0xFF);
    const int pulsedAlpha = std::clamp(static_cast<int>(alpha * (0.8f + 0.2f * beat)), 48, 255);
    const ImU32 col = (color & 0x00FFFFFFu) | (static_cast<ImU32>(pulsedAlpha) << IM_COL32_A_SHIFT);

    // Adjusted heart structure to be a bit taller/wider
    const ImVec2 left(center.x - s * 0.35f, center.y - s * 0.15f);
    const ImVec2 right(center.x + s * 0.35f, center.y - s * 0.15f);
    dl->AddCircleFilled(left, s * 0.45f, col, 16);
    dl->AddCircleFilled(right, s * 0.45f, col, 16);
    
    // More pronounced point at the bottom
    dl->AddTriangleFilled(
        ImVec2(center.x - s * 0.72f, center.y + s * 0.08f),
        ImVec2(center.x + s * 0.72f, center.y + s * 0.08f),
        ImVec2(center.x, center.y + s * 0.75f),
        col);
}

void DrawBotHeartIfEnabled(
    ImDrawList* drawList,
    const ImVec2& head,
    const ImVec2& feet,
    float boxH,
    const Visuals::EspDrawScale& scale,
    ImU32 color)
{
    if (!var::showRobots || !var::bot_heart || !drawList || boxH < 1.f)
        return;

    const float centerX = (head.x + feet.x) * 0.5f;
    const float centerY = (head.y + feet.y) * 0.5f;
    const float drawBoxH = (std::max)(boxH, 12.f);
    const float boxWidth = drawBoxH * 0.65f;
    const float boxDim = (std::min)(boxWidth, drawBoxH);
    float heartRadius = boxDim * 0.34f;
    if (scale.espScale > 0.f)
        heartRadius *= scale.espScale;
    const float maxRadius = (std::max)(boxDim * 0.52f, 6.f);
    heartRadius = (std::clamp)(heartRadius, 5.f, maxRadius);
    DrawPulsatingHeart(drawList, ImVec2(centerX, centerY), heartRadius, color);
}

enum class EspPosTargetKind : uint8_t {
    Player,
    World,
    Robot
};

struct EspPosReadTarget {
    EspPosTargetKind kind{};
    size_t index = 0;
    uintptr_t rootComponent = 0;
    float distance = 0.f;
    Engine::FVector3d c2w{};
    Vector3 relative{};
};

static Vector3 ResolveScatterWorldPos(const Engine::FVector3d& c2w, const Vector3& relative)
{
    if (c2w.x != 0.0 || c2w.y != 0.0 || c2w.z != 0.0) {
        const Vector3 w = Engine::ToVector3(c2w);
        if (IsPlausibleWorldPos(w))
            return w;
    }
    if (IsPlausibleWorldPos(relative))
        return relative;
    return {};
}

static bool ResolvePcmForEspFrame(
    Engine& eng,
    uintptr_t& pc,
    uintptr_t& pawn,
    uintptr_t& root,
    uintptr_t& pcm)
{
    pc = eng.PlayerController;
    pawn = eng.AcknowledgedPawn;
    root = eng.RootComponent;
    pcm = eng.PlayerCameraManager;
    return eng.ResolvePlayerCameraManagerLadder(
        pc,
        pawn,
        pcm,
        eng.PersistentLevel,
        eng.Actors,
        /*nocacheFov=*/false);
}

static bool CameraOkForEsp(const Engine::CameraCache& cam)
{
    if (!IsUsableCameraFov(cam.FOV))
        return false;
    return IsPlausibleWorldPos(cam.Location);
}

static int g_camLeadSkips = 0;  // ghost-guard skips, surfaced in [debugCam]

// Points that failed with the paint camera and were re-projected with
// g_Camera instead - two cameras placing entities inside one paint.
static int g_ghostProjFallback = 0;
// #endregion

static bool ResolveLiveRenderCamera(
    const Engine::EspRenderFrame& frame,
    Engine::CameraCache& outCam,
    RenderCamDebug* outDbg = nullptr)
{
    // Paint path must NOT DMA. TryBuildCameraFromPcmPov stalled Present;
    // worker UpdateCamera publishes g_Camera; frame.camera is the backup.
    // Rotation extrapolate still bridges look-drag without touching the FPGA bus.
    static Engine::CameraCache s_prevCam{};
    static bool s_prevOk = false;
    static auto s_prevTp = std::chrono::steady_clock::time_point{};

    auto applyRotExtrap = [&](Engine::CameraCache& cam) {
        const auto now = std::chrono::steady_clock::now();
        const Engine::CameraCache raw = cam;
        if (s_prevOk) {
            const float dtSec = std::chrono::duration<float>(now - s_prevTp).count();
            if (dtSec > 0.0005f && dtSec < 0.05f) {
                const float dyaw = static_cast<float>(
                    raw.Rotation.y - s_prevCam.Rotation.y);
                const float dpitch = static_cast<float>(
                    raw.Rotation.x - s_prevCam.Rotation.x);
                // CAM3: fixed camera-freshness lead. The old 0.35 x paint-delta
                // lead scaled with the PAINT gap, so its size wobbled frame to
                // frame and every rate-guard trip popped boxes back (user: ESP
                // jumps). g_Camera now publishes @8ms on its own HIGHEST-prio
                // thread, so the true present-time camera is at most ~8ms old.
                // Lead = yawRate * 8ms sampling window, same every paint for a
                // given turn speed => smooth, no snap-back. Clamp 0.4 deg.
                constexpr float kCameraLeadSec = 0.008f;
                constexpr float kMaxDeg = 0.4f;
                constexpr float kMaxLeadRateDegPerSec = 320.f;
                const float yawRate = std::fabs(dyaw) / dtSec;
                const float pitchRate = std::fabs(dpitch) / dtSec;
                // The 320 deg/s rate guard used to snap the lead OFF for a
                // frame (all boxes jump back 0.4 deg) whenever the look rate
                // spiked - visible as "ESP off target" while turning, plus a
                // paint-thread file write every 5s. The clamp already bounds
                // the lead, so always apply it: continuous, no snap-back.
                float leadYaw = dyaw * (kCameraLeadSec / dtSec);
                float leadPitch = dpitch * (kCameraLeadSec / dtSec);
                if (leadYaw > kMaxDeg) leadYaw = kMaxDeg;
                else if (leadYaw < -kMaxDeg) leadYaw = -kMaxDeg;
                if (leadPitch > kMaxDeg) leadPitch = kMaxDeg;
                else if (leadPitch < -kMaxDeg) leadPitch = -kMaxDeg;
                cam.Rotation.y += leadYaw;
                cam.Rotation.x += leadPitch;
                // #region agent log
                if (outDbg) {
                    outDbg->leadApplied = true;
                    outDbg->leadYaw = leadYaw;
                    outDbg->leadPitch = leadPitch;
                }
                // #endregion
            }
        }
        s_prevCam = raw;
        s_prevOk = true;
        s_prevTp = now;
    };

    {
        std::shared_lock<std::shared_mutex> lock(engine.m_cameraMutex);
        if (CameraOkForEsp(engine.g_Camera)) {
            outCam = engine.g_Camera;
            // #region agent log
            if (outDbg)
                outDbg->src = "g_Cam";
            // #endregion
            applyRotExtrap(outCam);
            return true;
        }
    }
    if (frame.valid && CameraOkForEsp(frame.camera)) {
        outCam = frame.camera;
        // #region agent log
        if (outDbg)
            outDbg->src = "FRAME";
        // #endregion
        applyRotExtrap(outCam);
        return true;
    }
    return false;
}

static bool ProjectWorldEspPoint(
    const Vector3& worldPos,
    const Engine::CameraCache& frameCam,
    Vector3& outScreen)
{
    // Paint does no DMA: a miss here is deterministic geometry, never a flaky
    // read. Falling back to the live g_Camera placed that one box via a
    // DIFFERENT camera than the rest of the frame (renderCam has the rotation
    // lead applied; g_Camera does not) - a mixed-camera placement that
    // repainted the label at a slightly offset pixel spot for one frame.
    // Skip it instead: if frameCam cannot project it, it is out of view.
    return engine.ProjectWorldLocationToScreen(worldPos, outScreen, frameCam);
}

// GHOST1 (Fix #10): the Fix #6/#7 sticky-screen holds repainted labels at
// their LAST screen coordinates when WorldToScreen failed. If the projection
// failed because the object left the view (camera rotated past it), that
// stale coordinate is a random mid-screen spot — a phantom "Locker" flash
// with nothing there. Only allow the hold while the camera still faces the
// object's world position; otherwise skip the draw entirely.
static bool WorldPointRoughlyInView(
    const Engine::CameraCache& cam, const Vector3& p)
{
    const double dx = static_cast<double>(p.x) - cam.Location.x;
    const double dy = static_cast<double>(p.y) - cam.Location.y;
    const double dz = static_cast<double>(p.z) - cam.Location.z;
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1.0)
        return true;
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    const double pitch = cam.Rotation.x * kDeg2Rad;
    const double yaw = cam.Rotation.y * kDeg2Rad;
    const double fx = std::cos(pitch) * std::cos(yaw);
    const double fy = std::cos(pitch) * std::sin(yaw);
    const double fz = std::sin(pitch);
    const double dot = (dx * fx + dy * fy + dz * fz) / len;
    const double fovDeg =
        (cam.FOV > 1.f && cam.FOV < 179.f) ? static_cast<double>(cam.FOV) : 90.0;
    const double halfCone = (std::min)(fovDeg * 0.75, 85.0);
    return dot >= std::cos(halfCone * kDeg2Rad);
}

static ImU32 PlayerEspColor(const Engine::PlayerCacheEntry& actor)
{
    const float* c = actor.isVisible ? var::esp_color_visible : var::esp_color_invisible;
    return EspDraw::ColorFromRGBA(c);
}

static ImU32 BotEspColor(bool visible, bool isBreaked = false)
{
    const float* c = isBreaked
        ? var::color_dead_bots
        : (visible ? var::bot_color_visible : var::bot_color_invisible);
    return EspDraw::ColorFromRGBA(c);
}

static ImVec2 LabelTextSize(const char* text, float distanceM)
{
    if (!text || !text[0])
        return ImVec2(0.f, 0.f);
    ImFont* font = ImGui::GetFont();
    const float px = Visuals::LabelTextPx(distanceM);
    if (font)
        return font->CalcTextSizeA(px, FLT_MAX, 0.f, text);
    return ImGui::CalcTextSize(text);
}

static float LabelTextHeight(const char* text, float distanceM)
{
    if (!text || !text[0])
        return 0.f;
    ImFont* font = ImGui::GetFont();
    const float px = Visuals::LabelTextPx(distanceM);
    if (font)
        return font->CalcTextSizeA(px, FLT_MAX, 0.f, text).y;
    return ImGui::CalcTextSize(text).y;
}

static void DrawWeaponLabel(
    ImDrawList* drawList,
    const Engine::PlayerCacheEntry& actor,
    float anchorX,
    float& labelStackY)
{
    if (!var::show_weapon)
        return;

    std::string active = actor.weaponName;
    std::string stowed0 = actor.stowedWeapon0;
    std::string stowed1 = actor.stowedWeapon1;
    if (!engine.IsPlayerWeaponEspLabel(active) && active != "Unarmed")
        active.clear();
    if (!engine.IsPlayerWeaponEspLabel(stowed0))
        stowed0.clear();
    if (!engine.IsPlayerWeaponEspLabel(stowed1))
        stowed1.clear();

    // Prefer a real gun over placeholder Unarmed.
    if ((active.empty() || active == "Unarmed") && !stowed0.empty()) {
        active = stowed0;
        stowed0.clear();
    } else if ((active.empty() || active == "Unarmed") && !stowed1.empty()) {
        active = stowed1;
        stowed1.clear();
    }
    if (active == stowed0) stowed0.clear();
    if (active == stowed1) stowed1.clear();

    // Active weapon — centered on head X.
    if (!active.empty()) {
        std::string weaponLabel = active;
        if (active != "Unarmed") {
            const std::string clip = LoadoutFormat::ClipText(actor.weaponClip);
            if (!clip.empty())
                weaponLabel += " " + clip;
        }
        const ImU32 wColor = (active != "Unarmed" && actor.weaponQuality > 0)
            ? static_cast<ImU32>(RarityTierColor(actor.weaponQuality))
            : IM_COL32(220, 220, 220, 255);
        EspDraw::DrawLabelEsp(
            drawList,
            ImVec2(anchorX, labelStackY),
            weaponLabel.c_str(),
            wColor,
            actor.Distance);
        labelStackY -= LabelTextHeight(weaponLabel.c_str(), actor.Distance) + 2.f;
    }

    // Stowed weapons.
    for (const std::string* sw : { &stowed0, &stowed1 }) {
        if (sw->empty()) continue;
        const ImU32 sColor = IM_COL32(160, 160, 160, 180);
        EspDraw::DrawLabelEsp(
            drawList,
            ImVec2(anchorX, labelStackY),
            sw->c_str(),
            sColor,
            actor.Distance);
        labelStackY -= LabelTextHeight(sw->c_str(), actor.Distance) + 1.f;
    }
}

static ImU32 SquadColor(uint8_t idx)
{
    switch (idx) {
    case 1: return IM_COL32(255, 60, 60, 255);
    case 2: return IM_COL32(255, 200, 60, 255);
    case 3: return IM_COL32(60, 200, 255, 255);
    default: return IM_COL32(200, 200, 200, 255);
    }
}

static float StackPlayerLabels(
    ImDrawList* drawList,
    const Engine::PlayerCacheEntry& actor,
    float headX,
    float& labelStackY,
    double camYawDeg,
    const Visuals::EspDrawScale& scale)
{
    // 1. Group Name, Squad, and Distance onto a single clear line closest to the player's head.
    if (var::names || var::show_distance || var::show_squad_idx) {
        std::string topText;
        
        if (var::show_squad_idx && !actor.isAlly && actor.squadIdx > 0) {
            const std::string tag = SquadRoster::SquadTag(actor.squadIdx);
            if (!tag.empty())
                topText += "[" + tag + "] ";
        }
        
        if (var::names) {
            topText += actor.ActorName.empty() ? "Raider" : actor.ActorName;
        } else if (topText.empty() && var::show_distance) {
            topText = "Player";
        }
        
        if (var::show_distance) {
            char distBuf[32]{};
            snprintf(distBuf, sizeof(distBuf), " [%.0fm]", actor.Distance);
            topText += distBuf;
        }

        if (!topText.empty()) {
            ImU32 textColor = IM_COL32(255, 255, 255, 255);
            if (var::show_squad_idx && !actor.isAlly && actor.squadIdx > 0) {
                textColor = SquadColor(actor.squadIdx);
            } else if (!actor.isVisible) {
                textColor = IM_COL32(255, 120, 120, 255); // Red indicator if occluded
            }
            
            EspDraw::DrawLabelEsp(
                drawList,
                ImVec2(headX, labelStackY),
                topText.c_str(),
                textColor,
                actor.Distance);
            labelStackY -= LabelTextHeight(topText.c_str(), actor.Distance) + 4.f;
        }
    }

    // 1.5. DBNO badge + revive countdown ring (feature #4).
    if (var::show_dbno_badge && actor.isDbno) {
        EspDraw::DrawLabelEsp(
            drawList,
            ImVec2(headX, labelStackY),
            "DOWNED",
            IM_COL32(255, 70, 70, 240),
            actor.Distance);
        labelStackY -= LabelTextHeight("DOWNED", actor.Distance) + 2.f;
        const float remain = actor.reviveRemainS;
        if (remain > 0.f && actor.reviveTotalS > 0.f) {
            const std::string timerText = ReviveBadge::FormatCountdown(remain);
            EspDraw::DrawLabelEsp(
                drawList,
                ImVec2(headX, labelStackY),
                timerText.c_str(),
                IM_COL32(255, 180, 80, 240),
                actor.Distance);
            // Countdown ring to the left of the timer text.
            const float frac =
                ReviveBadge::RingFraction(remain, actor.reviveTotalS);
            const ImVec2 textSize = LabelTextSize(timerText.c_str(), actor.Distance);
            const ImVec2 ringC(
                headX - textSize.x * 0.5f - 9.f,
                labelStackY - textSize.y * 0.5f);
            drawList->AddCircle(ringC, 5.5f, IM_COL32(70, 70, 70, 200), 16, 1.5f);
            drawList->PathClear();
            drawList->PathArcTo(
                ringC, 5.5f, -1.5707963f, -1.5707963f + 6.2831853f * frac, 24);
            drawList->PathStroke(
                IM_COL32(255, 140, 60, 255), ImDrawFlags_None, 2.f);
            labelStackY -= textSize.y + 2.f;
        }
    }

    // 2. Weapons stack logically above the name block.
    if (var::show_weapon) {
        DrawWeaponLabel(drawList, actor, headX, labelStackY);
    }

    // 3. Armor tier + plate count (loadout readout) - independent of the
    // weapon line so the armor state is visible even when it is the only thing
    // that resolved.
    if (var::show_armor_line) {
        const std::string armorLine =
            LoadoutFormat::ArmorText(actor.armorTier, actor.armorPlates);
        if (!armorLine.empty()) {
            EspDraw::DrawLabelEsp(
                drawList,
                ImVec2(headX, labelStackY),
                armorLine.c_str(),
                IM_COL32(150, 200, 255, 210),
                actor.Distance);
            labelStackY -= LabelTextHeight(armorLine.c_str(), actor.Distance) + 1.f;
        }
    }

    // 3.5. Full kit readout (phase 2): stowed tool, safe pouch (rarity) and
    // belt/pack slot capacity. Stowed guns join only when the weapon line is
    // off, so nothing is printed twice.
    if (var::show_player_kit) {
        LoadoutFormat::KitParts kit;
        if (!var::show_weapon) {
            kit.stowed0 = actor.stowedWeapon0;
            kit.stowed1 = actor.stowedWeapon1;
        }
        kit.tool = actor.kitTool;
        kit.pouch = actor.kitPouch;
        kit.pouchRarity = actor.kitPouchRarity;
        kit.beltSlots = actor.kitBeltSlots;
        kit.packSlots = actor.kitPackSlots;
        const std::string kitLine = LoadoutFormat::KitText(kit, 90);
        if (!kitLine.empty()) {
            EspDraw::DrawLabelEsp(
                drawList,
                ImVec2(headX, labelStackY),
                kitLine.c_str(),
                IM_COL32(200, 200, 170, 200),
                actor.Distance);
            labelStackY -= LabelTextHeight(kitLine.c_str(), actor.Distance) + 1.f;
        }
    }

    // 4. Identity (feature #6) + status tags (phase 2): permanent SteamID64
    // and [Bot]/[Spec]/[Done] under the loadout block.
    {
        std::string idText;
        if (var::show_steam_ids && actor.steamId64 != 0)
            idText = SquadRoster::SteamLabel(actor.steamId64);
        const std::string tags = SquadRoster::StatusTag(
            (actor.statusFlags & 1u) != 0,
            (actor.statusFlags & 2u) != 0,
            (actor.statusFlags & 4u) != 0);
        if (!tags.empty()) {
            if (!idText.empty())
                idText += " ";
            idText += tags;
        }
        if (!idText.empty()) {
            EspDraw::DrawLabelEsp(
                drawList,
                ImVec2(headX, labelStackY),
                idText.c_str(),
                IM_COL32(170, 170, 190, 190),
                actor.Distance);
            labelStackY -= LabelTextHeight(idText.c_str(), actor.Distance) + 1.f;
        }
    }

    // 5. Look arrow (feature #5): points where the player is actually aiming.
    if (var::show_look_arrows && actor.hasAimYaw) {
        const double angleRad =
            LookArrow::ArrowAngleRad(actor.aimYawDeg, camYawDeg);
        LookArrow::Point tip, left, right;
        LookArrow::ArrowPoints(
            headX, labelStackY - 6.f, angleRad, 7.f, tip, left, right);
        drawList->AddTriangleFilled(
            ImVec2(tip.x, tip.y),
            ImVec2(left.x, left.y),
            ImVec2(right.x, right.y),
            EspDraw::ColorFromRGBA(var::color_look_arrow));
        labelStackY -= 14.f;
    }

    return labelStackY;
}

} // namespace

static void RenderPlayerEspFromFrame(
    const std::vector<Engine::EspFramePlayer>& players,
    const Engine::CameraCache& frameCam,
    uint64_t collectStampMs);

static void RenderWorldEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& world,
    const Engine::CameraCache& frameCam);

static void RenderRobotEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& robots,
    const Engine::CameraCache& frameCam,
    uint64_t collectStampMs);

static void DrawPlayerSkeletonFromCache(
    ImDrawList* drawList,
    Engine& eng,
    const Engine::PlayerCacheEntry& actor,
    const Engine::CameraCache& frameCam,
    ImU32 color,
    float distanceM)
{
    if (!drawList)
        return;

    const Visuals::EspDrawScale scale =
        Visuals::ComputeEspScaleFromDistance(distanceM);
    // Draw skeletons slightly bolder with a thin 1px black outline 
    // so they pop nicely and don't look stringy.
    const float thickness = (std::max)(2.0f, scale.lineThickness * 1.5f);
    const ImU32 outlineColor = IM_COL32(0, 0, 0, 200);

    for (const auto& [boneA, boneB] : eng.SkeletonLinksArcRaiders) {
        ImVec2 a{};
        ImVec2 b{};
        if (!EspDraw::TrySkeletonSegmentScreen(eng, frameCam, actor, boneA, boneB, a, b))
            continue;

        // Draw outline line underneath
        drawList->AddLine(a, b, outlineColor, thickness + 2.f);
        // Draw primary line on top
        drawList->AddLine(a, b, color, thickness);
    }
}

bool Engine::ShouldDrawPlayerEsp(const PlayerCacheEntry& entry) const
{
    // Drawing is the scanner's admission flag, but it can be false when a new
    // entry was inserted after the bounded EntityList refresh frontier. A
    // validated first position is sufficient for frame admission; distance and
    // ally filters below remain authoritative.
    if (!entry.Drawing && !entry.positionInitialized) {
        if (!var::show_radar)
            return false;
        const float radarM = EffectiveRadarRangeM();
        if (radarM <= 0.f || entry.Distance > radarM)
            return false;
    }
    if (entry.isAlly && var::hide_allies)
        return false;
    // HealthInfo@PS+0x530 is wrong on PioneerPS; health may read 0 — still draw.
    // Do not use the stale scanner distance as a hard render gate; the frame
    // collector recomputes current distance after the camera sample.
    if (entry.Distance < 0.f)
        return false;
    // Do not use the scanner's cached distance here. It is sampled by a slower
    // worker and can be stale for a flying/fast-moving target. The frame
    // collector recomputes distance from the current camera below; applying
    // the range gate before that drops a bot that is actually close now.
    if (!IsPlausibleWorldPos(entry.WorldPos))
        return false;
    return true;
}

bool Engine::ShouldDrawRobotEsp(uintptr_t actorKey, const WorldCacheEntry& entry) const
{
    // A slow scanner flag must not promote an actor that has not been observed
    // in the current actor array. A plausible position alone is not liveness:
    // stale roots can continue returning a plausible vector after despawn.
    // The high-frequency position sampler is the independent current-actor
    // proof; if it has not succeeded recently, keep the entry out of the frame.
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool currentDrawProof = BotEspExpiry::CurrentBotDrawProof(
        entry.lastActorSeenMs, entry.livePositionSampleMs, nowMs);
    Vector3 selectedPosition{};
    const bool hasDrawablePosition = BotEspPosition::Select(
        entry.WorldPos, entry.CenterWorldPos,
        [](const Vector3& p) { return IsPlausibleWorldPos(p); },
        selectedPosition);
    if (!currentDrawProof) {
        const bool actorSeenRecently = entry.lastActorSeenMs != 0
            && (nowMs < entry.lastActorSeenMs
                || nowMs - entry.lastActorSeenMs
                    <= BotEspExpiry::kMaxActorSeenAgeMs);
        LogGhostBotDecision(actorKey, entry,
            !actorSeenRecently ? "actor_not_current" : "no_live_position");
        return false;
    }
    if (!entry.Drawing && !(entry.botIdentityProven && hasDrawablePosition)) {
        if (var::showRobots || var::robotAimEnabled)
            return false;
        if (!var::show_radar)
            return false;
        const float radarM = EffectiveRadarRangeM();
        if (radarM <= 0.f || entry.Distance > radarM)
            return false;
    }
    uintptr_t localPawn = 0;
    {
        std::shared_lock<std::shared_mutex> slock(m_stateMutex);
        localPawn = AcknowledgedPawn;
    }
    if (localPawn && actorKey == localPawn)
        return false;
    if (IsCachedPlayer(actorKey))
        return false;
    // The robot scanner's distance is also stale for fast/flying targets. Keep
    // admission independent of it; CollectEspRenderFrame recomputes the
    // distance from the frame camera before the paint path applies range.
    if (entry.IsBreaked && !var::show_dead_bots)
        return false;
    // Ghost-box kill (BotEspExpiry): a proven bot whose position sample is
    // frozen is a corpse husk or a despawned actor ("ghost bots, nothing
    // there"). Normal samples are <= ~2.1s old even under DMA load, so a 3s
    // stale sample means the sampler stopped updating that entry.
    {
        if (!BotEspExpiry::PosSampleFresh(
                entry.positionSampleMs, entry.admittedMs, nowMs)) {
            LogGhostBotDecision(actorKey, entry, "stale_sample");
            return false;
        }
    }
    // Structural container evidence is a fallback veto for unclassified
    // entries. Once RobotList has positively verified the actor, a temporary
    // empty/garbled display label must not reclassify it as loot and erase the
    // bot from the frame.
    if (!entry.botIdentityProven
        && WorldScan::LooksLikeContainerActor(actorKey, entry.ActorName)
        && !IsAcceptedBotEspLabel(*const_cast<Engine*>(this), entry.ActorName))
        return false;
    return true;
}

bool Engine::ShouldDrawWorldEsp(const WorldCacheEntry& entry) const
{
    if (!entry.Drawing)
        return false;

    WorldLootFilterView filterView{
        entry.worldCategory,
        entry.ActorName,
        entry.ItemDisplayName,
        entry.lootValue,
        entry.lootRarityTier};
    const bool allowEsp =
        AnyWorldEspEnabled() && getAllowWorldEntry(entry);
    const bool allowRadar =
        var::show_radar && WorldCategoryVisibleOnRadar(filterView);
	return allowEsp || allowRadar;
}

// Extraction hatch countdown (SDK FExtractionInfo): refresh State + timer on
// the frame-build copy so a timer that STARTS after admission lights up.
//
// FExtractionInfo { double startedTs; double time; } is ambiguous by design:
// the drop calls +0x8 "duration in seconds", but the game's own marker widget
// ticks a live remaining. Let the data decide — sample twice:
//   decreasing at local rate -> live remaining (use directly)
//   constant                -> duration (subtract elapsed since the observed
//                              startedTs transition / first sight)
namespace {
struct ExtractTimerState {
    double prevField = -1.0;
    double lastStartedTs = -1.0;
    uint64_t prevMs = 0;
    uint64_t startObsMs = 0;
};
std::unordered_map<uintptr_t, ExtractTimerState> s_extractTimerState;
} // namespace

static void RefreshExtractionTimer(Engine::WorldCacheEntry& e)
{
    const uintptr_t actor = e.APawn; // hatch entries stash the actor key here
    if (!actor)
        return;
    e.extractState = static_cast<int8_t>(
        Memory::read<uint8_t>(actor + Offsets::ExtractionPoint_State));

    const double startedTs = Memory::read<double>(
        actor + Offsets::ExtractionPoint_ExtractionInfo);
    const double timeField = Memory::read<double>(
        actor + Offsets::ExtractionPoint_ExtractionInfo + 8);

    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    if (timeField <= 0.0 || timeField > 600.0) {
        e.extractRemainS = -1.0;
        e.extractRemainStampMs = 0;
        return; // no timer (or implausible garbage)
    }

    if (s_extractTimerState.size() > 512)
        s_extractTimerState.clear();
    ExtractTimerState& st = s_extractTimerState[actor];

    double remain = timeField; // default: live-remaining semantics
    if (st.prevMs != 0 && st.prevField > 0.0) {
        const double dtS = static_cast<double>(nowMs - st.prevMs) / 1000.0;
        const double drop = st.prevField - timeField;
        const bool ticking =
            dtS > 0.0 && drop >= dtS * 0.25 && drop <= dtS * 4.0 + 1.0;
        if (!ticking) {
            // Constant field: duration semantics — anchor elapsed on the
            // observed start (a startedTs transition means it just began).
            if (st.startObsMs == 0 || startedTs != st.lastStartedTs)
                st.startObsMs = nowMs;
            remain = timeField - static_cast<double>(nowMs - st.startObsMs) / 1000.0;
        }
    } else {
        st.startObsMs = nowMs; // first sight — anchor for duration mode
    }

    st.prevField = timeField;
    st.lastStartedTs = startedTs;
    st.prevMs = nowMs;

    e.extractRemainS = remain > 0.0 ? remain : 0.0;
    e.extractRemainStampMs = nowMs;
}

bool Engine::CollectEspRenderFrame(EspRenderFrame& out)
{
    out = {};
    out.frameSeq = m_espFrameSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    out.worldGeneration = m_worldGeneration.load(std::memory_order_acquire);
    m_espFrameDiagnostics.lastCollectStartMs.store(
        SteadyEspNowMs(), std::memory_order_release);

    if (!IsEspRaidActive()) {
        RecordEspFrameFailure(EspFrameResult::Inactive);
        return false;
    }

    if (!g_scatter.valid())
        g_scatter.init();
    if (!g_scatter.valid()) {
        RecordEspFrameFailure(EspFrameResult::ScatterInitFailed);
        return false;
    }

    const FeaturePolicy::AimFeatures frameAimFeatures{
        var::enable_aimbot,
        var::robotAimEnabled,
        var::enable_triggerbot};
    if (var::enableesp || var::show_radar
        || FeaturePolicy::ShouldRunAimPass(frameAimFeatures)) {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        out.players.reserve(playerCache.size());
        const uint64_t missNowMs = PlayerEspMiss::NowMs();
        int frameNotDrawing = 0;
        int frameNotInitialized = 0;
        int frameAlly = 0;
        int frameDistance = 0;
        int framePosition = 0;
        for (const auto& [key, entry] : playerCache) {
            if (!entry.Drawing)
                ++frameNotDrawing;
            if (!entry.positionInitialized)
                ++frameNotInitialized;
            if (entry.isAlly && var::hide_allies)
                ++frameAlly;
            if (entry.Distance < 2.f || entry.Distance > PlayerCollectMaxM() * 1.05f)
                ++frameDistance;
            if (!IsPlausibleWorldPos(entry.WorldPos))
                ++framePosition;
            bool include = false;
            bool espSelected = false;
            if (var::enableesp || var::show_radar) {
                if (ShouldDrawPlayerEsp(entry)) {
                    include = true;
                    espSelected = true;
                }
            }
            if (!include && FeaturePolicy::ShouldCollectPlayerTargets(frameAimFeatures)) {
                if (!entry.isAlly && !entry.bIsDead
                    && entry.Distance <= var::aimbot_distance
                    && IsPlausibleWorldPos(entry.WorldPos))
                    include = true;
            }
            if ((var::enableesp || var::show_radar) && !espSelected) {
                // Per-player miss reason (first failing gate wins) — the
                // PlayerEspMissLog answer to "which player is missing and why".
                // An aimbot-only collect still counts as an ESP miss; with ESP
                // fully off nothing is "missing", so no notes are recorded.
                PlayerEspMiss::Reason reason =
                    PlayerEspMiss::Reason::FrameGateReject;
                if (!IsPlausibleWorldPos(entry.WorldPos))
                    reason = PlayerEspMiss::Reason::FramePosition;
                else if (entry.isAlly && var::hide_allies)
                    reason = PlayerEspMiss::Reason::FrameAllyHidden;
                else if (!entry.positionInitialized)
                    reason = PlayerEspMiss::Reason::FrameNotInitialized;
                else if (entry.Distance < 2.f
                    || entry.Distance > PlayerCollectMaxM() * 1.05f)
                    reason = PlayerEspMiss::Reason::FrameDistance;
                else if (!entry.Drawing)
                    reason = PlayerEspMiss::Reason::FrameNotDrawing;
                PlayerEspMiss::Global().NoteAt(missNowMs, key, entry.ActorName,
                    entry.Distance, reason);
            }
            if (!include)
                continue;
            if (espSelected)
                PlayerEspMiss::Global().NoteSelectedAt(
                    missNowMs, key, entry.ActorName);

            EspFramePlayer framePlayer{};
            framePlayer.actorKey = key;
            framePlayer.entry = entry;
            out.players.push_back(std::move(framePlayer));
        }
        SetPlayerFrameResult(out.players.size(), frameNotDrawing,
            frameNotInitialized, frameAlly, frameDistance, framePosition);
    }

    if (AnyWorldEspEnabled() || var::show_radar || var::enableesp) { // near-field reveal: collect world frames while ESP is on
        size_t worldReserve = 0;
        {
            std::shared_lock<std::shared_mutex> lock(m_containerCacheMutex);
            worldReserve += containerCache.size();
        }
        {
            std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
            worldReserve += itemCache.size();
        }
        out.world.reserve(worldReserve);

        // Same actor must not draw twice (e.g. Oil pickup + Crate mis-admit).
        // Prefer item/pickup cache over container cache for a shared key.
        std::unordered_set<uintptr_t> worldKeys;
        auto appendWorld = [&](const std::unordered_map<uintptr_t, WorldCacheEntry>& cache) {
            for (const auto& [key, entry] : cache) {
                if (!ShouldDrawWorldEsp(entry))
                    continue;
                if (IsGroundPickupGoneSticky(key)) {
                    ++g_worldEspDbg.skipPickedUp;
                    continue;
                }
                if (!worldKeys.insert(key).second)
                    continue;

                EspFrameWorld frameWorld{};
                frameWorld.actorKey = key;
                frameWorld.entry = entry;
                // Extraction hatches: refresh SDK state + countdown every frame
                // build so timers that start after admission light up live.
                if (frameWorld.entry.worldCategory ==
                    static_cast<uint8_t>(WorldItemCategory::Hatch))
                    RefreshExtractionTimer(frameWorld.entry);
                out.world.push_back(std::move(frameWorld));
            }
        };

        {
            std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
            appendWorld(itemCache);
        }
        {
            std::shared_lock<std::shared_mutex> lock(m_containerCacheMutex);
            appendWorld(containerCache);
        }
    }

    if (var::showRobots || var::robotAimEnabled || var::show_radar) {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        out.robots.reserve(robotCache.size());
        for (const auto& [key, entry] : robotCache) {
            if (!ShouldDrawRobotEsp(key, entry)) {
                LogGhostBotDecision(key, entry, "collect_gate");
                continue;
            }

            EspFrameWorld frameRobot{};
            frameRobot.actorKey = key;
            frameRobot.entry = entry;
            out.robots.push_back(std::move(frameRobot));
        }
    }

    uintptr_t pc = 0;
    uintptr_t pawn = 0;
    uintptr_t root = 0;
    uintptr_t pcm = 0;
    const bool pcmOk = ResolvePcmForEspFrame(*this, pc, pawn, root, pcm);

    // CL-1341255: POV read directly from the active ViewTarget in CamManager.
    uintptr_t povBase = 0;
    if (pcmOk)
        povBase = pcm;

    uintptr_t rootComp = root;
    if (pawn && IsValidPointer(pawn)) {
        const uintptr_t pawnRoot =
            Memory::read<uintptr_t>(pawn + Offsets::RootComponent);
        if (pawnRoot && IsValidPointer(pawnRoot))
            rootComp = pawnRoot;
    }

    // Flicker fix (debug-5681af): do NOT re-scatter every entity position here.
    // PositionRefreshPass (~20ms) already NOCACHE-scatters Drawing roots into the
    // caches we just copied. A second ~280-actor NOCACHE scatter + serial
    // ResolveLootActorRoot/ReadSceneWorldPos fallbacks co-timed with hitch_pos
    // as ~200–1400ms hitch_frame storms every ~10s (VMM TLB expiry).
    // Camera POV scatter only — keep entry.WorldPos from PositionRefreshPass.
    float povFov = 0.f;
    float defFov = 0.f;
    const float jsonFov = 0.f;
    FVector3d povLoc{};
    FVector3d povRot{};
    FVector3d pawnWorld{};
    Vector3 ctrlRot{};

    if (pcmOk && povBase) {
        g_scatter.prepare(povBase + Offsets::CameraPOV_Location, povLoc);
        g_scatter.prepare(povBase + Offsets::CameraPOV_Rotation, povRot);
        g_scatter.prepare(povBase + Offsets::CameraPOV_FOV, povFov);
        g_scatter.prepare(pcm + Offsets::DefaultFOV, defFov);
    }
    if (rootComp && IsValidPointer(rootComp)) {
        // ComponentToWorld has two candidate slots on this build (live-pinned
        // 0x310, derived fallback 0x2D0) — the probe keeps the pawn scatter on the
        // slot that actually holds a plausible world position, and the read is the
        // FTransform translation inside that block.
        g_scatter.prepare(
            rootComp + Engine::ProbeComponentToWorldOffset(rootComp)
                + Offsets::Transform_Translation,
            pawnWorld);
    }
    if (pc && IsValidPointer(pc))
        g_scatter.prepare(pc + Offsets::ControlRotation, ctrlRot);

    const bool scatterOk = g_scatter.execute();
    if (!scatterOk) {
        // The cache snapshots above are still useful. If the live camera is
        // valid, publish them instead of dropping bots and loot for one failed
        // camera scatter batch.
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        if (!CameraOkForEsp(g_Camera)) {
            RecordEspFrameFailure(EspFrameResult::ScatterExecuteFailed);
            return false;
        }
        out.camera = g_Camera;
    }

    Vector3 pawnPos = Engine::ToVector3(pawnWorld);
    if (!IsPlausibleWorldPos(pawnPos) && rootComp && IsValidPointer(rootComp))
        pawnPos = Memory::read<Vector3>(rootComp + Offsets::RelativeLocation);

    const bool pawnOk = IsPlausibleWorldPos(pawnPos);
    if (scatterOk && !BuildCameraCacheFromPovReads(
            pcmOk,
            povFov,
            defFov,
            jsonFov,
            povLoc,
            povRot,
            pawnPos,
            pawnOk,
            ctrlRot,
            pc && IsValidPointer(pc),
            out.camera,
            nullptr)) {
        // Entity snapshots are already collected. If the dedicated camera
        // worker has a valid live camera, keep publishing the frame rather
        // than throwing away bots and loot for one failed camera build.
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        if (!CameraOkForEsp(g_Camera)) {
            RecordEspFrameFailure(EspFrameResult::CameraFailed);
            return false;
        }
        out.camera = g_Camera;
    }

    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // Intentional ESP-only lead: tighter 0.12s clamp and no velMag boost
    // (Aimbot uses ComputeVelocityLeadDelta with 0.20/0.25 + velMag). Keep
    // separate so paint frames stay conservative vs aim prediction.
    auto extrapolateEntry = [&](Vector3& pos, const Vector3& vel, uint64_t lastUpdateMs) {
        if (lastUpdateMs <= 0.f)
            return;
        const float dtSec =
            (nowMs - static_cast<uint64_t>(lastUpdateMs)) * 0.001f;
        if (dtSec <= 0.f || dtSec > 0.12f)
            return;
        const float ex = static_cast<float>(vel.x * dtSec);
        const float ey = static_cast<float>(vel.y * dtSec);
        const float ez = static_cast<float>(vel.z * dtSec);
        pos.x += ex;
        pos.y += ey;
        pos.z += ez;
    };

    for (EspFramePlayer& framePlayer : out.players) {
        Engine::PlayerCacheEntry& e = framePlayer.entry;
        if (!IsPlausibleWorldPos(e.WorldPos))
            continue;
        extrapolateEntry(e.WorldPos, e.cachedVelocity, e.lastVelocityUpdate);
        // Extrapolate skeleton bones too so they stay on target with the
        // ESP box — without this the skeleton lags one DMA cycle behind.
        for (auto& wb : e.boneData.bonesWorldDouble)
            extrapolateEntry(wb, e.cachedVelocity, e.lastVelocityUpdate);
    }

    for (EspFrameWorld& frameRobot : out.robots) {
        Vector3 selected{};
        if (BotEspPosition::Select(
                frameRobot.entry.WorldPos,
                frameRobot.entry.CenterWorldPos,
                IsPlausibleWorldPos,
                selected))
            frameRobot.entry.WorldPos = selected;
    }

    // Keep bot frame entries at their raw DMA sample. Prediction is applied
    // once in RenderRobotEspFromFrame, immediately before projection, using
    // the actual paint-time gap rather than the worker's earlier timestamp.
    // This prevents variable frame-builder delay from becoming visible stepping.
    for (EspFrameWorld& frameRobot : out.robots) {
        (void)frameRobot;
    }

    {
        const Vector3 distRef = ResolveDistanceReference(out.camera, pawn);
        for (EspFramePlayer& framePlayer : out.players) {
            Engine::PlayerCacheEntry& e = framePlayer.entry;
            if (!IsPlausibleWorldPos(e.WorldPos))
                continue;
            const Vector3& wp = e.WorldPos;
            const double dx = static_cast<double>(wp.x) - distRef.x;
            const double dy = static_cast<double>(wp.y) - distRef.y;
            const double dz = static_cast<double>(wp.z) - distRef.z;
            e.Distance = static_cast<float>(
                std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0);
        }
        for (EspFrameWorld& frameRobot : out.robots) {
            Engine::WorldCacheEntry& e = frameRobot.entry;
            if (!IsPlausibleWorldPos(e.WorldPos))
                continue;
            const Vector3& wp = e.WorldPos;
            const double dx = static_cast<double>(wp.x) - distRef.x;
            const double dy = static_cast<double>(wp.y) - distRef.y;
            const double dz = static_cast<double>(wp.z) - distRef.z;
            e.Distance = static_cast<float>(
                std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0);
        }
    }

    // Apply range only after current-camera distance is known. The old
    // pre-collection check used the scanner's stale Distance and could reject
    // a target that had just flown into range.
    const float playerMaxM = PlayerCollectMaxM();
    if (playerMaxM > 0.f) {
        out.players.erase(
            std::remove_if(
                out.players.begin(), out.players.end(),
                [playerMaxM](const EspFramePlayer& item) {
                    const bool culled =
                        item.entry.Distance > playerMaxM * 1.05f;
                    if (culled) {
                        // Selected earlier, then dropped by the fresh-camera
                        // range gate — the most confusing miss, so name it.
                        PlayerEspMiss::Global().Note(item.actorKey,
                            item.entry.ActorName, item.entry.Distance,
                            PlayerEspMiss::Reason::FrameRangeCull);
                    }
                    return culled;
                }),
            out.players.end());
    }

    out.robots.erase(
        std::remove_if(
            out.robots.begin(),
            out.robots.end(),
            [](const EspFrameWorld& item) {
                return !IsPlausibleWorldPos(item.entry.WorldPos);
            }),
        out.robots.end());

    if (var::skeleton || var::silhouette || var::box || var::enable_aimbot) {
        const float boneMaxM =
            var::esp_distance > 0.f ? var::esp_distance : var::kMaxDistanceSliderM;
        const float aimBoneMaxM = var::enable_aimbot && var::aimbot_distance > 0.f
            ? var::aimbot_distance
            : boneMaxM;
        const float readMaxM = (std::max)(boneMaxM, aimBoneMaxM);
        std::vector<size_t> boneIndices;
        boneIndices.reserve(out.players.size());
        for (size_t i = 0; i < out.players.size(); ++i) {
            const Engine::PlayerCacheEntry& entry = out.players[i].entry;
            if (entry.Distance > readMaxM)
                continue;
            if (!entry.APawn || !IsValidPointer(entry.APawn))
                continue;
            boneIndices.push_back(i);
        }

        std::sort(
            boneIndices.begin(),
            boneIndices.end(),
            [&](size_t a, size_t b) {
                return out.players[a].entry.Distance < out.players[b].entry.Distance;
            });

        if (boneIndices.size() > kMaxEspFrameBoneReads)
            boneIndices.resize(kMaxEspFrameBoneReads);

        for (size_t idx : boneIndices) {
            Engine::PlayerCacheEntry& entry = out.players[idx].entry;
            const uintptr_t liveMesh = GetActorBoneMesh(entry.APawn);
            if (liveMesh)
                entry.actorMesh = liveMesh;
            GetBones(entry);
        }
    }

    out.collectStampMs = SteadyNowMs();
    out.valid = true;
    RecordEspFramePublished(out);

    // #region agent log
    // Flush only — world Drawing transitions are owned by FinalizeWorldCacheMap
    // (World.cpp). CollectEspRenderFrame is the render collect path checkpoint.
    WorldScan::MaybeFlushFlickerScore();
    // Paint-stall diagnostic drain: worker thread only, never the paint path.
    // Writes paint_stall NDJSON (max/avg iteration + spike list) to the log.
    PaintStallFlushToLog();
    // #endregion

    return true;
}

static void DrawPlayerEspList(
    const std::vector<const Engine::PlayerCacheEntry*>& actors,
    const Engine::CameraCache& frameCam)
{
    if (!var::enableesp)
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    const float silhouetteMaxM = var::EffectiveSilhouetteMaxM();

    // Screen-space EMA smoothing — keyed on actor key, cleared each frame.
    // Smooths the projected head/feet so the box glides instead of snapping.
    struct SmoothedPos {
        ImVec2 head{};
        ImVec2 feet{};
        bool valid = false;
        int misses = 0;  // consecutive frames this actor was not drawn
    };
    static std::unordered_map<uintptr_t, SmoothedPos> s_smooth;
    // Raised from 0.6 — the skeleton/silhouette draw the raw unsmoothed bones,
    // so a laggier box could visibly drift off-target during fast movement.
    static constexpr float kSmoothAlpha = 0.85f;   // 0=no smooth, 1=instant.
    // A single missed frame (DMA hiccup, one-frame proj/pick fail) must not
    // erase the smoothed position — that makes the box jump on re-entry.
    // Keep the state across short gaps and only prune after a real absence.
    static constexpr int kSmoothMaxMisses = 15;  // ~0.25s at 60fps
    std::unordered_set<uintptr_t> seenThisFrame;

    for (const Engine::PlayerCacheEntry* actor : actors) {
        if (!actor->APawn || !engine.IsValidPointer(actor->APawn))
            continue;
        // A validated first position completes admission even when the
        // slower EntityList ring has not set Drawing yet. Keep the original
        // Drawing check for never-initialized entries; otherwise a live player
        // can be collected into the frame and then immediately discarded here.
        if (!actor->Drawing && !actor->positionInitialized) {
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintPlayer,
                actor->APawn, false, WorldScan::FlickerCause::VisMiss);
            // #endregion
            continue;
        }
        if (actor->Distance < 2.f)
            continue;
        if (!IsPlausibleWorldPos(actor->WorldPos)) {
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintPlayer,
                actor->APawn, false, WorldScan::FlickerCause::PosFail);
            // #endregion
            continue;
        }

        // Frame-synced WorldPos (scatter + velocity) — no per-paint live DMA.
        const Engine::PlayerCacheEntry& live = *actor;

        Vector3 headWorld{};
        Vector3 feetWorld{};
        if (!EspDraw::ResolvePlayerHeadFeetWorld(live, headWorld, feetWorld)) {
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintPlayer,
                actor->APawn, false, WorldScan::FlickerCause::PosFail);
            // #endregion
            continue;
        }
        // Player hop debug: log first 3 players every 2s
        ImVec2 head{};
        ImVec2 feet{};
        if (!EspDraw::WorldToScreenBox(engine, frameCam, headWorld, feetWorld, head, feet)) {
            // PROJ2 (Fix #13): looking away is not flicker — only count when
            // the player is still roughly in the camera cone.
            if (WorldPointRoughlyInView(frameCam, headWorld)) {
                // #region agent log
                WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintPlayer,
                    actor->APawn, false, WorldScan::FlickerCause::ProjFail);
                // #endregion
            }
            continue;
        }
        if (!EspDraw::IsEspBoxOnScreen(head, feet)) {
            // Looking away is not flicker.
            continue;
        }

        // ── EMA smoothing ──────────────────────────────────────────────
        seenThisFrame.insert(actor->APawn);
        auto it = s_smooth.find(actor->APawn);
        if (it != s_smooth.end() && it->second.valid) {
            SmoothedPos& prev = it->second;
            head.x = prev.head.x + (head.x - prev.head.x) * kSmoothAlpha;
            head.y = prev.head.y + (head.y - prev.head.y) * kSmoothAlpha;
            feet.x = prev.feet.x + (feet.x - prev.feet.x) * kSmoothAlpha;
            feet.y = prev.feet.y + (feet.y - prev.feet.y) * kSmoothAlpha;
        }
        s_smooth[actor->APawn] = { head, feet, true };
        // ───────────────────────────────────────────────────────────────

        // #region agent log
        WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintPlayer,
            actor->APawn, true, WorldScan::FlickerCause::Other);
        // #endregion

        const ImU32 color = PlayerEspColor(*actor);
        const float boxH = feet.y - head.y;
        if (boxH < 2.f)
            continue;

        const Visuals::EspDrawScale scale =
            Visuals::ComputeEspScaleFromBox(boxH, live.Distance);

        const bool wantSilhouette =
            var::silhouette && live.Distance <= silhouetteMaxM;
        bool drewSilhouette = false;

        if (wantSilhouette) {
            Visuals::HumanSilhouetteInput silIn{};
            if (EspDraw::BuildHumanSilhouetteInput(engine, live, frameCam, silIn)) {
                const ImU32 fill = (color & 0x00FFFFFFu)
                    | (static_cast<ImU32>(std::clamp(
                           static_cast<int>((color >> IM_COL32_A_SHIFT) & 0xFF) / 2 + 64,
                           48,
                           180)) << IM_COL32_A_SHIFT);
                Visuals::DrawHumanSilhouetteFilled(
                    drawList, silIn, fill, var::silhouette_soft_fill);
                drewSilhouette = true;
            }
        }
        // Skeleton when silhouette off, OR when silhouette wanted but head/neck
        // failed to build (otherwise Silhouette-on kills the stick figure head).
        if (var::skeleton && !drewSilhouette) {
            DrawPlayerSkeletonFromCache(
                drawList, engine, live, frameCam, color, live.Distance);
        }

        // Skeleton-lag probe: box anchor (red) vs bone pelvis (green). The gap
        // plus the age text shows WHICH lag term dominates: sample age (DMA
        // latency / bone-read cap), extrapolation age, or a real mesh-vs-capsule
        // offset that exists in the game itself.
        if (var::debug_skeleton_lag) {
            const uint64_t nowMsDbg = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            const bool havePelvis = live.boneData.valid.test(
                static_cast<size_t>(UniBone::Pelvis));
            if (havePelvis && live.boneData.readStampMs != 0) {
                const Vector3& pelvisW =
                    live.boneData.bonesWorldDouble[static_cast<size_t>(UniBone::Pelvis)];
                const double dW = std::sqrt(
                    (pelvisW.x - live.WorldPos.x) * (pelvisW.x - live.WorldPos.x)
                    + (pelvisW.y - live.WorldPos.y) * (pelvisW.y - live.WorldPos.y)
                    + (pelvisW.z - live.WorldPos.z) * (pelvisW.z - live.WorldPos.z));

                Vector3 anchorScr{};
                Vector3 pelvisScr{};
                const bool okA = engine.ProjectWorldLocationToScreen(
                    live.WorldPos, anchorScr, frameCam);
                const bool okP = engine.ProjectWorldLocationToScreen(
                    pelvisW, pelvisScr, frameCam);
                if (okA && okP) {
                    const float dPx = static_cast<float>(std::sqrt(
                        (pelvisScr.x - anchorScr.x) * (pelvisScr.x - anchorScr.x)
                        + (pelvisScr.y - anchorScr.y) * (pelvisScr.y - anchorScr.y)));

                    const ImVec2 aS(static_cast<float>(anchorScr.x),
                                    static_cast<float>(anchorScr.y));
                    const ImVec2 pS(static_cast<float>(pelvisScr.x),
                                    static_cast<float>(pelvisScr.y));
                    drawList->AddLine(aS, pS, IM_COL32(255, 255, 0, 200), 1.0f);
                    drawList->AddCircleFilled(aS, 3.5f, IM_COL32(255, 70, 70, 255));
                    drawList->AddCircle(aS, 5.5f, IM_COL32(255, 70, 70, 220));
                    drawList->AddCircleFilled(pS, 3.5f, IM_COL32(70, 255, 70, 255));
                    drawList->AddCircle(pS, 5.5f, IM_COL32(70, 255, 70, 220));

                    const uint64_t ageMs = nowMsDbg - live.boneData.readStampMs;
                    const float extAgeMs = live.lastVelocityUpdate != 0
                        ? static_cast<float>(nowMsDbg - live.lastVelocityUpdate)
                        : -1.f;
                    const float speed = static_cast<float>(std::sqrt(
                        live.cachedVelocity.x * live.cachedVelocity.x
                        + live.cachedVelocity.y * live.cachedVelocity.y
                        + live.cachedVelocity.z * live.cachedVelocity.z));

                    char dbgBuf[160];
                    snprintf(dbgBuf, sizeof(dbgBuf),
                        "age %llums | ext %.0fms | dW %.0fcm | dS %.0fpx | v %.0f",
                        static_cast<unsigned long long>(ageMs),
                        extAgeMs, dW, dPx, speed);
                    drawList->AddText(
                        ImVec2(pS.x + 10.f, pS.y - 8.f),
                        IM_COL32(255, 255, 120, 255), dbgBuf);

                    // Console + file line — closest drawn player, 500ms cadence.
                    // File copy lands in debug-c190fb.log so the probe is
                    // readable outside the exe console.
                    static uint64_t s_skelLastPrintMs = 0;
                    static float s_skelBestDist = -1.f;
                    static char s_skelLine[192] = {};
                    static struct {
                        uint64_t key; double ageMs, extAgeMs, dW, dPx, speed, dist;
                    } s_skelVals{};
                    if (s_skelBestDist < 0.f || live.Distance < s_skelBestDist) {
                        s_skelBestDist = live.Distance;
                        s_skelVals = { static_cast<uint64_t>(live.APawn),
                            static_cast<double>(ageMs), extAgeMs,
                            dW, dPx, speed, live.Distance };
                        snprintf(s_skelLine, sizeof(s_skelLine),
                            "[debugSkel] key=%llu age=%llums extAge=%.0fms "
                            "dWorld=%.1fcm dScreen=%.1fpx speed=%.0fcm/s dist=%.0fm",
                            static_cast<unsigned long long>(live.APawn),
                            static_cast<unsigned long long>(ageMs),
                            extAgeMs, dW, dPx, speed, live.Distance);
                    }
                    if (nowMsDbg - s_skelLastPrintMs >= 500 && s_skelLine[0]) {
                        std::cout << s_skelLine << std::endl;
                        {
                            std::ofstream lf(kArcVerifyPath, std::ios::app);
                            if (lf) {
                                lf << "{\"location\":\"Esp.cpp\"," 
                                   << "\"message\":\"skel_lag\"," 
                                   << "\"data\":{\"key\":" << s_skelVals.key
                                   << ",\"ageMs\":" << s_skelVals.ageMs
                                   << ",\"extAgeMs\":" << s_skelVals.extAgeMs
                                   << ",\"dWcm\":" << s_skelVals.dW
                                   << ",\"dSpx\":" << s_skelVals.dPx
                                   << ",\"speed\":" << s_skelVals.speed
                                   << ",\"dist\":" << s_skelVals.dist
                                   << "},\"ts\":" << nowMsDbg << "}\n";
                            }
                        }
                        s_skelLastPrintMs = nowMsDbg;
                        s_skelLine[0] = 0;
                        s_skelBestDist = -1.f;
                    }
                }
            }
        }

        if (var::box) {
            const Vector3 screenTop{ head.x, head.y, 0.0 };
            const Vector3 screenBottom{ feet.x, feet.y, 0.0 };
            Visuals::Box(screenTop, screenBottom, actor->isVisible, color, 0, scale);
        }

        float labelStackY = head.y;
        if (var::health) {
            const float boxWidth = boxH * 0.65f;
            labelStackY = Visuals::HealthShieldBarsAboveHead(
                head.x,
                head.y,
                boxWidth,
                actor->health,
                actor->maxhealth,
                actor->shield,
                actor->maxshield,
                scale,
                drawList);
        }

        if (var::names || var::show_weapon || var::show_distance || var::show_squad_idx
            || var::show_armor_line || var::show_dbno_badge
            || var::show_steam_ids || var::show_look_arrows)
            StackPlayerLabels(
                drawList, live, head.x, labelStackY, frameCam.Rotation.y, scale);

        if (var::snaplines && EspDraw::IsEspPointOnScreen(feet))
            EspDraw::DrawSnaplineEsp(drawList, feet, color, live.Distance);
    }

    // Prune actors that disappeared this frame so stale smoothed positions
    // don't carry over and cause a ghost slide on re-entry — but only after
    // kSmoothMaxMisses consecutive misses. One-frame flickers (DMA hiccups,
    // brief proj/pos fails) must keep the state so the box glides back instead
    // of snapping.
    for (auto it = s_smooth.begin(); it != s_smooth.end(); ) {
        if (!seenThisFrame.contains(it->first)) {
            if (++it->second.misses >= kSmoothMaxMisses)
                it = s_smooth.erase(it);
            else
                ++it;
        } else {
            it->second.misses = 0;
            ++it;
        }
    }
}

static void RenderPlayerEspFromFrame(
    const std::vector<Engine::EspFramePlayer>& players,
    const Engine::CameraCache& frameCam,
    uint64_t collectStampMs)
{
    if (!var::enableesp)
        return;

    static thread_local std::vector<Engine::PlayerCacheEntry> drawEntries;
    drawEntries.clear();
    drawEntries.reserve(players.size());

    std::unordered_set<uintptr_t> seenKeys;
    std::unordered_set<uintptr_t> seenPlayerStates;
    std::unordered_set<uintptr_t> seenPawns;

    for (const Engine::EspFramePlayer& item : players) {
        if (!IsPlausibleWorldPos(item.entry.WorldPos))
            continue;
        if (item.entry.Distance < 2.f)
            continue;
        if (!seenKeys.insert(item.actorKey).second)
            continue;
        if (item.entry.actorState) {
            if (!seenPlayerStates.insert(item.entry.actorState).second)
                continue;
        } else if (item.entry.APawn && !seenPawns.insert(item.entry.APawn).second) {
            continue;
        }
        drawEntries.push_back(item.entry);
    }

    // Paint-gap velocity continuation: extrapolation ran at COLLECT time with
    // its own clamp, but paint happens 40-90ms later — during that gap the
    // skeleton/world entries freeze while the game moves on. Continue the
    // same cached velocity over the collect→present delta (pure math, no DMA).
    // Clamp bounds worst-case drift if paint stalls.
    if (collectStampMs != 0) {
        const uint64_t paintNowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        float gapSec = static_cast<float>(paintNowMs - collectStampMs) * 0.001f;
        if (gapSec > 0.f && gapSec <= 0.25f) {
            for (Engine::PlayerCacheEntry& e : drawEntries) {
                const Vector3& v = e.cachedVelocity;
                if (v.x == 0.f && v.y == 0.f && v.z == 0.f)
                    continue;
                e.WorldPos.x += v.x * gapSec;
                e.WorldPos.y += v.y * gapSec;
                e.WorldPos.z += v.z * gapSec;
                for (auto& wb : e.boneData.bonesWorldDouble) {
                    wb.x += v.x * gapSec;
                    wb.y += v.y * gapSec;
                    wb.z += v.z * gapSec;
                }
            }
        }
    }

    std::sort(
        drawEntries.begin(),
        drawEntries.end(),
        [](const Engine::PlayerCacheEntry& a, const Engine::PlayerCacheEntry& b) {
            return a.Distance > b.Distance;
        });

    // Bones already refreshed in CollectEspRenderFrame (once per frame build).
    // Per-paint GetBones doubled DMA load and drove FPGA packet loss red.

    std::vector<const Engine::PlayerCacheEntry*> actors;
    actors.reserve(drawEntries.size());
    for (const Engine::PlayerCacheEntry& entry : drawEntries)
        actors.push_back(&entry);

    DrawPlayerEspList(actors, frameCam);
}

static void RenderRobotEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& robots,
    const Engine::CameraCache& frameCam,
    uint64_t collectStampMs)
{
    if (!FeaturePolicy::ShouldRenderRobotEsp(var::showRobots))
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    const Engine::EngineStateSnapshot stateSnap = engine.GetStateSnapshot();
    const Vector3 distRef = engine.ResolveDistanceReference(
        frameCam, stateSnap.acknowledgedPawn);

    struct BotPaintFunnel {
        int input = 0;
        int invalid = 0;
        int playerVeto = 0;
        int dead = 0;
        int position = 0;
        int distance = 0;
        int projection = 0;
        int label = 0;
        int offscreen = 0;
        int painted = 0;
        const char* closestReason = "none";
        uintptr_t closestKey = 0;
        float closestDistance = -1.f;
        bool closestDrawing = false;
        bool closestProven = false;
        uint64_t closestSampleAgeMs = 0;
    } funnel;
    const auto noteSkip = [&](const char* reason, uintptr_t key,
                              const Engine::WorldCacheEntry& robot, float distance) {
        LogGhostBotDecision(key, robot, reason, distance);
        if (distance >= 0.f && (funnel.closestDistance < 0.f
            || distance < funnel.closestDistance)) {
            funnel.closestReason = reason;
            funnel.closestKey = key;
            funnel.closestDistance = distance;
            funnel.closestDrawing = robot.Drawing;
            funnel.closestProven = robot.botIdentityProven;
            const uint64_t now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            funnel.closestSampleAgeMs = robot.positionSampleMs != 0
                && now >= robot.positionSampleMs ? now - robot.positionSampleMs : 0;
        }
    };

    // #region agent log
    // Bots that vanish from the render frame entirely (collect drop) blink
    // without ever hitting a paint skip branch — diff the frame's key set.
    {
        static std::unordered_set<uintptr_t> s_prevPaintBotKeys;
        std::unordered_set<uintptr_t> cur;
        cur.reserve(robots.size());
        for (const Engine::EspFrameWorld& item : robots)
            cur.insert(item.actorKey);
        for (uintptr_t prev : s_prevPaintBotKeys) {
            if (!cur.contains(prev))
                WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                    prev, false, WorldScan::FlickerCause::EvictReadmit);
        }
        s_prevPaintBotKeys = std::move(cur);
    }
    // #endregion

    for (const Engine::EspFrameWorld& item : robots) {
        ++funnel.input;
        const uintptr_t key = item.actorKey;
        const Engine::WorldCacheEntry& robot = item.entry;
        if (!key || !engine.IsValidPointer(key)) {
            ++funnel.invalid;
            noteSkip("invalid", key, robot, -1.f);
            continue;
        }
        if (engine.IsCachedPlayerTry(key)) {
            ++funnel.playerVeto;
            noteSkip("playerVeto", key, robot, -1.f);
            continue;
        }
        if (robot.IsBreaked && !var::show_dead_bots) {
            ++funnel.dead;
            noteSkip("dead", key, robot, -1.f);
            continue;
        }
        if (!IsPlausibleWorldPos(robot.WorldPos)) {
            ++funnel.position;
            noteSkip("position", key, robot, -1.f);
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                key, false, WorldScan::FlickerCause::PosFail);
            // #endregion
            continue;
        }
        // Container veto already applied in ShouldDrawRobotEsp / collect.

        const Vector3& wp = robot.WorldPos;
        const double bdx = static_cast<double>(wp.x) - distRef.x;
        const double bdy = static_cast<double>(wp.y) - distRef.y;
        const double bdz = static_cast<double>(wp.z) - distRef.z;
        const float distM = static_cast<float>(
            std::sqrt(bdx * bdx + bdy * bdy + bdz * bdz) / 100.0);

        const float botMaxM =
            var::bot_esp_distance > 0.f ? var::bot_esp_distance : var::kMaxDistanceSliderM;
        // Dist-edge hysteresis: must match the robot worker's off band (1.15x),
        // otherwise the paint path hard-cuts a bot the worker is still holding.
        constexpr float kDistOffFactor = 1.15f;
        if (distM > botMaxM * kDistOffFactor) {
            ++funnel.distance;
            noteSkip("distance", key, robot, distM);
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                key, false, WorldScan::FlickerCause::DistEdge);
            // #endregion
            continue;
        }

        Engine::WorldCacheEntry drawEntry = robot;

        // Render the track at a fixed delay behind the newest sample and
        // interpolate between the two newest samples (Core/BotMotion.hpp).
        // Snapping to the newest sample is what made the box step once per
        // sample and jump backward on a late one; extrapolation with a noisy
        // velocity then moved it around inside the interval. This replaces both.
        const uint64_t paintNowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const BotMotion::RenderState renderState = BotMotion::ResolveRenderPos(
            BotMotion::Sample{drawEntry.prevSampleWorldPos, drawEntry.prevSampleMs},
            BotMotion::Sample{drawEntry.WorldPos, drawEntry.positionSampleMs},
            drawEntry.cachedVelocity,
            paintNowMs);
        if (renderState.valid) {
            Vector3 target = renderState.pos;
            // Correction smoothing: a late sample corrects the extrapolated
            // target by a jump-sized prediction error, and snapping to it is the
            // visible teleport. Carry the last rendered anchor and glide to the
            // new target at the bot's own speed plus a bounded correction budget,
            // which absorbs the error below perception instead of in one frame.
            {
                struct BotWorldSmooth {
                    Vector3 pos{};
                    uint64_t ms = 0;
                    bool valid = false;
                };
                static std::unordered_map<uintptr_t, BotWorldSmooth> s_botWorldSmooth;
                BotWorldSmooth& smooth = s_botWorldSmooth[key];
                const uint64_t dtMs = (smooth.valid && paintNowMs > smooth.ms)
                    ? paintNowMs - smooth.ms : 0;
                if (smooth.valid && dtMs > 0) {
                    target = BotMotion::SmoothCorrection(
                        smooth.pos, target, dtMs,
                        BotMotion::SpeedOf(drawEntry.cachedVelocity));
                }
                smooth.pos = target;
                smooth.ms = paintNowMs;
                smooth.valid = true;
                if (s_botWorldSmooth.size() > 4096)
                    s_botWorldSmooth.clear();
            }
            const Vector3 delta{
                target.x - drawEntry.WorldPos.x,
                target.y - drawEntry.WorldPos.y,
                target.z - drawEntry.WorldPos.z};
            if (delta.x != 0.0 || delta.y != 0.0 || delta.z != 0.0) {
                drawEntry.WorldPos.x += delta.x;
                drawEntry.WorldPos.y += delta.y;
                drawEntry.WorldPos.z += delta.z;
                if (drawEntry.hasBotHeadWorldPos
                    && IsPlausibleWorldPos(drawEntry.BotHeadWorldPos)) {
                    drawEntry.BotHeadWorldPos.x += delta.x;
                    drawEntry.BotHeadWorldPos.y += delta.y;
                    drawEntry.BotHeadWorldPos.z += delta.z;
                }
                if (IsPlausibleWorldPos(drawEntry.CenterWorldPos)) {
                    drawEntry.CenterWorldPos.x += delta.x;
                    drawEntry.CenterWorldPos.y += delta.y;
                    drawEntry.CenterWorldPos.z += delta.z;
                }
                for (int i = 0; i < drawEntry.BotPartCount; ++i) {
                    if (!IsPlausibleWorldPos(drawEntry.BotPartPos[i]))
                        continue;
                    drawEntry.BotPartPos[i].x += delta.x;
                    drawEntry.BotPartPos[i].y += delta.y;
                    drawEntry.BotPartPos[i].z += delta.z;
                }
            }
        }

        Vector3 headWorld{};
        Vector3 feetWorld{};
        if (!EspDraw::ResolveBotHeadFeetWorld(drawEntry, headWorld, feetWorld)) {
            ++funnel.position;
            noteSkip("headFeet", key, robot, distM);
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                key, false, WorldScan::FlickerCause::PosFail);
            // #endregion
            continue;
        }

        ImVec2 head{};
        ImVec2 feet{};
        // Paint does no DMA: a WorldToScreenBox miss is deterministic geometry
        // (behind near plane / outside bounds), never a flaky read. Repainting
        // the last screen coords here stamped a ghost copy of the box at a
        // stale pixel position for up to 1.5s.
        if (!EspDraw::WorldToScreenBox(engine, frameCam, headWorld, feetWorld, head, feet)) {
            // Flying constructables can keep a plausible scene-root position
            // while their rendered mesh center/head is the useful projection
            // anchor. Retry those already-cached anchors before dropping the
            // bot. This is paint-thread only: no DMA and no new identity gate.
            bool recovered = false;
            const Vector3* fallbackAnchors[] = {
                &drawEntry.CenterWorldPos,
                &drawEntry.BotHeadWorldPos,
            };
            for (const Vector3* anchor : fallbackAnchors) {
                if (!anchor || !IsPlausibleWorldPos(*anchor))
                    continue;
                Vector3 fallbackHead = *anchor;
                Vector3 fallbackFeet = *anchor;
                fallbackHead.z += 90.0;
                fallbackFeet.z -= 90.0;
                if (EspDraw::WorldToScreenBox(
                        engine, frameCam, fallbackHead, fallbackFeet, head, feet)) {
                    recovered = true;
                    break;
                }
            }
            if (!recovered) {
                ++funnel.projection;
                noteSkip("projection", key, robot, distM);
                if (WorldPointRoughlyInView(frameCam, headWorld)) {
                    // #region agent log
                    WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                        key, false, WorldScan::FlickerCause::ProjFail);
                    // #endregion
                }
                continue;
            }
        }

        // ── Bot skeleton/position lag probe + light smoothing ──────────
        {
            struct BotSmooth { ImVec2 head{}; ImVec2 feet{}; bool valid = false; };
            static std::unordered_map<uintptr_t, BotSmooth> s_botSmooth;
            static constexpr float kBotSmoothAlpha = 1.0f;
            auto bs = s_botSmooth.find(key);
            if (bs != s_botSmooth.end() && bs->second.valid) {
                head.x = bs->second.head.x + (head.x - bs->second.head.x) * kBotSmoothAlpha;
                head.y = bs->second.head.y + (head.y - bs->second.head.y) * kBotSmoothAlpha;
                feet.x = bs->second.feet.x + (feet.x - bs->second.feet.x) * kBotSmoothAlpha;
                feet.y = bs->second.feet.y + (feet.y - bs->second.feet.y) * kBotSmoothAlpha;
            }
            s_botSmooth[key] = { head, feet, true };
            if (s_botSmooth.size() > 1024)
                s_botSmooth.clear();

            if (var::debug_skeleton_lag) {
                const uint64_t nowDbg = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                // The rendered (interpolated) anchor, not the raw sample: the
                // probe has to measure what is actually painted.
                const Vector3 anchorW = drawEntry.WorldPos;
                const Vector3 markerW = robot.hasBotHeadWorldPos
                    && IsPlausibleWorldPos(robot.BotHeadWorldPos)
                    ? robot.BotHeadWorldPos
                    : headWorld;
                Vector3 anchorS{};
                Vector3 markerS{};
                const bool anchorOk = engine.ProjectWorldLocationToScreen(
                    anchorW, anchorS, frameCam);
                const bool markerOk = engine.ProjectWorldLocationToScreen(
                    markerW, markerS, frameCam);
                if (anchorOk && markerOk) {
                    const double dx = markerW.x - anchorW.x;
                    const double dy = markerW.y - anchorW.y;
                    const double dz = markerW.z - anchorW.z;
                    const double dWorld = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const float dScreen = static_cast<float>(std::sqrt(
                        (markerS.x - anchorS.x) * (markerS.x - anchorS.x)
                        + (markerS.y - anchorS.y) * (markerS.y - anchorS.y)));
                    const float speed = static_cast<float>(std::sqrt(
                        robot.cachedVelocity.x * robot.cachedVelocity.x
                        + robot.cachedVelocity.y * robot.cachedVelocity.y
                        + robot.cachedVelocity.z * robot.cachedVelocity.z));
                    const uint64_t sampleAge = robot.positionSampleMs != 0
                        ? nowDbg - robot.positionSampleMs : 0;
                    const uint64_t velocityAge = robot.lastVelocityUpdate != 0
                        ? nowDbg - robot.lastVelocityUpdate : 0;
                    // Per-frame rendered displacement for this bot. On a smooth
                    // track stepCm stays proportional to stepDtMs; a choppy one
                    // alternates near-zero and large steps.
                    static std::unordered_map<uintptr_t,
                        std::pair<Vector3, uint64_t>> s_botRenderStep;
                    double stepCm = 0.0;
                    uint64_t stepDtMs = 0;
                    {
                        const auto sit = s_botRenderStep.find(key);
                        if (sit != s_botRenderStep.end()) {
                            const double sx = anchorW.x - sit->second.first.x;
                            const double sy = anchorW.y - sit->second.first.y;
                            const double sz = anchorW.z - sit->second.first.z;
                            stepCm = std::sqrt(sx * sx + sy * sy + sz * sz);
                            stepDtMs = nowDbg > sit->second.second
                                ? nowDbg - sit->second.second : 0;
                        }
                        s_botRenderStep[key] = { anchorW, nowDbg };
                        if (s_botRenderStep.size() > 1024)
                            s_botRenderStep.clear();
                    }

                    drawList->AddLine(
                        ImVec2(static_cast<float>(anchorS.x), static_cast<float>(anchorS.y)),
                        ImVec2(static_cast<float>(markerS.x), static_cast<float>(markerS.y)),
                        IM_COL32(255, 170, 40, 210), 1.0f);
                    drawList->AddCircleFilled(
                        ImVec2(static_cast<float>(anchorS.x), static_cast<float>(anchorS.y)),
                        3.5f, IM_COL32(255, 70, 70, 255));
                    drawList->AddCircleFilled(
                        ImVec2(static_cast<float>(markerS.x), static_cast<float>(markerS.y)),
                        3.5f, IM_COL32(70, 170, 255, 255));

                    char botDbg[256]{};
                    snprintf(botDbg, sizeof(botDbg),
                        "bot age %llums velAge %llums dW %.0fcm dS %.0fpx v %.0f "
                        "int %llums delay %llums lead %llums step %.0fcm/%llums",
                        static_cast<unsigned long long>(sampleAge),
                        static_cast<unsigned long long>(velocityAge),
                        dWorld, dScreen, speed,
                        static_cast<unsigned long long>(renderState.intervalMs),
                        static_cast<unsigned long long>(renderState.delayMs),
                        static_cast<unsigned long long>(renderState.leadMs),
                        stepCm,
                        static_cast<unsigned long long>(stepDtMs));
                    drawList->AddText(
                        ImVec2(static_cast<float>(markerS.x) + 10.f,
                               static_cast<float>(markerS.y) - 8.f),
                        IM_COL32(255, 210, 100, 255), botDbg);

                    static uint64_t s_lastBotProbeLog = 0;
                    static float s_bestBotDist = -1.f;
                    static struct {
                        uint64_t key = 0;
                        uint64_t sampleAge = 0;
                        uint64_t velocityAge = 0;
                        double dWorld = 0.0;
                        float dScreen = 0.f;
                        float speed = 0.f;
                        float dist = 0.f;
                        uint64_t intervalMs = 0;
                        uint64_t delayMs = 0;
                        uint64_t leadMs = 0;
                        double stepCm = 0.0;
                        uint64_t stepDtMs = 0;
                    } best{};
                    if (s_bestBotDist < 0.f || distM < s_bestBotDist) {
                        s_bestBotDist = distM;
                        best = { static_cast<uint64_t>(key), sampleAge, velocityAge,
                            dWorld, dScreen, speed, distM,
                            renderState.intervalMs, renderState.delayMs,
                            renderState.leadMs, stepCm, stepDtMs };
                    }
                    if (nowDbg - s_lastBotProbeLog >= 500) {
                        std::ofstream lf(kArcVerifyPath, std::ios::app);
                        if (lf) {
                            lf << "{\"location\":\"Esp.cpp\",\"message\":\"bot_skel_lag\","
                               << "\"data\":{\"key\":" << best.key
                               << ",\"sampleAgeMs\":" << best.sampleAge
                               << ",\"velocityAgeMs\":" << best.velocityAge
                               << ",\"dWorldCm\":" << best.dWorld
                               << ",\"dScreenPx\":" << best.dScreen
                               << ",\"speed\":" << best.speed
                               << ",\"dist\":" << best.dist
                               << ",\"intervalMs\":" << best.intervalMs
                               << ",\"delayMs\":" << best.delayMs
                               << ",\"leadMs\":" << best.leadMs
                               << ",\"stepCm\":" << best.stepCm
                               << ",\"stepDtMs\":" << best.stepDtMs
                               << "},\"ts\":" << nowDbg << "}\n";
                        }
                        s_lastBotProbeLog = nowDbg;
                        s_bestBotDist = -1.f;
                    }
                }
            }
        }
        // ───────────────────────────────────────────────────────────────

        const ImU32 color_base = BotEspColor(robot.isVisible, robot.IsBreaked);
        const ImU32 color = color_base;
        const float boxH = feet.y - head.y;
        const Visuals::EspDrawScale scale =
            Visuals::ComputeEspScaleFromBox(boxH > 1.f ? boxH : 24.f, distM);

        // Paint path must stay DMA-free. Live GetActorFNameString / GetActorClassFName
        // / ResolveEnemyAssetBotLabel here stalled Present (espMs 500-700). Names are
        // resolved on the robot worker into ActorName / ItemDisplayName. allowDma=false
        // so a flaky fname (Constructable token) skips this bot instead of running the
        // DMA label chain inside Present (the ~20s 100-440ms ghost-copy stall).
        const std::string& fname = robot.ActorName;
        std::string botLabel = ResolveBotDrawLabel(key, robot.ActorName, fname, /*allowDma=*/false);
        if (botLabel.empty() && !robot.ItemDisplayName.empty())
            botLabel = ResolveBotDrawLabel(key, robot.ItemDisplayName, fname, /*allowDma=*/false);
        // Constructable is an internal admission token, not a displayable bot.
        // A cache entry must have a real accepted bot identity before paint;
        // otherwise props/effects with a transient enemy-data pointer become
        // ghost ESP. The worker retries label resolution on its next pass.
        if (botLabel.empty() || !IsAcceptedBotEspLabel(engine, botLabel, fname)) {
            ++funnel.label;
            noteSkip("label", key, robot, distM);
            if (!robot.botIdentityProven) {
                RecordBotDrawLabelMiss();
                // #region agent log
                WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                    key, false, WorldScan::FlickerCause::LabelMiss);
                // #endregion
                continue;
            }
            botLabel = "Bot";
        }
        if (!EspDraw::IsEspBoxOnScreen(head, feet)) {
            ++funnel.offscreen;
            noteSkip("offscreen", key, robot, distM);
            // Looking away is not flicker — leave the track as-is so return
            // to screen does not count as an on->off->on blink.
            continue;
        }

        ++funnel.painted;

        // #region agent log
        WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
            key, true, WorldScan::FlickerCause::Other);
        // #endregion

        // #region agent log
        // bot_appear (verify log): the end-to-end discovery funnel — first
        // seen (pending-lane insert) -> admitted (cache) -> FIRST PAINTED
        // (past every draw gate, on screen). appearMs = seen->painted,
        // admitMs = seen->cached, paintMs = cached->painted. Keyed by
        // firstSeenMs so a reused address spawning a new bot logs again.
        {
            static std::unordered_map<uintptr_t, uint64_t> s_botAppearLogged;
            if (robot.firstSeenMs != 0
                && s_botAppearLogged[key] != robot.firstSeenMs) {
                if (s_botAppearLogged.size() > 4096)
                    s_botAppearLogged.clear();
                s_botAppearLogged[key] = robot.firstSeenMs;
                const uint64_t nowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                const long long appearMs =
                    static_cast<long long>(nowMs - robot.firstSeenMs);
                const long long admitMs =
                    (robot.admittedMs && robot.admittedMs >= robot.firstSeenMs)
                        ? static_cast<long long>(robot.admittedMs - robot.firstSeenMs)
                        : -1;
                const long long paintMs =
                    (robot.admittedMs && nowMs >= robot.admittedMs)
                        ? static_cast<long long>(nowMs - robot.admittedMs)
                        : -1;
                std::ofstream f(kArcVerifyPath, std::ios::app);
                if (f) {
                    char labelEsc[48]{};
                    snprintf(labelEsc, sizeof(labelEsc), "%.40s", botLabel.c_str());
                    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    f << "{\"sessionId\":\"c190fb\",\"runId\":\"appear\","
                      << "\"location\":\"Esp.cpp:paintBot\",\"message\":\"bot_appear\","
                      << "\"data\":{\"key\":" << key
                      << ",\"name\":\"" << labelEsc
                      << "\",\"appearMs\":" << appearMs
                      << ",\"admitMs\":" << admitMs
                      << ",\"paintMs\":" << paintMs
                      << ",\"dist\":" << distM
                      << ",\"fallback\":" << (botLabel == "Bot" ? 1 : 0)
                      << "},\"timestamp\":" << ts << "}\n";
                }
            }
        }
        // #endregion

        // Bot health bar + heart
        float botLabelY = head.y;
        if (robot.maxhealth > 0.f) {
            botLabelY = Visuals::HealthShieldBarsAboveHead(
                head.x, head.y, boxH * 0.65f,
                robot.health, robot.maxhealth,
                0.f, 0.f, scale, drawList);
        }
        if (var::showRobots && var::bot_heart)
            DrawBotHeartIfEnabled(drawList, head, feet, boxH, scale, color);

        if (var::bot_box) {
            const Vector3 screenTop{ head.x, head.y, 0.0 };
            const Vector3 screenBottom{ feet.x, feet.y, 0.0 };
            Visuals::Box(screenTop, screenBottom, robot.isVisible, color, 0, scale);
        }

        if (var::bot_names && !botLabel.empty()) {
            Visuals::Names(
                botLabel,
                head.x,
                botLabelY,
                scale,
                ImColor(255, 255, 255, 255));
        }

        // Best-effort loadout line (weapon + armor tier) for bots that carry an
        // inventory. Everything is self-filtered at read time, so an empty
        // line is the normal case for plain ARC constructables.
        if (var::show_bot_loadout) {
            std::string loadoutLine = robot.weaponName;
            const std::string clip = LoadoutFormat::ClipText(robot.weaponClip);
            if (!clip.empty())
                loadoutLine += loadoutLine.empty() ? clip : " " + clip;
            const std::string armorLine = LoadoutFormat::ArmorText(robot.armorTier, 0.f);
            if (!armorLine.empty()) {
                if (!loadoutLine.empty())
                    loadoutLine += " | ";
                loadoutLine += armorLine;
            }
            if (!loadoutLine.empty()) {
                float loadoutY = botLabelY;
                if (var::bot_names && !botLabel.empty() && ImGui::GetFont()) {
                    loadoutY -= ImGui::GetFont()->CalcTextSizeA(
                        Visuals::LabelTextPx(distM), FLT_MAX, 0.f, botLabel.c_str()).y + 2.f;
                }
                EspDraw::DrawLabelEsp(
                    drawList,
                    ImVec2(head.x, loadoutY),
                    loadoutLine.c_str(),
                    IM_COL32(200, 200, 200, 200),
                    distM);
            }
        }

        if (var::bot_show_distance) {
            char distBuf[32]{};
            snprintf(distBuf, sizeof(distBuf), "%.0fm", distM);
            float labelY = head.y - 4.f;
            if (var::bot_names && !botLabel.empty()) {
                const float nameH = ImGui::GetFont()->CalcTextSizeA(
                    Visuals::LabelTextPx(distM),
                    FLT_MAX, 0.f,
                    botLabel.c_str()).y;
                labelY -= nameH + 4.f;
            }
            EspDraw::DrawLabelEsp(
                drawList,
                ImVec2(head.x, labelY),
                distBuf,
                color,
                distM);
        }

        // Bot vision cone + alertness tag (feature #7): the fan shows what the
        // bot can see, red = Combat (it is hunting something).
        if (var::show_bot_vision && robot.visionValid && distM <= 150.f) {
            VisionCone::Vec2 fan[10];
            const int fanCount = VisionCone::FanPoints(
                static_cast<float>(robot.WorldPos.x),
                static_cast<float>(robot.WorldPos.y),
                robot.facingYawDeg, robot.sightHalfAngleDeg,
                robot.sightRadiusCm, fan, 10);
            if (fanCount >= 3) {
                ImVec2 poly[12];
                int polyN = 0;
                bool allOk = true;
                for (int i = 0; i < fanCount && polyN < 12; ++i) {
                    Vector3 scr{};
                    if (!engine.ProjectWorldLocationToScreen(
                            Vector3{ fan[i].x, fan[i].y, robot.WorldPos.z },
                            scr, frameCam)) {
                        allOk = false;
                        break;
                    }
                    poly[polyN++] = ImVec2(
                        static_cast<float>(scr.x), static_cast<float>(scr.y));
                }
                if (allOk && polyN >= 3) {
                    const bool hunting = robot.alertness >= 3;
                    const ImU32 fill = hunting
                        ? IM_COL32(255, 60, 40, 46)
                        : IM_COL32(255, 200, 60, 34);
                    const ImU32 edge = hunting
                        ? IM_COL32(255, 60, 40, 150)
                        : IM_COL32(255, 200, 60, 110);
                    drawList->AddConvexPolyFilled(poly, polyN, fill);
                    drawList->AddPolyline(
                        poly, polyN, edge, ImDrawFlags_Closed, 1.2f);
                }
            }
            if (var::show_bot_alertness) {
                const char* tag = VisionCone::AlertnessTag(robot.alertness);
                if (tag && tag[0]) {
                    EspDraw::DrawLabelEsp(
                        drawList,
                        ImVec2(head.x, feet.y + 12.f),
                        tag,
                        robot.alertness >= 3
                            ? IM_COL32(255, 80, 60, 235)
                            : IM_COL32(240, 220, 120, 220),
                        distM);
                }
            }
        }

        // Per-part damage pips (feature #8): hp fraction per bot part at its
        // part position (index alignment is best-effort) - a blown-off leg is
        // a red pip on the leg.
        if (var::show_bot_parts && robot.partHpCount > 0 && robot.BotPartCount > 0) {
            int pipN = robot.partHpCount < robot.BotPartCount
                ? robot.partHpCount : robot.BotPartCount;
            if (pipN > 10)
                pipN = 10;
            for (int i = 0; i < pipN; ++i) {
                const PartDamage::Pip cls = PartDamage::Classify(robot.partHp[i]);
                if (cls == PartDamage::Pip::Unread)
                    continue;
                if (!IsPlausibleWorldPos(robot.BotPartPos[i]))
                    continue;
                Vector3 scr{};
                if (!engine.ProjectWorldLocationToScreen(
                        robot.BotPartPos[i], scr, frameCam))
                    continue;
                ImU32 pipCol = IM_COL32(120, 220, 120, 220);
                if (cls == PartDamage::Pip::Hurt)
                    pipCol = IM_COL32(240, 200, 70, 230);
                else if (cls == PartDamage::Pip::Critical)
                    pipCol = IM_COL32(240, 120, 50, 235);
                else if (cls == PartDamage::Pip::Dead)
                    pipCol = IM_COL32(200, 40, 40, 240);
                drawList->AddCircleFilled(
                    ImVec2(static_cast<float>(scr.x), static_cast<float>(scr.y)),
                    3.2f, pipCol, 8);
            }
            const int damaged =
                PartDamage::CountBelow(robot.partHp, robot.partHpCount, 0.95f);
            if (damaged > 0 || robot.destroyedParts > 0) {
                std::string summary =
                    PartDamage::SummaryText(damaged, robot.partHpCount);
                if (robot.destroyedParts > 0)
                    summary += " (" + std::to_string(robot.destroyedParts) + " destroyed)";
                EspDraw::DrawLabelEsp(
                    drawList,
                    ImVec2(head.x, feet.y + 26.f),
                    summary.c_str(),
                    IM_COL32(240, 140, 90, 225),
                    distM);
            }
        }

        if (var::bot_snaplines && EspDraw::IsEspPointOnScreen(feet))
            EspDraw::DrawSnaplineEsp(drawList, feet, color, distM);
    }

    // One compact funnel sample per second keeps the expensive paint path
    // observable without making Present do file I/O every frame.
    {
        static uint64_t s_lastFunnelLogMs = 0;
        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (nowMs - s_lastFunnelLogMs >= 1000) {
            s_lastFunnelLogMs = nowMs;
            std::ofstream f(kArcVerifyPath, std::ios::app);
            if (f) {
                f << "{\"location\":\"Esp.cpp:paintBot\",\"message\":\"bot_paint_funnel\","
                  << "\"data\":{\"input\":" << funnel.input
                  << ",\"invalid\":" << funnel.invalid
                  << ",\"playerVeto\":" << funnel.playerVeto
                  << ",\"dead\":" << funnel.dead
                  << ",\"position\":" << funnel.position
                  << ",\"distance\":" << funnel.distance
                  << ",\"projection\":" << funnel.projection
                  << ",\"label\":" << funnel.label
                  << ",\"offscreen\":" << funnel.offscreen
                  << ",\"painted\":" << funnel.painted
                  << ",\"closestReason\":\"" << funnel.closestReason
                  << "\",\"closestKey\":" << funnel.closestKey
                  << ",\"closestDist\":" << funnel.closestDistance
                  << ",\"closestDrawing\":" << (funnel.closestDrawing ? 1 : 0)
                  << ",\"closestProven\":" << (funnel.closestProven ? 1 : 0)
                  << ",\"closestAgeMs\":" << funnel.closestSampleAgeMs
                  << "},\"ts\":" << nowMs << "}\n";
            }
        }
    }
}

// #region agent log
// Ghost/flicker tracer. On-screen only: this build is Windows subsystem with
// no AllocConsole, so std::cout traces are invisible. Gated on
// var::show_debug_overlay by the caller. Pure observation, no draw changes.

struct GhostTraceEvent {
    uint64_t paint = 0;
    char kind[12]{};
    char src[8]{};
    float dyaw = 0.f;
    float dpitch = 0.f;
    float dloc = 0.f;
    float dtMs = 0.f;
    uint64_t frameSeq = 0;
    int frameAgeMs = 0;
    int entJumps = 0;
    int mixedCam = 0;
    char worst[40]{};
    float x0 = 0.f;
    float y0 = 0.f;
    float x1 = 0.f;
    float y1 = 0.f;
};

struct GhostEntTrack {
    float x = 0.f;
    float y = 0.f;
    float dx = 0.f;
    float dy = 0.f;
    uint64_t paint = 0;
};

static constexpr int kGhostEventSlots = 8;

static void GhostTracePaint(
    const Engine::EspRenderFrame& frame,
    const Engine::CameraCache& renderCam,
    const RenderCamDebug& camDbg)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl)
        return;

    static uint64_t s_paint = 0;
    static uint64_t s_camZigs = 0;
    static uint64_t s_srcFlips = 0;
    static uint64_t s_entZigs = 0;
    static GhostTraceEvent s_events[kGhostEventSlots]{};
    static int s_eventCount = 0;
    static int s_eventHead = 0;
    static std::chrono::steady_clock::time_point s_flashUntil{};

    static Engine::CameraCache s_cam1{};
    static Engine::CameraCache s_cam2{};
    static bool s_have1 = false;
    static bool s_have2 = false;
    static std::chrono::steady_clock::time_point s_prevTp{};
    static char s_prevSrc[8] = "none";
    static std::unordered_map<uintptr_t, GhostEntTrack> s_ent;

    ++s_paint;
    const auto now = std::chrono::steady_clock::now();
    const float dtMs = s_have1
        ? std::chrono::duration<float, std::milli>(now - s_prevTp).count()
        : 0.f;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;

    auto wrapDeg = [](double d) {
        while (d > 180.0)
            d -= 360.0;
        while (d < -180.0)
            d += 360.0;
        return d;
    };

    // Out-and-back detector: a ghost flash steps off the trend for ONE paint
    // and comes straight back. A real flick keeps the same sign.
    float dyawNow = 0.f;
    float dyawPrev = 0.f;
    float dpitchNow = 0.f;
    float dpitchPrev = 0.f;
    float dlocNow = 0.f;
    float dlocPrev = 0.f;
    if (s_have1) {
        dyawNow = static_cast<float>(
            wrapDeg(renderCam.Rotation.y - s_cam1.Rotation.y));
        dpitchNow = static_cast<float>(
            wrapDeg(renderCam.Rotation.x - s_cam1.Rotation.x));
        const double lx = renderCam.Location.x - s_cam1.Location.x;
        const double ly = renderCam.Location.y - s_cam1.Location.y;
        const double lz = renderCam.Location.z - s_cam1.Location.z;
        dlocNow = static_cast<float>(std::sqrt(lx * lx + ly * ly + lz * lz));
    }
    if (s_have2) {
        dyawPrev = static_cast<float>(
            wrapDeg(s_cam1.Rotation.y - s_cam2.Rotation.y));
        dpitchPrev = static_cast<float>(
            wrapDeg(s_cam1.Rotation.x - s_cam2.Rotation.x));
        const double lx = s_cam1.Location.x - s_cam2.Location.x;
        const double ly = s_cam1.Location.y - s_cam2.Location.y;
        const double lz = s_cam1.Location.z - s_cam2.Location.z;
        dlocPrev = static_cast<float>(std::sqrt(lx * lx + ly * ly + lz * lz));
    }

    bool camZig = false;
    if (s_have2) {
        constexpr float kMinDeg = 1.5f;
        if (std::fabs(dyawNow) > kMinDeg && std::fabs(dyawPrev) > kMinDeg
            && dyawNow * dyawPrev < 0.f)
            camZig = true;
        if (std::fabs(dpitchNow) > kMinDeg && std::fabs(dpitchPrev) > kMinDeg
            && dpitchNow * dpitchPrev < 0.f)
            camZig = true;
        if (dlocNow > 200.f && dlocPrev > 200.f)
            camZig = true;
    }

    const bool srcFlip = s_have1 && std::strcmp(camDbg.src, s_prevSrc) != 0;

    // Per-entity screen zigzag: overlay steady but one object teleports.
    int entJumps = 0;
    char worst[40]{};
    float wx0 = 0.f;
    float wy0 = 0.f;
    float wx1 = 0.f;
    float wy1 = 0.f;
    float worstMag = 0.f;
    const float jumpPx = disp.x > 16.f ? disp.x * 0.18f : 200.f;

    auto traceEnt = [&](uintptr_t key, const Vector3& worldPos,
                        const std::string& label) {
        if (!key || !IsPlausibleWorldPos(worldPos))
            return;
        Vector3 scr{};
        if (!engine.ProjectWorldLocationToScreen(worldPos, scr, renderCam))
            return;
        const float x = static_cast<float>(scr.x);
        const float y = static_cast<float>(scr.y);
        float keepDx = 0.f;
        float keepDy = 0.f;
        auto it = s_ent.find(key);
        if (it != s_ent.end() && it->second.paint + 3 >= s_paint) {
            const float dx = x - it->second.x;
            const float dy = y - it->second.y;
            const float mag = std::sqrt(dx * dx + dy * dy);
            const float pmag = std::sqrt(
                it->second.dx * it->second.dx + it->second.dy * it->second.dy);
            const float dot = dx * it->second.dx + dy * it->second.dy;
            if (mag > jumpPx && pmag > jumpPx && dot < 0.f) {
                ++entJumps;
                if (mag > worstMag) {
                    worstMag = mag;
                    wx0 = it->second.x;
                    wy0 = it->second.y;
                    wx1 = x;
                    wy1 = y;
                    std::snprintf(worst, sizeof(worst), "%s",
                        label.empty() ? "?" : label.c_str());
                }
            }
            keepDx = dx;
            keepDy = dy;
        }
        GhostEntTrack& track = s_ent[key];
        track.x = x;
        track.y = y;
        track.dx = keepDx;
        track.dy = keepDy;
        track.paint = s_paint;
    };

    for (const Engine::EspFrameWorld& w : frame.world) {
        traceEnt(w.actorKey, w.entry.WorldPos,
            w.entry.ItemDisplayName.empty()
                ? w.entry.ActorName
                : w.entry.ItemDisplayName);
    }
    for (const Engine::EspFrameWorld& b : frame.robots)
        traceEnt(b.actorKey, b.entry.WorldPos, b.entry.ActorName);
    const std::string kPlayerLabel = "player";
    for (const Engine::EspFramePlayer& pl : frame.players)
        traceEnt(pl.actorKey, pl.entry.WorldPos, kPlayerLabel);

    if (s_ent.size() > 8192)
        s_ent.clear();

    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
    const int frameAgeMs = frame.collectStampMs != 0
        ? static_cast<int>(nowMs - frame.collectStampMs)
        : -1;

    const int mixedCam = g_ghostProjFallback;
    g_ghostProjFallback = 0;

    if (camZig || srcFlip || entJumps > 0) {
        GhostTraceEvent ev{};
        ev.paint = s_paint;
        std::snprintf(ev.kind, sizeof(ev.kind), "%s",
            camZig ? "CAMZIG" : (srcFlip ? "SRCFLIP" : "ENTZIG"));
        std::snprintf(ev.src, sizeof(ev.src), "%s", camDbg.src);
        ev.dyaw = dyawNow;
        ev.dpitch = dpitchNow;
        ev.dloc = dlocNow;
        ev.dtMs = dtMs;
        ev.frameSeq = frame.frameSeq;
        ev.frameAgeMs = frameAgeMs;
        ev.entJumps = entJumps;
        ev.mixedCam = mixedCam;
        std::snprintf(ev.worst, sizeof(ev.worst), "%s", worst);
        ev.x0 = wx0;
        ev.y0 = wy0;
        ev.x1 = wx1;
        ev.y1 = wy1;
        s_events[s_eventHead] = ev;
        s_eventHead = (s_eventHead + 1) % kGhostEventSlots;
        if (s_eventCount < kGhostEventSlots)
            ++s_eventCount;
        if (camZig)
            ++s_camZigs;
        if (srcFlip)
            ++s_srcFlips;
        if (entJumps > 0)
            ++s_entZigs;
        s_flashUntil = now + std::chrono::milliseconds(400);
    }

    s_cam2 = s_cam1;
    s_have2 = s_have1;
    s_cam1 = renderCam;
    s_have1 = true;
    s_prevTp = now;
    std::snprintf(s_prevSrc, sizeof(s_prevSrc), "%s", camDbg.src);

    ImFont* font = ImGui::GetFont();
    const float fs = 14.f;
    const float panelW = 620.f;
    const float px = disp.x > panelW + 40.f ? disp.x - panelW - 20.f : 20.f;
    const float py = 60.f;
    const float panelH = 34.f + (kGhostEventSlots + 6) * 16.f;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + panelW, py + panelH),
        IM_COL32(0, 0, 0, 225));
    dl->AddText(font, 16.f, ImVec2(px + 8.f, py + 5.f),
        IM_COL32(255, 200, 100, 255), "GHOST TRACER");
    float ry = py + 28.f;

    char line[320];
    std::snprintf(line, sizeof(line),
        "paint %llu | src %s | lead %s %.2f/%.2f | leadSkips %d | dt %.1fms",
        static_cast<unsigned long long>(s_paint),
        camDbg.src,
        camDbg.leadApplied ? "ON" : (camDbg.leadSkipped ? "SKIP" : "-"),
        camDbg.leadYaw,
        camDbg.leadPitch,
        g_camLeadSkips,
        dtMs);
    dl->AddText(font, fs, ImVec2(px + 8.f, ry), IM_COL32(200, 220, 255, 255), line);
    ry += 16.f;

    std::snprintf(line, sizeof(line),
        "camzig %llu | srcflip %llu | entzig %llu | mixedCam %d",
        static_cast<unsigned long long>(s_camZigs),
        static_cast<unsigned long long>(s_srcFlips),
        static_cast<unsigned long long>(s_entZigs),
        mixedCam);
    dl->AddText(font, fs, ImVec2(px + 8.f, ry), IM_COL32(200, 220, 255, 255), line);
    ry += 16.f;

    std::snprintf(line, sizeof(line),
        "seq %llu age %dms | ply %d bot %d wld %d | dyaw %.2f dpitch %.2f dloc %.0f",
        static_cast<unsigned long long>(frame.frameSeq),
        frameAgeMs,
        static_cast<int>(frame.players.size()),
        static_cast<int>(frame.robots.size()),
        static_cast<int>(frame.world.size()),
        dyawNow,
        dpitchNow,
        dlocNow);
    dl->AddText(font, fs, ImVec2(px + 8.f, ry), IM_COL32(200, 220, 255, 255), line);
    ry += 16.f;

    const auto& diag = engine.m_espFrameDiagnostics;
    const uint64_t currentGeneration =
        engine.m_worldGeneration.load(std::memory_order_acquire);
    const uint64_t collectStart = diag.lastCollectStartMs.load(std::memory_order_acquire);
    const uint64_t publishMs = diag.lastPublishMs.load(std::memory_order_acquire);
    const uint64_t failureMs = diag.lastFailureMs.load(std::memory_order_acquire);
    const int collectAge = collectStart != 0 && nowMs >= collectStart
        ? static_cast<int>(nowMs - collectStart) : -1;
    const int publishAge = publishMs != 0 && nowMs >= publishMs
        ? static_cast<int>(nowMs - publishMs) : -1;
    const int failureAge = failureMs != 0 && nowMs >= failureMs
        ? static_cast<int>(nowMs - failureMs) : -1;
    std::snprintf(line, sizeof(line),
        "health %s | collect %dms | publish %dms | fail %dms x%d",
        EspFrameResultName(diag.lastResult.load(std::memory_order_acquire)),
        collectAge,
        publishAge,
        failureAge,
        diag.consecutiveFailures.load(std::memory_order_acquire));
    dl->AddText(font, fs, ImVec2(px + 8.f, ry), IM_COL32(160, 220, 255, 255), line);
    ry += 16.f;

    std::snprintf(line, sizeof(line),
        "gen pub %llu cur %llu | published p%d b%d w%d | cam %s",
        static_cast<unsigned long long>(frame.worldGeneration),
        static_cast<unsigned long long>(currentGeneration),
        diag.lastPlayerCount.load(std::memory_order_relaxed),
        diag.lastRobotCount.load(std::memory_order_relaxed),
        diag.lastWorldCount.load(std::memory_order_relaxed),
        camDbg.src);
    dl->AddText(font, fs, ImVec2(px + 8.f, ry), IM_COL32(160, 220, 255, 255), line);
    ry += 16.f;

    std::snprintf(line, sizeof(line),
        "world n%d draw%d | skip pos%d dist%d proj%d allow%d picked%d",
        g_worldEspDbg.frameEntries,
        g_worldEspDbg.rendered,
        g_worldEspDbg.skipPos,
        g_worldEspDbg.skipDist,
        g_worldEspDbg.skipProj,
        g_worldEspDbg.skipAllow,
        g_worldEspDbg.skipPickedUp);
    dl->AddText(font, fs, ImVec2(px + 8.f, ry), IM_COL32(160, 220, 255, 255), line);
    ry += 18.f;

    for (int i = 0; i < s_eventCount; ++i) {
        const int idx =
            (s_eventHead - 1 - i + kGhostEventSlots * 2) % kGhostEventSlots;
        const GhostTraceEvent& ev = s_events[idx];
        std::snprintf(line, sizeof(line),
            "#%llu %s src=%s dy=%.1f dp=%.1f dl=%.0f dt=%.0f seq=%llu age=%d ent=%d mix=%d %s (%.0f,%.0f)>(%.0f,%.0f)",
            static_cast<unsigned long long>(ev.paint),
            ev.kind,
            ev.src,
            ev.dyaw,
            ev.dpitch,
            ev.dloc,
            ev.dtMs,
            static_cast<unsigned long long>(ev.frameSeq),
            ev.frameAgeMs,
            ev.entJumps,
            ev.mixedCam,
            ev.worst,
            ev.x0,
            ev.y0,
            ev.x1,
            ev.y1);
        dl->AddText(font, fs, ImVec2(px + 8.f, ry),
            IM_COL32(255, 120, 120, 255), line);
        ry += 16.f;
    }

    if (now < s_flashUntil) {
        dl->AddRect(ImVec2(3.f, 3.f), ImVec2(disp.x - 3.f, disp.y - 3.f),
            IM_COL32(255, 40, 40, 255), 0.f, 0, 6.f);
    }
}
// #endregion

// #region collision debug (Stage 2): wireframe blocks + LOS rays
namespace {

struct TriDrawCtx {
    Engine::CameraCache cam{};
    float radius = 2000.f;
    int visited = 0;
    int queued = 0;
};

void DrawCollisionTriCb(const CollisionMirror::Tri& t, void* rawCtx)
{
    auto* ctx = static_cast<TriDrawCtx*>(rawCtx);
    ++ctx->visited;
    Vector3 a, b, c;
    if (!engine.ProjectWorldLocationToScreen(t.p0, a, ctx->cam))
        return;
    if (!engine.ProjectWorldLocationToScreen(t.p1, b, ctx->cam))
        return;
    if (!engine.ProjectWorldLocationToScreen(t.p2, c, ctx->cam))
        return;
    const ImU32 col = IM_COL32(140, 150, 170, 110);
    g_renderQueue.addLine(ImVec2((float)a.x, (float)a.y), ImVec2((float)b.x, (float)b.y), col, 1.f);
    g_renderQueue.addLine(ImVec2((float)b.x, (float)b.y), ImVec2((float)c.x, (float)c.y), col, 1.f);
    g_renderQueue.addLine(ImVec2((float)c.x, (float)c.y), ImVec2((float)a.x, (float)a.y), col, 1.f);
    ++ctx->queued;
}

void DrawCollisionDebug(
    const Engine::EspRenderFrame& frame,
    const Engine::CameraCache& renderCam)
{
    // Blocks: wireframe of the published KD-tree triangles near the camera.
    // try_lock inside ForEachTriNear — if a rebuild is mid-swap, skip the
    // frame (never block paint).
    if (var::collision_debug_draw) {
        TriDrawCtx ctx{ renderCam, 2000.f };
        const size_t visited =
            CollisionMirror::ForEachTriNear(
                renderCam.Location, ctx.radius,
                &DrawCollisionTriCb, &ctx);
        {
            static std::chrono::steady_clock::time_point sLastDbgDraw{};
            const auto nowD = std::chrono::steady_clock::now();
            if (nowD - sLastDbgDraw > std::chrono::seconds(1)) {
                sLastDbgDraw = nowD;
                const auto dts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::ofstream f(kArcVerifyPath, std::ios::app);
                if (f) {
                    f << "{\"sessionId\":\"c190fb\",\"location\":\"Esp.cpp\","
                      << "\"message\":\"collision_draw\","
                      << "\"data\":{\"ready\":" << (CollisionMirror::IsReady() ? 1 : 0)
                      << ",\"tris\":" << CollisionMirror::TriangleCount()
                      << ",\"near\":" << visited
                      << ",\"queued\":" << ctx.queued
                      << ",\"camX\":" << static_cast<long long>(renderCam.Location.x)
                      << ",\"camY\":" << static_cast<long long>(renderCam.Location.y)
                      << ",\"camZ\":" << static_cast<long long>(renderCam.Location.z)
                      << "}" << ",\"timestamp\":" << dts << "}\n";
                }
            }
        }
    }

    if (!var::collision_debug_rays)
        return;

    // Rays: camera → each bot's world position, green if clear, red if blocked.
    for (const auto& r : frame.robots) {
        const Vector3& target = r.entry.WorldPos;
        if (target.x == 0.0 && target.y == 0.0 && target.z == 0.0)
            continue;
        Vector3 fromScr, toScr;
        if (!engine.ProjectWorldLocationToScreen(renderCam.Location, fromScr, renderCam))
            continue;
        if (!engine.ProjectWorldLocationToScreen(target, toScr, renderCam))
            continue;
        const CollisionMirror::RayHit hit =
            CollisionMirror::QueryRay(renderCam.Location, target);
        const ImU32 col = hit.visible
            ? IM_COL32(60, 220, 90, 200)
            : IM_COL32(230, 70, 70, 220);
        g_renderQueue.addLine(
            ImVec2((float)fromScr.x, (float)fromScr.y),
            ImVec2((float)toScr.x, (float)toScr.y),
            col, 1.5f);
    }
}

} // namespace
// #endregion

void Engine::RenderEsp()
{
    const bool drawPlayers = var::enableesp;
    const bool drawBots = FeaturePolicy::ShouldRenderRobotEsp(var::showRobots);
    const bool drawWorld = AnyWorldEspEnabled();
    const bool drawCollDbg = var::collision_debug_draw || var::collision_debug_rays;
    if (!drawPlayers && !drawBots && !drawWorld && !var::show_radar && !drawCollDbg)
        return;

    SetProjectionViewport(
        ImGui::GetIO().DisplaySize.x,
        ImGui::GetIO().DisplaySize.y);

    // Never block paint on m_espFrameMutex. Keep only a current, fresh frame;
    // the atomic retained pointer is reset by ClearEspCaches on world changes.
    std::shared_ptr<const EspRenderFrame> paintFrame;
    {
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            if (m_espFrameShared && IsCurrentFreshEspFrame(*m_espFrameShared))
                m_espPaintFrame.store(m_espFrameShared);
            // Do not clear the retained snapshot merely because the newest
            // worker snapshot is briefly stale. The next successful publish
            // replaces it; ClearEspCaches still clears it on world changes.
        }
        paintFrame = m_espPaintFrame.load();
    }
    if (!paintFrame || !IsCurrentFreshEspFrame(*paintFrame)) {
        RenderEspFrameHealthFallback(paintFrame.get(), nullptr);
        return;
    }
    const EspRenderFrame& frame = *paintFrame;

    // #region agent log
    // Sub-phase probe: measure where the [Rend] stall time goes. Memory-only,
    // drained by the worker flush (AgentLog.h PaintSubPhaseNote).
    auto nowT = []() { return std::chrono::steady_clock::now(); };
    auto msT = [](std::chrono::steady_clock::time_point a,
                  std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<float, std::milli>(b - a).count();
    };
    const auto tR0 = nowT();
    // #endregion

    Engine::CameraCache renderCam{};
    RenderCamDebug camDbg{};
    if (!ResolveLiveRenderCamera(frame, renderCam, &camDbg)) {
        RenderEspFrameHealthFallback(&frame, &camDbg);
        return;
    }
    // #region agent log
    PaintSubPhaseNote(0, msT(tR0, nowT()));  // cam
    // #endregion

    g_renderQueue.newFrame();

    // Draw order: items first (bottom), then players, bots last (top) — so
    // bot ESP is the most visible layer and items never cover enemies. ImGui
    // renders later commands on top, so this call order IS the z-stack.
    if (drawWorld) {
        // Same live POV as players/bots. Using frame.camera here lagged behind
        // g_Camera during stick rotation and drove paintWorld projFail spikes.
        const auto tW0 = nowT();
        RenderWorldEspFromFrame(frame.world, renderCam);
        // #region agent log
        PaintSubPhaseNote(1, msT(tW0, nowT()));  // world
        // #endregion
    }
    if (drawPlayers) {
        const auto tP0 = nowT();
        RenderPlayerEspFromFrame(frame.players, renderCam, frame.collectStampMs);
        // #region agent log
        PaintSubPhaseNote(2, msT(tP0, nowT()));  // player
        // #endregion
    }        if (drawBots) {
            const auto tB0 = nowT();
        RenderRobotEspFromFrame(frame.robots, renderCam, frame.collectStampMs);
        // #region agent log
        PaintSubPhaseNote(3, msT(tB0, nowT()));  // bot
        // #endregion
    }

    if (drawCollDbg) {
        const auto tC0 = nowT();
        DrawCollisionDebug(frame, renderCam);
        // #region agent log
        PaintSubPhaseNote(4, msT(tC0, nowT()));  // collision debug
        // #endregion
    }

    g_renderQueue.endFrame();

    if (ImDrawList* drawList = ImGui::GetForegroundDrawList()) {
        RenderQueue::flushToDrawList(
            drawList,
            g_renderQueue.takeCommands());
        if (var::show_debug_overlay && drawWorld) {
            const bool useFrame = CameraOkForEsp(frame.camera);
            const char* camSrc = useFrame ? "FRAME" : "g_Cam";
            Engine::CameraCache gCam{};
            {
                std::shared_lock<std::shared_mutex> lock(engine.m_cameraMutex);
                gCam = engine.g_Camera;
            }
            char dbgTxt1[256], dbgTxt2[256];
            std::snprintf(dbgTxt1, sizeof(dbgTxt1),
                "Cam: %s | Loc: %.0f,%.0f,%.0f | Rot: %.1f,%.1f,%.1f | FOV: %.1f",
                camSrc,
                renderCam.Location.x, renderCam.Location.y, renderCam.Location.z,
                renderCam.Rotation.x, renderCam.Rotation.y, renderCam.Rotation.z,
                renderCam.FOV);
            std::snprintf(dbgTxt2, sizeof(dbgTxt2),
                "g_Cam: %s | Loc: %.0f,%.0f,%.0f | Rot: %.1f,%.1f,%.1f | FOV: %.1f",
                CameraOkForEsp(gCam) ? "ok" : "BAD",
                gCam.Location.x, gCam.Location.y, gCam.Location.z,
                gCam.Rotation.x, gCam.Rotation.y, gCam.Rotation.z,
                gCam.FOV);
            drawList->AddText(ImVec2(10, 10), 0xFF00FF00, dbgTxt1);
            drawList->AddText(ImVec2(10, 10 + ImGui::GetFontSize() + 8), 0xFF00FF00, dbgTxt2);
        }
    }

    // #region agent log
    if (var::show_debug_overlay) {
        const auto tG0 = nowT();
        GhostTracePaint(frame, renderCam, camDbg);
        PaintSubPhaseNote(4, msT(tG0, nowT()));  // tracer
    }
    // #endregion

    if (var::show_debug_overlay && drawWorld) {
        static auto lastWorldDbg = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastWorldDbg >= std::chrono::seconds(1)) {
            lastWorldDbg = now;
            std::cout << "[debugCam] src="
                << (CameraOkForEsp(frame.camera) ? "frame" : "g_Camera")
                << " loc=" << renderCam.Location.x << "," << renderCam.Location.y << "," << renderCam.Location.z
                << " rot=" << renderCam.Rotation.x << "," << renderCam.Rotation.y << "," << renderCam.Rotation.z
                << " fov=" << renderCam.FOV
                << " leadSkip=" << g_camLeadSkips
                << std::endl;
            std::cout << "[debugWorldEsp] frame=" << g_worldEspDbg.frameEntries
                << " rendered=" << g_worldEspDbg.rendered
                << " skipAllow=" << g_worldEspDbg.skipAllow
                << " skipPos=" << g_worldEspDbg.skipPos
                << " skipDist=" << g_worldEspDbg.skipDist
                << " skipProj=" << g_worldEspDbg.skipProj
                << " skipPickedUp=" << g_worldEspDbg.skipPickedUp
                << std::endl;
            g_worldEspDbg = {};
        }    }
}

static void RenderWorldEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& world,
    const Engine::CameraCache& frameCam)
{
    WorldEspDebugStats dbg{}
;
    dbg.frameEntries = static_cast<int>(world.size());

    if (!AnyWorldEspEnabled() && !var::enableesp)
        return; // near-field reveal: still paint near entries per-entry below

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    // Paint path must stay DMA-free. Frame builder already resolved WorldPos + labels.
    const Vector3& distRef = frameCam.Location;

    for (const Engine::EspFrameWorld& item : world) {
        const Engine::WorldCacheEntry& entry = item.entry;
        if (!engine.getAllowWorldEntry(entry)) {
            ++dbg.skipAllow;
            continue;
        }
        if (IsGroundPickupGoneSticky(item.actorKey)) {
            ++dbg.skipPickedUp;
            continue;
        }
        if (!IsPlausibleWorldPos(entry.WorldPos)) {
            ++dbg.skipPos;
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintWorld,
                item.actorKey, false, WorldScan::FlickerCause::PosFail);
            // #endregion
            continue;
        }

        const Vector3& worldPos = entry.WorldPos;
        const double dx = static_cast<double>(worldPos.x) - distRef.x;
        const double dy = static_cast<double>(worldPos.y) - distRef.y;
        const double dz = static_cast<double>(worldPos.z) - distRef.z;
        const float distM = static_cast<float>(
            std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0);

        const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
        const bool isGroundLoot = IsGroundLootEspCategory(cat);
        const bool isContainerEsp =
            WorldCategoryIsContainerProp(cat) && !isGroundLoot;
        const std::string& fname = entry.ActorName;

        if (!isContainerEsp && !isGroundLoot) {
            Vector3 headScreen{};
            if (!ProjectWorldEspPoint(worldPos, frameCam, headScreen))
                continue;

            constexpr float kWorldItemHeightCm = 50.f;
            Vector3 feetWorld = worldPos;
            feetWorld.z -= kWorldItemHeightCm;
            Vector3 feetScreen{};
            if (!ProjectWorldEspPoint(feetWorld, frameCam, feetScreen))
                continue;

            const float boxH = static_cast<float>(std::abs(feetScreen.y - headScreen.y));
            if (boxH < 2.f)
                continue;
        }

        std::string label;
        if (isGroundLoot) {
            label = entry.ItemDisplayName;
            if (label.empty() || IsGenericWorldEspLabel(label) || !IsPlausibleEspLabel(label)) {
                label = ResolveWorldDrawLabel(
                    entry.worldCategory,
                    entry.ActorName,
                    entry.ItemDisplayName);
            }
        } else if (isContainerEsp) {
            label = entry.ItemDisplayName;
            if (label.empty() || IsJunkWorldEspLabel(label) || IsGarbledEspLabel(label)
                || !IsCleanContainerName(label)) {
                // Audit #14: try class fname keyword lookup before falling back
                // to "Crate". ClassFName is stored at ContainerList admission.
                if (!entry.ClassFName.empty()) {
                    const std::string kw = DirectContainerKeywordLabel(
                        entry.ActorName, entry.ClassFName, {});
                    if (!kw.empty() && !IsJunkWorldEspLabel(kw))
                        label = kw;
                }
                if (label.empty() || IsJunkWorldEspLabel(label)
                    || IsGarbledEspLabel(label) || !IsCleanContainerName(label))
                    label = ContainerCategoryFallbackLabel(cat);
            }
            if (var::show_world_open_container
                && cat == WorldItemCategory::OpenedContainer
                && label.find("(Open)") == std::string::npos) {
                label = AppendContainerOpenSuffix(std::move(label));
            }
        } else {
            label = entry.ItemDisplayName;
            if (label.empty() || IsGenericWorldEspLabel(label) || !IsPlausibleEspLabel(label)) {
                label = ResolveWorldDrawLabel(
                    entry.worldCategory,
                    entry.ActorName,
                    entry.ItemDisplayName);
            }
        }

        if (!isContainerEsp && !isGroundLoot
            && (label.empty() || IsGenericWorldEspLabel(label)
                || !IsPlausibleEspLabel(label) || IsJunkWorldEspLabel(label)
                || IsGarbledEspLabel(label)))
            continue;
        if (isGroundLoot && (label.empty() || IsJunkWorldEspLabel(label)
                || IsGarbledEspLabel(label) || !IsPlausibleEspLabel(label)
                || IsGenericWorldEspLabel(label)))
            continue;
        if (label.empty())
            continue;
        if (isContainerEsp && (IsJunkWorldEspLabel(label) || IsGarbledEspLabel(label)))
            label = ContainerCategoryFallbackLabel(cat);

        label = FormatEspDisplayLabel(label);
        if (label.empty()) {
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintWorld,
                item.actorKey, false, WorldScan::FlickerCause::LabelMiss);
            // #endregion
            continue;
        }
        // Quest items get the star prefix (UTF-8 U+2605) — the only label in
        // the app with one. Escaped as raw bytes to survive MSVC codepages.
        if (cat == WorldItemCategory::QuestItem)
            label = "\xE2\x98\x85 " + label;

        int lootValue = entry.lootValue;
        int lootTier = entry.lootRarityTier;
        WorldLootFilterView filterView{
            entry.worldCategory,
            entry.ActorName.empty() ? fname : entry.ActorName,
            label,
            lootValue,
            lootTier};
        const bool looksLikeContainer = isContainerEsp
            || WorldLootEntryLooksLikeContainer(filterView);
        if (looksLikeContainer) {
            lootValue = 0;
            lootTier = 0;
            filterView.lootValue = 0;
            filterView.lootRarityTier = 0;
        }
        // Smooth range exit: fade the label over the last part of its draw
        // range instead of popping off at the hard limit. Band = 20% of the
        // category range (min 4 m); fully faded = skip.
        const float lootMaxM = WorldLootPickupMaxDrawMeters(cat, &filterView);
        const float lootFadeBand = (std::max)(4.f, lootMaxM * 0.2f);
        float lootAlpha = 1.f;
        if (distM > lootMaxM - lootFadeBand)
            lootAlpha = std::clamp((lootMaxM - distM) / lootFadeBand, 0.f, 1.f);
        // Quest items breathe slowly so they catch the eye in a loot pile.
        if (cat == WorldItemCategory::QuestItem) {
            const float t = static_cast<float>(ImGui::GetTime());
            lootAlpha *= 0.72f + 0.28f * (0.5f + 0.5f * sinf(t * 3.5f));
        }
        if (lootAlpha <= 0.02f) {
            ++dbg.skipDist;
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintWorld,
                item.actorKey, false, WorldScan::FlickerCause::DistEdge);
            // #endregion
            continue;
        }

        const bool isPickup = LootItemLooksLikePickup(filterView);

        // Paint does no DMA: a ProjectWorldEspPoint miss is deterministic
        // geometry, never a flaky read. Holding the last screen pos here
        // stamped phantom labels at stale pixel positions for up to 400ms.
        Vector3 screen{};
        if (!ProjectWorldEspPoint(worldPos, frameCam, screen)) {
            ++dbg.skipProj;
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintWorld,
                item.actorKey, false, WorldScan::FlickerCause::ProjFail);
            // #endregion
            continue;
        }

        const float screenW = ImGui::GetIO().DisplaySize.x;
        const float screenH = ImGui::GetIO().DisplaySize.y;
        if (screen.x < -8.f || screen.x > screenW + 8.f
            || screen.y < -8.f || screen.y > screenH + 8.f)
            continue;

        ImU32 color = WorldLootLabelColor(
            cat,
            lootTier,
            isPickup && !looksLikeContainer);
        if (lootAlpha < 1.f) {
            const unsigned fadedA = static_cast<unsigned>(
                static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFu)
                * lootAlpha);
            color = (color & 0x00FFFFFFu) | (fadedA << IM_COL32_A_SHIFT);
        }

        // Looted crates: grey + "(Looted)" tag instead of a whole trip to an
        // empty box. RGB replaced, the distance-fade alpha is kept.
        const bool lootedGrey =
            var::grey_looted_containers && 0 < entry.cachedOpened;
        if (lootedGrey)
            color = (color & 0xFF000000u) | (IM_COL32(140, 140, 140, 255) & 0x00FFFFFFu);

        // Extraction hatches: append the replicated SDK state to the label.
        std::string drawLabel = label;
        if (lootedGrey)
            drawLabel += " (Looted)";
        // Crate contents preview: "Crate [Bandage x2, Med Kit]" answers "is
        // that crate worth running to" from across the room.
        if (var::show_crate_contents && 0 < entry.crateStackCount
            && (isContainerEsp || looksLikeContainer)) {
            drawLabel += " [";
            drawLabel += CrateContents::JoinSummary(
                entry.crateStacks, entry.crateStackCount, 56);
            drawLabel += "]";
        }
        // Dispenser ports (phase 2): " (2 ports)" when the container ejects
        // loot from dispenser drop points and its socket-loot mesh resolved.
        if (var::show_crate_contents && entry.socketLootMesh
            && (isContainerEsp || looksLikeContainer))
            drawLabel += CrateContents::PortsTag(entry.dispenserPorts);
        if (var::show_stack_counts && 1 < entry.stackAmount
            && !isContainerEsp && !looksLikeContainer) {
            char amtBuf[16]{};
            snprintf(amtBuf, sizeof(amtBuf), " x%d", entry.stackAmount);
            drawLabel += amtBuf;
        }
        if (cat == WorldItemCategory::Hatch && entry.extractState >= 0) {
            const char* state = "Unknown";
            switch (entry.extractState) {
            case 0: state = "Dormant"; break;
            case 1: state = "Disabled"; break;
            case 2: state = "Broken"; break;
            case 3: state = "Closed"; break;
            case 4: state = "Closing"; break;
            case 5: state = "Active"; break;
            case 6: state = "Arriving"; break;
            case 7: state = "Ready"; break;
            default: break;
            }
            drawLabel += " (";
            drawLabel += state;
            drawLabel += ")";
        }
        // Extraction countdown (FExtractionInfo): "Hatch (Arriving) [0:23]".
        if (cat == WorldItemCategory::Hatch && entry.extractRemainS >= 0.0) {
            double r = entry.extractRemainS;
            if (entry.extractRemainStampMs != 0) {
                const uint64_t nowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                if (nowMs > entry.extractRemainStampMs)
                    r -= static_cast<double>(nowMs - entry.extractRemainStampMs) / 1000.0;
            }
            if (r < 0.0)
                r = 0.0;
            const int secs = static_cast<int>(r + 0.999);
            char tbuf[16]{};
            snprintf(tbuf, sizeof(tbuf), " [%d:%02d]", secs / 60, secs % 60);
            drawLabel += tbuf;
        }

        char buf[160]{};
        if (var::show_loot_value && lootValue > 0 && isPickup && !isContainerEsp
            && !looksLikeContainer)
            snprintf(buf, sizeof(buf), "%s [%d] [%.0fm]", drawLabel.c_str(), lootValue, distM);
        else
            snprintf(buf, sizeof(buf), "%s [%.0fm]", drawLabel.c_str(), distM);

        EspDraw::DrawLabelEsp(
            drawList,
            ImVec2(static_cast<float>(screen.x), static_cast<float>(screen.y)),
            buf,
            color,
            distM);
        ++dbg.rendered;
        // #region agent log
        WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintWorld,
            item.actorKey, true, WorldScan::FlickerCause::Other);
        // #endregion
    }

    g_worldEspDbg = dbg;
    // File-log the ghost-guard skips (throttled 5s) so leadSkip is verifiable
    // in the debug log — [debugCam] only prints with the debug overlay on.
    if (g_camLeadSkips > 0) {
        static std::chrono::steady_clock::time_point s_lastLeadLog{};
        const auto nowLl = std::chrono::steady_clock::now();
        if (nowLl - s_lastLeadLog >= std::chrono::seconds(5)) {
            s_lastLeadLog = nowLl;
            std::ofstream lf(kArcVerifyPath, std::ios::app);
            if (lf) {
                const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                lf << "{\"location\":\"Esp.cpp\",\"message\":\"cam_lead_skip\","
                   << "\"data\":{\"skips\":" << g_camLeadSkips
                   << "},\"ts\":" << ts << "}\n";
            }
            g_camLeadSkips = 0;
        }
    }
}

void Engine::RenderFovCircle()
{
    const FeaturePolicy::AimFeatures features{
        var::enable_aimbot,
        var::robotAimEnabled,
        var::enable_triggerbot};
    if (!FeaturePolicy::ShouldRenderFov(var::show_fov, features)
        || var::aimbot_fov <= 0.f)
        return;

    float gameFov = 90.f;
    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        gameFov = g_Camera.FOV;
    }
    Visuals::FovCircle(var::aimbot_fov, gameFov);
}

namespace {

constexpr float kCrosshairPi = 3.14159265358979323846f;

ImVec2 CrosshairRotateOffset(float x, float y, float cosA, float sinA)
{
    return ImVec2(x * cosA - y * sinA, x * sinA + y * cosA);
}

void CrosshairDrawRotatedLine(
    ImDrawList* drawList,
    const ImVec2& center,
    float x1,
    float y1,
    float x2,
    float y2,
    float cosA,
    float sinA,
    ImU32 color,
    float thickness)
{
    if (!drawList)
        return;
    const ImVec2 o1 = CrosshairRotateOffset(x1, y1, cosA, sinA);
    const ImVec2 o2 = CrosshairRotateOffset(x2, y2, cosA, sinA);
    drawList->AddLine(
        ImVec2(center.x + o1.x, center.y + o1.y),
        ImVec2(center.x + o2.x, center.y + o2.y),
        color,
        thickness);
}

void CrosshairDrawPlus(
    ImDrawList* drawList,
    const ImVec2& center,
    float size,
    float gap,
    float thickness,
    ImU32 color,
    float angleRad)
{
    const float cosA = std::cos(angleRad);
    const float sinA = std::sin(angleRad);
    const float g = (std::max)(0.f, gap);
    CrosshairDrawRotatedLine(drawList, center, -size, 0.f, -g, 0.f, cosA, sinA, color, thickness);
    CrosshairDrawRotatedLine(drawList, center, g, 0.f, size, 0.f, cosA, sinA, color, thickness);
    CrosshairDrawRotatedLine(drawList, center, 0.f, -size, 0.f, -g, cosA, sinA, color, thickness);
    CrosshairDrawRotatedLine(drawList, center, 0.f, g, 0.f, size, cosA, sinA, color, thickness);
}

void CrosshairDrawCenterDot(
    ImDrawList* drawList,
    const ImVec2& center,
    float radius,
    ImU32 color)
{
    if (!drawList || radius <= 0.f)
        return;
    drawList->AddCircleFilled(center, radius, color);
}

void CrosshairDrawInvertedT(
    ImDrawList* drawList,
    const ImVec2& center,
    float size,
    float thickness,
    ImU32 color,
    float angleRad)
{
    const float cosA = std::cos(angleRad);
    const float sinA = std::sin(angleRad);
    const float barY = -size * 0.5f;
    CrosshairDrawRotatedLine(drawList, center, -size, barY, size, barY, cosA, sinA, color, thickness);
    CrosshairDrawRotatedLine(drawList, center, 0.f, barY, 0.f, size, cosA, sinA, color, thickness);
}

void CrosshairDrawTriangle(
    ImDrawList* drawList,
    const ImVec2& center,
    float size,
    float thickness,
    ImU32 color,
    float angleRad)
{
    if (!drawList)
        return;
    constexpr float kTwoPi = kCrosshairPi * 2.f;
    constexpr float kThird = kTwoPi / 3.f;
    ImVec2 pts[3]{};
    for (int i = 0; i < 3; ++i) {
        const float a = angleRad - kCrosshairPi * 0.5f + static_cast<float>(i) * kThird;
        pts[i] = ImVec2(
            center.x + std::cos(a) * size,
            center.y + std::sin(a) * size);
    }
    for (int i = 0; i < 3; ++i)
        drawList->AddLine(pts[i], pts[(i + 1) % 3], color, thickness);
}

} // namespace

void Engine::RenderOverlayCrosshair()
{
    if (!var::show_crosshair)
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (disp.x <= 0.f || disp.y <= 0.f)
        return;

    const ImVec2 center(disp.x * 0.5f, disp.y * 0.5f);
    const ImU32 color = EspDraw::ColorFromRGBA(var::crosshair_color);
    const float size = (std::max)(1.f, var::crosshair_size);
    const float thickness = (std::max)(0.5f, var::crosshair_thickness);
    const float gap = (std::max)(0.f, var::crosshair_gap);
    const float spinRadPerSec =
        var::crosshair_spin_rpm * (kCrosshairPi * 2.f / 60.f);
    const float angleRad = static_cast<float>(ImGui::GetTime()) * spinRadPerSec;

    switch (var::crosshair_style) {
    case 0: // Classic
        CrosshairDrawPlus(drawList, center, size, gap, thickness, color, 0.f);
        CrosshairDrawCenterDot(drawList, center, thickness * 0.5f + 0.5f, color);
        break;
    case 1: // Dot
        CrosshairDrawCenterDot(drawList, center, size * 0.5f, color);
        break;
    case 2: // Tight
        CrosshairDrawPlus(drawList, center, size * 0.65f, 0.f, thickness, color, 0.f);
        break;
    case 3: // Circle
        drawList->AddCircle(center, size, color, 0, thickness);
        CrosshairDrawPlus(drawList, center, size * 0.35f, 0.f, thickness, color, 0.f);
        break;
    case 4: // T static
        CrosshairDrawInvertedT(drawList, center, size, thickness, color, 0.f);
        break;
    case 5: // T spin
        CrosshairDrawInvertedT(drawList, center, size, thickness, color, angleRad);
        break;
    case 6: // Cross spin
        CrosshairDrawPlus(drawList, center, size, gap, thickness, color, angleRad);
        break;
    case 7: // Tri spin
        CrosshairDrawTriangle(drawList, center, size, thickness, color, angleRad);
        break;
    default:
        CrosshairDrawPlus(drawList, center, size, gap, thickness, color, 0.f);
        CrosshairDrawCenterDot(drawList, center, thickness * 0.5f + 0.5f, color);
        break;
    }
}

void Engine::RenderRaidHud()
{
    if (!var::show_raid_hud)
        return;
    const RaidHudState st = GetRaidHud();
    if (!st.valid)
        return;
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    // Extrapolate the clock between worker refreshes (feature #9).
    double remain = st.timeLeftS;
    if (remain >= 0.0 && nowMs > st.stampMs)
        remain -= static_cast<double>(nowMs - st.stampMs) / 1000.0;
    if (remain < 0.0)
        remain = 0.0;

    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::SetNextWindowPos(
        ImVec2(ds.x * 0.5f, 6.f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::Begin("##raid_hud", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
    if (st.timeLeftS >= 0.0)
        ImGui::Text("Raid %s", RaidClock::FormatClock(remain).c_str());
    if (st.graceS >= 0.0) {
        ImGui::SameLine();
        ImGui::TextColored(
            ImVec4(1.f, 0.8f, 0.3f, 1.f),
            "| Grace %s", RaidClock::FormatClock(st.graceS).c_str());
    }
    if (st.gamePhase >= 0) {
        ImGui::SameLine();
        ImGui::Text("| %s", RaidClock::PhaseText(st.gamePhase).c_str());
    }
    if (st.enemyCount >= 0) {
        ImGui::SameLine();
        ImGui::Text("| Enemies %d", st.enemyCount);
    }
    if (st.pickupCount >= 0) {
        ImGui::SameLine();
        ImGui::Text("| Loot %d", st.pickupCount);
    }
    ImGui::End();
}

void Engine::RenderActivityFeed()
{
    if (!var::show_activity_feed)
        return;
    const ActivityFeed::Feed feed = GetActivityFeed();
    if (feed.Count() <= 0)
        return;
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::SetNextWindowPos(
        ImVec2(8.f, ds.y - 8.f), ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::Begin("##activity_feed", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
    for (int i = 0; i < feed.Count(); ++i) {
        const ActivityFeed::Entry& e = feed.At(i);
        ImGui::Text("[%s] %s",
            ActivityFeed::AgeText(nowMs, e.stampMs).c_str(), e.text);
    }
    ImGui::End();
}

void Engine::RenderRadar(bool interactive)
{
    if (!var::show_radar)
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    // Never CollectEspRenderFrame on the paint thread (DMA). try_lock only.
    std::shared_ptr<const EspRenderFrame> frameShared;
    {
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex, std::try_to_lock);
        if (lock.owns_lock())
            frameShared = m_espFrameShared;
    }
    if (!frameShared || !IsCurrentFreshEspFrame(*frameShared))
        return;
    const EspRenderFrame& frame = *frameShared;

    Engine::CameraCache frameCam{};
    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        frameCam = g_Camera;
    }
    if (frame.valid) {
        Engine::CameraCache liveCam{};
        if (ResolveLiveRenderCamera(frame, liveCam))
            frameCam = liveCam;
    }
    if (!IsPlausibleWorldPos(frameCam.Location))
        return;

    const float screenW = ImGui::GetIO().DisplaySize.x;
    const float screenH = ImGui::GetIO().DisplaySize.y;
    const float radarPx = var::radar_scale > 0.f ? var::radar_scale : 80.f;
    float cx = screenW * std::clamp(var::radar_pos_x_norm, 0.05f, 0.95f);
    float cy = screenH * std::clamp(var::radar_pos_y_norm, 0.05f, 0.95f);
    const float rangeM = var::radar_range > 0.f ? var::radar_range : 100.f;
    const float yawRad = static_cast<float>(engine.DegToRad(static_cast<double>(frameCam.Rotation.y)));
    const float cosYaw = std::cos(yawRad);
    const float sinYaw = std::sin(yawRad);

    if (interactive && screenW > 1.f && screenH > 1.f) {
        ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
        ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::Begin(
            "##radar_drag_layer",
            nullptr,
            ImGuiWindowFlags_NoDecoration
                | ImGuiWindowFlags_NoBackground
                | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoSavedSettings
                | ImGuiWindowFlags_NoNav
                | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::SetCursorScreenPos(ImVec2(cx - radarPx, cy - radarPx));
        ImGui::InvisibleButton("##radar_drag", ImVec2(radarPx * 2.f, radarPx * 2.f));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            var::radar_pos_x_norm = std::clamp(
                var::radar_pos_x_norm + delta.x / screenW, 0.05f, 0.95f);
            var::radar_pos_y_norm = std::clamp(
                var::radar_pos_y_norm + delta.y / screenH, 0.05f, 0.95f);
            cx = screenW * var::radar_pos_x_norm;
            cy = screenH * var::radar_pos_y_norm;
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    const ImU32 bgCol = IM_COL32(16, 16, 22, 190);
    const ImU32 borderCol = IM_COL32(80, 80, 100, 220);
    if (var::radar_shape_circle) {
        drawList->AddCircleFilled(ImVec2(cx, cy), radarPx, bgCol, 48);
        drawList->AddCircle(ImVec2(cx, cy), radarPx, borderCol, 48, 1.5f);
    } else {
        drawList->AddRectFilled(
            ImVec2(cx - radarPx, cy - radarPx),
            ImVec2(cx + radarPx, cy + radarPx),
            bgCol);
        drawList->AddRect(
            ImVec2(cx - radarPx, cy - radarPx),
            ImVec2(cx + radarPx, cy + radarPx),
            borderCol,
            0.f,
            0,
            1.5f);
    }
    drawList->AddCircleFilled(ImVec2(cx, cy), 3.f, IM_COL32(255, 255, 255, 220), 12);

    // Map mode (feature #11): whole-map fit when the minimap bounds resolved.
    bool mapBoundsOk = false;
    const RadarProjection::Bounds mapBounds = GetRadarBounds(mapBoundsOk);

    auto projectBlip = [&](const Vector3& worldPos, float& outX, float& outY) -> bool {
        if (!IsPlausibleWorldPos(worldPos))
            return false;

        if (var::radar_map_mode) {
            float mx = 0.f, my = 0.f;
            if (mapBoundsOk && RadarProjection::MapFitted(
                    worldPos.x, worldPos.y, mapBounds, radarPx, mx, my)) {
                outX = cx + mx;
                outY = cy + my;
                return true;
            }
            // Bounds not resolved yet: north-up player-centered fallback,
            // still geographically oriented.
            if (RadarProjection::NorthUp(
                    worldPos.x, worldPos.y,
                    frameCam.Location.x, frameCam.Location.y,
                    static_cast<double>(rangeM) * 100.0, radarPx, mx, my)) {
                outX = cx + mx;
                outY = cy + my;
                return true;
            }
            return false;
        }

        Vector3 delta = worldPos - frameCam.Location;
        delta.z = 0.f;

        const float distCm = static_cast<float>(
            std::sqrt(delta.x * delta.x + delta.y * delta.y));
        const float distM = distCm / 100.f;
        if (distM > rangeM)
            return false;

        float fwd = static_cast<float>(delta.x * cosYaw + delta.y * sinYaw);
        float rgt = static_cast<float>(-delta.x * sinYaw + delta.y * cosYaw);

        const float fwdM = fwd / 100.f;
        const float rgtM = rgt / 100.f;

        float offX = (rgtM / rangeM) * radarPx;
        float offY = -(fwdM / rangeM) * radarPx;

        const float offLenSq = offX * offX + offY * offY;
        const float maxOff = radarPx;
        if (offLenSq > maxOff * maxOff) {
            const float offLen = std::sqrt(offLenSq);
            const float scale = maxOff / offLen;
            offX *= scale;
            offY *= scale;
        }

        outX = cx + offX;
        outY = cy + offY;
        return true;
    };

    auto drawBlip = [&](float x, float y, ImU32 color, float distanceM,
                        float baseRadius = 3.5f, bool hollow = false) {
        const float dx = x - cx;
        const float dy = y - cy;
        const float clipR = (std::max)(radarPx - 1.f, 1.f);
        if (dx * dx + dy * dy > clipR * clipR)
            return;

        const float radius = std::clamp(
            baseRadius * Visuals::EspDistanceScale(distanceM),
            1.8f,
            baseRadius * 1.12f);
        // Underground-floor blips draw hollow + thinner (feature #11).
        if (hollow)
            drawList->AddCircle(ImVec2(x, y), radius, color, 12, 1.5f);
        else
            drawList->AddCircleFilled(ImVec2(x, y), radius, color, 12);
    };

    auto pickerColor = [](const float rgba[4]) -> ImU32 {
        return EspDraw::ColorFromRGBA(rgba);
    };

    // Radar must never block the paint thread: the player scanner holds
    // m_playerCacheMutex exclusively during DMA-backed scans, and a blocking
    // shared_lock here stalled Present for 26-600ms on a ~20s cadence (user:
    // "copy of everything flashes off to the side"). try_lock + skip the
    // frame if the scanner is mid-scan - the radar is auxiliary, one skipped
    // radar frame is invisible.
    {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex, std::defer_lock);
        if (!lock.try_lock())
            return;
        for (const auto& [key, actor] : playerCache) {
            (void)key;
            if (!ShouldDrawPlayerEsp(actor))
                continue;
            if (actor.isAlly && var::hide_allies)
                continue;
            if (actor.Distance > rangeM)
                continue;
            if (!IsPlausibleWorldPos(actor.WorldPos))
                continue;

            float rx{}, ry{};
            if (!projectBlip(actor.WorldPos, rx, ry))
                continue;

            const ImU32 color = actor.isVisible
                ? pickerColor(var::esp_color_visible)
                : pickerColor(var::esp_color_invisible);
            if (actor.isAlly && var::radar_ally_arrows) {
                const float playerYawRad = static_cast<float>(
                    engine.DegToRad(static_cast<double>(actor.facingYaw)));
                const float relAngle = playerYawRad - yawRad;
                const float arrowLen = 8.f;
                const float baseSpread = 2.3f;
                float tipX = rx + sinf(relAngle) * arrowLen;
                float tipY = ry - cosf(relAngle) * arrowLen;
                float lx = rx + sinf(relAngle + baseSpread) * arrowLen * 0.4f;
                float ly = ry - cosf(relAngle + baseSpread) * arrowLen * 0.4f;
                float rx2 = rx + sinf(relAngle - baseSpread) * arrowLen * 0.4f;
                float ry2 = ry - cosf(relAngle - baseSpread) * arrowLen * 0.4f;
                drawList->AddTriangleFilled(
                    ImVec2(tipX, tipY), ImVec2(lx, ly), ImVec2(rx2, ry2), color);
            } else {
                drawBlip(rx, ry, color, actor.Distance, 3.5f,
                    var::radar_underground_dim
                        && RadarProjection::IsUnderground(
                            actor.WorldPos.z - frameCam.Location.z));
            }
        }
    }

    if (!frame.valid)
        return;

    for (const EspFrameWorld& item : frame.robots) {
        const WorldCacheEntry& robot = item.entry;
        if (!item.actorKey || !engine.IsValidPointer(item.actorKey))
            continue;
        if (robot.IsBreaked && !var::show_dead_bots)
            continue;
        if (robot.Distance > rangeM)
            continue;
        if (!IsPlausibleWorldPos(robot.WorldPos))
            continue;

        float rx{}, ry{};
        if (!projectBlip(robot.WorldPos, rx, ry))
            continue;

        const ImU32 color = robot.IsBreaked
            ? pickerColor(var::color_dead_bots)
            : (robot.isVisible
                ? pickerColor(var::bot_color_visible)
                : pickerColor(var::bot_color_invisible));
        drawBlip(rx, ry, color, robot.Distance, 3.5f,
            var::radar_underground_dim
                && RadarProjection::IsUnderground(
                    robot.WorldPos.z - frameCam.Location.z));
    }

    // Quest items: always shown as a gold star (never filtered by loot
    // rarity/value or the SP-special gate) so objectives pop on the radar.
    auto drawStar = [&](float scx, float scy, float distM, ImU32 color) {
        ImVec2 c(scx, scy);
        const float R = std::clamp(
            4.0f * Visuals::EspDistanceScale(distM), 3.2f, 4.6f);
        const float r = R * 0.42f;
        ImVec2 pts[10];
        for (int i = 0; i < 10; ++i) {
            const float ang = -static_cast<float>(M_PI) / 2.f + static_cast<float>(i) * static_cast<float>(M_PI) / 5.f;
            const float rad = (i % 2 == 0) ? R : r;
            pts[i] = ImVec2(c.x + std::cos(ang) * rad, c.y + std::sin(ang) * rad);
        }
        drawList->AddConvexPolyFilled(pts, 10, color);
        drawList->AddPolyline(pts, 10, IM_COL32(255, 255, 255, 180), true, 1.f);
    };

    for (const EspFrameWorld& item : frame.world) {
        const WorldCacheEntry& entry = item.entry;
        if (!IsPlausibleWorldPos(entry.WorldPos))
            continue;

        const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
        if (cat == WorldItemCategory::QuestItem) {
            float rx{}, ry{};
            if (!projectBlip(entry.WorldPos, rx, ry))
                continue;
            drawStar(rx, ry, entry.Distance, IM_COL32(255, 200, 60, 255));
            continue;
        }

        WorldLootFilterView filterView{
            entry.worldCategory,
            entry.ActorName,
            entry.ItemDisplayName,
            entry.lootValue,
            entry.lootRarityTier};
        if (!WorldCategoryVisibleOnRadar(filterView))
            continue;

        float rx{}, ry{};
        if (!projectBlip(entry.WorldPos, rx, ry))
            continue;

        const bool isPickup = LootItemLooksLikePickup(filterView);
        const ImU32 color = isPickup
            ? WorldLootLabelColor(cat, entry.lootRarityTier, true)
            : WorldCategoryLabelColor(cat);
        drawBlip(rx, ry, color, entry.Distance, isPickup ? 3.f : 2.5f,
            var::radar_underground_dim
                && RadarProjection::IsUnderground(
                    entry.WorldPos.z - frameCam.Location.z));
    }
}
