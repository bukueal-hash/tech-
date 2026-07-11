#pragma once
#include <Windows.h>
#include <math.h>
#include <array>
#include <cmath>
#define M_PI		3.14159265358979323846	
#define Assert( _exp ) ((void)0)
#define RAD2DEG(x) ((x) * (180.0 / M_PI))
#define DEG2RAD(x) ((x) * (M_PI / 180.0f))

inline float deg2rad(float degrees) {
	return degrees * (M_PI / 180.0f);
}

// Converte radianos para graus
inline float rad2deg(float radians) {
	return radians * (180.0f / M_PI);
}

struct BaseMatrix
{
	double at[16]{};
};

struct matrix3x4_t
{
	matrix3x4_t() {}
	matrix3x4_t(
		double m00, double m01, double m02, double m03,
		double m10, double m11, double m12, double m13,
		double m20, double m21, double m22, double m23)
	{
		m_flMatVal[0][0] = m00;	m_flMatVal[0][1] = m01; m_flMatVal[0][2] = m02; m_flMatVal[0][3] = m03;
		m_flMatVal[1][0] = m10;	m_flMatVal[1][1] = m11; m_flMatVal[1][2] = m12; m_flMatVal[1][3] = m13;
		m_flMatVal[2][0] = m20;	m_flMatVal[2][1] = m21; m_flMatVal[2][2] = m22; m_flMatVal[2][3] = m23;
	}

	double* operator[](int i) { Assert((i >= 0) && (i < 3)); return m_flMatVal[i]; }
	const double* operator[](int i) const { Assert((i >= 0) && (i < 3)); return m_flMatVal[i]; }
	double* Base() { return &m_flMatVal[0][0]; }
	const double* Base() const { return &m_flMatVal[0][0]; }

	double m_flMatVal[3][4];
};

class QAngle
{
public:
	QAngle(void)
	{
		Init();
	}
	QAngle(double X, double Y, double Z)
	{
		Init(X, Y, Z);
	}
	QAngle(const double* clr)
	{
		Init(clr[0], clr[1], clr[2]);
	}

	void Init(double ix = 0.0f, double iy = 0.0f, double iz = 0.0f)
	{
		pitch = ix;
		yaw = iy;
		roll = iz;
	}

	double operator[](int i) const
	{
		return ((double*)this)[i];
	}
	double& operator[](int i)
	{
		return ((double*)this)[i];
	}

	QAngle& operator+=(const QAngle& v)
	{
		pitch += v.pitch; yaw += v.yaw; roll += v.roll;
		return *this;
	}
	QAngle& operator-=(const QAngle& v)
	{
		pitch -= v.pitch; yaw -= v.yaw; roll -= v.roll;
		return *this;
	}
	QAngle& operator*=(double fl)
	{
		pitch *= fl;
		yaw *= fl;
		roll *= fl;
		return *this;
	}
	QAngle& operator*=(const QAngle& v)
	{
		pitch *= v.pitch;
		yaw *= v.yaw;
		roll *= v.roll;
		return *this;
	}
	QAngle& operator/=(const QAngle& v)
	{
		pitch /= v.pitch;
		yaw /= v.yaw;
		roll /= v.roll;
		return *this;
	}
	QAngle& operator+=(double fl)
	{
		pitch += fl;
		yaw += fl;
		roll += fl;
		return *this;
	}
	QAngle& operator/=(double fl)
	{
		pitch /= fl;
		yaw /= fl;
		roll /= fl;
		return *this;
	}
	QAngle& operator-=(double fl)
	{
		pitch -= fl;
		yaw -= fl;
		roll -= fl;
		return *this;
	}

	QAngle& operator=(const QAngle& vOther)
	{
		pitch = vOther.pitch; yaw = vOther.yaw; roll = vOther.roll;
		return *this;
	}

	QAngle operator-(void) const
	{
		return QAngle(-pitch, -yaw, -roll);
	}
	QAngle operator+(const QAngle& v) const
	{
		return QAngle(pitch + v.pitch, yaw + v.yaw, roll + v.roll);
	}
	QAngle operator-(const QAngle& v) const
	{
		return QAngle(pitch - v.pitch, yaw - v.yaw, roll - v.roll);
	}
	QAngle operator*(double fl) const
	{
		return QAngle(pitch * fl, yaw * fl, roll * fl);
	}
	QAngle operator*(const QAngle& v) const
	{
		return QAngle(pitch * v.pitch, yaw * v.yaw, roll * v.roll);
	}
	QAngle operator/(double fl) const
	{
		return QAngle(pitch / fl, yaw / fl, roll / fl);
	}
	QAngle operator/(const QAngle& v) const
	{
		return QAngle(pitch / v.pitch, yaw / v.yaw, roll / v.roll);
	}

