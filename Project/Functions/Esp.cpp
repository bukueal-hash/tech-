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
#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <vector>

extern Engine engine;

namespace {

constexpr size_t kMaxEspFrameBoneReads = 16;

struct WorldEspDebugStats {
    int frameEntries = 0;
    int rendered = 0;
    int skipAllow = 0;
    int skipPos = 0;
    int skipDist = 0;
    int skipProj = 0;
    int skipPickedUp = 0;
    int drawingFalse = 0;
    int drawingTrue = 0;
    int pickedUp = 0;
    int dupKey = 0;
};

WorldEspDebugStats g_worldEspDbg{};

// Paint-thread watchdog: records timing but does NOT write to the 280+MB
// debug log on every frame (that I/O IS the freeze). Timestamps go to a
// separate tiny ring-buffer file so we can diagnose without the disk stall.
struct EspPaintWatchScope {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool frameLockBusy = false;
    uint64_t frameSeq = 0;

    ~EspPaintWatchScope()
    {
        static auto lastEnd = std::chrono::steady_clock::time_point{};
        static int ringIdx = 0;
        const auto now = std::chrono::steady_clock::now();
        const int gapMs = lastEnd.time_since_epoch().count() == 0
            ? 0
            : static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                started - lastEnd).count());
        const int renderMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count());
        lastEnd = now;

        if (gapMs < 100 && renderMs < 50 && !frameLockBusy)
            return;

        // Write to a SEPARATE ring-buffer file so we never open
        // the 280MB debug-c190fb.log from the paint thread.
        static constexpr int kRingSize = 64;
        static char ring[kRingSize][256]{};
        snprintf(ring[ringIdx], sizeof(ring[0]),
            "esp_paint gapMs=%d renderMs=%d lockBusy=%d seq=%llu\n",
            gapMs, renderMs, frameLockBusy ? 1 : 0,
            static_cast<unsigned long long>(frameSeq));
        ringIdx = (ringIdx + 1) % kRingSize;

        // Flush ring to disk only every 32 writes or on stall
        static int flushCount = 0;
        if (++flushCount >= 32 || gapMs > 200 || renderMs > 100) {
            flushCount = 0;
            std::ofstream f("F:/Test/ARCs/paint_watch.log", std::ios::trunc);
            if (f) {
                for (int i = 0; i < kRingSize; ++i) {
                    int idx = (ringIdx - kRingSize + i) % kRingSize;
                    if (idx < 0) idx += kRingSize;
                    if (ring[idx][0])
                        f << ring[idx];
                }
            }
        }
    }
};

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

    if (isOpened) {
        if (IsJunkWorldEspLabel(baseLabel) || IsGarbledEspLabel(baseLabel)
            || !IsPlausibleEspLabel(baseLabel))
            baseLabel = ContainerCategoryFallbackEspLabel(WorldItemCategory::OpenedContainer);
        if (!IsJunkWorldEspLabel(baseLabel) && !IsGarbledEspLabel(baseLabel))
            return AppendContainerOpenSuffix(std::move(baseLabel));
        return FormatEspDisplayLabel(baseLabel);
    }
    return FormatEspDisplayLabel(baseLabel);
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

static bool ResolveLiveRenderCamera(
    const Engine::EspRenderFrame& frame,
    Engine::CameraCache& outCam)
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


    // Active weapon — centered on head X (same as name/distance).
    if (!active.empty()) {
        // Append ammo clip count when available (e.g. "Tempest III [30]")
        std::string weaponLabel = active;
        if (actor.weaponClip > 0 && active != "Unarmed") {
            weaponLabel += " [" + std::to_string(actor.weaponClip) + "]";
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
        labelStackY -= LabelTextHeight(weaponLabel.c_str(), actor.Distance) + 4.f;
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
    const Visuals::EspDrawScale& scale)
{
    if (var::show_squad_idx && !actor.isAlly && actor.squadIdx > 0) {
        char tagBuf[16]{};
        snprintf(tagBuf, sizeof(tagBuf), "@%u", (unsigned)actor.squadIdx);
        EspDraw::DrawLabelEsp(
            drawList,
            ImVec2(headX, labelStackY),
            tagBuf,
            SquadColor(actor.squadIdx),
            actor.Distance);
        labelStackY -= LabelTextHeight(tagBuf, actor.Distance) + 2.f;
    }
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
    const Engine::CameraCache& frameCam,
    int& outDrawn);

static void RenderWorldEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& world,
    const Engine::CameraCache& frameCam,
    int& outDrawn, int& outLabelUs, int& outProjUs);

