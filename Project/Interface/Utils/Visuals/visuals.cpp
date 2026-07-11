#include "visuals.hpp"
#include "../Variables/index.h"
#include <algorithm>
#include <cmath>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace {

constexpr float kEspRefBoxHeight = 175.f;
constexpr float kEspDistRefM = 30.f;
constexpr float kEspScaleMin = 0.55f;
constexpr float kEspScaleMax = 1.0f;
constexpr float kEspTextMinPx = 9.f;
constexpr float kEspTextBasePx = 15.f;

Visuals::EspDrawScale MakeScale(float espScale)
{
    Visuals::EspDrawScale s{};
    s.espScale = std::clamp(espScale, kEspScaleMin, kEspScaleMax);
    const float userText = std::clamp(var::esp_text_scale, 0.5f, 3.0f);
    s.textPx = std::max(kEspTextMinPx, kEspTextBasePx * s.espScale * userText);
    s.lineThickness = std::clamp(0.55f + s.espScale * 0.45f, 0.55f, 1.35f);
    return s;
}

} // namespace

namespace Visuals {

float AimbotFovRadiusPx(float aimbotFovDegrees, float gameFovDegrees, float screenW)
{
    if (aimbotFovDegrees <= 0.f || screenW < 1.f)
        return 0.f;

    float gameFov = gameFovDegrees;
    if (gameFov <= 1.f || gameFov > 179.f)
        gameFov = 90.f;

    const float aimHalfRad = static_cast<float>((aimbotFovDegrees * 0.5f) * 3.14159265f / 180.0f);
    const float gameHalfRad = static_cast<float>((gameFov * 0.5f) * 3.14159265f / 180.0f);
    const float tanGame = tanf(gameHalfRad);
    if (tanGame < 0.0001f)
        return 4.f;

    float radius = (screenW * 0.5f) * tanf(aimHalfRad) / tanGame;
    return radius < 4.f ? 4.f : radius;
}

} // namespace Visuals

float Visuals::EspDistanceScale(float distanceMeters)
{
    const float d = std::max(distanceMeters, 1.f);
    const float distScale = std::min(kEspScaleMax, kEspDistRefM / d);
    return std::max(kEspScaleMin, distScale);
}

float Visuals::TextScaleFromDistance(float distanceMeters)
{
    return EspDistanceScale(distanceMeters) * EspUserTextScale();
}

float Visuals::LabelTextPx(float distanceMeters)
{
    return MakeScale(EspDistanceScale(distanceMeters)).textPx;
}

void Visuals::DrawScaledLabel(
    ImDrawList* drawList,
    const char* text,
    float anchorX,
    float anchorY,
    ImU32 color,
    float distanceMeters,
    bool centerX,
    bool aboveAnchor)
{
    if (!drawList || !text || !*text)
        return;

    ImFont* font = ImGui::GetFont();
    const float fontSize = LabelTextPx(distanceMeters);
    const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, text);

    float x = anchorX;
    if (centerX)
        x -= size.x * 0.5f;
    const float y = aboveAnchor ? (anchorY - size.y - 2.f) : anchorY;

    const float outline = std::max(1.f, fontSize * 0.07f);
    const ImU32 outlineColor = IM_COL32(0, 0, 0, 210);
    drawList->AddText(font, fontSize, ImVec2(x - outline, y), outlineColor, text);
    drawList->AddText(font, fontSize, ImVec2(x + outline, y), outlineColor, text);
    drawList->AddText(font, fontSize, ImVec2(x, y - outline), outlineColor, text);
    drawList->AddText(font, fontSize, ImVec2(x, y + outline), outlineColor, text);
    drawList->AddText(font, fontSize, ImVec2(x, y), color, text);
}

