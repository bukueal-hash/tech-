#include "OverlayHost.h"

#include <cstdio>
#include <dwmapi.h>

namespace {

HWND g_overlayHwnd = nullptr;
UINT* g_resizeW = nullptr;
UINT* g_resizeH = nullptr;
int g_selectedMonitor = 0;

bool QueryMonitorBounds(int index, int* outX, int* outY, int* outW, int* outH)
{
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (!EnumDisplayDevicesW(nullptr, index, &dd, 0))
        return false;
    if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE))
        return false;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
        return false;

    if (outX)
        *outX = dm.dmPosition.x;
    if (outY)
        *outY = dm.dmPosition.y;
    if (outW)
        *outW = static_cast<int>(dm.dmPelsWidth);
    if (outH)
        *outH = static_cast<int>(dm.dmPelsHeight);
    return true;
}

} // namespace

void ApplyOverlayMode(HWND hwnd, bool interactive)
{
    if (!hwnd)
        return;

    static bool s_initialized = false;
    static bool s_lastInteractive = false;
    static bool s_hasApplied = false;

    if (!s_initialized) {
        const MARGINS margins{ -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
        s_initialized = true;
    }

    if (s_hasApplied && interactive == s_lastInteractive)
        return;

    s_hasApplied = true;
    s_lastInteractive = interactive;

    LONG style = GetWindowLong(hwnd, GWL_EXSTYLE);
    style |= WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST;
    if (interactive)
        style &= ~WS_EX_TRANSPARENT;
    else
        style |= WS_EX_TRANSPARENT;

    SetWindowLong(hwnd, GWL_EXSTYLE, style);
    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void OverlayDisplay_Bind(HWND overlayHwnd, UINT* resizeWidth, UINT* resizeHeight)
{
    g_overlayHwnd = overlayHwnd;
    g_resizeW = resizeWidth;
    g_resizeH = resizeHeight;
}

int OverlayDisplay_GetMonitorCount()
{
    int count = 0;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0))
            break;
        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE)
            ++count;
    }
    return count;
}

bool OverlayDisplay_GetMonitorBounds(int index, int* outX, int* outY, int* outW, int* outH)
{
    int active = 0;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0))
            break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE))
            continue;
        if (active == index)
            return QueryMonitorBounds(static_cast<int>(i), outX, outY, outW, outH);
        ++active;
    }
    return false;
}

std::string OverlayDisplay_GetMonitorLabel(int index)
{
    int x = 0, y = 0, w = 0, h = 0;
    if (!OverlayDisplay_GetMonitorBounds(index, &x, &y, &w, &h)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Monitor %d (unavailable)", index + 1);
        return buf;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "Monitor %d — %dx%d @ (%d,%d)", index + 1, w, h, x, y);
    return buf;
}

int OverlayDisplay_GetSelectedMonitor()
{
    return g_selectedMonitor;
}

void OverlayDisplay_SetSelectedMonitor(int index)
{
    if (index < 0)
        index = 0;
    const int count = OverlayDisplay_GetMonitorCount();
    if (count > 0 && index >= count)
        index = count - 1;
    g_selectedMonitor = index;
}

bool OverlayDisplay_ApplySelectedMonitor()
{
    if (!g_overlayHwnd)
        return false;

    int x = 0, y = 0, w = 0, h = 0;
    if (!OverlayDisplay_GetMonitorBounds(g_selectedMonitor, &x, &y, &w, &h))
        return false;

    SetWindowPos(g_overlayHwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (g_resizeW && g_resizeH) {
        *g_resizeW = static_cast<UINT>(w);
        *g_resizeH = static_cast<UINT>(h);
    }

    return true;
}
