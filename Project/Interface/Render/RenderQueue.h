#pragma once

#include "../../ThirdParty/ImGui/imgui.h"

#include <condition_variable>
#include <mutex>
#include <vector>

struct RenderCommand {
    ImVec2 a{};
    ImVec2 b{};
    ImU32 color = IM_COL32(255, 255, 255, 255);
    float thickness = 1.f;
};

// Double-buffered deferred draw list (Streck g_RenderQueue pattern).
// Live ESP only queues lines (snaplines); other primitives draw via ImGui directly.
class RenderQueue {
public:
    void newFrame();
    void endFrame();

    void addLine(const ImVec2& from, const ImVec2& to, ImU32 color, float thickness);

    std::vector<RenderCommand> takeCommands();

    static void flushToDrawList(ImDrawList* drawList, const std::vector<RenderCommand>& cmds);

private:
    std::vector<RenderCommand> m_buf1;
    std::vector<RenderCommand> m_buf2;
    std::vector<RenderCommand>* m_write = &m_buf1;
    std::vector<RenderCommand>* m_read = &m_buf2;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_ready = false;
};

extern RenderQueue g_renderQueue;
