#pragma once
#include <Windows.h>
#include <math.h>
#include <cmath>
#include <array>

#define M_PI 3.14159265358979323846
#define RAD2DEG(x) ((x) * (180.0 / M_PI))
#define DEG2RAD(x) ((x) * (M_PI / 180.0))

struct matrix3x4_t
{
    matrix3x4_t() {}
    matrix3x4_t(
        double m00, double m01, double m02, double m03,
        double m10, double m11, double m12, double m13,
        double m20, double m21, double m22, double m23)
    {
        m_flMatVal[0][0] = m00; m_flMatVal[0][1] = m01; m_flMatVal[0][2] = m02; m_flMatVal[0][3] = m03;
        m_flMatVal[1][0] = m10; m_flMatVal[1][1] = m11; m_flMatVal[1][2] = m12; m_flMatVal[1][3] = m13;
        m_flMatVal[2][0] = m20; m_flMatVal[2][1] = m21; m_flMatVal[2][2] = m22; m_flMatVal[2][3] = m23;
    }

    double* operator[](int i) { return m_flMatVal[i]; }
    const double* operator[](int i) const { return m_flMatVal[i]; }
    double* Base() { return &m_flMatVal[0][0]; }
    const double* Base() const { return &m_flMatVal[0][0]; }

    double m_flMatVal[3][4];
};

class Vector2
{
public:
    Vector2() : x(0.f), y(0.f) {}
    Vector2(double _x, double _y) : x(_x), y(_y) {}

    double x;
    double y;

    inline double Distance(Vector2 v)
    {
        return sqrt(pow(v.x - x, 2) + pow(v.y - y, 2));
    }

    inline Vector2 operator+(Vector2 v) {
        return { x + v.x, y + v.y };
    }

    inline Vector2 operator-(Vector2 v) {
        return { x - v.x, y - v.y };
    }

    inline Vector2 flip() {
        return { y, x };
    }
};

class Vector3
{
public:
    Vector3() : x(0.f), y(0.f), z(0.f) {}
    Vector3(double _x, double _y, double _z) : x(_x), y(_y), z(_z) {}

    double x;
    double y;
    double z;

    inline double Dot(Vector3 v) const
    {
        return x * v.x + y * v.y + z * v.z;
    }

    inline double Distance(Vector3 v)
    {
        return sqrt(pow(v.x - x, 2) + pow(v.y - y, 2) + pow(v.z - z, 2));
    }

    inline double Length()
    {
        return sqrt(x * x + y * y + z * z);
    }

    inline bool Empty()
    {
        return !x && !y && !z;
    }

    inline double DistTo(Vector3 ape)
    {
        return (*this - ape).Length();
    }

    inline Vector3 operator+(Vector3 v) const
    {
        return Vector3(x + v.x, y + v.y, z + v.z);
    }

    inline Vector3 operator-(Vector3 v) const
    {
        return Vector3(x - v.x, y - v.y, z - v.z);
    }

    inline Vector3 operator*(double f) const
    {
        return Vector3(x * f, y * f, z * f);
    }

    inline Vector3 operator/(double f) const
    {
        return Vector3(x / f, y / f, z / f);
    }

    inline Vector3& operator+=(Vector3 v)
    {
        x += v.x; y += v.y; z += v.z;
        return *this;
    }

    inline Vector3& operator-=(Vector3 v)
    {
        x -= v.x; y -= v.y; z -= v.z;
        return *this;
    }

    inline bool operator==(Vector3 v) const
    {
        return x == v.x && y == v.y && z == v.z;
    }

    inline bool operator!=(Vector3 v) const
    {
        return !(*this == v);
    }
};

namespace BoneID
{
    // Arc Raiders CL-1233465 skeleton indices (fremework::game::bones)
    const int32_t Root = 0;
    const int32_t Pelvis = 1;
    const int32_t Spine01 = 2;
    const int32_t Spine02 = 3;
    const int32_t Spine03 = 4;
    const int32_t Spine = 5;
    const int32_t Chest = 5;
    const int32_t Neck = 6;
    const int32_t Head = 7;

    const int32_t L_Clavicle = 8;
    const int32_t L_UpperArm = 9;
    const int32_t L_Forearm = 10;
    const int32_t L_Hand = 11;

    const int32_t R_Clavicle = 42;
    const int32_t R_UpperArm = 43;
    const int32_t R_Forearm = 44;
    const int32_t R_Hand = 45;

    const int32_t L_Thigh = 65;
    const int32_t L_Calf = 66;
    const int32_t L_Foot = 67;

    const int32_t R_Thigh = 69;
    const int32_t R_Calf = 70;
    const int32_t R_Foot = 71;
}
