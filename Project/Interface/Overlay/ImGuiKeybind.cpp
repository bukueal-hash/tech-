#define WIN32_LEAN_AND_MEAN

#include "ImGuiKeybind.h"

#include "../../Input/InputBind.h"



#include "../../ThirdParty/ImGui/imgui.h"

#include "../../ThirdParty/ImGui/imgui_internal.h"

#include "../Utils/AutoConfig.h"



#include <Windows.h>

#include <cfloat>

#include <map>

#include <vector>



namespace {



const char* const kVkKeyLabels[] = {

    "-", "Mouse 1", "Mouse 2", "CN", "Mouse 3", "Mouse 4", "Mouse 5", "-", "Back", "Tab",

    "-", "-", "CLR", "Enter", "-", "-", "Shift", "CTL", "Menu", "Pause", "Caps Lock",

    "KAN", "-", "JUN", "FIN", "KAN", "-", "Escape", "CON", "NCO", "ACC", "MAD", "Space",

    "PGU", "PGD", "End", "Home", "Left", "Up", "Right", "Down", "SEL", "PRI", "EXE", "PRI",

    "INS", "Delete", "HEL", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", "-", "-",

    "-", "-", "-", "-", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",

    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "WIN", "WIN", "APP",

    "-", "SLE", "Numpad 0", "Numpad 1", "Numpad 2", "Numpad 3", "Numpad 4", "Numpad 5",

    "Numpad 6", "Numpad 7", "Numpad 8", "Numpad 9", "MUL", "ADD", "SEP", "MIN", "Delete",

    "DIV", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13",

    "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "-", "-",

    "-", "-", "-", "-", "-", "-", "NUM", "SCR", "EQU", "MAS", "TOY", "OYA", "OYA", "-", "-",

    "-", "-", "-", "-", "-", "-", "-", "Shift", "Shift", "Ctrl", "Ctrl", "Alt", "Alt",

};



constexpr int kVkKeyLabelCount = static_cast<int>(sizeof(kVkKeyLabels) / sizeof(kVkKeyLabels[0]));



bool IsVkDown(int vk) {

    return (GetAsyncKeyState(vk) & 0x8000) != 0;

}



const char* KeyDisplayName(int key) {

    if (InputBindCodeIsGamepad(key))

        return InputBindCodeLabel(key);

    if (key >= 0 && key < kVkKeyLabelCount)

        return kVkKeyLabels[key];

    return "Unknown";

}



} // namespace



bool ImGui::Keybind(const char* label, int* key) {

    ImGuiWindow* window = ImGui::GetCurrentWindow();

    if (window->SkipItems)

        return false;



    ImGui::PushID(label);

    const ImGuiID id = ImGui::GetID("##keybind_widget");



    static std::map<ImGuiID, bool> s_binding;

    static std::map<ImGuiID, bool> s_waitRelease;

    static std::map<ImGuiID, bool> s_waitingFrame;

    static std::map<ImGuiID, std::vector<bool>> s_prevDown;



    bool& binding = s_binding[id];

    bool& waitRelease = s_waitRelease[id];

    bool& waitingFrame = s_waitingFrame[id];

    std::vector<bool>& prev = s_prevDown[id];

    if (prev.size() < 256)

        prev.resize(256);



    auto snapshotInput = [&]() {

        for (int i = 0; i < 5; i++)

            prev[static_cast<size_t>(0x01 + i)] = IsVkDown(0x01 + i);

        for (int i = 0x08; i <= 0xA5 && i < 256; i++)

            prev[static_cast<size_t>(i)] = IsVkDown(i);

    };



    const float wrapX = ImGui::GetCursorStartPos().x + ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(wrapX);
    ImGui::TextWrapped("%s", label);
    ImGui::PopTextWrapPos();

    ImGui::SetNextItemWidth(-FLT_MIN);
    const float button_w = ImGui::GetContentRegionAvail().x;



    char buf_display[64];

    if (binding) {

        strcpy_s(buf_display, "Press any key... (Esc to clear)");

    } else if (*key != 0) {

        strcpy_s(buf_display, KeyDisplayName(*key));

    } else {

        strcpy_s(buf_display, "[ Click to bind ]");

    }



    bool value_changed = false;



    if (ImGui::Button(buf_display, ImVec2(button_w, 0.f))) {

        binding = true;

        waitRelease = false;

        waitingFrame = false;

        snapshotInput();

    }



    if (binding) {

        ImGui::TextUnformatted("Release mouse, then press key or pad");

        if (!waitRelease) {

            if (!IsVkDown(VK_LBUTTON)) {

                waitRelease = true;

                waitingFrame = true;

                snapshotInput();

            }

        } else if (!waitingFrame) {
            int k = 0;
            for (int i = 0; i < 5 && k == 0; i++) {
                const int vk = 0x01 + i;
                if (IsVkDown(vk) && !prev[static_cast<size_t>(vk)])
                    k = vk;
            }
            if (k == 0) {
                for (int i = 0x08; i <= 0xA5 && i < 256 && k == 0; i++) {
                    if (i == VK_LBUTTON || i == VK_RBUTTON || i == VK_MBUTTON
                        || i == VK_XBUTTON1 || i == VK_XBUTTON2)
                        continue;
                    if (IsVkDown(i) && !prev[static_cast<size_t>(i)])
                        k = i;
                }
            }
            if (k == 0) {
                int gpk = 0;
                if (ImGuiTryBindGamepadKey(&gpk))
                    k = gpk;
            }
            snapshotInput();
            if (k == VK_ESCAPE) {
                *key = 0;
                value_changed = true;
                binding = false;
            } else if (k != 0) {
                *key = k;
                value_changed = true;
                binding = false;
            }
        } else {

            waitingFrame = false;

        }

    }



    ImGui::PopID();

    if (value_changed)
        AutoConfig_MarkDirty();
    return value_changed;
}

