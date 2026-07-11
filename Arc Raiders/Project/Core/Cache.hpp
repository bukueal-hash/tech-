#pragma once

#include <iostream>
#include <cstdint>
#include <Windows.h>
#include <tlhelp32.h>
#include "Memory.h"
#include "Vector.hpp"
#include <d3d9.h>
#include <thread>
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

template<typename T>
inline bool read_array(uint64_t address, T* array, size_t len) {
    Memory::read((PVOID)address, array, sizeof(T) * len);
    return true;
}

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
    struct Vector3 Translation;  // 0x20(0x18)
    char pad_56[8];  // 0x38(0x8)
    struct Vector3 Scale3D;  // 0x40(0x18)
    char pad_88[8];  // 0x58(0x8)

    D3DMATRIX ToMatrixWithScale()
    {
        D3DMATRIX m;
        m._41 = Translation.x;
        m._42 = Translation.y;
        m._43 = Translation.z;

        float x2 = Rotation.x + Rotation.x;
        float y2 = Rotation.y + Rotation.y;
        float z2 = Rotation.z + Rotation.z;

        float xx2 = Rotation.x * x2;
        float yy2 = Rotation.y * y2;
        float zz2 = Rotation.z * z2;
        m._11 = (1.0f - (yy2 + zz2)) * Scale3D.x;
        m._22 = (1.0f - (xx2 + zz2)) * Scale3D.y;
        m._33 = (1.0f - (xx2 + yy2)) * Scale3D.z;

        float yz2 = Rotation.y * z2;
        float wx2 = Rotation.w * x2;
        m._32 = (yz2 - wx2) * Scale3D.z;
        m._23 = (yz2 + wx2) * Scale3D.y;

        float xy2 = Rotation.x * y2;
        float wz2 = Rotation.w * z2;
        m._21 = (xy2 - wz2) * Scale3D.y;
        m._12 = (xy2 + wz2) * Scale3D.x;

        float xz2 = Rotation.x * z2;
        float wy2 = Rotation.w * y2;
        m._31 = (xz2 + wy2) * Scale3D.z;
        m._13 = (xz2 - wy2) * Scale3D.x;

        m._14 = 0.0f;
        m._24 = 0.0f;
        m._34 = 0.0f;
        m._44 = 1.0f;

        return m;
    }

    D3DMATRIX ToMatrixWithScaleArcRaiders()
    {
        D3DMATRIX m{};
        m._41 = Translation.x;
        m._42 = Translation.y;
        m._43 = Translation.z;

        // Use double em vez de float
        double x2 = Rotation.x + Rotation.x;
        double y2 = Rotation.y + Rotation.y;
        double z2 = Rotation.z + Rotation.z;
        double xx2 = Rotation.x * x2;
        double yy2 = Rotation.y * y2;
        double zz2 = Rotation.z * z2;

        m._11 = (1.0f - (yy2 + zz2)) * Scale3D.x;
        m._22 = (1.0f - (xx2 + zz2)) * Scale3D.y;
        m._33 = (1.0f - (xx2 + yy2)) * Scale3D.z;

        double yz2 = Rotation.y * z2;
        double wx2 = Rotation.w * x2;
        m._32 = (yz2 - wx2) * Scale3D.z;
        m._23 = (yz2 + wx2) * Scale3D.y;

        double xy2 = Rotation.x * y2;
        double wz2 = Rotation.w * z2;
        m._21 = (xy2 - wz2) * Scale3D.y;
        m._12 = (xy2 + wz2) * Scale3D.x;

        double xz2 = Rotation.x * z2;
        double wy2 = Rotation.w * y2;
        m._31 = (xz2 + wy2) * Scale3D.z;
        m._13 = (xz2 - wy2) * Scale3D.x;

        m._14 = 0.0f;
        m._24 = 0.0f;
        m._34 = 0.0f;
        m._44 = 1.0f;

        return m;
    }
};