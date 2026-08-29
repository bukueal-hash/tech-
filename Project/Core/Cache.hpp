#pragma once

#include <iostream>
#include <cstdint>
#include <Windows.h>
#include <tlhelp32.h>
#include "Memory.h"
#include "Vector.hpp"
#include "BoneMath.hpp"
#include <d3d9.h>
#include <algorithm>
#include <unordered_map>
#include <string>
#include <chrono>
#include <bitset>

enum class UniBone : uint8_t {
    Root,
    Pelvis,

    Spine1,
    Spine2,
    Spine3,
    Chest,
    Neck,
    Head,

    ClavicleL,
    UpperArmL,
    LowerArmL,
    HandL,

    ClavicleR,
    UpperArmR,
    LowerArmR,
    HandR,

    ThighL,
    CalfL,
    FootL,

    ThighR,
    CalfR,
    FootR,

    Count
};

struct Bone2DF {
    UniBone bone;
    Vector3 pos;
};

struct BoneData {
    std::array<Vector3, (size_t)UniBone::Count> bonesDouble;
    std::array<Vector3, (size_t)UniBone::Count> bonesWorldDouble;
    std::bitset<(size_t)UniBone::Count> valid;
    bool isVisible = false;
    // Steady-clock ms when this bone sample was taken — skeleton-lag probe
    // measures draw-time age against it.
    uint64_t readStampMs = 0;
};