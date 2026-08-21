#include "Dumper.h"

#include "DumperState.h"
#include "DumperWorker.h"

#include "../DMA/Memory.h"
#include "../Interface/Overlay/MenuLayout.h"
#include "../ThirdParty/ImGui/imgui.h"

#include <string>

namespace Dumper {

namespace {

const char* StatusLabel(DumpStatus s)
{
    switch (s) {
    case DumpStatus::Idle: return "Idle";
    case DumpStatus::Running: return "Running";
    case DumpStatus::Done: return "Done";
    case DumpStatus::Error: return "Error";
    case DumpStatus::Cancelled: return "Cancelled";
    }
    return "?";
}

} // namespace

void DrawHelpDumperTab()
{
    auto& state = DumperState::Instance();
    const DumpStatus status = state.GetStatus();
    const bool dmaOk = g_mem.GetBase() != 0;
    const float wrapX = ArcMenuLayout::ContentWrapX();

    ImGui::PushTextWrapPos(wrapX);

    if (!dmaOk)
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "DMA not attached — attach to PioneerGame first");
    else
        ImGui::TextColored(ImVec4(0.3f, 1.f, 0.3f, 1.f), "DMA attached  base=0x%llX  size=0x%X",
            static_cast<unsigned long long>(g_mem.GetBase()), g_mem.GetImageSize());

    ImGui::TextWrapped("Status: %s  (%.1fs)  Progress: %d%%",
        StatusLabel(status), state.GetElapsedSeconds(), state.GetProgress());
    ImGui::ProgressBar(state.GetProgress() / 100.f, ImVec2(-1.f, 0.f));

    const bool running = (status == DumpStatus::Running);
    ImGui::BeginDisabled(!dmaOk || running);
    if (ImGui::Button("Scan Globals"))
        StartDump(DumpMode::GlobalsOnly);
    ImGui::SameLine();
    if (ImGui::Button("Full SDK Dump"))
        StartDump(DumpMode::FullSdk);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Cancel"))
        state.RequestCancel();
    ImGui::EndDisabled();

    const std::string err = state.GetError();
    if (!err.empty())
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Error: %s", err.c_str());

    ImGui::Separator();
    ImGui::TextWrapped("Output: help/globals.json, help/dump_report.txt, help/dump.txt (full dump only; help/sdk.txt is manual reference)");
    ImGui::TextWrapped("Logs: help/dumper_log.txt (dumper), help/session_log.txt (debug + console)");
    ImGui::TextWrapped("v1 writes files only — manually update Offsets.h / SteamDecrypt.hpp after validating.");

    ImGui::PopTextWrapPos();

    ImGui::Separator();
    if (ImGui::BeginChild("##dumper_log", ImVec2(0.f, 180.f), true)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        const auto lines = state.GetLogSnapshot();
        const size_t start = lines.size() > 40 ? lines.size() - 40 : 0;
        for (size_t i = start; i < lines.size(); ++i)
            ImGui::TextWrapped("%s", lines[i].c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();

    const auto globals = state.GetGlobals();
    if (!globals.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("Last globals:");
        if (ImGui::BeginTable("##dumper_globals",
                3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("RVA", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("Confidence", ImGuiTableColumnFlags_WidthStretch, 0.30f);
            ImGui::TableHeadersRow();
            for (const auto& g : globals) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextWrapped("%s", g.name.c_str());
                ImGui::TableSetColumnIndex(1);
                if (g.rva)
                    ImGui::TextWrapped("0x%llX", static_cast<unsigned long long>(g.rva));
                else
                    ImGui::TextWrapped("—");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", g.confidence.c_str());
            }
            ImGui::EndTable();
        }
    }
}

} // namespace Dumper
