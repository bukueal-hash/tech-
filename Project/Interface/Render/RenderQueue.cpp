#include "RenderQueue.h"

#include <algorithm>

RenderQueue g_renderQueue;

void RenderQueue::newFrame()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_write->clear();
}

void RenderQueue::endFrame()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::swap(m_write, m_read);
    m_ready = true;
    m_cv.notify_one();
}

void RenderQueue::addLine(const ImVec2& from, const ImVec2& to, ImU32 color, float thickness)
{
    RenderCommand cmd{};
    cmd.type = RenderCmdType::Line;
    cmd.a = from;
    cmd.b = to;
    cmd.color = color;
    cmd.thickness = thickness;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_write->push_back(std::move(cmd));
}

void RenderQueue::addRect(const ImVec2& min, const ImVec2& max, ImU32 color, float thickness)
{
    RenderCommand cmd{};
    cmd.type = RenderCmdType::Rect;
    cmd.a = min;
    cmd.b = max;
    cmd.color = color;
    cmd.thickness = thickness;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_write->push_back(std::move(cmd));
}

void RenderQueue::addRectFilled(const ImVec2& min, const ImVec2& max, ImU32 color)
{
    RenderCommand cmd{};
    cmd.type = RenderCmdType::RectFilled;
    cmd.a = min;
    cmd.b = max;
    cmd.color = color;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_write->push_back(std::move(cmd));
}

void RenderQueue::addCircle(
    const ImVec2& center,
    float radius,
    ImU32 color,
    int segments,
    float thickness)
{
    RenderCommand cmd{};
    cmd.type = RenderCmdType::Circle;
    cmd.a = center;
    cmd.radius = radius;
    cmd.color = color;
    cmd.segments = segments;
    cmd.thickness = thickness;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_write->push_back(std::move(cmd));
}

void RenderQueue::addCircleFilled(
    const ImVec2& center,
    float radius,
    ImU32 color,
    int segments)
{
    RenderCommand cmd{};
    cmd.type = RenderCmdType::CircleFilled;
    cmd.a = center;
    cmd.radius = radius;
    cmd.color = color;
    cmd.segments = segments;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_write->push_back(std::move(cmd));
}

void RenderQueue::addText(
    float x,
    float y,
    ImU32 color,
    const char* text,
    float fontSize,
    bool centerX)
{
    if (!text || !text[0])
        return;
    RenderCommand cmd{};
    cmd.type = RenderCmdType::Text;
    cmd.a = ImVec2(x, y);
    cmd.color = color;
    cmd.text = text;
    cmd.fontSize = fontSize;
    cmd.centerX = centerX;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_write->push_back(std::move(cmd));
}

std::vector<RenderCommand> RenderQueue::takeCommands()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::move(*m_read);
}

void RenderQueue::flushToDrawList(ImDrawList* drawList, const std::vector<RenderCommand>& cmds)
{
    if (!drawList)
        return;

    ImFont* font = ImGui::GetFont();
    for (const RenderCommand& cmd : cmds) {
        switch (cmd.type) {
        case RenderCmdType::Line:
            drawList->AddLine(cmd.a, cmd.b, cmd.color, cmd.thickness);
            break;
        case RenderCmdType::Rect:
            drawList->AddRect(cmd.a, cmd.b, cmd.color, 0.f, 0, cmd.thickness);
            break;
        case RenderCmdType::RectFilled:
            drawList->AddRectFilled(cmd.a, cmd.b, cmd.color);
            break;
        case RenderCmdType::Circle:
            drawList->AddCircle(cmd.a, cmd.radius, cmd.color, cmd.segments, cmd.thickness);
            break;
        case RenderCmdType::CircleFilled:
            drawList->AddCircleFilled(cmd.a, cmd.radius, cmd.color, cmd.segments);
            break;
        case RenderCmdType::Text: {
            ImVec2 pos = cmd.a;
            if (cmd.centerX && font && cmd.fontSize > 0.f) {
                const ImVec2 sz = font->CalcTextSizeA(cmd.fontSize, FLT_MAX, 0.f, cmd.text.c_str());
                pos.x -= sz.x * 0.5f;
            }
            if (font && cmd.fontSize > 0.f)
                drawList->AddText(font, cmd.fontSize, pos, cmd.color, cmd.text.c_str());
            else
                drawList->AddText(pos, cmd.color, cmd.text.c_str());
            break;
        }
        default:
            break;
        }
    }
}
