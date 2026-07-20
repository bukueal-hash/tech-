#pragma once

// Live intervals (ms) for TaskManager lanes. Pointers are passed into TimedTask
// so menu/debug can tweak later without re-registering tasks.

namespace TaskIntervals {

// Hot lane
inline double aimMs = 4.0;
inline double cameraMs = 8.0;
inline double frameMs = 12.0;
inline double positionMs = 16.0;

// Main lane
inline double updateMs = 18.0;
inline double entityMs = 22.0;
inline double robotMs = 80.0; // mild interval, heavy DMA

// Features lane (cold heavies + mild vis)
inline double containerMs = 3000.0;
inline double itemMs = 5000.0;
inline double visMs = 500.0;

} // namespace TaskIntervals