static void RenderRobotEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& robots,
    const Engine::CameraCache& frameCam,
    int& outDrawn);

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
    // Distance hysteresis: use 105% of max distance for the "keep drawing"
    // check. An entity must move 5% beyond the threshold before it disappears,
    // preventing the distance-edge flicker that caused 41% of all logged
    // flickers (315/408). The scanner's Drawing flag uses the strict distance,
    // so new entities still appear at the correct range.
    const float maxM = PlayerCollectMaxM();
    if (maxM > 0.f && entry.Distance > maxM * 1.05f)
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
    // Same 5% distance hysteresis as player ESP.
    const float maxM = BotCollectMaxM();
    if (maxM > 0.f && entry.Distance > maxM * 1.05f)
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
        int dbgDrawingFalse = 0, dbgDrawingTrue = 0, dbgPickedUp = 0, dbgDupKey = 0;
        auto appendWorld = [&](const std::unordered_map<uintptr_t, WorldCacheEntry>& cache) {
            for (const auto& [key, entry] : cache) {
                static_assert(sizeof(entry.Drawing) == 1, "Drawing must be bool");
                if (!entry.Drawing) {
                    ++dbgDrawingFalse;
                    continue;
                }
                ++dbgDrawingTrue;
                if (!ShouldDrawWorldEsp(entry)) {
                    // Drawing was true but ShouldDrawWorldEsp still said no
                    // (category disabled, getAllowWorldEntry rejected, etc.)
                    continue;
                }
                if (IsGroundPickupGoneSticky(key)) {
                    ++dbgPickedUp;
                    continue;
                }
                if (!worldKeys.insert(key).second) {
                    ++dbgDupKey;
                    continue;
                }

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
        // Debug: log why WorldDraw=0 with 1384 cached
        g_worldEspDbg.drawingFalse = dbgDrawingFalse;
        g_worldEspDbg.drawingTrue = dbgDrawingTrue;
        g_worldEspDbg.pickedUp = dbgPickedUp;
        g_worldEspDbg.dupKey = dbgDupKey;
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
            // Resolve bot label on the worker thread (DMA allowed) so the
            // paint thread never calls ResolveBotDrawLabel (blocking DMA).
            frameRobot.robotLabel = ResolveBotDrawLabel(key, entry.ActorName, entry.ActorName);
            if (frameRobot.robotLabel.empty() && !entry.ItemDisplayName.empty())
                frameRobot.robotLabel = ResolveBotDrawLabel(key, entry.ItemDisplayName, entry.ActorName);
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
        g_scatter.prepare(
            rootComp + Offsets::ComponentToWorld + 0x20,
            pawnWorld);
    }
    if (pc && IsValidPointer(pc))
        g_scatter.prepare(pc + Offsets::ControlRotation, ctrlRot);

    if (!g_scatter.execute())
        return false;


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

    // Intentional ESP-only lead: tighter 0.12s clamp and no velMag boost
    // (Aimbot uses ComputeVelocityLeadDelta with 0.20/0.25 + velMag). Keep
    // separate so paint frames stay conservative vs aim prediction.
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

    // #region agent log
    // Flush only — world Drawing transitions are owned by FinalizeWorldCacheMap
    // (World.cpp). CollectEspRenderFrame is the render collect path checkpoint.
    WorldScan::MaybeFlushFlickerScore();
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
    struct SmoothedPos { ImVec2 head{}; ImVec2 feet{}; bool valid = false; };
    static std::unordered_map<uintptr_t, SmoothedPos> s_smooth;
    static constexpr float kSmoothAlpha = 0.35f;  // 0=no smooth, 1=instant. 0.35 ~ 4-frame settle
    std::unordered_set<uintptr_t> seenThisFrame;

    for (const Engine::PlayerCacheEntry* actor : actors) {
        if (!actor->APawn || !engine.IsValidPointer(actor->APawn))
            continue;
        if (!actor->Drawing) {
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

    // Prune actors that disappeared this frame so stale smoothed positions
    // don't carry over and cause a ghost slide on re-entry.
    for (auto it = s_smooth.begin(); it != s_smooth.end(); ) {
        if (!seenThisFrame.contains(it->first))
            it = s_smooth.erase(it);
        else
            ++it;
    }
}

static void RenderPlayerEspFromFrame(
    const std::vector<Engine::EspFramePlayer>& players,
    const Engine::CameraCache& frameCam,
    int& outDrawn)
{
    outDrawn = 0;
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
    outDrawn = static_cast<int>(actors.size());
}

static void RenderRobotEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& robots,
    const Engine::CameraCache& frameCam,
    int& outDrawn)
{
    outDrawn = 0;
    if (!var::showRobots)
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    // Paint-thread-safe: GetStateSnapshot takes shared_lock(m_stateMutex)
    // which blocks when Update/EntityList hold the unique lock during DMA.
    // frameCam.Location is already valid (resolved by frame builder).
    const Vector3 distRef = IsPlausibleWorldPos(frameCam.Location)
        ? frameCam.Location
        : engine.ResolveDistanceReference(frameCam, 0);

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
        const uintptr_t key = item.actorKey;
        const Engine::WorldCacheEntry& robot = item.entry;
        if (!key || !engine.IsValidPointer(key))
            continue;
        // FREEZE FIX: IsCachedPlayer takes a BLOCKING shared_lock on
        // m_playerCacheMutex. Called per-bot per-frame on the paint thread it
        // stalled Present ~200-530ms whenever EntityList held the unique_lock
        // for its cache swap. ShouldDrawRobotEsp already filters players out
        // in CollectEspRenderFrame (worker thread) — this check is redundant.
        if (robot.IsBreaked && !var::show_dead_bots)
            continue;
        if (!IsPlausibleWorldPos(robot.WorldPos)) {
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
        if (distM > botMaxM) {
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                key, false, WorldScan::FlickerCause::DistEdge);
            // #endregion
            continue;
        }

        Engine::WorldCacheEntry drawEntry = robot;

        Vector3 headWorld{};
        Vector3 feetWorld{};
        if (!EspDraw::ResolveBotHeadFeetWorld(drawEntry, headWorld, feetWorld)) {
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                key, false, WorldScan::FlickerCause::PosFail);
            // #endregion
            continue;
        }

        ImVec2 head{};
        ImVec2 feet{};
        // PROJ1 (Fix #7): same sticky-screen hold as world labels — a brief
        // WorldToScreenBox miss must not blank the bot box.
        static std::unordered_map<uintptr_t, std::pair<ImVec2, ImVec2>> s_botLastScreen;
        static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> s_botLastScreenWhen;
        // PROJ2 (Fix #13): 400ms was short during hard turns — bots still in
        // view briefly flake W2S and blink. Hold a bit longer while the
        // in-view gate still blocks ghost mid-screen flashes.
        static constexpr auto kBotScreenFreezeTtl = std::chrono::milliseconds(700);
        const auto botPaintNow = std::chrono::steady_clock::now();
        if (!EspDraw::WorldToScreenBox(engine, frameCam, headWorld, feetWorld, head, feet)) {
            // GHOST1: same in-view gate as world labels — no stale-coordinate
            // phantom boxes after camera rotation.
            const bool stillInView = WorldPointRoughlyInView(frameCam, headWorld);
            if (stillInView) {
                if (const auto fit = s_botLastScreenWhen.find(key);
                    fit != s_botLastScreenWhen.end()
                    && botPaintNow - fit->second <= kBotScreenFreezeTtl) {
                    const auto& last = s_botLastScreen[key];
                    head = last.first;
                    feet = last.second;
                } else {
                    // #region agent log
                    // PROJ2 (Fix #13): only count projFail while the bot is
                    // still in the view cone. Looking away used to inflate
                    // paintBots 8-12 every turn.
                    WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                        key, false, WorldScan::FlickerCause::ProjFail);
                    // #endregion
                    continue;
                }
            } else {
                // Looking away — not flicker.
                continue;
            }
        } else {
            s_botLastScreen[key] = { head, feet };
            s_botLastScreenWhen[key] = botPaintNow;
            if (s_botLastScreen.size() > 2048) {
                s_botLastScreen.clear();
                s_botLastScreenWhen.clear();
            }
        }

        // ── EMA smoothing (same as player path) ───────────────────────
        {
            struct BotSmooth { ImVec2 head{}; ImVec2 feet{}; bool valid = false; };
            static std::unordered_map<uintptr_t, BotSmooth> s_botSmooth;
            static constexpr float kBotSmoothAlpha = 0.35f;
            auto bs = s_botSmooth.find(key);
            if (bs != s_botSmooth.end() && bs->second.valid) {
                head.x = bs->second.head.x + (head.x - bs->second.head.x) * kBotSmoothAlpha;
                head.y = bs->second.head.y + (head.y - bs->second.head.y) * kBotSmoothAlpha;
                feet.x = bs->second.feet.x + (feet.x - bs->second.feet.x) * kBotSmoothAlpha;
                feet.y = bs->second.feet.y + (feet.y - bs->second.feet.y) * kBotSmoothAlpha;
            }
            s_botSmooth[key] = { head, feet, true };
            // Prune stale entries
            if (s_botSmooth.size() > 1024)
                s_botSmooth.clear();
        }
        // ───────────────────────────────────────────────────────────────

        const ImU32 color_base = BotEspColor(robot.isVisible, robot.IsBreaked);
        const float boxH = feet.y - head.y;
        const Visuals::EspDrawScale scale =
            Visuals::ComputeEspScaleFromBox(boxH > 1.f ? boxH : 24.f, distM);

        // Paint path is DMA-free. The frame builder pre-resolved this label
        // into robotLabel so the paint thread never calls ResolveBotDrawLabel
        // (which does blocking DMA reads: GetActorClassFName, etc).
        const std::string& fname = robot.ActorName;
        std::string botLabel = item.robotLabel;
        // No real name → no ESP (heart/dist alone = ghost bots; log GC Electrified).
        // Never use ARC/Bot/Oil placeholders. IsAnyBotActor alone is not enough.

        // Hatch (extraction point) — separate from bots, own color
        bool isHatch = false;
        {
            std::string lower = botLabel;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            isHatch = lower.find("hatch") != std::string::npos;
        }
        if (isHatch && !var::showHatches)
            continue;

        const ImU32 color = isHatch
            ? ImGui::ColorConvertFloat4ToU32(ImVec4(
                var::color_hatches[0], var::color_hatches[1],
                var::color_hatches[2], var::color_hatches[3]))
            : color_base;
        if (botLabel.empty() || !IsAcceptedBotEspLabel(engine, botLabel, fname)) {
            RecordBotDrawLabelMiss();
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
                key, false, WorldScan::FlickerCause::LabelMiss);
            // #endregion
            continue;
        }
        if (!EspDraw::IsEspBoxOnScreen(head, feet)) {
            // Looking away is not flicker — leave the track as-is so return
            // to screen does not count as an on->off->on blink.
            continue;
        }

        // #region agent log
        WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintBot,
            key, true, WorldScan::FlickerCause::Other);
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

        ++outDrawn;

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

