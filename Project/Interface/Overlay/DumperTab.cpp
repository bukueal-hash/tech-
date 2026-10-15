#include "Menu.h"
#include "MenuLayout.h"
#include "../Dumper/Dumper.h"
#include "../../ThirdParty/ImGui/imgui.h"

namespace arc_ui {

void DrawArcDumperTab()
{
    if (ImGui::Button("Run SDK Dump")) {
        Dumper::ClearLog();
        Dumper::RunDump();
    }

    const std::string& output = Dumper::GetDumpOutput();
    if (!output.empty()) {
        ImGui::InputTextMultiline("##dump_output", (char*)output.c_str(), output.size(), ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        
        // Auto-scroll to bottom if dumping
        if (Dumper::IsDumping() && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    } else if (Dumper::IsDumping()) {
        ImGui::Text("Dumping...");
    }
}

}