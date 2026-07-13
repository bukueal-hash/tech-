#pragma once
#include "../../../Core/Engine.h"
#include "../../../ThirdParty/ImGui/imgui.h"
#include "../../../ThirdParty/ImGui/imgui_internal.h"

namespace Visuals {

struct EspDrawScale {
    float espScale = 1.f;
    float textPx = 14.f;
    float lineThickness = 1.f;
};

/** Settings slider scale for ESP labels (0.5–3.0). */
float EspUserTextScale();

/** Bukupex-style min(1, 30/d) with far-distance floor (~0.55 at 250m). */
float EspDistanceScale(float distanceMeters);

float TextScaleFromDistance(float distanceMeters);
/** Pixel font size for distance-scaled ESP labels. */
float LabelTextPx(float distanceMeters);
void DrawScaledLabel(
    ImDrawList* drawList,
    const char* text,
    float anchorX,
    float anchorY,
    ImU32 color,
    float distanceMeters,
    bool centerX = true,
    bool aboveAnchor = true);
void Headline(int width, int height, Vector2 target, int distance);

EspDrawScale ComputeEspScaleFromBox(float boxHeightPx, float distanceMeters = 0.f);
/** Perspective player height in px: screenH * 180 / (distM * 100), clamped 4–400. */
float EstimateBoxHeightPx(float distanceMeters, float screenH);
/** Same grow/shrink as enemies: box height when known, else distance-based estimate. */
EspDrawScale ComputeEspScaleUnified(float boxHeightPx, float distanceMeters, float screenHeightPx);
EspDrawScale ComputeEspScaleFromDistance(float distanceMeters);
EspDrawScale ComputeEspScaleFromLootMarker(float markerScreenHeightPx);

void DrawScaledText(const char* text, float centerX, float y, const EspDrawScale& scale, ImU32 color);
ImVec2 CalcScaledTextSize(const char* text, float textPx);

void SnapLinesDouble(const Vector3& start, const Vector3& end, ImColor color, float thickness);

struct HumanSilhouetteInput {
    ImVec2 head{};
    ImVec2 neck{};
    ImVec2 chest{};
    ImVec2 waist{};
    ImVec2 pelvis{};
    ImVec2 clavicleL{};
    ImVec2 clavicleR{};
    ImVec2 upperArmL{};
    ImVec2 lowerArmL{};
    ImVec2 handL{};
    ImVec2 upperArmR{};
    ImVec2 lowerArmR{};
    ImVec2 handR{};
    ImVec2 thighL{};
    ImVec2 calfL{};
    ImVec2 footL{};
    ImVec2 thighR{};
    ImVec2 calfR{};
    ImVec2 footR{};
    bool hasHead = false;
    bool hasNeck = false;
    bool hasChest = false;
    bool hasPelvis = false;
    bool hasClavicleL = false;
    bool hasClavicleR = false;
    bool hasUpperArmL = false;
    bool hasLowerArmL = false;
    bool hasHandL = false;
    bool hasUpperArmR = false;
    bool hasLowerArmR = false;
    bool hasHandR = false;
    bool hasThighL = false;
    bool hasCalfL = false;
    bool hasFootL = false;
    bool hasThighR = false;
    bool hasCalfR = false;
    bool hasFootR = false;
    /** True only when upper+lower arm both projected (hand optional). */
    bool hasArmL = false;
    bool hasArmR = false;
    /** True only when thigh+calf both projected (foot optional). */
    bool hasLegL = false;
    bool hasLegR = false;
};

/** Single filled human body outline from bone screen positions. */
void DrawHumanSilhouetteFilled(ImDrawList* dl, const HumanSilhouetteInput& in, ImU32 fill);
void Names(const std::string& name, float center_x, float top_y, const EspDrawScale& scale, ImColor color);
void HealthBar(float min_x, float min_y, float max_y, float health, const EspDrawScale& scale, const ImColor* fillOverride = nullptr);
ImColor HealthColorFromPct(float hpPct);
void DrawHealthBar(
    ImDrawList* dl,
    float x,
    float y,
    int shield,
    int maxShield,
    int armorType,
    int health,
    float scale,
    const char* overlayLine1 = nullptr,
    const char* overlayLine2 = nullptr,
    bool overlay1BlackCenteredInHp = false,
    float overlayTextScale = 1.f,
    const ImVec4* overlayTextColor = nullptr);

/** HP + shield bar above head. Returns name anchor Y. */
float HealthShieldBarsAboveHead(
    float centerX,
    float boxTop,
    float boxWidth,
    float health,
    float maxHealth,
    float shield,
    float maxShield,
    const EspDrawScale& scale,
    ImDrawList* drawList = nullptr);
void Box(const Vector3& headScreenPos, const Vector3& feetScreenPos, bool visible, ImU32 color, int boxType, const EspDrawScale& scale);
void BoxScreenRect(float x, float y, float w, float h, ImU32 color, const EspDrawScale& scale);
void DrawCorneredBox(float X, float Y, float W, float H, const ImU32& color, float thickness);
void FovCircle(float aimbotFovDegrees, float gameFovDegrees);
float AimbotFovRadiusPx(float aimbotFovDegrees, float gameFovDegrees, float screenW);
}