// Append-only microsecond-precision paint breakdown. One line per slow paint.
// Uses ring buffer in memory, flushed to disk on every slow frame.
struct PaintBreakdownRing {
    static constexpr int kRing = 128;
    struct Row {
        uint64_t seq = 0;
        int totalUs = 0;
        int frameCopyUs = 0;
        int cameraUs = 0;
        int playersUs = 0;
        int robotsUs = 0;
        int worldUs = 0;
        int flushUs = 0;
        int nP = 0, nR = 0, nW = 0;
        int nPDraw = 0, nRDraw = 0, nWDraw = 0;  // entities that actually drew
        int worldLabelUs = 0;  // time inside label resolution for world entries
        int worldProjUs = 0;   // time inside projection for world entries
        bool lockBusy = false;
    };
    Row ring[kRing]{};
    int idx = 0;
    void push(Row r) {
        ring[idx] = r;
        idx = (idx + 1) % kRing;
        flush();
    }
    void flush() {
        std::ofstream f("F:/Test/ARCs/paint_breakdown.log", std::ios::trunc);
        if (!f) return;
        for (int i = 0; i < kRing; ++i) {
            int ri = (idx - kRing + i + kRing * 2) % kRing;
            const Row& r = ring[ri];
            if (!r.seq) continue;
            f << "seq=" << r.seq
              << " total=" << r.totalUs
              << " frameCopy=" << r.frameCopyUs
              << " cam=" << r.cameraUs
              << " players=" << r.playersUs
              << " robots=" << r.robotsUs
              << " world=" << r.worldUs
              << " flush=" << r.flushUs
              << " nP=" << r.nP << "/" << r.nPDraw
              << " nR=" << r.nR << "/" << r.nRDraw
              << " nW=" << r.nW << "/" << r.nWDraw
              << " wLabel=" << r.worldLabelUs
              << " wProj=" << r.worldProjUs
              << (r.lockBusy ? " LOCKBUSY" : "")
              << "\n";
        }
    }
};

