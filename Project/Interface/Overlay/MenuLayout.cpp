#include "MenuLayout.h"

#include "../Utils/AutoConfig.h"

#include <cfloat>
#include <cstdarg>

namespace ArcMenuLayout {

namespace {

// Subtle highlight under any text the mouse is over, so plain labels respond
// to hover the same way buttons do.
void HighlightHoveredItem()
{
    if (ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
            IM_COL32(255, 255, 255, 22));
    }
}

// Screen rect of the most recently drawn Label(). Consumed by
// ArcMenuHoverTooltip() so the row tooltip also fires when the mouse is over
// the label text instead of only the control itself.
bool g_labelRectValid = false;
ImVec2 g_labelRectMin(0.f, 0.f);
ImVec2 g_labelRectMax(0.f, 0.f);

} // namespace

float ContentWrapX()
{
    return ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x;
}

void Label(const char* text)
{
    ImGui::PushTextWrapPos(ContentWrapX());
    ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();

    g_labelRectValid = true;
    g_labelRectMin = ImGui::GetItemRectMin();
    g_labelRectMax = ImGui::GetItemRectMax();
    HighlightHoveredItem();
}

void HoverableText(const char* text)
{
    ImGui::PushTextWrapPos(ContentWrapX());
    ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();
    HighlightHoveredItem();
}

void HoverableTextF(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::PushTextWrapPos(ContentWrapX());
    ImGui::TextV(fmt, args);
    ImGui::PopTextWrapPos();
    va_end(args);
    HighlightHoveredItem();
}

void HoverableTextColoredF(const ImVec4& color, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(ContentWrapX());
    ImGui::TextV(fmt, args);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    va_end(args);
    HighlightHoveredItem();
}

bool LastLabelRect(ImVec2* outMin, ImVec2* outMax)
{
    const bool ok = g_labelRectValid;
    if (ok) {
        if (outMin)
            *outMin = g_labelRectMin;
        if (outMax)
            *outMax = g_labelRectMax;
    }
    g_labelRectValid = false; // consumed by the row tooltip
    return ok;
}

void ResetHoverState()
{
    g_labelRectValid = false;
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
    HighlightHoveredItem();
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
    HighlightHoveredItem();
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
