#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/AssetNames.h"
#include "../Core/Memory.h"
#include "../Core/WorldItemCategory.h"
#include "../Functions/WorldScanCommon.h"
#include "../Functions/RobotList.h"
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
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <vector>

extern Engine engine;

namespace {

constexpr size_t kMaxEspFramePosReads = 768;
constexpr size_t kMaxEspFrameBoneReads = 16;
constexpr size_t kReservedPlayerPosReads = 48;

namespace {

struct WorldEspDebugStats {
    int frameEntries = 0;
    int rendered = 0;
    int skipAllow = 0;
    int skipPos = 0;
    int skipDist = 0;
    int skipProj = 0;
};

WorldEspDebugStats g_worldEspDbg{};

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

static std::string ResolveContainerEspDrawLabel(
    uintptr_t key,
    const Engine::WorldCacheEntry& entry,
    const std::string& fname,
    const std::string& classFname)
{
    const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
    const std::string fnameHint = entry.ActorName.empty() ? fname : entry.ActorName;
    const bool isOpened = var::show_world_open_container
        && (cat == WorldItemCategory::OpenedContainer
            || ContainerLootLooksOpenedAny(key, fnameHint));

    if (isOpened
        && entry.ItemDisplayName.find("(Open)") != std::string::npos
        && !IsJunkWorldEspLabel(entry.ItemDisplayName)
        && !IsGarbledEspLabel(entry.ItemDisplayName))
        return FormatEspDisplayLabel(entry.ItemDisplayName);

    std::string baseLabel;

    if (const std::string kw = DirectContainerKeywordLabel(
            entry.ActorName.empty() ? fname : entry.ActorName,
            classFname,
            GetActorDataAssetFName(key));
        !kw.empty() && !IsJunkWorldEspLabel(kw))
        baseLabel = kw;

    if (baseLabel.empty()
        && IsCleanContainerName(entry.ItemDisplayName)
        && !IsJunkWorldEspLabel(entry.ItemDisplayName)
        && !IsGarbledEspLabel(entry.ItemDisplayName))
        baseLabel = entry.ItemDisplayName;

    std::string hint = entry.ActorName;
    if (hint.empty())
        hint = fname;
    if (hint.empty())
        hint = classFname;

    if (baseLabel.empty() && !hint.empty()) {
        if (const std::string fromTable = ResolveContainerDisplayLabel(hint, {});
            IsCleanContainerName(fromTable))
            baseLabel = fromTable;
        if (baseLabel.empty()) {
            if (const std::string fromWorld = LookupWorldObjectByFName(hint);
                IsCleanContainerName(fromWorld))
                baseLabel = fromWorld;
        }
    }

    if (baseLabel.empty()) {
        if (const std::string memName = engine.GetEnglishItemName(key);
            IsCleanContainerName(memName))
            baseLabel = memName;
    }

    if (baseLabel.empty() && !hint.empty()) {
        if (const std::string resolved = ResolveContainerEspDisplayName(key, hint);
            IsCleanContainerName(resolved))
            baseLabel = resolved;
    }

    if (baseLabel.empty())
        baseLabel = ContainerCategoryFallbackLabel(
            isOpened ? WorldItemCategory::OpenedContainer : cat);

    if (isOpened)
        return AppendContainerOpenSuffix(std::move(baseLabel));
    return FormatEspDisplayLabel(baseLabel);
}

} // namespace

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
    float maxM = 0.f;
    if (var::enableesp)
        maxM = (std::max)(maxM, var::esp_distance);
    if (var::show_radar)
        maxM = (std::max)(maxM, EffectiveRadarRangeM());
    return maxM;
}

inline float BotCollectMaxM()
{
    float maxM = 0.f;
    if (var::showRobots || var::robotAimEnabled)
        maxM = (std::max)(maxM, var::bot_esp_distance);
    if (var::show_radar)
        maxM = (std::max)(maxM, EffectiveRadarRangeM());
    return maxM;
}

void DrawPulsatingHeart(ImDrawList* dl, ImVec2 center, float baseRadius, ImU32 color)
{
    if (!dl || baseRadius < 0.5f)
        return;

    const float t = static_cast<float>(ImGui::GetTime());
    const float beat = 0.5f + 0.5f * sinf(t * 6.283185f * 1.25f);
    const float scale = 0.82f + 0.18f * beat;
    const float s = baseRadius * scale;

    const int alpha = static_cast<int>((color >> IM_COL32_A_SHIFT) & 0xFF);
    const int pulsedAlpha =
        std::clamp(static_cast<int>(alpha * (0.65f + 0.35f * beat)), 48, 255);
    const ImU32 col =
        (color & 0x00FFFFFFu) | (static_cast<ImU32>(pulsedAlpha) << IM_COL32_A_SHIFT);

    const ImVec2 left(center.x - s * 0.28f, center.y - s * 0.12f);
    const ImVec2 right(center.x + s * 0.28f, center.y - s * 0.12f);
    dl->AddCircleFilled(left, s * 0.38f, col, 12);
    dl->AddCircleFilled(right, s * 0.38f, col, 12);
    dl->AddTriangleFilled(
        ImVec2(center.x - s * 0.52f, center.y + s * 0.02f),
        ImVec2(center.x + s * 0.52f, center.y + s * 0.02f),
        ImVec2(center.x, center.y + s * 0.58f),
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

    auto IsPcmValid = [&](uintptr_t p) {
        if (!p || !eng.IsValidPointer(p))
            return false;
        const float f = Memory::read<float>(p + Offsets::DefaultFOV);
        return f > 1.0f && f < 179.0f;
    };

    if (pc && !IsPcmValid(pcm))
        pcm = Memory::read<uintptr_t>(pc + Offsets::APlayerCameraManager);
    // LP may be 0 — still recover PCM via FName / PCOwner scan.
    if (!IsPcmValid(pcm))
        pcm = eng.GetCameraManagerFromActors();
    if (!IsPcmValid(pcm)) {
        const uintptr_t level = eng.PersistentLevel;
        const uintptr_t actors = eng.Actors;
        if (level && actors) {
            uintptr_t pcmPc = pc;
            uintptr_t pcmPawn = pawn;
            uintptr_t foundPcm = pcm;
            if (eng.ResolvePcFromLevelCameraManager(level, actors, pcmPc, pcmPawn, foundPcm)) {
                pcm = foundPcm;
                pc = pcmPc;
                pawn = pcmPawn;
            }
        }
    }

    return pcm && eng.IsValidPointer(pcm);
}

static bool CameraOkForEsp(const Engine::CameraCache& cam)
{
    if (!IsUsableCameraFov(cam.FOV))
        return false;
    return IsPlausibleWorldPos(cam.Location);
}

static bool ResolveLiveRenderCamera(
    const Engine::EspRenderFrame& frame,
    Engine::CameraCache& outCam)
{
    // Paint path must NOT DMA. TryBuildCameraFromPcmPov (read_nocache x4) stalled
    // Present 200-441ms per hitch (debugFlicker maxCamMs≈maxEspMs; H6 slow_cam).
    // Worker UpdateCamera already publishes g_Camera; frame.camera is the backup.
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
                constexpr float kLead = 0.5f;
                constexpr float kMaxDeg = 3.5f;
                float leadYaw = dyaw * kLead;
                float leadPitch = dpitch * kLead;
                if (leadYaw > kMaxDeg) leadYaw = kMaxDeg;
                else if (leadYaw < -kMaxDeg) leadYaw = -kMaxDeg;
                if (leadPitch > kMaxDeg) leadPitch = kMaxDeg;
                else if (leadPitch < -kMaxDeg) leadPitch = -kMaxDeg;
                cam.Rotation.y += leadYaw;
                cam.Rotation.x += leadPitch;
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
            applyRotExtrap(outCam);
            return true;
        }
    }
    if (frame.valid && CameraOkForEsp(frame.camera)) {
        outCam = frame.camera;
        applyRotExtrap(outCam);
        return true;
    }
    return false;
}