void Visuals::Headline(int width, int height, Vector2 target, int distance)
{
    auto vList = ImGui::GetForegroundDrawList();
    const auto start = ImVec2(width * 0.5f, height * 0.5f);
    const auto end = ImVec2(static_cast<float>(target.x), static_cast<float>(target.y));

    float radius = 8.0f * EspDistanceScale(static_cast<float>(distance));
    radius = std::clamp(radius, 2.0f, 8.0f);

    ImColor color;
    if (distance < 5)
        color = ImColor(255, 100, 100, 255);
    else if (distance < 10)
        color = ImColor(255, 200, 100, 255);
    else
        color = ImColor(255, 255, 255, 255);

    vList->AddCircleFilled(end, radius, color);
    vList->AddLine(start, end, IM_COL32(255, 255, 255, 120), 1.0f);
}

float Visuals::EspUserTextScale()
{
    return std::clamp(var::esp_text_scale, 0.5f, 3.0f);
}

float Visuals::EstimateBoxHeightPx(float distanceMeters, float screenH)
{
    const float distM = (std::max)(distanceMeters, 1.f);
    const float est = screenH * 180.f / (distM * 100.f);
    return (std::clamp)(est, 4.f, 400.f);
}

Visuals::EspDrawScale Visuals::ComputeEspScaleFromBox(float boxHeightPx, float distanceMeters)
{
    float scale = EspDistanceScale(distanceMeters > 0.f ? distanceMeters : kEspDistRefM);
    if (boxHeightPx >= 2.f) {
        const float boxScale =
            std::clamp(boxHeightPx / kEspRefBoxHeight, kEspScaleMin, kEspScaleMax);
        scale = (std::max)(scale, boxScale);
    }
    return MakeScale(scale);
}

Visuals::EspDrawScale Visuals::ComputeEspScaleUnified(
    float boxHeightPx,
    float distanceMeters,
    float screenHeightPx)
{
    if (boxHeightPx >= 2.f)
        return ComputeEspScaleFromBox(boxHeightPx, distanceMeters);
    const float estH = EstimateBoxHeightPx(distanceMeters, screenHeightPx);
    return ComputeEspScaleFromBox(estH, distanceMeters);
}

Visuals::EspDrawScale Visuals::ComputeEspScaleFromDistance(float distanceMeters)
{
    return ComputeEspScaleUnified(0.f, distanceMeters, 1080.f);
}

Visuals::EspDrawScale Visuals::ComputeEspScaleFromLootMarker(float markerScreenHeightPx)
{
    const float estDistM = markerScreenHeightPx > 1.f
        ? std::clamp(180.f * 1080.f / (markerScreenHeightPx * 100.f), 1.f, 250.f)
        : kEspDistRefM;
    return ComputeEspScaleFromDistance(estDistM);
}

ImVec2 Visuals::CalcScaledTextSize(const char* text, float textPx)
{
    if (!text || !*text)
        return ImVec2(0.f, 0.f);
    ImFont* font = ImGui::GetFont();
    if (!font)
        return ImGui::CalcTextSize(text);
    return font->CalcTextSizeA(textPx, FLT_MAX, 0.f, text);
}

void Visuals::DrawScaledText(const char* text, float centerX, float y, const EspDrawScale& scale, ImU32 color)
{
    if (!text || !*text)
        return;
    ImFont* font = ImGui::GetFont();
    if (!font)
        return;
    const ImVec2 size = font->CalcTextSizeA(scale.textPx, FLT_MAX, 0.f, text);
    ImVec2 pos(centerX - size.x * 0.5f, y);

    // Black outline/shadow for crisp readability against any background
    auto dl = ImGui::GetForegroundDrawList();
    const float outline = std::max(1.f, scale.textPx * 0.08f);
    const ImU32 outlineColor = IM_COL32(0, 0, 0, 200);
    dl->AddText(font, scale.textPx, ImVec2(pos.x - outline, pos.y), outlineColor, text);
    dl->AddText(font, scale.textPx, ImVec2(pos.x + outline, pos.y), outlineColor, text);
    dl->AddText(font, scale.textPx, ImVec2(pos.x, pos.y - outline), outlineColor, text);
    dl->AddText(font, scale.textPx, ImVec2(pos.x, pos.y + outline), outlineColor, text);
    dl->AddText(font, scale.textPx, pos, color, text);
}

