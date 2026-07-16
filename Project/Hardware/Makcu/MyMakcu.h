#pragma once

#include <string>

// Thin wrapper around makcu::Device for KmBox.cpp (MAKCU path).
struct MyMakcu {
    /** Returns e.g. "COM3" if a MAKCU device is found, else empty. */
    static std::string AutoDetectComPort();
    static void SetComPort(const std::string& port);
    static bool Initialize();
    static void Move(int x, int y);
    /// Segmented relative move (MAKCU firmware smooth path).
    static void MoveSmooth(int x, int y, uint32_t segments);
    /// Cubic-style curved move with one control point (see makcu::Device::mouseMoveBezier).
    static void MoveBezier(int x, int y, uint32_t segments, int32_t ctrl_x, int32_t ctrl_y);
    static void LeftClick();
};
