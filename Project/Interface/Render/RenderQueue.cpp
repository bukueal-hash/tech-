#include "RenderQueue.h"

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
    cmd.a = from;
    cmd.b = to;
    cmd.color = color;
    cmd.thickness = thickness;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_write->push_back(cmd);
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

    for (const RenderCommand& cmd : cmds)
        drawList->AddLine(cmd.a, cmd.b, cmd.color, cmd.thickness);
}
