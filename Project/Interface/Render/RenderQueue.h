#pragma once

#include "../../ThirdParty/ImGui/imgui.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

enum class RenderCmdType : uint8_t {
    Line,
    Rect,
    RectFilled,
    Circle,
    CircleFilled,
    Text,
};

struct RenderCommand {
    RenderCmdType type = RenderCmdType::Line;
    ImVec2 a{};
    ImVec2 b{};
    ImU32 color = IM_COL32(255, 255, 255, 255);
    float thickness = 1.f;
    float radius = 0.f;
    int segments = 12;
    std::string text;
    float fontSize = 0.f;
    bool centerX = false;
};

// Double-buffered deferred draw list (Streck g_RenderQueue pattern).
class RenderQueue {
public:
    void newFrame();
    void endFrame();

    void addLine(const ImVec2& from, const ImVec2& to, ImU32 color, float thickness);
    void addRect(const ImVec2& min, const ImVec2& max, ImU32 color, float thickness);
    void addRectFilled(const ImVec2& min, const ImVec2& max, ImU32 color);
    void addCircle(const ImVec2& center, float radius, ImU32 color, int segments, float thickness);
    void addCircleFilled(const ImVec2& center, float radius, ImU32 color, int segments);
    void addText(float x, float y, ImU32 color, const char* text, float fontSize, bool centerX);

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