void Visuals::FovCircle(float aimbotFovDegrees, float gameFovDegrees)
{
    if (aimbotFovDegrees <= 0.f)
        return;

    const float screenW = ImGui::GetIO().DisplaySize.x;
    const float screenH = ImGui::GetIO().DisplaySize.y;
    if (screenW < 1.f || screenH < 1.f)
        return;

    const float radius = AimbotFovRadiusPx(aimbotFovDegrees, gameFovDegrees, screenW);
    if (radius < 4.f)
        return;

    ImGui::GetForegroundDrawList()->AddCircle(
        ImVec2(screenW * 0.5f, screenH * 0.5f),
        radius,
        IM_COL32(255, 255, 255, 220),
        64,
        2.0f);
}

void Visuals::SnapLinesDouble(const Vector3& start, const Vector3& end, ImColor color, float thickness)
{
    ImGui::GetForegroundDrawList()->AddLine(
        ImVec2(static_cast<float>(start.x), static_cast<float>(start.y)),
        ImVec2(static_cast<float>(end.x), static_cast<float>(end.y)),
        color,
        thickness);
}

namespace {

bool AxisPerp(ImVec2 a, ImVec2 b, float& nx, float& ny, float& len)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    len = std::hypot(dx, dy);
    if (len < 0.5f)
        return false;
    nx = -dy / len;
    ny = dx / len;
    return true;
}

