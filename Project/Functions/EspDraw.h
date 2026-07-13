#pragma once

#include "../Core/Cache.hpp"
#include "../Core/Engine.h"
#include "../Interface/Utils/Visuals/visuals.hpp"
#include "../Interface/Render/RenderQueue.h"
#include "../ThirdParty/ImGui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cfloat>

namespace EspDraw {

inline bool ResolveBotHeadFeetWorld(const Vector3& worldPos, Vector3& headWorld, Vector3& feetWorld)
{
    if (!IsPlausibleWorldPos(worldPos))
        return false;
    constexpr float kHalfHeightCm = 90.f;
    headWorld = worldPos;
    feetWorld = worldPos;
    headWorld.z += kHalfHeightCm;
    feetWorld.z -= kHalfHeightCm;
    return true;
}

inline bool ResolveBotHeadFeetWorld(
    const Engine::WorldCacheEntry& robot,
    Vector3& headWorld,
    Vector3& feetWorld)
{
    if (robot.BotPartCount > 0) {
        float minZ = FLT_MAX;
        float maxZ = -FLT_MAX;
        double sumX = 0.0;
        double sumY = 0.0;
        int count = 0;

        for (int i = 0; i < robot.BotPartCount; ++i) {
            const Vector3& part = robot.BotPartPos[i];
            if (!IsPlausibleWorldPos(part))
                continue;

            const float z = static_cast<float>(part.z);
            minZ = (std::min)(minZ, z);
            maxZ = (std::max)(maxZ, z);
            sumX += part.x;
            sumY += part.y;
            ++count;
        }

        if (count > 0 && maxZ > minZ + 5.f) {
            const double inv = 1.0 / static_cast<double>(count);
            headWorld = Vector3{ sumX * inv, sumY * inv, static_cast<double>(maxZ) };
            feetWorld = Vector3{ sumX * inv, sumY * inv, static_cast<double>(minZ) };
            return true;
        }
    }

    return ResolveBotHeadFeetWorld(robot.WorldPos, headWorld, feetWorld);
}

inline bool ResolvePlayerHeadFeetWorld(
    const Engine::PlayerCacheEntry& actor,
    Vector3& headWorld,
    Vector3& feetWorld)
{
    constexpr float kHalfHeightCm = 90.f;
    // Head bone sits near skull center; extend above for full coverage.
    constexpr float kHeadTopPadCm = 18.f;
    // Feet bones sit on sole; extend slightly below ground contact.
    constexpr float kFeetBottomPadCm = 6.f;
    const Vector3 origin = actor.WorldPos;
    if (!IsPlausibleWorldPos(origin))
        return false;

    bool headOk = false;
    bool feetOk = false;

    if (actor.boneData.valid.test(static_cast<size_t>(UniBone::Head))) {
        headWorld = actor.boneData.bonesWorldDouble[static_cast<size_t>(UniBone::Head)];
        headOk = IsPlausibleWorldPos(headWorld);
        if (headOk)
            headWorld.z += kHeadTopPadCm;
    }

    if (actor.boneData.valid.test(static_cast<size_t>(UniBone::FootL))
        || actor.boneData.valid.test(static_cast<size_t>(UniBone::FootR))) {
        bool haveFoot = false;
        double lowestZ = 0.0;
        auto considerFoot = [&](UniBone bone) {
            if (!actor.boneData.valid.test(static_cast<size_t>(bone)))
                return;
            const Vector3& foot =
                actor.boneData.bonesWorldDouble[static_cast<size_t>(bone)];
            if (!IsPlausibleWorldPos(foot))
                return;
            if (!haveFoot || foot.z < lowestZ) {
                lowestZ = foot.z;
                feetWorld = foot;
                haveFoot = true;
            }
        };
        considerFoot(UniBone::FootL);
        considerFoot(UniBone::FootR);
        feetOk = haveFoot;
        if (feetOk)
            feetWorld.z -= kFeetBottomPadCm;
    } else if (actor.boneData.valid.test(static_cast<size_t>(UniBone::Pelvis))) {
        feetWorld = actor.boneData.bonesWorldDouble[static_cast<size_t>(UniBone::Pelvis)];
        feetOk = IsPlausibleWorldPos(feetWorld);
        if (feetOk)
            feetWorld.z -= kFeetBottomPadCm;
    }

    if (!headOk) {
        headWorld = origin;
        headWorld.z += kHalfHeightCm;
    }
    if (!feetOk) {
        feetWorld = origin;
        feetWorld.z -= kHalfHeightCm;
    }
    return true;
}

inline bool WorldToScreenBox(
    Engine& eng,
    const Engine::CameraCache& cam,
    const Vector3& headWorld,
    const Vector3& feetWorld,
    ImVec2& head,
    ImVec2& feet)
{
    Vector3 headScr{};
    Vector3 feetScr{};
    if (!eng.ProjectWorldLocationToScreen(headWorld, headScr, cam))
        return false;
    if (!eng.ProjectWorldLocationToScreen(feetWorld, feetScr, cam))
        return false;

    if (feetScr.y < headScr.y)
        std::swap(headScr, feetScr);

    const float height = static_cast<float>(feetScr.y - headScr.y);
    if (height < 2.f)
        return false;

    head = ImVec2(static_cast<float>(headScr.x), static_cast<float>(headScr.y));
    feet = ImVec2(static_cast<float>(feetScr.x), static_cast<float>(feetScr.y));
    return std::isfinite(head.x) && std::isfinite(head.y)
        && std::isfinite(feet.x) && std::isfinite(feet.y);
}

/** Same screen/world point as DrawBotHeartIfEnabled (box center). */
inline bool ResolveBotHeartScreenPoint(
    Engine& eng,
    const Engine::CameraCache& cam,
    const Engine::WorldCacheEntry& robot,
    Vector3& outScreenPos,
    Vector3& outWorldPos)
{
    Vector3 headWorld{};
    Vector3 feetWorld{};
    if (!ResolveBotHeadFeetWorld(robot, headWorld, feetWorld))
        return false;

    ImVec2 head{};
    ImVec2 feet{};
    if (!WorldToScreenBox(eng, cam, headWorld, feetWorld, head, feet))
        return false;

    outScreenPos.x = (head.x + feet.x) * 0.5;
    outScreenPos.y = (head.y + feet.y) * 0.5;
    outScreenPos.z = 0.0;

    outWorldPos.x = (headWorld.x + feetWorld.x) * 0.5;
    outWorldPos.y = (headWorld.y + feetWorld.y) * 0.5;
    outWorldPos.z = (headWorld.z + feetWorld.z) * 0.5;
    return true;
}

inline bool IsEspBoxOnScreen(
    const ImVec2& head,
    const ImVec2& feet,
    float margin = 8.f)
{
    constexpr float kOriginEps = 2.f;
    if (std::fabs(head.x) < kOriginEps && std::fabs(head.y) < kOriginEps
        && std::fabs(feet.x) < kOriginEps && std::fabs(feet.y) < kOriginEps)
        return false;

    const float cx = (head.x + feet.x) * 0.5f;
    const float cy = (head.y + feet.y) * 0.5f;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (cx < -margin || cy < -margin || cx > disp.x + margin || cy > disp.y + margin)
        return false;

    return true;
}

inline bool TryBuildPlayerBoneWorldAabb(
    const Engine::PlayerCacheEntry& actor,
    Vector3& outMin,
    Vector3& outMax)
{
    bool any = false;
    auto expand = [&](const Vector3& p) {
        if (!IsPlausibleWorldPos(p))
            return;
        if (!any) {
            outMin = p;
            outMax = p;
            any = true;
            return;
        }
        outMin.x = (std::min)(outMin.x, p.x);
        outMin.y = (std::min)(outMin.y, p.y);
        outMin.z = (std::min)(outMin.z, p.z);
        outMax.x = (std::max)(outMax.x, p.x);
        outMax.y = (std::max)(outMax.y, p.y);
        outMax.z = (std::max)(outMax.z, p.z);
    };

    for (size_t i = 0; i < static_cast<size_t>(UniBone::Count); ++i) {
        if (!actor.boneData.valid.test(i))
            continue;
        expand(actor.boneData.bonesWorldDouble[i]);
    }

    if (!any)
        return false;

    constexpr double pad = 8.0;
    outMin.x -= pad;
    outMin.y -= pad;
    outMin.z -= pad;
    outMax.x += pad;
    outMax.y += pad;
    outMax.z += pad;
    return true;
}

/** Bukupex GetBounds: project 8 AABB corners, return screen rect (top-left, bottom-right). */
inline bool ProjectWorldAabbToScreenRect(
    Engine& eng,
    const Engine::CameraCache& cam,
    const Vector3& wMin,
    const Vector3& wMax,
    ImVec2& outTopLeft,
    ImVec2& outBottomRight)
{
    const Vector3 corners[8] = {
        { wMin.x, wMin.y, wMin.z },
        { wMin.x, wMax.y, wMin.z },
        { wMax.x, wMax.y, wMin.z },
        { wMax.x, wMin.y, wMin.z },
        { wMax.x, wMax.y, wMax.z },
        { wMin.x, wMax.y, wMax.z },
        { wMin.x, wMin.y, wMax.z },
        { wMax.x, wMin.y, wMax.z },
    };

    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;
    bool any = false;

    for (const Vector3& corner : corners) {
        Vector3 scr{};
        if (!eng.ProjectWorldLocationToScreen(corner, scr, cam))
            continue;
        const float x = static_cast<float>(scr.x);
        const float y = static_cast<float>(scr.y);
        if (!std::isfinite(x) || !std::isfinite(y))
            continue;
        if (!any) {
            left = right = x;
            top = bottom = y;
            any = true;
        } else {
            left = (std::min)(left, x);
            right = (std::max)(right, x);
            top = (std::min)(top, y);
            bottom = (std::max)(bottom, y);
        }
    }

    if (!any)
        return false;

    const float w = right - left;
    const float h = bottom - top;
    if (w < 2.f || h < 2.f)
        return false;

    outTopLeft = ImVec2(left, top);
    outBottomRight = ImVec2(right, bottom);
    return true;
}

inline bool ResolvePlayerScreenBox(
    Engine& eng,
    const Engine::CameraCache& cam,
    const Engine::PlayerCacheEntry& actor,
    ImVec2& head,
    ImVec2& feet,
    ImVec2& topLeft,
    ImVec2& bottomRight,
    bool& usedAabb)
{
    usedAabb = false;

    if (actor.Drawing
        && actor.ScreenTop.x > 0.0 && actor.ScreenTop.y > 0.0
        && actor.ScreenBottom.x > 0.0 && actor.ScreenBottom.y > 0.0)
    {
        head = ImVec2(
            static_cast<float>(actor.ScreenTop.x),
            static_cast<float>(actor.ScreenTop.y));
        feet = ImVec2(
            static_cast<float>(actor.ScreenBottom.x),
            static_cast<float>(actor.ScreenBottom.y));
        const float boxH = feet.y - head.y;
        topLeft = ImVec2(head.x - boxH * 0.325f, head.y);
        bottomRight = ImVec2(head.x + boxH * 0.325f, feet.y);
        return true;
    }

    Vector3 wMin{};
    Vector3 wMax{};
    if (TryBuildPlayerBoneWorldAabb(actor, wMin, wMax)
        && ProjectWorldAabbToScreenRect(eng, cam, wMin, wMax, topLeft, bottomRight)) {
        const float cx = (topLeft.x + bottomRight.x) * 0.5f;
        head = ImVec2(cx, topLeft.y);
        feet = ImVec2(cx, bottomRight.y);
        usedAabb = true;
        return true;
    }

    Vector3 headWorld{};
    Vector3 feetWorld{};
    if (!ResolvePlayerHeadFeetWorld(actor, headWorld, feetWorld))
        return false;
    if (!WorldToScreenBox(eng, cam, headWorld, feetWorld, head, feet))
        return false;

    topLeft = ImVec2(head.x - (feet.y - head.y) * 0.325f, head.y);
    bottomRight = ImVec2(head.x + (feet.y - head.y) * 0.325f, feet.y);
    return true;
}

inline void DrawBoxEsp(
    ImDrawList* drawList,
    const ImVec2& head,
    const ImVec2& feet,
    ImU32 color,
    bool drawBox,
    float distanceM)
{
    if (!drawList || !drawBox)
        return;

    const float boxH = feet.y - head.y;
    if (boxH < 2.f)
        return;

    const Vector3 screenTop{ head.x, head.y, 0.0 };
    const Vector3 screenBottom{ feet.x, feet.y, 0.0 };
    const Visuals::EspDrawScale scale = Visuals::ComputeEspScaleFromBox(boxH, distanceM);
    Visuals::Box(screenTop, screenBottom, true, color, 0, scale);
}

inline void DrawLabelEsp(
    ImDrawList* drawList,
    const ImVec2& anchor,
    const char* text,
    ImU32 color,
    float distanceM,
    bool centerX = true)
{
    if (!drawList || !text || !text[0])
        return;

    Visuals::DrawScaledLabel(
        drawList,
        text,
        anchor.x,
        anchor.y,
        color,
        distanceM,
        centerX,
        true);
}

inline bool IsEspPointOnScreen(const ImVec2& pt, float margin = 32.f)
{
    if (!std::isfinite(pt.x) || !std::isfinite(pt.y))
        return false;
    if (std::fabs(pt.x) < 2.f && std::fabs(pt.y) < 2.f)
        return false;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    return pt.x >= -margin && pt.y >= -margin
        && pt.x <= disp.x + margin && pt.y <= disp.y + margin;
}

inline void DrawSnaplineEsp(
    ImDrawList* drawList,
    const ImVec2& feet,
    ImU32 color,
    float distanceM)
{
    (void)drawList;
    if (!IsEspPointOnScreen(feet))
        return;

    const Visuals::EspDrawScale scale = Visuals::ComputeEspScaleFromDistance(distanceM);
    const ImVec2 screenCenter(
        ImGui::GetIO().DisplaySize.x * 0.5f,
        ImGui::GetIO().DisplaySize.y);
    const float dx = feet.x - screenCenter.x;
    const float dy = feet.y - screenCenter.y;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float maxLenSq = (disp.x * disp.x) + (disp.y * disp.y);
    if ((dx * dx + dy * dy) > maxLenSq * 4.f)
        return;

    g_renderQueue.addLine(screenCenter, feet, color, scale.lineThickness);
}

inline bool TryProjectBoneScreen(
    Engine& eng,
    const Engine::CameraCache& cam,
    const Engine::PlayerCacheEntry& actor,
    UniBone bone,
    ImVec2& out)
{
    const size_t idx = static_cast<size_t>(bone);
    if (!actor.boneData.valid.test(idx))
        return false;

    Vector3 scr{};
    if (!eng.ProjectWorldLocationToScreen(actor.boneData.bonesWorldDouble[idx], scr, cam))
        return false;

    out.x = static_cast<float>(scr.x);
    out.y = static_cast<float>(scr.y);
    return std::isfinite(out.x) && std::isfinite(out.y);
}

/** World distance (cm) between two valid bones; -1 if either missing/implausible. */
inline float BoneWorldDistCm(
    const Engine::PlayerCacheEntry& actor,
    UniBone a,
    UniBone b)
{
    const size_t ia = static_cast<size_t>(a);
    const size_t ib = static_cast<size_t>(b);
    if (!actor.boneData.valid.test(ia) || !actor.boneData.valid.test(ib))
        return -1.f;
    const Vector3& wa = actor.boneData.bonesWorldDouble[ia];
    const Vector3& wb = actor.boneData.bonesWorldDouble[ib];
    if (!IsPlausibleWorldPos(wa) || !IsPlausibleWorldPos(wb))
        return -1.f;
    const double dx = wa.x - wb.x;
    const double dy = wa.y - wb.y;
    const double dz = wa.z - wb.z;
    return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
}

/**
 * True when both bones project and the world segment is anatomically plausible
 * (drops wrong-index / garbage joints that otherwise draw as squiggles).
 */
inline bool TrySkeletonSegmentScreen(
    Engine& eng,
    const Engine::CameraCache& cam,
    const Engine::PlayerCacheEntry& actor,
    UniBone boneA,
    UniBone boneB,
    ImVec2& outA,
    ImVec2& outB)
{
    constexpr float kMinSegCm = 2.f;
    constexpr float kMaxSegCm = 120.f;
    constexpr float kMaxFromPelvisCm = 250.f;

    if (!TryProjectBoneScreen(eng, cam, actor, boneA, outA))
        return false;
    if (!TryProjectBoneScreen(eng, cam, actor, boneB, outB))
        return false;

    const float segCm = BoneWorldDistCm(actor, boneA, boneB);
    if (segCm < kMinSegCm || segCm > kMaxSegCm)
        return false;

    if (actor.boneData.valid.test(static_cast<size_t>(UniBone::Pelvis))) {
        const float da = BoneWorldDistCm(actor, boneA, UniBone::Pelvis);
        const float db = BoneWorldDistCm(actor, boneB, UniBone::Pelvis);
        if ((da >= 0.f && da > kMaxFromPelvisCm)
            || (db >= 0.f && db > kMaxFromPelvisCm))
            return false;
    }
    return true;
}

inline bool BuildHumanSilhouetteInput(
    Engine& eng,
    const Engine::PlayerCacheEntry& actor,
    const Engine::CameraCache& cam,
    Visuals::HumanSilhouetteInput& out)
{
    out = {};

    auto project = [&](UniBone bone, ImVec2& pt, bool& has) {
        has = TryProjectBoneScreen(eng, cam, actor, bone, pt);
    };

    project(UniBone::Head, out.head, out.hasHead);
    project(UniBone::Neck, out.neck, out.hasNeck);
    project(UniBone::Chest, out.chest, out.hasChest);
    project(UniBone::Pelvis, out.pelvis, out.hasPelvis);
    project(UniBone::ClavicleL, out.clavicleL, out.hasClavicleL);
    project(UniBone::ClavicleR, out.clavicleR, out.hasClavicleR);

    project(UniBone::UpperArmL, out.upperArmL, out.hasUpperArmL);
    project(UniBone::LowerArmL, out.lowerArmL, out.hasLowerArmL);
    project(UniBone::HandL, out.handL, out.hasHandL);
    out.hasArmL = out.hasUpperArmL && out.hasLowerArmL;

    project(UniBone::UpperArmR, out.upperArmR, out.hasUpperArmR);
    project(UniBone::LowerArmR, out.lowerArmR, out.hasLowerArmR);
    project(UniBone::HandR, out.handR, out.hasHandR);
    out.hasArmR = out.hasUpperArmR && out.hasLowerArmR;

    project(UniBone::ThighL, out.thighL, out.hasThighL);
    project(UniBone::CalfL, out.calfL, out.hasCalfL);
    project(UniBone::FootL, out.footL, out.hasFootL);
    out.hasLegL = out.hasThighL && out.hasCalfL;

    project(UniBone::ThighR, out.thighR, out.hasThighR);
    project(UniBone::CalfR, out.calfR, out.hasCalfR);
    project(UniBone::FootR, out.footR, out.hasFootR);
    out.hasLegR = out.hasThighR && out.hasCalfR;

    if (out.hasChest && out.hasPelvis) {
        out.waist = ImVec2(
            (out.chest.x + out.pelvis.x) * 0.5f,
            (out.chest.y + out.pelvis.y) * 0.5f);
    } else if (out.hasChest) {
        out.waist = out.chest;
    }

    return out.hasHead && out.hasNeck;
}

} // namespace EspDraw
