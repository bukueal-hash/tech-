#include "visuals.hpp"

void Visuals::SnapLines(const Vector2& start, const Vector2& end, ImU32 color, float thickness)
{
    ImGui::GetForegroundDrawList()->AddLine(ImVec2(start.x, start.y), ImVec2(end.x, end.y), color, thickness);
}

void Visuals::SnapLinesDouble(const Vector3& start, const Vector3& end, ImColor color, float thickness)
{
    auto convertedcolor = color;
    ImGui::GetForegroundDrawList()->AddLine(ImVec2(start.x, start.y), ImVec2(end.x, end.y), convertedcolor, thickness);
}

auto DrawLine(const ImVec2& aPoint1, const ImVec2 aPoint2, ImU32 aColor, const FLOAT aLineWidth) -> VOID
{
    auto vList = ImGui::GetForegroundDrawList();
    vList->AddLine(aPoint1, aPoint2, aColor, aLineWidth);
}

void Visuals::Names(const std::string& name, float center_x, float top_y, float boxWidth, ImColor color)
{
    ImVec2 text_size = ImGui::CalcTextSize(name.c_str());

    float maxWidth = boxWidth * 0.95f;
    float scale = 1.0f;
    if (text_size.x > maxWidth)
        scale = maxWidth / text_size.x;

    float text_x = center_x - (text_size.x * scale) * 0.5f;
    float text_y = top_y - (text_size.y * scale) - 4.0f;

    ImGui::GetForegroundDrawList()->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize() * scale,
        ImVec2(text_x, text_y),
        color,
        name.c_str()
    );
}

void Visuals::HealthBar(float min_x, float min_y, float max_y, int health)
{
    const float bar_width = 3.5f;
    const float bar_offset = 2.0f;
    const float x = min_x - bar_width - bar_offset;
    const float y = min_y - 1;
    const float h = (max_y - min_y) + 2;

    int chealth = std::clamp(health, 0, 100);
    float fraction = chealth / 100.f;
    float filled_height = h * fraction;
    float filled_bottom = y + h;
    float filled_top = filled_bottom - filled_height;

    ImColor color;
    if (chealth > 75) color = ImColor(46, 204, 113, 255);
    else if (chealth > 50) color = ImColor(241, 196, 15, 255);
    else if (chealth > 25) color = ImColor(230, 126, 34, 255);
    else if (chealth > 10) color = ImColor(231, 76, 60, 255);
    else color = ImColor(192, 57, 43, 255);

    auto vList = ImGui::GetForegroundDrawList();
    vList->AddRectFilled(ImVec2(x, y), ImVec2(x + bar_width, y + h), ImColor(20, 20, 20, 180));
    if (filled_height > 0) {
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

inline void DrawFilledGradientBox(float x, float y, float w, float h, ImColor color)
{
    ImVec4 c = color.Value;
    ImColor top = ImColor(c.x * 0.8f, c.y * 0.8f, c.z * 0.8f, 0.18f);
    ImColor bottom = ImColor(c.x, c.y, c.z, 0.40f);
    ImDrawList* vList = ImGui::GetForegroundDrawList();

    ImVec2 topLeft = ImVec2(x + 1, y + 1);
    ImVec2 bottomRight = ImVec2(x + w - 1, y + h - 1);

    vList->AddRectFilledMultiColor(topLeft, bottomRight, top, top, bottom, bottom);

    vList->AddRect(ImVec2(x + 1, y + 1), ImVec2(x + w - 1, y + h - 1),
        ImColor(c.x * 1.3f, c.y * 1.3f, c.z * 1.3f, 0.09f), 0.0f, 0, 1.1f);

    vList->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ImColor(0, 0, 0, 230), 0.0f, 0, 2.0f);

    vList->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), color, 0.0f, 0, 1.1f);
}

auto Visuals::DrawCorneredBox(float X, float Y, float W, float H, const ImU32& color, float thickness) -> VOID
{
    auto vList = ImGui::GetForegroundDrawList();

    float lineW = (W / 3);
    float lineH = (H / 3);
    auto col = ImGui::GetColorU32(color);

    vList->AddLine(ImVec2(X, Y - thickness / 2), ImVec2(X, Y + lineH), col, thickness);//top left
    vList->AddLine(ImVec2(X - thickness / 2, Y), ImVec2(X + lineW, Y), col, thickness);

    vList->AddLine(ImVec2(X + W - lineW, Y), ImVec2(X + W + thickness / 2, Y), col, thickness);//top right horizontal
    vList->AddLine(ImVec2(X + W, Y - thickness / 2), ImVec2(X + W, Y + lineH), col, thickness);

    vList->AddLine(ImVec2(X, Y + H - lineH), ImVec2(X, Y + H + (thickness / 2)), col, thickness);//bot left
    vList->AddLine(ImVec2(X - thickness / 2, Y + H), ImVec2(X + lineW, Y + H), col, thickness);

    vList->AddLine(ImVec2(X + W - lineW, Y + H), ImVec2(X + W + thickness / 2, Y + H), col, thickness);//bot right
    vList->AddLine(ImVec2(X + W, Y + H - lineH), ImVec2(X + W, Y + H + (thickness / 2)), col, thickness);
}

