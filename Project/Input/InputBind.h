#pragma once

#include <cstdint>

/** While binding in ImGui::Keybind: if a controller button is held, write synthetic code and return true. */
bool ImGuiTryBindGamepadKey(int* outCode);

/** Canonical gamepad codes: 256–271. Legacy 115–127 / 243–246 still load but map to the same pads. */
bool InputBindCodeIsGamepad(int code);
bool InputBindIsDown(int code);
const char* InputBindCodeLabel(int code);