static Vector3 ResolveWorldEspDrawPos(
    uintptr_t actorKey,
    const Engine::WorldCacheEntry& entry)
{
    auto tryComp = [](uintptr_t comp) -> Vector3 {
        if (!comp || !engine.IsValidPointer(comp))
            return {};
        const Vector3 pos = Engine::ReadSceneWorldPos(comp);
        return IsPlausibleWorldPos(pos) ? pos : Vector3{};
    };

    // Same-frame scatter position is synchronized with frame camera — prefer it.
    if (IsPlausibleWorldPos(entry.WorldPos))
        return entry.WorldPos;

    const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
    const bool preferPickupCollider = IsGroundLootEspCategory(cat)
        || cat == WorldItemCategory::Items
        || cat == WorldItemCategory::Harvestable;

    if (actorKey && preferPickupCollider) {
        if (const uintptr_t lootRoot =
                Engine::ResolveLootActorRoot(actorKey, true)) {
            if (const Vector3 fromLoot = tryComp(lootRoot); IsPlausibleWorldPos(fromLoot))
                return fromLoot;
        }
    }

    if (const Vector3 fromRoot = tryComp(entry.rootComponent); IsPlausibleWorldPos(fromRoot))
        return fromRoot;

    if (actorKey) {
        const uintptr_t resolved = Engine::ResolveLootActorRoot(
            actorKey, preferPickupCollider);
        if (const Vector3 fromResolved = tryComp(resolved); IsPlausibleWorldPos(fromResolved))
            return fromResolved;

        const uintptr_t skMesh = engine.GetActorSkeletalMesh(actorKey);
        if (const Vector3 fromSk = tryComp(skMesh); IsPlausibleWorldPos(fromSk))
            return fromSk;

        const uintptr_t embark =
            Memory::read<uintptr_t>(actorKey + Offsets::EmbarkMesh);
        if (const Vector3 fromEmbark = tryComp(embark); IsPlausibleWorldPos(fromEmbark))
            return fromEmbark;
    }

    return entry.WorldPos;
}

static bool ProjectWorldEspPoint(
    const Vector3& worldPos,
    const Engine::CameraCache& frameCam,
    Vector3& outScreen)
{
    if (engine.ProjectWorldLocationToScreen(worldPos, outScreen, frameCam))
        return true;

    Engine::CameraCache liveCam{};
    {
        std::shared_lock<std::shared_mutex> lock(engine.m_cameraMutex);
        liveCam = engine.g_Camera;
    }
    if (IsUsableCameraFov(liveCam.FOV) && IsPlausibleWorldPos(liveCam.Location))
        return engine.ProjectWorldLocationToScreen(worldPos, outScreen, liveCam);
    return false;
}

static ImU32 PlayerEspColor(const Engine::PlayerCacheEntry& actor)
{
    const float* c = actor.isVisible ? var::esp_color_visible : var::esp_color_invisible;
    return IM_COL32(
        static_cast<int>(c[0] * 255.f),
        static_cast<int>(c[1] * 255.f),
        static_cast<int>(c[2] * 255.f),
        static_cast<int>(c[3] * 255.f));
}

static ImU32 BotEspColor(bool visible, bool isBreaked = false)
{
    const float* c = isBreaked
        ? var::color_dead_bots
        : (visible ? var::bot_color_visible : var::bot_color_invisible);
    return IM_COL32(
        static_cast<int>(c[0] * 255.f),
        static_cast<int>(c[1] * 255.f),
        static_cast<int>(c[2] * 255.f),
        static_cast<int>(c[3] * 255.f));
}

