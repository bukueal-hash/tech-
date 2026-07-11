#pragma once

#include <Windows.h>
#include <string>

/** Transparent ESP overlay — toggles click-through vs menu interaction. */
void ApplyOverlayMode(HWND hwnd, bool menuVisible);

/** Bind overlay HWND and optional MSBuild resize triggers (Project.cpp). */
void OverlayDisplay_Bind(HWND overlayHwnd, UINT* resizeWidth, UINT* resizeHeight);

int OverlayDisplay_GetMonitorCount();
bool OverlayDisplay_GetMonitorBounds(int index, int* outX, int* outY, int* outW, int* outH);
std::string OverlayDisplay_GetMonitorLabel(int index);

int OverlayDisplay_GetSelectedMonitor();
void OverlayDisplay_SetSelectedMonitor(int index);
bool OverlayDisplay_ApplySelectedMonitor();
