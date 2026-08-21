#include "MenuLayout.h"

#include "../Utils/AutoConfig.h"

#include <cfloat>

namespace ArcMenuLayout {

float ContentWrapX()
{
    return ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x;
}

void Label(const char* text)
{
    ImGui::PushTextWrapPos(ContentWrapX());
    ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();
}

void PushControlWidth()
{
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool SliderFloat(
    const char* label,
    const char* id,
    float* value,
    float vMin,
    float vMax,
    const char* fmt)
{
    Label(label);
    PushControlWidth();
    const bool changed = ImGui::SliderFloat(id, value, vMin, vMax, fmt);
    if (changed)
        AutoConfig_MarkDirty();
    return changed;
}

bool Combo(
    const char* label,
    const char* id,
    int* current,
    const char* const* items,
    int count)
{
    Label(label);
    PushControlWidth();
    const bool changed = ImGui::Combo(id, current, items, count);
    if (changed)
        AutoConfig_MarkDirty();
    return changed;
}

bool Checkbox(const char* label, bool* value)
{
    const bool changed = ImGui::Checkbox(label, value);
    if (changed)
        AutoConfig_MarkDirty();
    return changed;
}

static bool ColorEditAtColumnPos(const char* colorId, float color[4])
{
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + kColorColumnX);
    const bool changed = ImGui::ColorEdit4(colorId, color, kColorFlags);
    if (changed)
        AutoConfig_MarkDirty();
    return changed;
}

static void DualColorAtColumnPos(float colorA[4], const char* idA, float colorB[4], const char* idB)
{
    ColorEditAtColumnPos(idA, colorA);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorStartPos().x + kColorColumnX + kColorSwatchWidth + 8.f);
    ImGui::ColorEdit4(idB, colorB, kColorFlags);
}

bool ColorEditAtColumn(const char* colorId, float color[4])
{
    ImGui::SameLine();
    return ColorEditAtColumnPos(colorId, color);
}

bool CheckboxWithColorRow(const char* label, bool* enabled, float color[4], const char* colorId)
{
    const float startX = ImGui::GetCursorStartPos().x;
    ImGui::PushID(colorId);
    bool changed = false;
    if (ImGui::Checkbox("##cb", enabled))
        changed = true;
    ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetCursorPosX(startX + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushTextWrapPos(startX + kColorColumnX - 2.f);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    if (ColorEditAtColumn(colorId, color))
        changed = true;
    ImGui::PopID();
    return changed;
}

bool CheckboxWithDualColorRow(
    const char* label,
    bool* enabled,
    float colorA[4],
    const char* idA,
    float colorB[4],
    const char* idB)
{
    const float startX = ImGui::GetCursorStartPos().x;
    ImGui::PushID(idA);
    bool changed = false;
    if (ImGui::Checkbox("##cb", enabled))
        changed = true;
    ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetCursorPosX(startX + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushTextWrapPos(startX + kColorColumnX - 2.f);
    ImGui::TextUnformatted(label);
    ImGui::PopTextWrapPos();
    ImGui::SameLine();
    if (ColorEditAtColumnPos(idA, colorA))
        changed = true;
    ImGui::SameLine();
    ImGui::SetCursorPosX(startX + kColorColumnX + kColorSwatchWidth + 8.f);
    if (ImGui::ColorEdit4(idB, colorB, kColorFlags)) {
        changed = true;
        AutoConfig_MarkDirty();
    }
    ImGui::PopID();
    return changed;
}

bool BeginCombo(const char* label, const char* id, const char* previewValue)
{
    Label(label);
    PushControlWidth();
    return ImGui::BeginCombo(id, previewValue);
}

bool InputText(const char* label, const char* id, char* buf, int bufSize, ImGuiInputTextFlags flags)
{
    Label(label);
    PushControlWidth();
    return ImGui::InputText(id, buf, static_cast<size_t>(bufSize), flags);
}

} // namespace ArcMenuLayout