static ImU32 WeaponTierColor(int quality)
{
    switch (quality) {
    case 1: return IM_COL32(180, 180, 180, 255);
    case 2: return IM_COL32(100, 200, 100, 255);
    case 3: return IM_COL32(80, 140, 220, 255);
    case 4: return IM_COL32(160, 80, 200, 255);
    case 5: return IM_COL32(220, 180, 60, 255);
    default: return IM_COL32(220, 220, 220, 255);
    }
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
    if (!engine.IsPlayerWeaponEspLabel(active)
        && active != "Unarmed")
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
    if (active == stowed0)
        stowed0.clear();
    if (active == stowed1)
        stowed1.clear();

    // #region agent log
    {
        static auto s_lastWeaponLog = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - s_lastWeaponLog > std::chrono::milliseconds(1500)) {
            s_lastWeaponLog = now;
            std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
            if (f) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                auto esc = [](const std::string& s) {
                    std::string o;
                    o.reserve(s.size());
                    for (char c : s) {
                        if (c == '"' || c == '\\') o.push_back('\\');
                        o.push_back(c);
                    }
                    return o;
                };
                f << "{\"sessionId\":\"c190fb\",\"runId\":\"weapon-label\",\"hypothesisId\":\"H1\""
                  << ",\"location\":\"Esp.cpp:DrawWeaponLabel\",\"message\":\"player weapon stack\""
                  << ",\"data\":{\"rawActive\":\"" << esc(actor.weaponName)
                  << "\",\"rawS0\":\"" << esc(actor.stowedWeapon0)
                  << "\",\"rawS1\":\"" << esc(actor.stowedWeapon1)
                  << "\",\"drawActive\":\"" << esc(active)
                  << "\",\"drawS0\":\"" << esc(stowed0)
                  << "\",\"drawS1\":\"" << esc(stowed1)
                  << "\",\"anchorX\":" << anchorX
                  << "},\"timestamp\":" << ms << "}\n";
            }
        }
    }
    // #endregion

    // Active weapon — centered on head X (same as name/distance).
    if (!active.empty()) {
        const ImU32 wColor = (active != "Unarmed" && actor.weaponQuality > 0)
            ? WeaponTierColor(actor.weaponQuality)
            : IM_COL32(220, 220, 220, 255);
        EspDraw::DrawLabelEsp(
            drawList,
            ImVec2(anchorX, labelStackY),
            active.c_str(),
            wColor,
            actor.Distance);
        labelStackY -= LabelTextHeight(active.c_str(), actor.Distance) + 4.f;
    }

    // Stowed weapons — same center X (no indent / leading spaces).
    for (const std::string* sw : { &stowed0, &stowed1 }) {
        if (sw->empty()) continue;
        const ImU32 sColor = IM_COL32(180, 180, 180, 200);
        EspDraw::DrawLabelEsp(
            drawList,
            ImVec2(anchorX, labelStackY),
            sw->c_str(),
            sColor,
            actor.Distance);
        labelStackY -= LabelTextHeight(sw->c_str(), actor.Distance) + 2.f;
    }
}

static float StackPlayerLabels(
    ImDrawList* drawList,
    const Engine::PlayerCacheEntry& actor,
    float headX,
    float& labelStackY,
    const Visuals::EspDrawScale& scale)
{
    if (var::names) {
        const char* nameLabel = actor.ActorName.empty() ? "Raider" : actor.ActorName.c_str();
        Visuals::Names(
            nameLabel,
            headX,
            labelStackY,
            scale,
            ImColor(255, 255, 255, 255));
        labelStackY -= LabelTextHeight(nameLabel, actor.Distance) + 6.f;
    }

    if (var::show_weapon) {
        DrawWeaponLabel(drawList, actor, headX, labelStackY);
    }

    if (var::show_distance) {
        char distBuf[32]{};
        snprintf(distBuf, sizeof(distBuf), "%.0fm", actor.Distance);
        EspDraw::DrawLabelEsp(
            drawList,
            ImVec2(headX, labelStackY),
            distBuf,
            actor.isVisible ? IM_COL32(220, 220, 220, 255)
                            : IM_COL32(255, 120, 120, 255),
            actor.Distance);
        labelStackY -= LabelTextHeight(distBuf, actor.Distance) + 4.f;
    }

    return labelStackY;
}

} // namespace

static void RenderPlayerEspFromFrame(
    const std::vector<Engine::EspFramePlayer>& players,
    const Engine::CameraCache& frameCam);

static void RenderWorldEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& world,
    const Engine::CameraCache& frameCam);

static void RenderRobotEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& robots,
    const Engine::CameraCache& frameCam);

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
    const float thickness = scale.lineThickness;

    // Re-project world-space bones with the live render camera — bonesDouble[]
    // were baked on the entity-list thread with a stale ambient g_Camera.
    // Only draw anatomically plausible segments (both ends + length/pelvis gate).
    for (const auto& [boneA, boneB] : eng.SkeletonLinksArcRaiders) {
        ImVec2 a{};
        ImVec2 b{};
        if (!EspDraw::TrySkeletonSegmentScreen(eng, frameCam, actor, boneA, boneB, a, b))
            continue;

        drawList->AddLine(a, b, color, thickness);
    }
}

bool Engine::ShouldDrawPlayerEsp(const PlayerCacheEntry& entry) const
{
    if (!entry.Drawing) {
        if (!var::show_radar)
            return false;
        const float radarM = EffectiveRadarRangeM();
        if (radarM <= 0.f || entry.Distance > radarM)
            return false;
    }
    if (entry.isAlly && var::hide_allies)
        return false;
    // HealthInfo@PS+0x530 is wrong on PioneerPS; health may read 0 — still draw.
    if (entry.Distance < 2.f)
        return false;
    const float maxM = PlayerCollectMaxM();
    if (maxM > 0.f && entry.Distance > maxM)
        return false;
    if (!IsPlausibleWorldPos(entry.WorldPos))
        return false;
    return true;
}

