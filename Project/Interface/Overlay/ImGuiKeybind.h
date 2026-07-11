#pragma once



namespace ImGui {

/** Keybind widget (wait LMB release, edge KB, level pad). */

bool Keybind(const char* label, int* key, int* mode = nullptr, bool enablemode = false);

}