	double Length() const
	{
		return sqrt(pitch * pitch + yaw * yaw + roll * roll);
	}
	double LengthSqr(void) const
	{
		return (pitch * pitch + yaw * yaw + roll * roll);
	}
	bool IsZero(double tolerance = 0.01f) const
	{
		return (pitch > -tolerance && pitch < tolerance &&
			yaw > -tolerance && yaw < tolerance &&
			roll > -tolerance && roll < tolerance);
	}
	double pitch;
	double yaw;
	double roll;
};


class Vector3
{
public:
	Vector3() : x(0.f), y(0.f), z(0.f)
	{

	}

	Vector3(double _x, double _y, double _z) : x(_x), y(_y), z(_z)
	{

	}
	~Vector3()
	{

	}

	double x;
	double y;
	double z;

	inline double Dot(Vector3 v)
	{
		return x * v.x + y * v.y + z * v.z;
	}

	inline double Distance(Vector3 v)
	{
		return double(sqrtf(powf(v.x - x, 2.0) + powf(v.y - y, 2.0) + powf(v.z - z, 2.0)));
	}
	inline double Length() {
		return sqrt(x * x + y * y + z * z);
	}
	inline bool Empty()
	{
		if (!x && !y && !z)
			return true;
		else
			return false;
	}
	inline void Normalize()
	{
		while (x > 89.0f)
			x -= 180.f;

		while (x < -89.0f)
			x += 180.f;

		while (y > 180.f)
			y -= 360.f;

		while (y < -180.f)
			y += 360.f;
	}
	inline double DistTo(Vector3 ape)
	{
		return (*this - ape).Length();
	}
	inline double distance(Vector3 vec)
	{
		return sqrt(
			pow(vec.x - x, 2) +
			pow(vec.y - y, 2)
		);
	}
	inline Vector3 operator+(Vector3 v)
	{
		return Vector3(x + v.x, y + v.y, z + v.z);
	}

	inline Vector3 operator-(const Vector3& v) const
	{
		return Vector3(x - v.x, y - v.y, z - v.z);
	}

	inline Vector3 operator*(double flNum) { return Vector3(x * flNum, y * flNum, z * flNum); }
	inline double Size() {
		return Length();
	}
};
class Vector2
{
public:
	Vector2() : x(0.f), y(0.f)
	{

	}

	Vector2(double _x, double _y) : x(_x), y(_y)
	{

	}
	~Vector2()
	{

	}

	double x;
	double y;
	inline Vector2 operator+(int i) {
		return { x + i, y + i };
	}
	inline Vector2 operator-(Vector2 v) {
		return { x - v.x, y - v.y };
	}

	inline Vector2 flip() {
		return { y, x };
	}

};

inline double GetCrossDistance(double x1, double y1, double z1, double x2, double y2, double z2) {
	return sqrt(pow((x2 - x1), 2) + pow((y2 - y1), 2));
}
inline inline double calcDist(const Vector3 p1, const Vector3 p2)
{
	double x = p1.x - p2.x;
	double y = p1.y - p2.y;
	double z = p1.z - p2.z;
	return sqrt(x * x + y * y + z * z);
}

struct bone_t
{
	BYTE pad[0xCC];
	double x;
	BYTE pad2[0xC];
	double y;
	BYTE pad3[0xC];
	double z;
};

inline Vector3 VAR_Adds(Vector3 src, Vector3 dst)
{
	Vector3 diff;
	diff.x = src.x + dst.x;
	diff.y = src.y + dst.y;
	diff.z = src.z + dst.z;
	return diff;
}

inline Vector3 VAR_toAngless(Vector3 const& v)
{
	double R2D = 57.2957795130823;
	Vector3 angles;
	angles.z = 0.0;
	angles.x = R2D * asin(v.z);
	angles.y = R2D * atan2(v.y, v.x);
	return angles;
}