ImVec2 LerpPt(ImVec2 a, ImVec2 b, float t)
{
    return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

ImVec2 OffsetPt(ImVec2 p, float nx, float ny, float d)
{
    return ImVec2(p.x + nx * d, p.y + ny * d);
}

void FillSegment(ImDrawList* dl, ImVec2 a, ImVec2 b, float halfWa, float halfWb, ImU32 fill)
{
    float nx = 0.f, ny = 0.f, len = 0.f;
    if (!AxisPerp(a, b, nx, ny, len))
        return;
    const ImVec2 quad[4] = {
        OffsetPt(a, nx, ny, halfWa),
        OffsetPt(a, -nx, -ny, halfWa),
        OffsetPt(b, -nx, -ny, halfWb),
        OffsetPt(b, nx, ny, halfWb),
    };
    dl->AddConvexPolyFilled(quad, 4, fill);
}

void FillLimbChainSmooth(
    ImDrawList* dl,
    const ImVec2* joints,
    int count,
    const float* halfWidths,
    ImU32 fill)
{
    if (!dl || count < 2 || !halfWidths)
        return;
    for (int i = 0; i < count - 1; ++i) {
        FillSegment(
            dl,
            joints[i],
            joints[i + 1],
            halfWidths[i],
            halfWidths[i + 1],
            fill);
    }
    for (int i = 1; i < count - 1; ++i) {
        const float wPrev = halfWidths[i - 1];
        const float wNext = halfWidths[i];
        const float r = (std::min)(wPrev, wNext);
        if (r < 0.5f)
            continue;
        if (std::fabs(wPrev - wNext) > r * 0.12f)
            dl->AddCircleFilled(joints[i], r * 0.94f, fill, 10);
    }
}

void FillTorsoAlongSpine(
    ImDrawList* dl,
    const Visuals::HumanSilhouetteInput& in,
    float wNeck,
    float wChest,
    float wWaist,
    float wHip,
    ImU32 fill)
{
    const ImVec2 spineTop = in.neck;
    ImVec2 spineBot = in.pelvis;
    if (!in.hasPelvis) {
        ImVec2 waist = in.waist;
        if (waist.x == 0.f && waist.y == 0.f && in.hasChest)
            waist = LerpPt(in.chest, in.neck, 0.65f);
        spineBot = waist;
    }

    float nx = 0.f, ny = 0.f, slen = 0.f;
    if (!AxisPerp(spineTop, spineBot, nx, ny, slen))
        return;

    auto spinePt = [&](float t) { return LerpPt(spineTop, spineBot, t); };

    ImVec2 chestPt = in.hasChest ? in.chest : spinePt(0.32f);
    ImVec2 waistPt = in.waist;
    if (waistPt.x == 0.f && waistPt.y == 0.f)
        waistPt = spinePt(0.52f);
    ImVec2 hipPt = in.hasPelvis ? in.pelvis : waistPt;

    float wTop = wNeck;
    if (in.hasClavicleL && in.hasClavicleR) {
        const float shoulderSpan = std::hypot(in.clavicleR.x - in.clavicleL.x, in.clavicleR.y - in.clavicleL.y);
        wTop = std::max(wChest * 0.85f, shoulderSpan * 0.42f);
    }

    ImVec2 poly[10]{};
    int pn = 0;
    poly[pn++] = OffsetPt(spineTop, nx, ny, wTop);
    poly[pn++] = OffsetPt(chestPt, nx, ny, wChest);
    poly[pn++] = OffsetPt(waistPt, nx, ny, wWaist);
    poly[pn++] = OffsetPt(hipPt, nx, ny, wHip);
    poly[pn++] = OffsetPt(hipPt, -nx, -ny, wHip);
    poly[pn++] = OffsetPt(waistPt, -nx, -ny, wWaist);
    poly[pn++] = OffsetPt(chestPt, -nx, -ny, wChest);
    poly[pn++] = OffsetPt(spineTop, -nx, -ny, wTop);
    if (pn >= 3)
        dl->AddConvexPolyFilled(poly, pn, fill);
}

void DrawHumanSilhouetteFilledImpl(ImDrawList* dl, const Visuals::HumanSilhouetteInput& in, ImU32 fill)
{
    if (!dl || !in.hasHead || !in.hasNeck)
        return;

    ImVec2 footMid = in.footL;
    if (in.hasLegL && in.hasLegR)
        footMid = ImVec2((in.footL.x + in.footR.x) * 0.5f, (in.footL.y + in.footR.y) * 0.5f);
    else if (in.hasLegR)
        footMid = in.footR;

    const float bodyH = static_cast<float>((std::max)(std::hypot(in.head.x - footMid.x, in.head.y - footMid.y), 32.f));
    const float headR = std::clamp(
        static_cast<float>(std::hypot(in.head.x - in.neck.x, in.head.y - in.neck.y)) * 1.14f,
        7.f,
        bodyH * 0.115f);

    // Slightly more anatomical proportions (less blob, more human form).
    const float wNeck = bodyH * 0.032f;
    const float wChest = bodyH * 0.128f;
    const float wWaist = bodyH * 0.078f;
    const float wHip = bodyH * 0.108f;
    const float wArm = bodyH * 0.044f;
    const float wFore = bodyH * 0.032f;
    const float wHand = bodyH * 0.022f;
    const float wThigh = bodyH * 0.064f;
    const float wCalf = bodyH * 0.045f;
    const float wFoot = bodyH * 0.028f;

    const ImU32 fillSolid = fill;

    FillTorsoAlongSpine(dl, in, wNeck, wChest, wWaist, wHip, fillSolid);

    if (in.hasClavicleL && in.hasClavicleR)
        FillSegment(dl, in.clavicleL, in.clavicleR, wNeck * 0.9f, wNeck * 0.9f, fillSolid);

    if (in.hasLegL && in.hasLegR)
        FillSegment(dl, in.thighL, in.thighR, wHip * 0.7f, wHip * 0.65f, fillSolid);

    if (in.hasLegL) {
        if (in.hasPelvis) {
            const float hipBridge = (std::min)(wHip, wThigh);
            FillSegment(dl, in.pelvis, in.thighL, hipBridge, wThigh, fillSolid);
        }
        ImVec2 chain[3]{ in.thighL, in.calfL, in.footL };
        const float widths[3]{ wThigh, wCalf, wFoot };
        FillLimbChainSmooth(dl, chain, 3, widths, fillSolid);
    }
    if (in.hasLegR) {
        if (in.hasPelvis) {
            const float hipBridge = (std::min)(wHip, wThigh);
            FillSegment(dl, in.pelvis, in.thighR, hipBridge, wThigh, fillSolid);
        }
        ImVec2 chain[3]{ in.thighR, in.calfR, in.footR };
        const float widths[3]{ wThigh, wCalf, wFoot };
        FillLimbChainSmooth(dl, chain, 3, widths, fillSolid);
    }

    if (in.hasArmL) {
        if (in.hasClavicleL)
            FillSegment(dl, in.clavicleL, in.upperArmL, wArm * 0.92f, wArm, fillSolid);
        else if (in.hasChest)
            FillSegment(dl, in.chest, in.upperArmL, wChest * 0.38f, wArm, fillSolid);
        const float handDist = std::hypot(in.handL.x - in.lowerArmL.x, in.handL.y - in.lowerArmL.y);
        if (handDist > 2.f) {
            ImVec2 chain[3]{ in.upperArmL, in.lowerArmL, in.handL };
            const float widths[3]{ wArm, wFore, wHand };
            FillLimbChainSmooth(dl, chain, 3, widths, fillSolid);
        } else {
            ImVec2 chain[2]{ in.upperArmL, in.lowerArmL };
            const float widths[2]{ wArm, wFore };
            FillLimbChainSmooth(dl, chain, 2, widths, fillSolid);
        }
    }

    if (in.hasArmR) {
        if (in.hasClavicleR)
            FillSegment(dl, in.clavicleR, in.upperArmR, wArm * 0.92f, wArm, fillSolid);
        else if (in.hasChest)
            FillSegment(dl, in.chest, in.upperArmR, wChest * 0.38f, wArm, fillSolid);
        const float handDist = std::hypot(in.handR.x - in.lowerArmR.x, in.handR.y - in.lowerArmR.y);
        if (handDist > 2.f) {
            ImVec2 chain[3]{ in.upperArmR, in.lowerArmR, in.handR };
            const float widths[3]{ wArm, wFore, wHand };
            FillLimbChainSmooth(dl, chain, 3, widths, fillSolid);
        } else {
            ImVec2 chain[2]{ in.upperArmR, in.lowerArmR };
            const float widths[2]{ wArm, wFore };
            FillLimbChainSmooth(dl, chain, 2, widths, fillSolid);
        }
    }

    const float hdx = in.head.x - in.neck.x;
    const float hdy = in.head.y - in.neck.y;
    const float hLen = std::hypot(hdx, hdy);
    const float hux = hLen > 0.001f ? hdx / hLen : 0.f;
    const float huy = hLen > 0.001f ? hdy / hLen : -1.f;
    const float headRot = std::atan2(hdy, hdx) - IM_PI * 0.5f;
    const ImVec2 headCenter(
        in.head.x + hux * headR * 0.08f,
        in.head.y + huy * headR * 0.08f);
    const float neckJoin = (std::min)(headR * 0.62f, wNeck * 1.35f);
    if (neckJoin > 0.5f)
        dl->AddCircleFilled(in.neck, neckJoin, fillSolid, 14);

    if (in.hasLegL)
        dl->AddCircleFilled(in.footL, wFoot * 0.65f, fillSolid, 10);
    if (in.hasLegR)
        dl->AddCircleFilled(in.footR, wFoot * 0.65f, fillSolid, 10);

    dl->AddEllipseFilled(
        headCenter,
        ImVec2(headR * 0.82f, headR * 1.08f),
        fillSolid,
        headRot,
        24);
}

} // namespace