bool Engine::ShouldDrawRobotEsp(uintptr_t actorKey, const WorldCacheEntry& entry) const
{
    if (var::showRobots || var::robotAimEnabled) {
        if (!entry.Drawing)
            return false;
    } else if (!entry.Drawing) {
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
    const float maxM = BotCollectMaxM();
    if (maxM > 0.f && entry.Distance > maxM)
        return false;
    if (entry.IsBreaked && !var::show_dead_bots)
        return false;
    if (WorldScan::LooksLikeContainerActor(actorKey, entry.ActorName)
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

bool Engine::CollectEspRenderFrame(EspRenderFrame& out)
{
    out = {};
    out.frameSeq = m_espFrameSeq.fetch_add(1, std::memory_order_relaxed) + 1;

    if (!IsEspRaidActive())
        return false;

    if (!g_scatter.valid())
        g_scatter.init();
    if (!g_scatter.valid())
        return false;

    if (var::enableesp || var::show_radar || var::enable_aimbot) {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        out.players.reserve(playerCache.size());
        for (const auto& [key, entry] : playerCache) {
            bool include = false;
            if (var::enableesp || var::show_radar) {
                if (ShouldDrawPlayerEsp(entry))
                    include = true;
            }
            if (!include && var::enable_aimbot) {
                if (!entry.isAlly && !entry.bIsDead
                    && entry.Distance <= var::aimbot_distance
                    && IsPlausibleWorldPos(entry.WorldPos))
                    include = true;
            }
            if (!include)
                continue;

            EspFramePlayer framePlayer{};
            framePlayer.actorKey = key;
            framePlayer.entry = entry;
            out.players.push_back(std::move(framePlayer));
        }
    }

    if (AnyWorldEspEnabled() || var::show_radar) {
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
                if (!worldKeys.insert(key).second)
                    continue;

                EspFrameWorld frameWorld{};
                frameWorld.actorKey = key;
                frameWorld.entry = entry;
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
            if (!ShouldDrawRobotEsp(key, entry))
                continue;

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

    uintptr_t povBase = 0;
    if (pcmOk)
        povBase = pcm + Offsets::ViewTargetTarget + Offsets::ViewTargetPOV;

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
        g_scatter.prepare(povBase + Offsets::CameraLocation, povLoc);
        g_scatter.prepare(povBase + Offsets::CameraRotation, povRot);
        g_scatter.prepare(povBase + Offsets::CameraFOV, povFov);
        g_scatter.prepare(pcm + Offsets::DefaultFOV, defFov);
    }
    if (rootComp && IsValidPointer(rootComp)) {
        g_scatter.prepare(
            rootComp + Offsets::ComponentToWorld + 0x20,
            pawnWorld);
    }
    if (pc && IsValidPointer(pc))
        g_scatter.prepare(pc + Offsets::ControlRotation, ctrlRot);

    if (!g_scatter.execute())
        return false;

    // #region agent log
    {
        static auto s_lastFrameDmaLog = std::chrono::steady_clock::time_point{};
        const auto nowLog = std::chrono::steady_clock::now();
        if (s_lastFrameDmaLog.time_since_epoch().count() == 0
            || nowLog - s_lastFrameDmaLog >= std::chrono::seconds(2)) {
            s_lastFrameDmaLog = nowLog;
            std::ofstream f("F:/Test/ARCs/debug-5681af.log", std::ios::app);
            if (f) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"5681af\",\"runId\":\"post-fix\",\"hypothesisId\":\"H1\""
                  << ",\"location\":\"Esp.cpp:CollectEspRenderFrame\",\"message\":\"frame_cam_only\""
                  << ",\"data\":{\"players\":" << out.players.size()
                  << ",\"bots\":" << out.robots.size()
                  << ",\"world\":" << out.world.size()
                  << ",\"entityPosScatter\":0}"
                  << ",\"timestamp\":" << ms << "}\n";
            }
        }
    }
    // #endregion

    Vector3 pawnPos = Engine::ToVector3(pawnWorld);
    if (!IsPlausibleWorldPos(pawnPos) && rootComp && IsValidPointer(rootComp))
        pawnPos = Memory::read<Vector3>(rootComp + Offsets::RelativeLocation);

    const bool pawnOk = IsPlausibleWorldPos(pawnPos);
    if (!BuildCameraCacheFromPovReads(
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
            nullptr))
        return false;

    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    auto extrapolateEntry = [&](Vector3& pos, const Vector3& vel, float lastUpdateMs) {
        if (lastUpdateMs <= 0.f)
            return;
        const float dtSec =
            (nowMs - static_cast<uint64_t>(lastUpdateMs)) * 0.001f;
        if (dtSec <= 0.f || dtSec > 0.12f)
            return;
        pos.x += vel.x * dtSec;
        pos.y += vel.y * dtSec;
        pos.z += vel.z * dtSec;
    };

    for (EspFramePlayer& framePlayer : out.players) {
        Engine::PlayerCacheEntry& e = framePlayer.entry;
        if (!IsPlausibleWorldPos(e.WorldPos))
            continue;
        extrapolateEntry(e.WorldPos, e.cachedVelocity, e.lastVelocityUpdate);
    }

    for (EspFrameWorld& frameRobot : out.robots) {
        if (IsPlausibleWorldPos(frameRobot.entry.WorldPos))
            continue;
        if (IsPlausibleWorldPos(frameRobot.entry.CenterWorldPos))
            frameRobot.entry.WorldPos = frameRobot.entry.CenterWorldPos;
    }

    for (EspFrameWorld& frameRobot : out.robots) {
        Engine::WorldCacheEntry& e = frameRobot.entry;
        if (!IsPlausibleWorldPos(e.WorldPos))
            continue;
        extrapolateEntry(e.WorldPos, e.cachedVelocity, e.lastVelocityUpdate);
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

    out.valid = true;
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

    for (const Engine::PlayerCacheEntry* actor : actors) {
        if (!actor->APawn || !engine.IsValidPointer(actor->APawn))
            continue;
        if (!actor->Drawing)
            continue;
        if (actor->Distance < 2.f)
            continue;
        if (!IsPlausibleWorldPos(actor->WorldPos))
            continue;

        // Frame-synced WorldPos (scatter + velocity) — no per-paint live DMA.
        const Engine::PlayerCacheEntry& live = *actor;

        Vector3 headWorld{};
        Vector3 feetWorld{};
        if (!EspDraw::ResolvePlayerHeadFeetWorld(live, headWorld, feetWorld))
            continue;

        ImVec2 head{};
        ImVec2 feet{};
        if (!EspDraw::WorldToScreenBox(engine, frameCam, headWorld, feetWorld, head, feet))
            continue;
        if (!EspDraw::IsEspBoxOnScreen(head, feet))
            continue;

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

        if (var::names || var::show_weapon || var::show_distance)
            StackPlayerLabels(drawList, live, head.x, labelStackY, scale);

        if (var::snaplines && EspDraw::IsEspPointOnScreen(feet))
            EspDraw::DrawSnaplineEsp(drawList, feet, color, live.Distance);
    }
}

static void RenderPlayerEspFromFrame(
    const std::vector<Engine::EspFramePlayer>& players,
    const Engine::CameraCache& frameCam)
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

void Engine::RenderPlayerEspFromCache(const CameraCache& renderCam)
{
    if (!var::enableesp)
        return;

    static thread_local std::vector<PlayerCacheEntry> drawEntries;
    drawEntries.clear();

    {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        drawEntries.reserve(playerCache.size());
        for (const auto& [key, entry] : playerCache) {
            (void)key;
            if (!entry.Drawing)
                continue;
            if (!ShouldDrawPlayerEsp(entry))
                continue;
            drawEntries.push_back(entry);
        }
    }

    std::sort(
        drawEntries.begin(),
        drawEntries.end(),
        [](const PlayerCacheEntry& a, const PlayerCacheEntry& b) {
            return a.Distance > b.Distance;
        });

    std::vector<const PlayerCacheEntry*> actors;
    actors.reserve(drawEntries.size());
    for (const PlayerCacheEntry& entry : drawEntries)
        actors.push_back(&entry);

    DrawPlayerEspList(actors, renderCam);
}

static void RenderRobotEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& robots,
    const Engine::CameraCache& frameCam)
{
    if (!var::showRobots)
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    const Engine::EngineStateSnapshot stateSnap = engine.GetStateSnapshot();
    const Vector3 distRef = engine.ResolveDistanceReference(
        frameCam, stateSnap.acknowledgedPawn);

    for (const Engine::EspFrameWorld& item : robots) {
        const uintptr_t key = item.actorKey;
        const Engine::WorldCacheEntry& robot = item.entry;
        if (!key || !engine.IsValidPointer(key))
            continue;
        if (engine.IsCachedPlayer(key))
            continue;
        if (robot.IsBreaked && !var::show_dead_bots)
            continue;
        if (!IsPlausibleWorldPos(robot.WorldPos))
            continue;
        // Container veto already applied in ShouldDrawRobotEsp / collect.

        const Vector3& wp = robot.WorldPos;
        const double bdx = static_cast<double>(wp.x) - distRef.x;
        const double bdy = static_cast<double>(wp.y) - distRef.y;
        const double bdz = static_cast<double>(wp.z) - distRef.z;
        const float distM = static_cast<float>(
            std::sqrt(bdx * bdx + bdy * bdy + bdz * bdz) / 100.0);

        const float botMaxM =
            var::bot_esp_distance > 0.f ? var::bot_esp_distance : var::kMaxDistanceSliderM;
        if (distM > botMaxM)
            continue;

        Engine::WorldCacheEntry drawEntry = robot;

        Vector3 headWorld{};
        Vector3 feetWorld{};
        if (!EspDraw::ResolveBotHeadFeetWorld(drawEntry, headWorld, feetWorld)) {
            // #region agent log
            if (distM <= 12.f) {
                static auto s_lastHf = std::chrono::steady_clock::time_point{};
                const auto nowHf = std::chrono::steady_clock::now();
                if (s_lastHf.time_since_epoch().count() == 0
                    || nowHf - s_lastHf >= std::chrono::milliseconds(250)) {
                    s_lastHf = nowHf;
                    std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                    if (f) {
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        f << "{\"sessionId\":\"c190fb\",\"runId\":\"near-bot\",\"hypothesisId\":\"H4\""
                          << ",\"location\":\"Esp.cpp:RenderRobot\",\"message\":\"near_bot_headfeet_fail\""
                          << ",\"data\":{\"key\":" << key
                          << ",\"distM\":" << distM
                          << ",\"drawing\":" << (robot.Drawing ? 1 : 0)
                          << ",\"name\":\"" << robot.ActorName << "\""
                          << "},\"timestamp\":" << ms << "}\n";
                    }
                }
            }
            // #endregion
            continue;
        }

        ImVec2 head{};
        ImVec2 feet{};
        if (!EspDraw::WorldToScreenBox(engine, frameCam, headWorld, feetWorld, head, feet)) {
            // #region agent log
            if (distM <= 12.f) {
                static auto s_lastW2s = std::chrono::steady_clock::time_point{};
                const auto nowW2s = std::chrono::steady_clock::now();
                if (s_lastW2s.time_since_epoch().count() == 0
                    || nowW2s - s_lastW2s >= std::chrono::milliseconds(250)) {
                    s_lastW2s = nowW2s;
                    std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                    if (f) {
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        f << "{\"sessionId\":\"c190fb\",\"runId\":\"near-bot\",\"hypothesisId\":\"H4\""
                          << ",\"location\":\"Esp.cpp:RenderRobot\",\"message\":\"near_bot_w2s_fail\""
                          << ",\"data\":{\"key\":" << key
                          << ",\"distM\":" << distM
                          << ",\"camFov\":" << frameCam.FOV
                          << ",\"name\":\"" << robot.ActorName << "\""
                          << "},\"timestamp\":" << ms << "}\n";
                    }
                }
            }
            // #endregion
            continue;
        }

        const ImU32 color = BotEspColor(robot.isVisible, robot.IsBreaked);
        const float boxH = feet.y - head.y;
        const Visuals::EspDrawScale scale =
            Visuals::ComputeEspScaleFromBox(boxH > 1.f ? boxH : 24.f, distM);

        // Paint path must stay DMA-free. Live GetActorFNameString / GetActorClassFName
        // / ResolveEnemyAssetBotLabel here stalled Present (espMs 500-700). Names are
        // resolved on the robot worker into ActorName / ItemDisplayName.
        const std::string& fname = robot.ActorName;
        std::string botLabel = ResolveBotDrawLabel(key, robot.ActorName, fname);
        if (botLabel.empty() && !robot.ItemDisplayName.empty())
            botLabel = ResolveBotDrawLabel(key, robot.ItemDisplayName, fname);
        // No real name → no ESP (heart/dist alone = ghost bots; log GC Electrified).
        // Never use ARC/Bot/Oil placeholders. IsAnyBotActor alone is not enough.
        if (botLabel.empty() || !IsAcceptedBotEspLabel(engine, botLabel, fname)) {
            RecordBotDrawLabelMiss();
            // #region agent log
            if (distM <= 12.f) {
                static auto s_lastLbl = std::chrono::steady_clock::time_point{};
                const auto nowLbl = std::chrono::steady_clock::now();
                if (s_lastLbl.time_since_epoch().count() == 0
                    || nowLbl - s_lastLbl >= std::chrono::milliseconds(250)) {
                    s_lastLbl = nowLbl;
                    std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                    if (f) {
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        f << "{\"sessionId\":\"c190fb\",\"runId\":\"bot-gate\",\"hypothesisId\":\"GHOST\""
                          << ",\"location\":\"Esp.cpp:RenderRobot\",\"message\":\"near_bot_label_skip\""
                          << ",\"data\":{\"key\":" << key
                          << ",\"distM\":" << distM
                          << ",\"actorName\":\"" << robot.ActorName << "\""
                          << ",\"fname\":\"" << fname << "\""
                          << ",\"anyBot\":" << (ArcActorType::IsAnyBotActor(key) ? 1 : 0)
                          << "},\"timestamp\":" << ms << "}\n";
                    }
                }
            }
            // #endregion
            continue;
        }
        if (!EspDraw::IsEspBoxOnScreen(head, feet))
            continue;

        // Bot health bar + heart
        float botLabelY = head.y;
        if (robot.maxhealth > 0.f && robot.health > 0.f) {
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

        if (var::bot_snaplines && EspDraw::IsEspPointOnScreen(feet))
            EspDraw::DrawSnaplineEsp(drawList, feet, color, distM);
    }
}

// Flicker diagnosis: every whole-screen ESP blink is a paint that drew nothing.
// Classify WHY (frame not valid vs camera resolve fail vs frame builder stalled)
// so we fix the real gate instead of guessing. Enabled by show_debug_overlay.
struct FlickerDbg {
    uint64_t paints = 0;        // RenderEsp calls past the enable check
    uint64_t drew = 0;          // paints that reached the draw path
    uint64_t blankFrame = 0;    // bailed: frame.valid == false
    uint64_t blankCam = 0;      // bailed: ResolveLiveRenderCamera failed
    uint64_t staleFrame = 0;    // drew, but frame builder republished no new seq
    uint64_t blankStreak = 0;   // consecutive blank paints right now
    uint64_t maxBlankStreak = 0;// worst streak this window
    uint64_t lastFrameSeq = 0;
    // H3/H4/H5/H6/H7 flicker probes
    int prevTotalEntities = -1;
    uint64_t contentDrops = 0;  // new frame arrived >50% smaller than previous
    uint64_t paintGaps = 0;     // >150ms between paints while menu was closed
    uint64_t menuGaps = 0;      // >150ms gaps caused by INSERT menu (false positive)
    uint64_t loopGaps = 0;      // >150ms between Render() calls (true overlay hitch)
    int64_t maxPaintGapMs = 0;
    int64_t maxLoopGapMs = 0;
    int64_t maxEspMs = 0;       // slowest RenderEsp wall time (DMA/cam)
    int64_t maxCamMs = 0;       // slowest ResolveLiveRenderCamera
    std::chrono::steady_clock::time_point lastPaintTp{};
    std::chrono::steady_clock::time_point lastLoopTp{};
    bool menuInterrupted = false; // RenderEsp skipped because menu was open
};
static FlickerDbg g_flickerDbg;

static void FlickerNoteBlank(const char* reason)
{
    FlickerDbg& d = g_flickerDbg;
    ++d.blankStreak;
    if (d.blankStreak > d.maxBlankStreak)
        d.maxBlankStreak = d.blankStreak;
    static auto s_last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (s_last.time_since_epoch().count() == 0
        || now - s_last >= std::chrono::milliseconds(200)) {
        s_last = now;
        std::ofstream f("F:/Test/ARCs/debug-flicker.log", std::ios::app);
        if (f) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            f << "{\"sessionId\":\"flicker\",\"location\":\"Esp.cpp:RenderEsp\""
              << ",\"message\":\"blank_paint\",\"data\":{\"reason\":\"" << reason << "\""
              << ",\"streak\":" << d.blankStreak
              << "},\"timestamp\":" << ms << "}\n";
        }
    }
}

// Visible to Render.cpp — menu / loop hitch probes (H5 / H7).
void FlickerNoteMenuOpen()
{
    g_flickerDbg.menuInterrupted = true;
}

void FlickerNoteRenderLoop()
{
    // #region agent log
    if (!var::show_debug_overlay)
        return;
    const auto nowTp = std::chrono::steady_clock::now();
    if (g_flickerDbg.lastLoopTp.time_since_epoch().count() != 0) {
        const int64_t gapMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                nowTp - g_flickerDbg.lastLoopTp).count();
        if (gapMs > 150) {
            ++g_flickerDbg.loopGaps;
            if (gapMs > g_flickerDbg.maxLoopGapMs)
                g_flickerDbg.maxLoopGapMs = gapMs;
            std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
            if (f) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"c190fb\",\"runId\":\"flicker\",\"hypothesisId\":\"H7\""
                  << ",\"location\":\"Esp.cpp:FlickerNoteRenderLoop\",\"message\":\"loop_gap\""
                  << ",\"data\":{\"gapMs\":" << gapMs
                  << "},\"timestamp\":" << ms << "}\n";
            }
        }
    }
    g_flickerDbg.lastLoopTp = nowTp;
    // #endregion
}