inline Vector3 VAR_Subtracts(Vector3 src, Vector3 dst)
{
	Vector3 diff;
	diff.x = src.x - dst.x;
	diff.y = src.y - dst.y;
	diff.z = src.z - dst.z;
	return diff;
}

namespace BoneID
{
	const int32_t Head = 7;
	const int32_t Neck = 6;
	const int32_t Spine = 5;
	const int32_t Pelvis = 2;

	const int32_t L_Clavicle = 92;
	const int32_t L_UpperArm = 93;
	const int32_t L_Forearm = 94;
	const int32_t L_Hand = 95;

	const int32_t R_Clavicle = 65;
	const int32_t R_UpperArm = 66;
	const int32_t R_Forearm = 67;
	const int32_t R_Hand = 68;

	const int32_t L_Thigh = 125;
	const int32_t L_Calf = 126;
	const int32_t L_Foot = 127;

	const int32_t R_Thigh = 130;
	const int32_t R_Calf = 131;
	const int32_t R_Foot = 132;
}

enum Bone
{
	Root = 0,

	pelvis = BoneID::Pelvis,         // 2
	spine_01 = BoneID::Spine,          // 5
	spine_02 = BoneID::Spine,          // 5
	spine_03 = BoneID::Spine,          // 5
	neck_01 = BoneID::Neck,           // 6
	Head = BoneID::Head,           // 7

	// Braço esquerdo
	clavicle_l = BoneID::L_Clavicle,     // 92
	upperarm_l = BoneID::L_UpperArm,     // 93
	lowerarm_l = BoneID::L_Forearm,      // 94
	hand_l = BoneID::L_Hand,         // 95

	// Braço direito
	clavicle_r = BoneID::R_Clavicle,     // 65
	upperarm_r = BoneID::R_UpperArm,     // 66
	lowerarm_r = BoneID::R_Forearm,      // 67
	hand_r = BoneID::R_Hand,         // 68

	// Perna esquerda
	upperleg_l = BoneID::L_Thigh,        // 125
	lowerleg_l = BoneID::L_Calf,         // 126
	foot_l = BoneID::L_Foot,         // 127

	// Perna direita
	upperleg_r = BoneID::R_Thigh,        // 130
	lowerleg_r = BoneID::R_Calf,         // 131
	foot_r = BoneID::R_Foot,         // 132
};

inline void NormalizeAngles(Vector2& angle)
{
	while (angle.x > 89.0f)
		angle.x -= 180.f;

	while (angle.x < -89.0f)
		angle.x += 180.f;

	while (angle.y > 180.f)
		angle.y -= 360.f;

	while (angle.y < -180.f)
		angle.y += 360.f;
}

inline void normalY(double& f) {
	while (f > 180.f)
		f -= 360.f;

	while (f < -180.f)
		f += 360.f;
}

inline double VAR_Magnitudes(Vector3 vec)
{
	return sqrtf(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z);
}
inline double VAR_deltaDistances(Vector3 src, Vector3 dst)
{
	Vector3 diff = VAR_Subtracts(src, dst);
	return VAR_Magnitudes(diff);
}
inline void VAR_VectorAngless(const double* forward, double* angles)
{
	double	tmp, yaw, pitch;
	if (forward[1] == 0 && forward[0] == 0)
	{
		yaw = 0;
		if (forward[2] > 0)
			pitch = 270;
		else
			pitch = 90;
	}
	else
	{
		yaw = (atan2(forward[1], forward[0]) * 57.295779513082f);
		if (yaw < 0)
			yaw += 360;
		tmp = sqrt(forward[0] * forward[0] + forward[1] * forward[1]);
		pitch = (atan2(-forward[2], tmp) * 57.295779513082f);
		if (pitch < 0)
			pitch += 360;
	}
	angles[0] = pitch;
	angles[1] = yaw;
	angles[2] = 0;
}

