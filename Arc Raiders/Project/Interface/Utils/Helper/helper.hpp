#pragma once
#include <iomanip>
#include <cstring>
#include <Windows.h>
#include <TlHelp32.h>
#include <tchar.h>
#include <string_view>
#include <iostream>

namespace Helper {
    float MaxFloat(float a, float b);
    float MinFloat(float a, float b);
    void Log(std::string name, uintptr_t offset);
}