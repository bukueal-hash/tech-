#pragma once

#include <iostream>
#include <cstdint>
#include <Windows.h>
#include <tlhelp32.h>
#include "Memory.h"
#include "Vector.hpp"
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
};


// Transform
struct FQuat
{
    double x;
    double y;
    double z;
    double w;

    // Multiplica dois quaternions
    FQuat Multiply(const FQuat& other) const
    {
        return FQuat{
            w * other.x + x * other.w + y * other.z - z * other.y,
            w * other.y - x * other.z + y * other.w + z * other.x,
            w * other.z + x * other.y - y * other.x + z * other.w,
            w * other.w - x * other.x - y * other.y - z * other.z
        };
    }

    // Rotaciona um vetor pelo quaternion
    Vector3 RotateVector(const Vector3& v) const
    {
        Vector3 q(x, y, z);
        Vector3 t;

        // t = 2 * cross(q, v)
        t.x = 2.0 * (q.y * v.z - q.z * v.y);
        t.y = 2.0 * (q.z * v.x - q.x * v.z);
        t.z = 2.0 * (q.x * v.y - q.y * v.x);

        // result = v + w * t + cross(q, t)
        Vector3 result;
        result.x = v.x + w * t.x + (q.y * t.z - q.z * t.y);
        result.y = v.y + w * t.y + (q.z * t.x - q.x * t.z);
        result.z = v.z + w * t.z + (q.x * t.y - q.y * t.x);

        return result;
    }
};

struct FTransform
{
    struct FQuat Rotation;  // 0x0(0x20)
    Vector3 Translation;  // 0x20(0x18)
    char pad_56[8];  // 0x38(0x8)
    Vector3 Scale3D;  // 0x40(0x18)
    char pad_88[8];  // 0x58(0x8)

    D3DMATRIX ToMatrixWithScale() const
    {
        D3DMATRIX m;
        m._41 = static_cast<float>(Translation.x);
        m._42 = static_cast<float>(Translation.y);
        m._43 = static_cast<float>(Translation.z);

        const float x2 = static_cast<float>(Rotation.x + Rotation.x);
        const float y2 = static_cast<float>(Rotation.y + Rotation.y);
        const float z2 = static_cast<float>(Rotation.z + Rotation.z);

        const float xx2 = static_cast<float>(Rotation.x * x2);
        const float yy2 = static_cast<float>(Rotation.y * y2);
        const float zz2 = static_cast<float>(Rotation.z * z2);
        m._11 = static_cast<float>((1.0f - (yy2 + zz2)) * Scale3D.x);
        m._22 = static_cast<float>((1.0f - (xx2 + zz2)) * Scale3D.y);
        m._33 = static_cast<float>((1.0f - (xx2 + yy2)) * Scale3D.z);

        const float yz2 = static_cast<float>(Rotation.y * z2);
        const float wx2 = static_cast<float>(Rotation.w * x2);
        m._32 = static_cast<float>((yz2 - wx2) * Scale3D.z);
        m._23 = static_cast<float>((yz2 + wx2) * Scale3D.y);

        const float xy2 = static_cast<float>(Rotation.x * y2);
        const float wz2 = static_cast<float>(Rotation.w * z2);
        m._21 = static_cast<float>((xy2 - wz2) * Scale3D.y);
        m._12 = static_cast<float>((xy2 + wz2) * Scale3D.x);

        const float xz2 = static_cast<float>(Rotation.x * z2);
        const float wy2 = static_cast<float>(Rotation.w * y2);
        m._31 = static_cast<float>((xz2 + wy2) * Scale3D.z);
        m._13 = static_cast<float>((xz2 - wy2) * Scale3D.x);

        m._14 = 0.0f;
        m._24 = 0.0f;
        m._34 = 0.0f;
        m._44 = 1.0f;

        return m;
    }
};