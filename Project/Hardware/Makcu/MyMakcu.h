#pragma once

#include <string>

// Thin wrapper around makcu::Device for KmBox.cpp (MAKCU path).
struct MyMakcu {
    /** Returns e.g. "COM3" if a MAKCU device is found, else empty. */
    static std::string AutoDetectComPort();
    static void SetComPort(const std::string& port);
    static bool Initialize();
    static void Move(int x, int y);
    static void LeftClick();
    static void MouseDown();
    static void MouseUp();
};