void Visuals::DrawHumanSilhouetteFilled(ImDrawList* dl, const HumanSilhouetteInput& in, ImU32 fill)
{
    DrawHumanSilhouetteFilledImpl(dl, in, fill);
}

void Visuals::Names(const std::string& name, float center_x, float top_y, const EspDrawScale& scale, ImColor color)
{
    if (name.empty())
        return;

    ImFont* font = ImGui::GetFont();
    if (!font)
        return;

    const ImVec2 text_size = font->CalcTextSizeA(scale.textPx, FLT_MAX, 0.f, name.c_str());
    const float text_x = center_x - text_size.x * 0.5f;
    const float text_y = top_y - text_size.y - 4.0f * scale.espScale;

    auto dl = ImGui::GetForegroundDrawList();
    const float outline = std::max(1.f, scale.textPx * 0.08f);
    const ImU32 outlineColor = IM_COL32(0, 0, 0, 200);
    dl->AddText(font, scale.textPx, ImVec2(text_x - outline, text_y), outlineColor, name.c_str());
    dl->AddText(font, scale.textPx, ImVec2(text_x + outline, text_y), outlineColor, name.c_str());
    dl->AddText(font, scale.textPx, ImVec2(text_x, text_y - outline), outlineColor, name.c_str());
    dl->AddText(font, scale.textPx, ImVec2(text_x, text_y + outline), outlineColor, name.c_str());
    dl->AddText(font, scale.textPx, ImVec2(text_x, text_y), color, name.c_str());
}

