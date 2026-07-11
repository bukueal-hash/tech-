#include "../../../Core/Engine.h"
#include "../../Requeriments/imgui.h"
#include "../../Requeriments/imgui_internal.h"
#include <map>
#include <chrono>

namespace Visuals {
    void SnapLinesDouble(const Vector3& start, const Vector3& end, ImColor color, float thickness);
    void SnapLines(const Vector2& start, const Vector2& end, ImU32 color, float thickness = 0.1f);
    void Names(const std::string& name, float center_x, float top_y, float boxWidth, ImColor color);
    void HealthBar(float min_x, float min_y, float max_y, int health);
    void Box(const Vector3& headScreenPos, const Vector3& feetScreenPos, bool visible, ImU32 color, int boxType);
    void Headline(int width, int height, Vector2 target, int distance);
    void DrawCorneredBox(float X, float Y, float W, float H, const ImU32& color, float thickness);
    void DrawRectangleBox(float X, float Y, float W, float H, const ImU32& color, float thickness);
}