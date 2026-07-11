#pragma once

#include <cstdint>

/** While binding in ImGui::Keybind: if a controller button is held, write synthetic code and return true. */
bool ImGuiTryBindGamepadKey(int* outCode);

/** Canonical gamepad codes: 256–271. Legacy 115–127 / 243–246 still load but map to the same pads. */
bool InputBindCodeIsGamepad(int code);
uint32_t InputBindCodeToControllerMask(int code);
bool InputBindIsDown(int code);
bool InputBindIsEitherDown(int primaryCode, int secondaryCode);
const char* InputBindCodeLabel(int code);
/** Rewrite legacy overlapping pad codes to canonical 256+ for save/display. */
int InputBindNormalizeBindCode(int code);