ImColor Visuals::HealthColorFromPct(float hpPct)
{
    const float t = std::clamp(1.f - hpPct, 0.f, 1.f);
    const int r = static_cast<int>(255.f * t);
    const int g = static_cast<int>(255.f * (1.f - t));
    return ImColor(r, g, 0, 255);
}

namespace {

int SnapShieldTierMax(int maxShield)
{
    if (maxShield <= 0)
        return 0;
    if (maxShield <= 62)
        return 50;
    if (maxShield <= 87)
        return 75;
    if (maxShield <= 112)
        return 100;
    return 125;
}

void NormalizeShieldForHealthBar(int& shield, int& maxShield)
{
    if (shield < 0)
        shield = 0;
    if (maxShield < 0)
        maxShield = 0;
    if (maxShield <= 0 && shield <= 0)
        return;

    if (maxShield <= 0) {
        shield = 0;
        return;
    }

    const int tier = SnapShieldTierMax(maxShield);
    if (tier <= 0)
        return;

    const float ratio = std::clamp(static_cast<float>(shield) / static_cast<float>(maxShield), 0.f, 1.f);
    maxShield = tier;
    shield = static_cast<int>(std::round(ratio * static_cast<float>(tier)));
}

} // namespace

float Visuals::HealthShieldBarsAboveHead(
    float centerX,
    float boxTop,
    float /*boxWidth*/,
    float health,
    float maxHealth,
    float shield,
    float maxShield,
    const EspDrawScale& scale,
    ImDrawList* drawList)
{
    const float z = scale.espScale > 0.f ? scale.espScale : 1.f;

    int healthPct = static_cast<int>(std::round(
        100.f * std::clamp(health / std::max(maxHealth, 1.f), 0.f, 1.f)));
    healthPct = std::clamp(healthPct, 0, 100);

    int shieldHeal = static_cast<int>(std::round(std::max(0.f, shield)));
    int shieldMax = static_cast<int>(std::round(std::max(0.f, maxShield)));
    NormalizeShieldForHealthBar(shieldHeal, shieldMax);

    ImDrawList* dl = drawList ? drawList : ImGui::GetBackgroundDrawList();
    const float healthBarAnchorY = boxTop - 12.f * z;
    DrawHealthBar(
        dl,
        centerX,
        healthBarAnchorY,
        shieldHeal,
        shieldMax,
        0,
        healthPct,
        z);

  // Hex shield + HP bar extends ~46px above anchor y.
    return healthBarAnchorY - 50.f * z;
}