auto Visuals::DrawRectangleBox(float X, float Y, float W, float H, const ImU32& color, float thickness) -> VOID
{
    auto vList = ImGui::GetForegroundDrawList();

    float shadowOffset = 1.0f;
    ImU32 shadowCol = IM_COL32(0, 0, 0, 128);

    ImU32 col = ImGui::GetColorU32(color);

    float lineW = (W);
    float lineH = (H);
    float shadowThickness = thickness + shadowOffset;

    vList->AddLine(ImVec2(X, Y - shadowThickness / 2), ImVec2(X, Y + lineH), shadowCol, shadowThickness);
    vList->AddLine(ImVec2(X - shadowThickness / 2, Y), ImVec2(X + lineW, Y), shadowCol, shadowThickness);

    vList->AddLine(ImVec2(X + W - lineW, Y), ImVec2(X + W + shadowThickness / 2, Y), shadowCol, shadowThickness);
    vList->AddLine(ImVec2(X + W, Y - shadowThickness / 2), ImVec2(X + W, Y + lineH), shadowCol, shadowThickness);

    vList->AddLine(ImVec2(X, Y + H - lineH), ImVec2(X, Y + H + shadowThickness / 2), shadowCol, shadowThickness);
    vList->AddLine(ImVec2(X - shadowThickness / 2, Y + H), ImVec2(X + lineW, Y + H), shadowCol, shadowThickness);

    vList->AddLine(ImVec2(X + W - lineW, Y + H), ImVec2(X + W + shadowThickness / 2, Y + H), shadowCol, shadowThickness);
    vList->AddLine(ImVec2(X + W, Y + H - lineH), ImVec2(X + W, Y + H + shadowThickness / 2), shadowCol, shadowThickness);

    vList->AddLine(ImVec2(X, Y - thickness / 2), ImVec2(X, Y + lineH), col, thickness);
    vList->AddLine(ImVec2(X - thickness / 2, Y), ImVec2(X + lineW, Y), col, thickness);

    vList->AddLine(ImVec2(X + W - lineW, Y), ImVec2(X + W + thickness / 2, Y), col, thickness);
    vList->AddLine(ImVec2(X + W, Y - thickness / 2), ImVec2(X + W, Y + lineH), col, thickness);

    vList->AddLine(ImVec2(X, Y + H - lineH), ImVec2(X, Y + H + thickness / 2), col, thickness);
    vList->AddLine(ImVec2(X - thickness / 2, Y + H), ImVec2(X + lineW, Y + H), col, thickness);

    vList->AddLine(ImVec2(X + W - lineW, Y + H), ImVec2(X + W + thickness / 2, Y + H), col, thickness);
    vList->AddLine(ImVec2(X + W, Y + H - lineH), ImVec2(X + W, Y + H + (thickness / 2)), col, thickness);
}

void Visuals::Box(const Vector3& headScreenPos, const Vector3& feetScreenPos, bool visible, ImU32 color, int boxType)
{
    auto BottomPos = feetScreenPos;
    auto TopPos = headScreenPos;

    float boxHeight = fabsf(TopPos.y - BottomPos.y);
    float boxWidth = boxHeight * 0.65f;

    float startPosX = TopPos.x - boxWidth / 2.0f;
    float startPosY = TopPos.y;
    float endPosX = TopPos.x + boxWidth / 2.0f;
    float endPosY = BottomPos.y;

    boxType == 1 ? DrawCorneredBox(startPosX, startPosY, boxWidth, boxHeight, color, 1.0f) : DrawFilledGradientBox(startPosX, startPosY, boxWidth, boxHeight, color);
}

void Visuals::Headline(int width, int height, Vector2 target, int distance)
{
    auto vList = ImGui::GetForegroundDrawList();
    const auto start = ImVec2(width * 0.5f, height * 0.5f);
    const auto end = ImVec2(target.x, target.y);

    auto radius = 28.0f / std::powf(distance + 1, 0.75f);
    radius = std::clamp(radius, 2.0f, 8.0f);

    ImColor color;
    if (distance < 5)
    {
        color = ImColor(255, 100, 100, 255);
    }
    else if (distance < 10)
    {
        color = ImColor(255, 140, 80, 255);
    }
    else if (distance < 25)
    {
        color = ImColor(255, 200, 60, 255);
    }
    else if (distance < 50)
    {
        color = ImColor(255, 255, 255, 255);
    }
    else if (distance < 100)
    {
        color = ImColor(200, 200, 200, 220);
    }
    else
    {
        color = ImColor(150, 150, 150, 180);
    }

    const auto thickness = distance < 25 ? 1.5f : 1.0f;
    vList->AddLine(start, end, color, thickness);
    vList->AddCircleFilled(ImVec2(target.x, target.y), radius, color, 32);
    vList->AddCircle(ImVec2(target.x, target.y), radius, ImColor(0, 0, 0, 200), 32, 1.5f);
}