void Engine::RenderEsp()
{
    const bool drawPlayers = var::enableesp;
    const bool drawBots = var::showRobots || var::robotAimEnabled;
    const bool drawWorld = AnyWorldEspEnabled();
    if (!drawPlayers && !drawBots && !drawWorld && !var::show_radar)
        return;

    const bool flickerDbgOn = var::show_debug_overlay;
    const auto espStartTp = std::chrono::steady_clock::now();
    if (flickerDbgOn)
        ++g_flickerDbg.paints;

    SetProjectionViewport(
        ImGui::GetIO().DisplaySize.x,
        ImGui::GetIO().DisplaySize.y);

    // G2: never block paint on m_espFrameMutex. Frame builder unique_lock +
    // aimbot shared copies caused writer-preference waits → espMs=500-717
    // (debug-c190fb slow_esp == paint_gap). Keep last good frame if busy.
    static EspRenderFrame s_paintFrame{};
    int frameLockGot = 0;
    int64_t frameCopyMs = 0;
    {
        const auto tCopy0 = std::chrono::steady_clock::now();
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            frameLockGot = 1;
            if (m_lastEspFrame.valid)
                s_paintFrame = m_lastEspFrame;
        }
        frameCopyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tCopy0).count();
    }
    const EspRenderFrame& frame = s_paintFrame;
    if (!frame.valid) {
        if (flickerDbgOn) {
            ++g_flickerDbg.blankFrame;
            FlickerNoteBlank("frame_invalid");
        }
        return;
    }

    Engine::CameraCache renderCam{};
    const auto camStartTp = std::chrono::steady_clock::now();
    const bool camOk = ResolveLiveRenderCamera(frame, renderCam);
    if (flickerDbgOn) {
        const int64_t camMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - camStartTp).count();
        if (camMs > g_flickerDbg.maxCamMs)
            g_flickerDbg.maxCamMs = camMs;
        // #region agent log
        // H6: DMA camera resolve on the paint thread blocking Present.
        if (camMs > 50) {
            std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
            if (f) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"c190fb\",\"runId\":\"flicker\",\"hypothesisId\":\"H6\""
                  << ",\"location\":\"Esp.cpp:RenderEsp\",\"message\":\"slow_cam\""
                  << ",\"data\":{\"camMs\":" << camMs
                  << ",\"ok\":" << (camOk ? 1 : 0)
                  << "},\"timestamp\":" << ms << "}\n";
            }
        }
        // #endregion
    }
    if (!camOk) {
        if (flickerDbgOn) {
            ++g_flickerDbg.blankCam;
            FlickerNoteBlank("cam_fail");
        }
        return;
    }

    if (flickerDbgOn) {
        ++g_flickerDbg.drew;

        // #region agent log
        // H4/H5: gap between paints. Menu-open skips RenderEsp and must not
        // count as a real hitch (H5). Only count when menu stayed closed.
        {
            const auto nowTp = std::chrono::steady_clock::now();
            if (g_flickerDbg.lastPaintTp.time_since_epoch().count() != 0) {
                const int64_t gapMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        nowTp - g_flickerDbg.lastPaintTp).count();
                if (gapMs > 150) {
                    if (g_flickerDbg.menuInterrupted) {
                        ++g_flickerDbg.menuGaps;
                        std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                        if (f) {
                            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            f << "{\"sessionId\":\"c190fb\",\"runId\":\"flicker\",\"hypothesisId\":\"H5\""
                              << ",\"location\":\"Esp.cpp:RenderEsp\",\"message\":\"menu_gap\""
                              << ",\"data\":{\"gapMs\":" << gapMs
                              << "},\"timestamp\":" << ms << "}\n";
                        }
                    } else {
                        ++g_flickerDbg.paintGaps;
                        if (gapMs > g_flickerDbg.maxPaintGapMs)
                            g_flickerDbg.maxPaintGapMs = gapMs;
                        std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                        if (f) {
                            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            f << "{\"sessionId\":\"c190fb\",\"runId\":\"flicker\",\"hypothesisId\":\"H4\""
                              << ",\"location\":\"Esp.cpp:RenderEsp\",\"message\":\"paint_gap\""
                              << ",\"data\":{\"gapMs\":" << gapMs
                              << "},\"timestamp\":" << ms << "}\n";
                        }
                    }
                }
            }
            g_flickerDbg.menuInterrupted = false;
            g_flickerDbg.lastPaintTp = nowTp;
        }
        // #endregion

        // Same frameSeq as last paint = frame builder published nothing new
        // this interval (DMA/hitch stall) even though the old frame is valid.
        if (frame.frameSeq == g_flickerDbg.lastFrameSeq) {
            ++g_flickerDbg.staleFrame;
        } else {
            // #region agent log
            // H3: fresh frame arrived with far fewer entities than the previous
            // one — mass blink while paint keeps running.
            const int total = static_cast<int>(
                frame.players.size() + frame.robots.size() + frame.world.size());
            if (g_flickerDbg.prevTotalEntities >= 4
                && total * 2 < g_flickerDbg.prevTotalEntities) {
                ++g_flickerDbg.contentDrops;
                std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                if (f) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    f << "{\"sessionId\":\"c190fb\",\"runId\":\"flicker\",\"hypothesisId\":\"H3\""
                      << ",\"location\":\"Esp.cpp:RenderEsp\",\"message\":\"frame_content_drop\""
                      << ",\"data\":{\"before\":" << g_flickerDbg.prevTotalEntities
                      << ",\"after\":" << total
                      << ",\"players\":" << frame.players.size()
                      << ",\"bots\":" << frame.robots.size()
                      << ",\"world\":" << frame.world.size()
                      << "},\"timestamp\":" << ms << "}\n";
                }
            }
            g_flickerDbg.prevTotalEntities = total;
            // #endregion
        }
        g_flickerDbg.lastFrameSeq = frame.frameSeq;
        g_flickerDbg.blankStreak = 0;

        static auto s_lastRep = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_lastRep >= std::chrono::seconds(1)) {
            s_lastRep = now;
            const FlickerDbg d = g_flickerDbg;
            std::cout << "[debugFlicker] paints=" << d.paints
                << " drew=" << d.drew
                << " blankFrame=" << d.blankFrame
                << " blankCam=" << d.blankCam
                << " staleFrame=" << d.staleFrame
                << " maxBlankStreak=" << d.maxBlankStreak
                << " contentDrops=" << d.contentDrops
                << " paintGaps=" << d.paintGaps
                << " menuGaps=" << d.menuGaps
                << " loopGaps=" << d.loopGaps
                << " maxGapMs=" << d.maxPaintGapMs
                << " maxLoopMs=" << d.maxLoopGapMs
                << " maxCamMs=" << d.maxCamMs
                << " maxEspMs=" << d.maxEspMs
                << std::endl;
            std::ofstream lf("F:/Test/ARCs/debug-flicker.log", std::ios::app);
            if (lf) {
                const auto msn = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                lf << "{\"sessionId\":\"flicker\",\"location\":\"Esp.cpp:RenderEsp\""
                   << ",\"message\":\"flicker_summary\",\"data\":{"
                   << "\"paints\":" << d.paints
                   << ",\"drew\":" << d.drew
                   << ",\"blankFrame\":" << d.blankFrame
                   << ",\"blankCam\":" << d.blankCam
                   << ",\"staleFrame\":" << d.staleFrame
                   << ",\"maxBlankStreak\":" << d.maxBlankStreak
                   << "},\"timestamp\":" << msn << "}\n";
            }
            g_flickerDbg.maxBlankStreak = 0;
        }
    }

    g_renderQueue.newFrame();

    if (drawPlayers)
        RenderPlayerEspFromFrame(frame.players, renderCam);
    if (var::showRobots)
        RenderRobotEspFromFrame(frame.robots, renderCam);
    if (drawWorld) {
        // World loot positions were sampled with frame.camera. Projecting them with
        // live renderCam (rot-extrapolated) desyncs W2S → skipProj spikes and
        // labels blink while the fixed debug overlay stays solid (no projection).
        // Evidence: paintGaps=0 but skipProj=4-7 of frame=9, rendered oscillating 1-4.
        const Engine::CameraCache& worldCam =
            CameraOkForEsp(frame.camera) ? frame.camera : renderCam;
        RenderWorldEspFromFrame(frame.world, worldCam);
    }

    g_renderQueue.endFrame();

    if (flickerDbgOn) {
        const int64_t espMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - espStartTp).count();
        if (espMs > g_flickerDbg.maxEspMs)
            g_flickerDbg.maxEspMs = espMs;
        // #region agent log
        if (espMs > 50 || frameCopyMs > 20) {
            std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
            if (f) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"c190fb\",\"runId\":\"ghost-flicker\",\"hypothesisId\":\"G2\""
                  << ",\"location\":\"Esp.cpp:RenderEsp\",\"message\":\"slow_esp\""
                  << ",\"data\":{\"espMs\":" << espMs
                  << ",\"frameCopyMs\":" << frameCopyMs
                  << ",\"frameLockGot\":" << frameLockGot
                  << ",\"players\":" << frame.players.size()
                  << ",\"robots\":" << frame.robots.size()
                  << ",\"world\":" << frame.world.size()
                  << "},\"timestamp\":" << ms << "}\n";
            }
        }
        // #endregion
    }

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
            char dbgTxt[512];
            const int n = std::snprintf(dbgTxt, sizeof(dbgTxt),
                "Cam: %s | Loc: %.0f,%.0f,%.0f | Rot: %.1f,%.1f,%.1f | FOV: %.1f\n"
                "g_Cam: %s | Loc: %.0f,%.0f,%.0f | Rot: %.1f,%.1f,%.1f | FOV: %.1f",
                camSrc,
                renderCam.Location.x, renderCam.Location.y, renderCam.Location.z,
                renderCam.Rotation.x, renderCam.Rotation.y, renderCam.Rotation.z,
                renderCam.FOV,
                CameraOkForEsp(gCam) ? "ok" : "BAD",
                gCam.Location.x, gCam.Location.y, gCam.Location.z,
                gCam.Rotation.x, gCam.Rotation.y, gCam.Rotation.z,
                gCam.FOV);
            if (n > 0 && n < (int)sizeof(dbgTxt))
                drawList->AddText(ImVec2(10, 10), 0xFF00FF00, dbgTxt);
        }
    }

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
                << std::endl;
            std::cout << "[debugWorldEsp] frame=" << g_worldEspDbg.frameEntries
                << " rendered=" << g_worldEspDbg.rendered
                << " skipAllow=" << g_worldEspDbg.skipAllow
                << " skipPos=" << g_worldEspDbg.skipPos
                << " skipDist=" << g_worldEspDbg.skipDist
                << " skipProj=" << g_worldEspDbg.skipProj
                << " worldCam=" << (CameraOkForEsp(frame.camera) ? "frame" : "live")
                << std::endl;
        }
    }
}

