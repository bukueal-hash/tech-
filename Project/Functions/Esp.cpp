#include "../Core/Engine.h"
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

constexpr size_t kMaxEspFramePosReads = 384;
constexpr size_t kMaxEspFrameBoneReads = 32;
constexpr size_t kReservedPlayerPosReads = 64;

namespace {

struct WorldEspDebugStats {
    int frameEntries = 0;
    int rendered = 0;
    int skipAllow = 0;
    int skipPos = 0;
    int skipDist = 0;
    int skipPickedUp = 0;
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
            || ContainerLootLooksOpened(key, fnameHint));

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
    if (!IsPcmValid(pcm) && pc)
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
    {
        std::shared_lock<std::shared_mutex> lock(engine.m_cameraMutex);
        if (CameraOkForEsp(engine.g_Camera)) {
            outCam = engine.g_Camera;
            return true;
        }
    }
    if (frame.valid && CameraOkForEsp(frame.camera)) {
        outCam = frame.camera;
        return true;
    }
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

static ImU32 BotEspColor(bool visible)
{
    const float* c = visible ? var::bot_color_visible : var::bot_color_invisible;
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

static void DrawWeaponLabel(
    ImDrawList* drawList,
    const Engine::PlayerCacheEntry& actor,
    float anchorX,
    float anchorY)
{
    if (!var::show_weapon || actor.weaponName.empty())
        return;

    const ImU32 wColor = actor.weaponQuality > 0
        ? WeaponTierColor(actor.weaponQuality)
        : IM_COL32(200, 200, 200, 255);
    EspDraw::DrawLabelEsp(
        drawList,
        ImVec2(anchorX, anchorY),
        actor.weaponName.c_str(),
        wColor,
        actor.Distance);
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

static float StackPlayerLabels(
    ImDrawList* drawList,
    const Engine::PlayerCacheEntry& actor,
    float headX,
    float& labelStackY,
    const Visuals::EspDrawScale& scale)
{
    if (var::names && !actor.ActorName.empty()) {
        Visuals::Names(
            actor.ActorName,
            headX,
            labelStackY,
            scale,
            ImColor(255, 255, 255, 255));
        labelStackY -= LabelTextHeight(actor.ActorName.c_str(), actor.Distance) + 6.f;
    }

    if (var::show_weapon && !actor.weaponName.empty()) {
        DrawWeaponLabel(drawList, actor, headX, labelStackY);
        labelStackY -= LabelTextHeight(actor.weaponName.c_str(), actor.Distance) + 4.f;
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

static void RenderWorldEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& world,
    const Engine::CameraCache& frameCam);

static void RenderRobotEspFromFrame(
    const std::vector<Engine::EspFrameWorld>& robots,
    const Engine::CameraCache& frameCam);

static void DrawPlayerSkeletonFromCache(
    ImDrawList* drawList,
    const Engine& eng,
    const Engine::PlayerCacheEntry& actor,
    ImU32 color,
    float distanceM)
{
    if (!drawList)
        return;

    const Visuals::EspDrawScale scale =
        Visuals::ComputeEspScaleFromDistance(distanceM);
    const float thickness = scale.lineThickness;

    for (const auto& [boneA, boneB] : eng.SkeletonLinksArcRaiders) {
        const size_t idxA = static_cast<size_t>(boneA);
        const size_t idxB = static_cast<size_t>(boneB);
        if (!actor.boneData.valid.test(idxA) || !actor.boneData.valid.test(idxB))
            continue;

        const Vector3& a = actor.boneData.bonesDouble[idxA];
        const Vector3& b = actor.boneData.bonesDouble[idxB];
        if (a.x <= 0.0 || a.y <= 0.0 || b.x <= 0.0 || b.y <= 0.0)
            continue;

        drawList->AddLine(
            ImVec2(static_cast<float>(a.x), static_cast<float>(a.y)),
            ImVec2(static_cast<float>(b.x), static_cast<float>(b.y)),
            color,
            thickness);
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
    if (!entry.bIsDeathVerge && entry.health <= 0.f)
        return false;
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

void Engine::PublishEspSnapshot(
    CameraCache camera,
    std::unordered_map<uintptr_t, PlayerCacheEntry>&& players,
    std::unordered_map<uintptr_t, WorldCacheEntry>&& world,
    std::unordered_map<uintptr_t, WorldCacheEntry>&& robots)
{
    const int writeIdx = 1 - m_espSnapshotReadIdx.load(std::memory_order_acquire);
    m_espSnapshots[writeIdx].camera = camera;
    m_espSnapshots[writeIdx].players = std::move(players);
    m_espSnapshots[writeIdx].world = std::move(world);
    m_espSnapshots[writeIdx].robots = std::move(robots);
    m_espSnapshots[writeIdx].sequence =
        m_espSnapshotSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    m_espSnapshots[writeIdx].timestampMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    m_espSnapshotReadIdx.store(writeIdx, std::memory_order_release);
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

        auto appendWorld = [&](const std::unordered_map<uintptr_t, WorldCacheEntry>& cache) {
            for (const auto& [key, entry] : cache) {
                if (!ShouldDrawWorldEsp(entry))
                    continue;

                EspFrameWorld frameWorld{};
                frameWorld.actorKey = key;
                frameWorld.entry = entry;
                out.world.push_back(std::move(frameWorld));
            }
        };

        {
            std::shared_lock<std::shared_mutex> lock(m_containerCacheMutex);
            appendWorld(containerCache);
        }
        {
            std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
            appendWorld(itemCache);
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

    std::vector<EspPosReadTarget> posTargets;
    posTargets.reserve(out.world.size() + out.robots.size());

    auto queuePosTarget = [&](
        EspPosTargetKind kind,
        size_t index,
        uintptr_t rootComponent,
        float distance) {
        if (!rootComponent || !IsValidPointer(rootComponent))
            return;
        posTargets.push_back(
            EspPosReadTarget{ kind, index, rootComponent, distance, {}, {} });
    };

    for (size_t i = 0; i < out.world.size(); ++i)
        queuePosTarget(
            EspPosTargetKind::World,
            i,
            out.world[i].entry.rootComponent,
            out.world[i].entry.Distance);
    for (size_t i = 0; i < out.robots.size(); ++i)
        queuePosTarget(
            EspPosTargetKind::Robot,
            i,
            out.robots[i].entry.rootComponent,
            out.robots[i].entry.Distance);

    std::sort(posTargets.begin(), posTargets.end(),
        [](const EspPosReadTarget& a, const EspPosReadTarget& b) {
            const auto rank = [](EspPosTargetKind kind) -> int {
                switch (kind) {
                case EspPosTargetKind::Robot: return 0;
                case EspPosTargetKind::World: return 1;
                default: return 2;
                }
            };
            const int ra = rank(a.kind);
            const int rb = rank(b.kind);
            if (ra != rb)
                return ra < rb;
            return a.distance < b.distance;
        });

    if (posTargets.size() > kMaxEspFramePosReads)
        posTargets.resize(kMaxEspFramePosReads);

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

    for (EspPosReadTarget& target : posTargets) {
        g_scatter.prepare(
            target.rootComponent + Offsets::ComponentToWorld + 0x20,
            target.c2w);
        g_scatter.prepare(
            target.rootComponent + Offsets::RelativeLocation,
            target.relative);
    }

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

    for (const EspPosReadTarget& target : posTargets) {
        const Vector3 worldPos = ResolveScatterWorldPos(target.c2w, target.relative);
        if (!IsPlausibleWorldPos(worldPos))
            continue;

        switch (target.kind) {
        case EspPosTargetKind::World:
            out.world[target.index].entry.WorldPos = worldPos;
            break;
        case EspPosTargetKind::Robot:
            out.robots[target.index].entry.WorldPos = worldPos;
            break;
        default:
            break;
        }
    }

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

        if (actor->ScreenTop.x <= 0.0 || actor->ScreenTop.y <= 0.0
            || actor->ScreenBottom.x <= 0.0 || actor->ScreenBottom.y <= 0.0)
            continue;

        const ImVec2 head(
            static_cast<float>(actor->ScreenTop.x),
            static_cast<float>(actor->ScreenTop.y));
        const ImVec2 feet(
            static_cast<float>(actor->ScreenBottom.x),
            static_cast<float>(actor->ScreenBottom.y));

        const ImU32 color = PlayerEspColor(*actor);
        const float boxH = feet.y - head.y;
        if (boxH < 2.f)
            continue;

        const Visuals::EspDrawScale scale =
            Visuals::ComputeEspScaleFromBox(boxH, actor->Distance);

        const bool drawSilhouette =
            var::silhouette && actor->Distance <= silhouetteMaxM;
        const bool drawSkeleton =
            var::skeleton && !drawSilhouette;

        if (drawSilhouette) {
            Visuals::HumanSilhouetteInput silIn{};
            if (EspDraw::BuildHumanSilhouetteInput(engine, *actor, frameCam, silIn)) {
                const ImU32 fill = (color & 0x00FFFFFFu)
                    | (static_cast<ImU32>(std::clamp(
                           static_cast<int>((color >> IM_COL32_A_SHIFT) & 0xFF) / 2 + 64,
                           48,
                           180)) << IM_COL32_A_SHIFT);
                Visuals::DrawHumanSilhouetteFilled(drawList, silIn, fill);
            }
        } else if (drawSkeleton) {
            DrawPlayerSkeletonFromCache(drawList, engine, *actor, color, actor->Distance);
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
            StackPlayerLabels(drawList, *actor, head.x, labelStackY, scale);

        if (var::snaplines && EspDraw::IsEspPointOnScreen(feet))
            EspDraw::DrawSnaplineEsp(drawList, feet, color, actor->Distance);
    }
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
        if (WorldScan::LooksLikeContainerActor(key, robot.ActorName)
            && !IsAcceptedBotEspLabel(engine, robot.ActorName))
            continue;

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
        if (!EspDraw::ResolveBotHeadFeetWorld(drawEntry, headWorld, feetWorld))
            continue;

        ImVec2 head{};
        ImVec2 feet{};
        if (!EspDraw::WorldToScreenBox(engine, frameCam, headWorld, feetWorld, head, feet))
            continue;

        const ImU32 color = BotEspColor(robot.isVisible);
        const float boxH = feet.y - head.y;
        const Visuals::EspDrawScale scale =
            Visuals::ComputeEspScaleFromBox(boxH > 1.f ? boxH : 24.f, distM);

        std::string fname;
        if (robot.ActorName.empty()
            || robot.ActorName == kBotStructAdmissionToken
            || !IsAcceptedBotEspLabel(engine, robot.ActorName)) {
            fname = engine.GetActorFNameStringCached(key);
            if (fname.empty())
                fname = engine.GetActorFNameString(key);
        }

        const std::string botLabel =
            ResolveBotDrawLabel(key, robot.ActorName, fname);
        if (botLabel.empty()) {
            RecordBotDrawLabelMiss();
            continue;
        }
        if (!EspDraw::IsEspBoxOnScreen(head, feet))
            continue;

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
                head.y,
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

void Engine::RenderEsp()
{
    const bool drawPlayers = var::enableesp;
    const bool drawBots = var::showRobots || var::robotAimEnabled;
    const bool drawWorld = AnyWorldEspEnabled();
    if (!drawPlayers && !drawBots && !drawWorld && !var::show_radar)
        return;

    SetProjectionViewport(
        ImGui::GetIO().DisplaySize.x,
        ImGui::GetIO().DisplaySize.y);

    Engine::CameraCache renderCam{};
    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        renderCam = g_Camera;
    }
    if (!IsUsableCameraFov(renderCam.FOV) || !IsPlausibleWorldPos(renderCam.Location))
        return;

    g_renderQueue.newFrame();

    if (drawPlayers)
        RenderPlayerEspFromCache(renderCam);

    EspRenderFrame frame{};
    {
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex);
        frame = m_lastEspFrame;
    }

    Engine::CameraCache frameCam = renderCam;
    if (frame.valid)
        ResolveLiveRenderCamera(frame, frameCam);

    if (frame.valid) {
        if (var::showRobots)
            RenderRobotEspFromFrame(frame.robots, frameCam);
        if (drawWorld)
            RenderWorldEspFromFrame(frame.world, frameCam);
    }

    g_renderQueue.endFrame();

    if (ImDrawList* drawList = ImGui::GetForegroundDrawList()) {
        RenderQueue::flushToDrawList(
            drawList,
            g_renderQueue.takeCommands());
    }

    if (var::show_debug_overlay && drawWorld) {
        static auto lastWorldDbg = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastWorldDbg >= std::chrono::seconds(1)) {
            lastWorldDbg = now;
            std::cout << "[debugWorldEsp] frame=" << g_worldEspDbg.frameEntries
                << " rendered=" << g_worldEspDbg.rendered
                << " skipAllow=" << g_worldEspDbg.skipAllow
                << " skipPos=" << g_worldEspDbg.skipPos
                << " skipDist=" << g_worldEspDbg.skipDist
                << " skipPickedUp=" << g_worldEspDbg.skipPickedUp
                << " skipProj=" << g_worldEspDbg.skipProj
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

    const Engine::EngineStateSnapshot stateSnap = engine.GetStateSnapshot();
    const Vector3 distRef = engine.ResolveDistanceReference(frameCam, stateSnap.acknowledgedPawn);

    for (const Engine::EspFrameWorld& item : world) {
        const uintptr_t key = item.actorKey;
        const Engine::WorldCacheEntry& entry = item.entry;
        if (!engine.getAllowWorldEntry(entry)) {
            ++dbg.skipAllow;
            continue;
        }
        if (!IsPlausibleWorldPos(entry.WorldPos)) {
            ++dbg.skipPos;
            continue;
        }

        const Vector3 worldPos = entry.WorldPos;
        const double dx = static_cast<double>(worldPos.x) - distRef.x;
        const double dy = static_cast<double>(worldPos.y) - distRef.y;
        const double dz = static_cast<double>(worldPos.z) - distRef.z;
        const float distM = static_cast<float>(
            std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0);

        std::string fname = engine.GetActorFNameStringCached(key);
        if (fname.empty())
            fname = engine.GetActorFNameString(key);
        if (fname.empty() && !entry.ActorName.empty())
            fname = entry.ActorName;

        const auto cat = static_cast<WorldItemCategory>(entry.worldCategory);
        const bool isGroundLoot = IsGroundLootEspCategory(cat);
        const bool isContainerEsp =
            WorldCategoryIsContainerProp(cat) && !isGroundLoot;

        if (isGroundLoot
            && GroundLootLooksPickedUp(
                key, fname.empty() ? entry.ActorName : fname)) {
            ++dbg.skipPickedUp;
            continue;
        }

        WorldLootFilterView distFilterView{
            entry.worldCategory,
            entry.ActorName,
            entry.ItemDisplayName,
            entry.lootValue,
            entry.lootRarityTier};
        const float maxDrawM = WorldLootPickupMaxDrawMeters(cat, &distFilterView);
        if (distM > maxDrawM) {
            ++dbg.skipDist;
            continue;
        }

        std::string classFname;
        if (isContainerEsp)
            classFname = engine.GetActorClassFName(key);
        if (fname.empty() && !classFname.empty())
            fname = classFname;

        if (!isContainerEsp && !isGroundLoot) {
            Vector3 headScreen{};
            if (!engine.ProjectWorldLocationToScreen(worldPos, headScreen, frameCam))
                continue;

            constexpr float kWorldItemHeightCm = 50.f;
            Vector3 feetWorld = worldPos;
            feetWorld.z -= kWorldItemHeightCm;
            Vector3 feetScreen{};
            if (!engine.ProjectWorldLocationToScreen(feetWorld, feetScreen, frameCam))
                continue;

            const float boxH = static_cast<float>(std::abs(feetScreen.y - headScreen.y));
            if (boxH < 2.f)
                continue;
        }

        std::string label;
        if (isGroundLoot) {
            label = entry.ItemDisplayName;
            if (label.empty() || IsGenericWorldEspLabel(label) || !IsPlausibleEspLabel(label)) {
                const std::string memName = engine.GetEnglishItemName(key);
                if (!memName.empty() && IsPlausibleEspLabel(memName))
                    label = memName;
            }
            if (label.empty() || IsGenericWorldEspLabel(label) || !IsPlausibleEspLabel(label)) {
                label = ResolveWorldDisplayLabel(
                    key, fname, static_cast<int>(entry.worldCategory));
            }
            if (label.empty() || IsGenericWorldEspLabel(label) || !IsPlausibleEspLabel(label)) {
                label = ResolveWorldDrawLabel(
                    entry.worldCategory,
                    entry.ActorName,
                    entry.ItemDisplayName);
            }
        } else if (isContainerEsp) {
            label = ResolveContainerEspDrawLabel(key, entry, fname, classFname);
            if (label.empty() || IsJunkWorldEspLabel(label) || IsGarbledEspLabel(label))
                label = ContainerCategoryFallbackLabel(
                    static_cast<WorldItemCategory>(entry.worldCategory));
        } else {
            label = entry.ItemDisplayName;
            if (label.empty() || IsGenericWorldEspLabel(label) || !IsPlausibleEspLabel(label)) {
                const std::string memName = engine.GetEnglishItemName(key);
                if (!memName.empty() && !IsGenericWorldEspLabel(memName))
                    label = memName;
            }
            if (label.empty() || IsGenericWorldEspLabel(label) || !IsPlausibleEspLabel(label)) {
                label = ResolveWorldDisplayLabel(
                    key, fname, static_cast<int>(entry.worldCategory));
            }
        }

        // Non-container world rows still need a real label; ground loot always draws.
        if (!isContainerEsp && !isGroundLoot
            && (label.empty() || IsGenericWorldEspLabel(label)
                || !IsPlausibleEspLabel(label) || IsJunkWorldEspLabel(label)
                || IsGarbledEspLabel(label)))
            continue;
        if (isGroundLoot && (label.empty() || IsJunkWorldEspLabel(label)
                || IsGarbledEspLabel(label) || !IsPlausibleEspLabel(label))) {
            if (const std::string mem = engine.GetEnglishItemName(key);
                !mem.empty() && !IsJunkWorldEspLabel(mem)
                && !IsGarbledEspLabel(mem) && IsPlausibleEspLabel(mem))
                label = mem;
            if (label.empty() || IsJunkWorldEspLabel(label) || IsGarbledEspLabel(label)) {
                if (!entry.ItemDisplayName.empty()
                    && !IsJunkWorldEspLabel(entry.ItemDisplayName)
                    && !IsGarbledEspLabel(entry.ItemDisplayName))
                    label = entry.ItemDisplayName;
                else
                    label = "Dropped Pickup";
            }
        }
        if (label.empty())
            continue;
        if (isContainerEsp && (IsJunkWorldEspLabel(label) || IsGarbledEspLabel(label)))
            label = ContainerCategoryFallbackLabel(
                static_cast<WorldItemCategory>(entry.worldCategory));

        label = FormatEspDisplayLabel(label);
        if (label.empty())
            continue;

        int lootValue = entry.lootValue;
        int lootTier = entry.lootRarityTier;
        WorldLootFilterView preMetaView{
            entry.worldCategory,
            entry.ActorName.empty() ? fname : entry.ActorName,
            label,
            lootValue,
            lootTier};
        const bool preMetaContainer = isContainerEsp
            || WorldLootEntryLooksLikeContainer(preMetaView);
        if (preMetaContainer) {
            lootValue = 0;
            lootTier = 0;
        } else if (!isGroundLoot) {
            ResolveItemMetaForActor(engine, key, fname, label, lootTier, lootValue);
            if (lootValue <= 0 || lootTier <= 0) {
                const std::string memName = engine.GetEnglishItemName(key);
                if (!memName.empty() && memName != label) {
                    int altTier = 0;
                    int altValue = 0;
                    if (ResolveItemMetaForActor(engine, key, fname, memName, altTier, altValue)) {
                        if (lootValue <= 0)
                            lootValue = altValue;
                        if (lootTier <= 0)
                            lootTier = altTier;
                    }
                }
            }
        } else if (lootValue <= 0 || lootTier <= 0) {
            ResolveItemMetaForActor(
                engine, key, fname, label.empty() ? entry.ItemDisplayName : label,
                lootTier, lootValue);
        }

        WorldLootFilterView filterView{
            entry.worldCategory,
            label,
            label,
            lootValue,
            lootTier};
        if (distM > WorldLootPickupMaxDrawMeters(cat, &filterView)) {
            ++dbg.skipDist;
            continue;
        }

        const bool isPickup = LootItemLooksLikePickup(filterView);
        const bool looksLikeContainer = WorldLootEntryLooksLikeContainer(filterView);

        Vector3 screen{};
        if (!engine.ProjectWorldLocationToScreen(worldPos, screen, frameCam)) {
            ++dbg.skipProj;
            continue;
        }

        const float screenW = ImGui::GetIO().DisplaySize.x;
        const float screenH = ImGui::GetIO().DisplaySize.y;
        if (screen.x < -8.f || screen.x > screenW + 8.f
            || screen.y < -8.f || screen.y > screenH + 8.f)
            continue;

        const ImU32 color = WorldLootLabelColor(
            [&]() -> WorldItemCategory {
                if (!isContainerEsp || !var::show_world_open_container)
                    return cat;
                if (ContainerLootLooksOpened(
                        key,
                        entry.ActorName.empty() ? fname : entry.ActorName))
                    return WorldItemCategory::OpenedContainer;
                return cat;
            }(),
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

void Engine::RenderRadar()
{
    if (!var::show_radar)
        return;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList)
        return;

    EspRenderFrame frame{};
    {
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex);
        frame = m_lastEspFrame;
    }
    if (!frame.valid) {
        EspRenderFrame fresh{};
        if (CollectEspRenderFrame(fresh))
            frame = std::move(fresh);
    }

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
    const float cx = screenW * std::clamp(var::radar_pos_x_norm, 0.05f, 0.95f);
    const float cy = screenH * std::clamp(var::radar_pos_y_norm, 0.05f, 0.95f);
    const float rangeM = var::radar_range > 0.f ? var::radar_range : 100.f;
    const float yawRad = static_cast<float>(engine.DegToRad(static_cast<double>(frameCam.Rotation.y)));
    const float cosYaw = std::cos(yawRad);
    const float sinYaw = std::sin(yawRad);

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

        const ImU32 color = robot.isVisible
            ? pickerColor(var::bot_color_visible)
            : pickerColor(var::bot_color_invisible);
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