inline Vector3 Var_CalcAngless(Vector3& src, Vector3& dst)
{
	double output[3];
	double input[3] = { dst.x - src.x , dst.y - src.y, dst.z - src.z };
	VAR_VectorAngless(input, output);
	if (output[1] > 180) output[1] -= 360;
	if (output[1] < -180) output[1] += 360;
	if (output[0] > 180) output[0] -= 360;
	if (output[0] < -180) output[0] += 360;
	return { output[0], output[1], 0.f };
}
struct Vector4 {
	double x, y, z, w;
};
//void ProjW2s(Vector3 Position, Vector3& Screen)
//{
//	uintptr_t render = *reinterpret_cast<std::uintptr_t*>((uintptr_t)GetModuleHandleA(NULL) + OffsetsT1::ViewRender);
//	if (!render)return;
//	uintptr_t Matrixa = *reinterpret_cast<std::uintptr_t*>(OffsetsT1::ViewMatrix + render);
//	if (!Matrixa)return;
//	BaseMatrix Matrix = *reinterpret_cast<BaseMatrix*>(Matrixa);
//
//	Vector3 out;
//	double _x = Matrix.at[0] * Position.x + Matrix.at[1] * Position.y + Matrix.at[2] * Position.z + Matrix.at[3];
//	double _y = Matrix.at[4] * Position.x + Matrix.at[5] * Position.y + Matrix.at[6] * Position.z + Matrix.at[7];
//	out.z = Matrix.at[12] * Position.x + Matrix.at[13] * Position.y + Matrix.at[14] * Position.z + Matrix.at[15];
//
//	if (out.z < 0.1f) return;
//
//	_x *= 1.f / out.z;
//	_y *= 1.f / out.z;
//
//	out.x = ImGui::GetIO().DisplaySize.x * .5f;
//	out.y = ImGui::GetIO().DisplaySize.y * .5f;
//
//	out.x += 0.5f * _x * ImGui::GetIO().DisplaySize.x + 0.5f;
//	out.y -= 0.5f * _y * ImGui::GetIO().DisplaySize.y + 0.5f;
//	Screen = out;
//}

inline Vector2 RevolveCoordinatesSystem(double RevolveAngle, Vector2 OriginPos, Vector2 DestPos)
{
	Vector2 ResultPos;
	if (RevolveAngle == 0)
		return DestPos;
	ResultPos.x = OriginPos.x + (DestPos.x - OriginPos.x) * cos(RevolveAngle * M_PI / 180) + (DestPos.y - OriginPos.y) * sin(RevolveAngle * M_PI / 180);
	ResultPos.y = OriginPos.y - (DestPos.x - OriginPos.x) * sin(RevolveAngle * M_PI / 180) + (DestPos.y - OriginPos.y) * cos(RevolveAngle * M_PI / 180);
	return ResultPos;
}

inline double pi = 3.141592;
inline double Rad2Deg(double radianes) {
	return 360 / (2 * pi) * radianes;
}

inline void RotatePoint(double* pointToRotate, double* centerPoint, double angle, double* ReturnTo)
{
	angle = (double)(angle * (M_PI / (double)180));

	double cosTheta = (double)cos(angle);
	double sinTheta = (double)sin(angle);

	ReturnTo[0] = cosTheta * (pointToRotate[0] - centerPoint[0]) - sinTheta * (pointToRotate[1] - centerPoint[1]);
	ReturnTo[1] = sinTheta * (pointToRotate[0] - centerPoint[0]) + cosTheta * (pointToRotate[1] - centerPoint[1]);

	ReturnTo[0] += centerPoint[0];
	ReturnTo[1] += centerPoint[1];
}