static void RenderWorldEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& world,
    const Engine::CameraCache& frameCam)
{
    WorldEspDebugStats dbg{};
    dbg.frameEntries = static_cast<int>(world.size());

    if (!AnyWorldEspEnabled())
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    // Paint path must stay DMA-free. Frame builder already resolved WorldPos +
    // labels. Live GetActorFNameString / GetEnglishItemName / ContainerLootLooksOpenedAny
    // / ResolveWorldEspDrawPos here stalled Present 200-700ms (debugFlicker:
    // maxEspMs=713 with menuGaps=0; H6 slow_esp without matching slow_cam).
    const Vector3& distRef = frameCam.Location;

    for (const Engine::EspFrameWorld& item : world) {
        const Engine::WorldCacheEntry& entry = item.entry;
        if (!engine.getAllowWorldEntry(entry)) {
            ++dbg.skipAllow;
            continue;
        }
        if (!IsPlausibleWorldPos(entry.WorldPos)) {
            ++dbg.skipPos;
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
        if (label.empty())
            continue;

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
        if (distM > WorldLootPickupMaxDrawMeters(cat, &filterView)) {
            ++dbg.skipDist;
            continue;
        }

        const bool isPickup = LootItemLooksLikePickup(filterView);

        Vector3 screen{};
        if (!ProjectWorldEspPoint(worldPos, frameCam, screen)) {
            ++dbg.skipProj;
            continue;
        }

        const float screenW = ImGui::GetIO().DisplaySize.x;
        const float screenH = ImGui::GetIO().DisplaySize.y;
        if (screen.x < -8.f || screen.x > screenW + 8.f
            || screen.y < -8.f || screen.y > screenH + 8.f)
            continue;

        const ImU32 color = WorldLootLabelColor(
            cat,
            lootTier,
            isPickup && !looksLikeContainer);

        char buf[160]{};
        if (var::show_loot_value && lootValue > 0 && isPickup && !isContainerEsp
            && !looksLikeContainer)
            snprintf(buf, sizeof(buf), "%s [%d] [%.0fm]", label.c_str(), lootValue, distM);
        else
            snprintf(buf, sizeof(buf), "%s [%.0fm]", label.c_str(), distM);

        EspDraw::DrawLabelEsp(
            drawList,
            ImVec2(static_cast<float>(screen.x), static_cast<float>(screen.y)),
            buf,
            color,
            distM);
        ++dbg.rendered;
    }

    g_worldEspDbg = dbg;
}

void Engine::RenderFovCircle()
{
    if (!var::show_fov || var::aimbot_fov <= 0.f)
        return;

    float gameFov = 90.f;
    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        gameFov = g_Camera.FOV;
    }
    Visuals::FovCircle(var::aimbot_fov, gameFov);
}

