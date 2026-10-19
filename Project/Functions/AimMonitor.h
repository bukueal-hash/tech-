#pragma once
// Live aim-assist monitor: a snapshot of the aim tick for the Debug tab and
// the optional on-screen panel, plus the rolling in-memory copy of the aim
// status lines (the same text Build/aim_debug.log gets).
#include <cstdint>

namespace AimMonitor {

struct Live {
    bool keyheld = false;      // aim key currently held
    bool enable = false;       // Enable Aimbot master switch
    bool kmbox = false;        // hardware mouse initialized
    bool raid = false;         // raid active
    bool menuOpen = false;     // overlay menu open (aim paused)
    bool tickRan = false;      // RunAimAssist ran at least once this session
    uint64_t tickAgeMs = 0;    // ms since the last aim tick
    int moveCount = 0;         // moves sent to the device this session
    int moveOk = 0;            // of those, accepted by the device
    bool lastMoveOk = false;
    double errPx = -1.0;       // pixel error at the last pick (-1 = none)
    int lastDx = 0;
    int lastDy = 0;
    bool hasLastMove = false;
    uint64_t lastMoveAgeMs = 0;
    bool makcuConnected = false;
    char status[200] = {};     // latest status line
};

// Fills `out` from the live aim statics. Cheap; safe to call every frame.
void Snapshot(Live& out);

// Copies the newest status lines oldest-first into caller buffers.
// Returns how many lines were copied.
int CopyLog(char (*lines)[200], uint64_t* timesMs, int maxLines);

} // namespace AimMonitor