void Engine::RenderEsp()
{
    EspPaintWatchScope paintWatch{};

    const bool drawPlayers = var::enableesp;
    const bool drawBots = var::showRobots || var::robotAimEnabled;
    const bool drawWorld = AnyWorldEspEnabled();
    if (!drawPlayers && !drawBots && !drawWorld && !var::show_radar)
        return;

    using namespace std::chrono;
    auto nowUs = []() -> int {
        return static_cast<int>(duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count());
    };
    int t0 = nowUs();

    SetProjectionViewport(
        ImGui::GetIO().DisplaySize.x,
        ImGui::GetIO().DisplaySize.y);

    // Never block paint on m_espFrameMutex — keep last good frame if busy.
    static EspRenderFrame s_paintFrame{};
    {
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex, std::try_to_lock);
        if (!lock.owns_lock())
            paintWatch.frameLockBusy = true;
        else if (m_lastEspFrame.valid)
            s_paintFrame = m_lastEspFrame;
    }
    const EspRenderFrame& frame = s_paintFrame;
    paintWatch.frameSeq = frame.frameSeq;
    if (!frame.valid)
        return;

    int tFrameCopy = nowUs();
    int frameCopyUs = tFrameCopy - t0;

    Engine::CameraCache renderCam{};
    if (!ResolveLiveRenderCamera(frame, renderCam))
        return;
    int tCamera = nowUs();
    int cameraUs = tCamera - tFrameCopy;

    g_renderQueue.newFrame();

    // Track how many entities actually drew (not skipped)
    static int s_lastPlayersDrawn = 0, s_lastRobotsDrawn = 0, s_lastWorldDrawn = 0;
    static int s_lastWorldLabelUs = 0, s_lastWorldProjUs = 0;

    if (drawPlayers)
        RenderPlayerEspFromFrame(frame.players, renderCam, s_lastPlayersDrawn);
    int tPlayers = nowUs();
    int playersUs = tPlayers - tCamera;

    if (var::showRobots)
        RenderRobotEspFromFrame(frame.robots, renderCam, s_lastRobotsDrawn);
    int tRobots = nowUs();
    int robotsUs = tRobots - tPlayers;

    if (drawWorld) {
        // Same live POV as players/bots. Using frame.camera here lagged behind
        // g_Camera during stick rotation and drove paintWorld projFail spikes.
        RenderWorldEspFromFrame(frame.world, renderCam, s_lastWorldDrawn,
            s_lastWorldLabelUs, s_lastWorldProjUs);
    }
    int tWorld = nowUs();
    int worldUs = tWorld - tRobots;

    g_renderQueue.endFrame();

    if (ImDrawList* drawList = ImGui::GetForegroundDrawList()) {
        RenderQueue::flushToDrawList(
            drawList,
            g_renderQueue.takeCommands());
    }
    int tFlush = nowUs();
    int flushUs = tFlush - tWorld;
    int totalUs = tFlush - t0;

    // Log breakdown when slow (>100ms = 100000us) or periodically
    static int logCounter = 0;
    if (totalUs > 100000 || (++logCounter % 120 == 0)) {
        static PaintBreakdownRing s_ring;
        PaintBreakdownRing::Row row;
        row.seq = frame.frameSeq;
        row.totalUs = totalUs;
        row.frameCopyUs = frameCopyUs;
        row.cameraUs = cameraUs;
        row.playersUs = playersUs;
        row.robotsUs = robotsUs;
        row.worldUs = worldUs;
        row.flushUs = flushUs;
        row.nP = static_cast<int>(frame.players.size());
        row.nR = static_cast<int>(frame.robots.size());
        row.nW = static_cast<int>(frame.world.size());
        row.nPDraw = s_lastPlayersDrawn;
        row.nRDraw = s_lastRobotsDrawn;
        row.nWDraw = s_lastWorldDrawn;
        row.worldLabelUs = s_lastWorldLabelUs;
        row.worldProjUs = s_lastWorldProjUs;
        row.lockBusy = paintWatch.frameLockBusy;
        s_ring.push(row);
    }

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
        if (ImDrawList* drawList2 = ImGui::GetForegroundDrawList()) {
            drawList2->AddText(ImVec2(10, 10), 0xFF00FF00, dbgTxt1);
            drawList2->AddText(ImVec2(10, 10 + ImGui::GetFontSize() + 8), 0xFF00FF00, dbgTxt2);
        }
    }
}