void Visuals::HealthBar(float min_x, float min_y, float max_y, float health, const EspDrawScale& scale, const ImColor* fillOverride)
{
    const float bar_width = std::max(2.f, 3.5f * scale.espScale);
    const float bar_offset = 2.0f * scale.espScale;
    const float x = min_x - bar_width - bar_offset;
    const float y = min_y - 1.f;
    const float h = (max_y - min_y) + 2.f;

    int chealth = std::clamp((int)health, 0, 100);
    float fraction = chealth / 100.f;
    float filled_height = h * fraction;
    float filled_bottom = y + h;
    float filled_top = filled_bottom - filled_height;

    ImColor color;
    if (fillOverride)
        color = *fillOverride;
    else if (chealth > 75) color = ImColor(46, 204, 113, 255);
    else if (chealth > 50) color = ImColor(241, 196, 15, 255);
    else if (chealth > 25) color = ImColor(230, 126, 34, 255);
    else if (chealth > 10) color = ImColor(231, 76, 60, 255);
    else color = ImColor(192, 57, 43, 255);

    auto vList = ImGui::GetForegroundDrawList();
    vList->AddRectFilled(ImVec2(x, y), ImVec2(x + bar_width, y + h), ImColor(20, 20, 20, 180));
    if (filled_height > 0) {
        if (fillOverride) {
            vList->AddRectFilled(ImVec2(x + 1, filled_top), ImVec2(x + bar_width - 1, filled_bottom), color);
        } else {
        int r_top = int(color.Value.x * 255.0f * 1.17f);
        int g_top = int(color.Value.y * 255.0f * 1.17f);
        int b_top = int(color.Value.z * 255.0f * 1.17f);

        if (r_top > 255) r_top = 255;
        if (g_top > 255) g_top = 255;
        if (b_top > 255) b_top = 255;

        int a_top = int(color.Value.w * 255.0f);

        int r_bottom = int(color.Value.x * 255.0f * 0.7f);
        int g_bottom = int(color.Value.y * 255.0f * 0.7f);
        int b_bottom = int(color.Value.z * 255.0f * 0.7f);
        int a_bottom = int(color.Value.w * 255.0f);

        if (r_bottom > 255) r_bottom = 255;
        if (g_bottom > 255) g_bottom = 255;
        if (b_bottom > 255) b_bottom = 255;

        ImColor top(r_top, g_top, b_top, a_top);
        ImColor bottom(r_bottom, g_bottom, b_bottom, a_bottom);

        vList->AddRectFilledMultiColor(ImVec2(x + 1, filled_top), ImVec2(x + bar_width - 1, filled_bottom), top, top, bottom, bottom);
        }
    }
}

void Visuals::DrawCorneredBox(float X, float Y, float W, float H, const ImU32& color, float thickness)
{
    auto vList = ImGui::GetForegroundDrawList();

    // Crisp anti-aliased rounded rectangle instead of jagged cornered lines
    const float rounding = std::max(2.f, thickness * 1.5f);
    vList->AddRect(
        ImVec2(X, Y),
        ImVec2(X + W, Y + H),
        color,
        rounding,
        ImDrawFlags_None,
        thickness);
}

void Visuals::Box(const Vector3& headScreenPos, const Vector3& feetScreenPos, bool visible, ImU32 color, int boxType, const EspDrawScale& scale)
{
    (void)visible;
    (void)boxType;

    auto BottomPos = feetScreenPos;
    auto TopPos = headScreenPos;

    const float boxHeight = fabsf(static_cast<float>(TopPos.y - BottomPos.y));
    const float boxWidth = boxHeight * 0.65f;

    if (boxHeight < 1.0f || boxWidth < 1.0f)
        return;

    if (TopPos.x != TopPos.x || TopPos.y != TopPos.y ||
        BottomPos.x != BottomPos.x || BottomPos.y != BottomPos.y)
        return;

    const float startPosX = static_cast<float>(TopPos.x - boxWidth / 2.0f);
    const float startPosY = static_cast<float>(TopPos.y);

    // Black outline for crisp contrast against any background
    auto dl = ImGui::GetForegroundDrawList();
    const float outlineW = std::max(1.0f, scale.lineThickness * 0.5f);
    DrawCorneredBox(startPosX - outlineW, startPosY - outlineW, boxWidth + outlineW * 2.f, boxHeight + outlineW * 2.f, IM_COL32(0, 0, 0, 180), outlineW);
    DrawCorneredBox(startPosX, startPosY, boxWidth, boxHeight, color, scale.lineThickness);
}

void Visuals::BoxScreenRect(float x, float y, float w, float h, ImU32 color, const EspDrawScale& scale)
{
    if (w < 1.f || h < 1.f)
        return;
    if (x != x || y != y)
        return;
    // Black outline for crisp contrast against any background
    auto dl = ImGui::GetForegroundDrawList();
    const float outlineW = std::max(1.0f, scale.lineThickness * 0.5f);
    DrawCorneredBox(x - outlineW, y - outlineW, w + outlineW * 2.f, h + outlineW * 2.f, IM_COL32(0, 0, 0, 180), outlineW);
    DrawCorneredBox(x, y, w, h, color, scale.lineThickness);
}