inline Vector3 WorldToRadar(double Yaw, Vector3 Origin, Vector3 LocalOrigin, double PosX, double PosY, Vector3 Size, bool& outbuff)
{
	bool flag = false;
	double num = static_cast<double>(Yaw);
	double num2 = num * 0.017453292519943295;
	double num3 = static_cast<double>(std::cos(num2));
	double num4 = static_cast<double>(std::sin(num2));
	double num5 = Origin.x - LocalOrigin.x;
	double num6 = Origin.y - LocalOrigin.y;

	// Calculate the transformed coordinates
	Vector3 vector;
	vector.x = (num6 * num3 - num5 * num4) / 150.0f;
	vector.y = (num5 * num3 + num6 * num4) / 150.0f;

	// Adjusted vector to center around PosX and PosY
	Vector3 vector2;
	vector2.x = vector.x + PosX;
	vector2.y = -vector.y + PosY;

	// Clamping within radar boundaries
	if (vector2.x > PosX + Size.x)
	{
		vector2.x = PosX + Size.x;
		flag = true;
	}
	else if (vector2.x < PosX)
	{
		vector2.x = PosX;
		flag = true;
	}

	if (vector2.y > PosY + Size.y)
	{
		vector2.y = PosY + Size.y;
		flag = true;
	}
	else if (vector2.y < PosY)
	{
		vector2.y = PosY;
		flag = true;
	}

	outbuff = flag;
	return vector2;
}
inline void VectorAnglesRadar(Vector3& forward, Vector3& angles)
{
	if (forward.x == 0.f && forward.y == 0.f)
	{
		angles.x = forward.z > 0.f ? -90.f : 90.f;
		angles.y = 0.f;
	}
	else
	{
		angles.x = RAD2DEG(atan2(-forward.z, forward.Size()));
		angles.y = RAD2DEG(atan2(forward.y, forward.z));
	}
	angles.z = 0.f;
}
inline void RotateTriangle(std::array<Vector3, 3>& points, double rotation)
{
	const auto points_center = (points.at(0) + points.at(1) + points.at(2));
	points_center.x / 3;
	points_center.y / 3;
	points_center.z / 3;
	for (auto& point : points)
	{
		// Translate point to the origin (relative to the center)
		point = point - points_center;

		// Store the current x and y values
		const auto temp_x = point.x;
		const auto temp_y = point.y;

		// Convert rotation from degrees to radians
		const auto theta = DEG2RAD(rotation);
		const auto c = cosf(theta);
		const auto s = sinf(theta);

		// Perform the rotation using 2D rotation matrix
		point.x = temp_x * c - temp_y * s;
		point.y = temp_x * s + temp_y * c;

		// Translate the point back
		point = point + points_center;
	}
}

// FVECTORS


struct FVector2D
{
	union
	{
		struct { double X, Y; };
		struct { double x, y; };
		struct { double pitch, yaw; };
		struct { double value[2]; };
	};

	FVector2D() : x(0), y(0) {}
	FVector2D(double x) : x(x), y(0) {}
	FVector2D(double x, double y) : x(x), y(y) {}
	FVector2D(double value[2]) : x(value[0]), y(value[1]) {}
};

struct FVector
{
	union
	{
		struct { double X, Y, Z; };
		struct { double x, y, z; };
		struct { double pitch, yaw, roll; };
		struct { double value[3]; };
	};

	constexpr FVector(double x = 0.f, double y = 0.f, double z = 0.f) noexcept : X{ x }, Y{ y }, Z{ z } {}

	[[nodiscard]] friend constexpr auto operator-(const FVector& a, const FVector& b) noexcept -> FVector
	{
		return { a.X - b.X, a.Y - b.Y, a.Z - b.Z };
	}

	[[nodiscard]] friend constexpr auto operator+(const FVector& a, const FVector& b) noexcept -> FVector
	{
		return { a.X + b.X, a.Y + b.Y, a.Z + b.Z };
	}

	[[nodiscard]] friend constexpr auto operator*(const FVector& a, const FVector& b) noexcept -> FVector
	{
		return { a.X * b.X, a.Y * b.Y, a.Z * b.Z };
	}

	[[nodiscard]] friend constexpr auto operator*(const FVector& v, double f) noexcept -> FVector
	{
		return { v.X * f, v.Y * f, v.Z * f };
	}

	[[nodiscard]] friend constexpr auto operator/(const FVector& v, double f) noexcept -> FVector
	{
		return { v.X / f, v.Y / f, v.Z / f };
	}

	[[nodiscard]] friend constexpr auto operator+(const FVector& v, double f) noexcept -> FVector
	{
		return { v.X + f, v.Y + f, v.Z + f };
	}

	[[nodiscard]] friend constexpr auto operator-(double f, const FVector& v) noexcept -> FVector
	{
		return{ f - v.X, f - v.Y, f - v.Z };
	}

	constexpr auto& operator+=(const FVector& v) noexcept
	{
		X += v.X;
		Y += v.Y;
		Z += v.Z;
		return *this;
	}

	constexpr auto& operator-=(const FVector& v) noexcept
	{
		X -= v.X;
		Y -= v.Y;
		Z -= v.Z;
		return *this;
	}

	constexpr auto& operator*=(const FVector& v) noexcept
	{
		X *= v.X;
		Y *= v.Y;
		Z *= v.Z;
		return *this;
	}