static void RenderWorldEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& world,
    const Engine::CameraCache& frameCam,
    int& outDrawn, int& outLabelUs, int& outProjUs)
{
    outDrawn = 0;
    outLabelUs = 0;
    outProjUs = 0;
    WorldEspDebugStats dbg{};
    dbg.frameEntries = static_cast<int>(world.size());

    if (!AnyWorldEspEnabled())
        return;

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
            // #region agent log
            WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintWorld,
                item.actorKey, false, WorldScan::FlickerCause::DistEdge);
            // #endregion
            continue;
        }

        const bool isPickup = LootItemLooksLikePickup(filterView);

        // PROJ1 (Fix #6): a single failed WorldToScreen blanked the label for
        // that frame (paintWorld projFail spikes of 40-50 per 10s). Hold the
        // last good screen pos briefly so a transient miss no longer blinks.
        static std::unordered_map<uintptr_t, std::pair<Vector3, std::chrono::steady_clock::time_point>>
            s_worldLastScreen;
        static constexpr auto kWorldScreenFreezeTtl = std::chrono::milliseconds(400);
        const auto paintNow = std::chrono::steady_clock::now();

        Vector3 screen{};
        bool projected = ProjectWorldEspPoint(worldPos, frameCam, screen);
        if (projected) {
            s_worldLastScreen[item.actorKey] = { screen, paintNow };
            if (s_worldLastScreen.size() > 4096)
                s_worldLastScreen.clear();
        } else {
            ++dbg.skipProj;
            // GHOST1: never repaint a stale screen pos once the camera no
            // longer faces the object — that painted phantom labels at
            // random spots after rotation.
            if (const auto fit = s_worldLastScreen.find(item.actorKey);
                fit != s_worldLastScreen.end()
                && paintNow - fit->second.second <= kWorldScreenFreezeTtl
                && WorldPointRoughlyInView(frameCam, worldPos)) {
                screen = fit->second.first;
                projected = true;
            } else {
                // #region agent log
                WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintWorld,
                    item.actorKey, false, WorldScan::FlickerCause::ProjFail);
                // #endregion
                continue;
            }
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
        ++outDrawn;
        // #region agent log
        WorldScan::NoteFlickerDrawing(WorldScan::FlickerChannel::PaintWorld,
            item.actorKey, true, WorldScan::FlickerCause::Other);
        // #endregion
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
        return EspDraw::ColorFromRGBA(rgba);
    };

    {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;  // FREEZE FIX: never block the paint thread on cache swap
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
                drawBlip(rx, ry, color, actor.Distance);
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