void Engine::RenderRadar(bool interactive)
{
    if (!var::show_radar)
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    // Never CollectEspRenderFrame on the paint thread (DMA). try_lock only.
    EspRenderFrame frame{};
    {
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex, std::try_to_lock);
        if (lock.owns_lock())
            frame = m_lastEspFrame;
    }
    if (!frame.valid)
        return;

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

    auto projectBlip = [&](const Vector3& worldPos, float& outX, float& outY) -> bool {
        if (!IsPlausibleWorldPos(worldPos))
            return false;

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

    auto drawBlip = [&](float x, float y, ImU32 color, float distanceM, float baseRadius = 3.5f) {
        const float dx = x - cx;
        const float dy = y - cy;
        const float clipR = (std::max)(radarPx - 1.f, 1.f);
        if (dx * dx + dy * dy > clipR * clipR)
            return;

        const float radius = std::clamp(
            baseRadius * Visuals::EspDistanceScale(distanceM),
            1.8f,
            baseRadius * 1.12f);
        drawList->AddCircleFilled(ImVec2(x, y), radius, color, 12);
    };

    auto pickerColor = [](const float rgba[4]) -> ImU32 {
        return IM_COL32(
            static_cast<int>(rgba[0] * 255.f),
            static_cast<int>(rgba[1] * 255.f),
            static_cast<int>(rgba[2] * 255.f),
            static_cast<int>(rgba[3] * 255.f));
    };

    {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
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
            drawBlip(rx, ry, color, actor.Distance);
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
        drawBlip(rx, ry, color, robot.Distance);
    }

    for (const EspFrameWorld& item : frame.world) {
        const WorldCacheEntry& entry = item.entry;
        if (!IsPlausibleWorldPos(entry.WorldPos))
            continue;

        const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
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
        drawBlip(rx, ry, color, entry.Distance, isPickup ? 3.f : 2.5f);
    }
}