	constexpr auto& operator/=(const FVector& v) noexcept
	{
		X /= v.X;
		Y /= v.Y;
		Z /= v.Z;
		return *this;
	}

	const double DotProduct(FVector coords)
	{
		return (this->X * coords.X) + (this->Y * coords.Y) + (this->Z * coords.Z);
	}

	bool isValid()
	{
		return (this->X != 0 && this->Y != 0 && this->Z != 0);
	}

	double length()
	{
		return (double)sqrt(X * X + Y * Y + Z * Z);
	}

	double distance(FVector vec)
	{
		double _x = this->X - vec.X;
		double _y = this->Y - vec.Y;
		double _z = this->Z - vec.Z;
		return sqrt((_x * _x) + (_y * _y) + (_z * _z));
	}

	FVector normalize()
	{
		FVector newvec;
		newvec.X = this->X / length();
		newvec.Y = this->Y / length();
		newvec.Z = this->Z / length();
		return newvec;
	}

	FVector GetSafeNormal(double tolerance = 1e-8f) const {
		const double squareSum = X * X + Y * Y + Z * Z;
		if (squareSum > tolerance) {
			const double scale = InvSqrt(squareSum);
			return FVector(X * scale, Y * scale, Z * scale);
		}
		return FVector(0, 0, 0);
	}

	FVector Rotation() const
	{
		const double yaw = atan2(Y, X) * (180.0f / M_PI);
		const double pitch = atan2(Z, sqrt(X * X + Y * Y)) * (180.0f / M_PI);
		const double roll = 0.0f; // If you don't have roll data, you can set this to zero
		return FVector(yaw, pitch, roll);
	}

	FVector Lerp(const FVector& target, double alpha) const {
		return *this + (target - *this) * alpha;
	}

	static double InvSqrt(double x) {
		union {
			double f;
			int32_t i;
		} tmp;
		tmp.f = x;
		tmp.i = 0x5f3759df - (tmp.i >> 1);
		return 0.5f * tmp.f * (3.0f - x * tmp.f * tmp.f);
	}
};

struct FRotator
{
	union
	{
		struct { double X, Y, Z; };
		struct { double x, y, z; };
		struct { double pitch, yaw, roll; };
		struct { double Pitch, Yaw, Roll; };
		struct { double value[3]; };
	};

	[[nodiscard]] friend constexpr auto operator-(const FRotator& a, const FRotator& b) noexcept -> FRotator
	{
		return { a.Pitch - b.Pitch, a.Yaw - b.Yaw, a.Roll - b.Roll };
	}

	[[nodiscard]] friend constexpr auto operator+(const FRotator& a, const FRotator& b) noexcept -> FRotator
	{
		return { a.Pitch + b.Pitch, a.Yaw + b.Yaw, a.Roll + b.Roll };
	}

	[[nodiscard]] friend constexpr auto operator*(const FRotator& a, const FRotator& b) noexcept -> FRotator
	{
		return { a.Pitch * b.Pitch, a.Yaw * b.Yaw, a.Roll * b.Roll };
	}

	[[nodiscard]] friend constexpr auto operator*(const FRotator& a, double b) noexcept -> FRotator
	{
		return { a.Pitch * b, a.Yaw * b, a.Roll * b };
	}

	[[nodiscard]] friend constexpr auto operator/(const FRotator& a, double b) noexcept -> FRotator
	{
		return { a.Pitch / b, a.Yaw / b, a.Roll / b };
	}

	double* fromAngle() const
	{
		double temp[3] = {
			std::cos(deg2rad(Pitch)) * std::cos(deg2rad(Yaw)),
			std::cos(deg2rad(Pitch)) * std::sin(deg2rad(Yaw)),
			std::sin(deg2rad(Pitch))
		};

		return temp;
	}

	constexpr auto& operator+=(const FRotator& o) noexcept
	{
		Pitch += o.Pitch;
		Yaw += o.Yaw;
		Roll += o.Roll;
		return *this;
	}

	constexpr auto& normalize() noexcept
	{
		Pitch = std::isfinite(Pitch) ? std::remainder(Pitch, 360.f) : 0.f;
		Yaw = std::isfinite(Yaw) ? std::remainder(Yaw, 360.f) : 0.f;
		Roll = 0.f;
		return *this;
	}

};