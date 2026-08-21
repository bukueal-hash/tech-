#pragma once

#include "../../ThirdParty/ImGui/imgui.h"

namespace ArcMenuLayout {

constexpr float kLabelColumnWidth = 152.f;
constexpr float kColorSwatchWidth = 32.f;
constexpr float kColorColumnX = kLabelColumnWidth;
constexpr float kContainerSpColumnX = kColorColumnX + kColorSwatchWidth + 14.f;
constexpr ImGuiColorEditFlags kColorFlags =
    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;

float ContentWrapX();
void Label(const char* text);
void PushControlWidth();
bool SliderFloat(
    const char* label,
    const char* id,
    float* value,
    float vMin,
    float vMax,
    const char* fmt);
bool Combo(
    const char* label,
    const char* id,
    int* current,
    const char* const* items,
    int count);
bool Checkbox(const char* label, bool* value);
bool ColorEditAtColumn(const char* colorId, float color[4]);
bool CheckboxWithColorRow(const char* label, bool* enabled, float color[4], const char* colorId);
bool CheckboxWithDualColorRow(
    const char* label,
    bool* enabled,
    float colorA[4],
    const char* idA,
    float colorB[4],
    const char* idB);
bool BeginCombo(const char* label, const char* id, const char* previewValue);
bool InputText(const char* label, const char* id, char* buf, int bufSize, ImGuiInputTextFlags flags = 0);

} // namespace ArcMenuLayout
