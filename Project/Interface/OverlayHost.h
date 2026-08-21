#pragma once

#include <Windows.h>
#include <string>

/** Transparent ESP overlay — clickable when menu or an overlay HUD panel needs mouse. */
void ApplyOverlayMode(HWND hwnd, bool interactive);

/** Bind overlay HWND and optional MSBuild resize triggers (Project.cpp). */
void OverlayDisplay_Bind(HWND overlayHwnd, UINT* resizeWidth, UINT* resizeHeight);

int OverlayDisplay_GetMonitorCount();
bool OverlayDisplay_GetMonitorBounds(int index, int* outX, int* outY, int* outW, int* outH);
std::string OverlayDisplay_GetMonitorLabel(int index);

int OverlayDisplay_GetSelectedMonitor();
void OverlayDisplay_SetSelectedMonitor(int index);
bool OverlayDisplay_ApplySelectedMonitor();
