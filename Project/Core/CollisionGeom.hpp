#pragma once
// FKAggregateGeom reader: the collision shapes a UBodySetup carries, decoded
// from raw bytes into primitives a visibility test can query.
//
// Header-only and free of DMA, so the parsers, the rotator conversion and the
// world-space AABB are unit-testable without a target process (Project.Tests).
// The caller supplies the bytes; this file never reads memory.
//
// ── Layout provenance ───────────────────────────────────────────────────────
// Every offset below was read out of THIS build's dump (sdk/sdk.txt), by struct:
//   Engine.KAggregateGeom        line 189003 - nine TArray headers, 0x10 each
//   Engine.KSphereElem           line 189094 - Center 0x30, Radius 0x48, size 0x50
//   Engine.KBoxElem              line 189027 - Center 0x30, Rotation 0x48,
//                                               X/Y/Z 0x60/0x64/0x68, size 0x70
//   Engine.KSphylElem            line 189103 - Center 0x30, Rotation 0x48,
//                                               Radius 0x60, Length 0x64, size 0x68
//   Engine.KConvexElem           line 189039 - VertexData 0x30, IndexData 0x40,
//                                               ElemBox 0x50, Transform 0x90, size 0x110
//   Engine.KShapeElem            line 189070 - RestOffset 0x8, Name 0xC,
//                                               CollisionEnabled 0x19
//   CoreUObject.Box              line 326210 sibling - min 0x0, max 0x18
//   CoreUObject.Rotator          line 290160 - pitch 0x0, Yaw 0x8, Roll 0x10 (doubles)
//   CoreUObject.Vector           line 326210 - X 0x0, Y 0x8, Z 0x10 (doubles)
//
// ── The two traps that cause the classic "the whole world is blocked" bug ───
//   1. AggGeom is INLINE inside UBodySetup. There is no pointer to follow:
//      the caller reads kHeaderBytes at BodySetup + Offsets::BodySetup_AggGeom.
//   2. Box X/Y/Z and capsule Length are FULL extents, not half. Every primitive
//      this file produces already carries half-extents, and it says so in the
//      field name, so no consumer can halve twice.
//
// Engine note for the query stage: a capsule's axis is local +Z of the element
// frame before its own Rotation is applied, and KSphereElem has no rotation.
// Only Sphere/Box/Sphyl/Convex have a byte layout in this dump; the remaining
// five arrays are counted and reported as unsupported rather than guessed at.

#include "BoneMath.hpp"  // FQuat, FTransform, Vector3 (Vector.hpp)

#include <algorithm>
#include <array>
#include <utility>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace CollisionGeom {

// ── Layout ──────────────────────────────────────────────────────────────────
namespace Layout {

// KAggregateGeom: nine TArray<uint8> headers, 0x10 bytes each = 0x90.
constexpr size_t kHeaderBytes = 0x90;
constexpr size_t kArrayStride = 0x10;

// Array header slots, in KAggregateGeom member order (ci 0..8).
constexpr size_t kArraySphere = 0x00;
constexpr size_t kArrayBox = 0x10;
constexpr size_t kArraySphyl = 0x20;
constexpr size_t kArrayConvex = 0x30;
constexpr size_t kArrayTaperedCapsule = 0x40;
constexpr size_t kArrayLevelSet = 0x50;
constexpr size_t kArraySkinnedLevelSet = 0x60;
constexpr size_t kArrayMLLevelSet = 0x70;
constexpr size_t kArraySkinnedTriangleMesh = 0x80;

// KShapeElem head, shared by every element type (derived data starts at 0x30).
constexpr size_t kShapeElem_CollisionEnabled = 0x19;

constexpr size_t kSphereElemSize = 0x50;
constexpr size_t kSphereElem_Center = 0x30;
constexpr size_t kSphereElem_Radius = 0x48;

constexpr size_t kBoxElemSize = 0x70;
constexpr size_t kBoxElem_Center = 0x30;
constexpr size_t kBoxElem_Rotation = 0x48;
constexpr size_t kBoxElem_X = 0x60;
constexpr size_t kBoxElem_Y = 0x64;
constexpr size_t kBoxElem_Z = 0x68;

constexpr size_t kSphylElemSize = 0x68;
constexpr size_t kSphylElem_Center = 0x30;
constexpr size_t kSphylElem_Rotation = 0x48;
constexpr size_t kSphylElem_Radius = 0x60;
constexpr size_t kSphylElem_Length = 0x64;

constexpr size_t kConvexElemSize = 0x110;
constexpr size_t kConvexElem_VertexData = 0x30;
constexpr size_t kConvexElem_IndexData = 0x40;
constexpr size_t kConvexElem_ElemBox = 0x50;
constexpr size_t kConvexElem_Transform = 0x90;

constexpr size_t kBox_Min = 0x00;
constexpr size_t kBox_Max = 0x18;

constexpr size_t kRotator_Pitch = 0x00;
constexpr size_t kRotator_Yaw = 0x08;
constexpr size_t kRotator_Roll = 0x10;

// A count beyond this is corrupt metadata, not a shape the game made.
constexpr int32_t kMaxSaneArrayCount = 4096;
// Hull point cloud and triangle list bounds. A hull past these is refused and the
// element falls back to its ElemBox stand-in, rather than read partially.
constexpr int32_t kMaxHullVertices = 1024;
constexpr int32_t kMaxHullIndices = 6144;

} // namespace Layout

enum class Shape : uint8_t {
    Sphere = 0,
    Box = 1,
    Sphyl = 2,          // capsule
    Convex = 3,
    TaperedCapsule = 4, // in the dump, no layout decoded here
    LevelSet = 5,       // "
    SkinnedLevelSet = 6,
    MLLevelSet = 7,
    SkinnedTriangleMesh = 8,
    Count = 9
};

constexpr int kShapeCount = static_cast<int>(Shape::Count);

inline const char* ShapeName(Shape s)
{
    switch (s) {
    case Shape::Sphere: return "sphere";
    case Shape::Box: return "box";
    case Shape::Sphyl: return "capsule";
    case Shape::Convex: return "convex";
    case Shape::TaperedCapsule: return "tapered_capsule";
    case Shape::LevelSet: return "level_set";
    case Shape::SkinnedLevelSet: return "skinned_level_set";
    case Shape::MLLevelSet: return "ml_level_set";
    case Shape::SkinnedTriangleMesh: return "skinned_triangle_mesh";
    default: return "?";
    }
}

/** Byte size of one element, 0 when this dump gives no usable layout. */
inline size_t ElementSize(Shape s)
{
    switch (s) {
    case Shape::Sphere: return Layout::kSphereElemSize;
    case Shape::Box: return Layout::kBoxElemSize;
    case Shape::Sphyl: return Layout::kSphylElemSize;
    case Shape::Convex: return Layout::kConvexElemSize;
    default: return 0;
    }
}

inline Shape ArraySlotShape(size_t slotOffset)
{
    switch (slotOffset) {
    case Layout::kArraySphere: return Shape::Sphere;
    case Layout::kArrayBox: return Shape::Box;
    case Layout::kArraySphyl: return Shape::Sphyl;
    case Layout::kArrayConvex: return Shape::Convex;
    case Layout::kArrayTaperedCapsule: return Shape::TaperedCapsule;
    case Layout::kArrayLevelSet: return Shape::LevelSet;
    case Layout::kArraySkinnedLevelSet: return Shape::SkinnedLevelSet;
    case Layout::kArrayMLLevelSet: return Shape::MLLevelSet;
    case Layout::kArraySkinnedTriangleMesh: return Shape::SkinnedTriangleMesh;
    default: return Shape::Count;
    }
}

// ── Raw header / element views ──────────────────────────────────────────────
struct TArrayHeader
{
    uintptr_t data = 0;
    int32_t count = 0;
    int32_t max = 0;

    bool Empty() const { return count <= 0; }

    /**
     * Non-empty, sane count, and a plausible user-space pointer. Max is treated
     * as advisory: UE keeps Max >= Num, but a build that wrote 0 there must not
     * hide an otherwise readable array behind a false negative.
     */
    bool Usable() const
    {
        return data != 0 && count > 0 && count <= Layout::kMaxSaneArrayCount
            && (max == 0 || (max >= count && max <= Layout::kMaxSaneArrayCount))
            && data >= 0x10000;
    }
};

struct AggGeomHeader
{
    TArrayHeader sphere;
    TArrayHeader box;
    TArrayHeader sphyl;
    TArrayHeader convex;
    TArrayHeader taperedCapsule;
    TArrayHeader levelSet;
    TArrayHeader skinnedLevelSet;
    TArrayHeader mlLevelSet;
    TArrayHeader skinnedTriangleMesh;

    int32_t TotalElements() const
    {
        return sphere.count + box.count + sphyl.count + convex.count
            + taperedCapsule.count + levelSet.count + skinnedLevelSet.count
            + mlLevelSet.count + skinnedTriangleMesh.count;
    }

    /** Any array that claims elements this reader knows how to parse. */
    bool AnyParseable() const
    {
        return sphere.Usable() || box.Usable() || sphyl.Usable() || convex.Usable();
    }
};

/** Read a TArray header out of a raw block. No DMA, no alignment assumption. */
inline TArrayHeader ReadTArrayHeader(const uint8_t* p)
{
    TArrayHeader h;
    std::memcpy(&h.data, p, sizeof(h.data));
    std::memcpy(&h.count, p + 0x08, sizeof(h.count));
    std::memcpy(&h.max, p + 0x0C, sizeof(h.max));
    return h;
}

/** Decode all nine headers from a kHeaderBytes block. */
inline AggGeomHeader ParseAggGeomHeader(const uint8_t* block)
{
    AggGeomHeader h;
    if (!block)
        return h;
    h.sphere = ReadTArrayHeader(block + Layout::kArraySphere);
    h.box = ReadTArrayHeader(block + Layout::kArrayBox);
    h.sphyl = ReadTArrayHeader(block + Layout::kArraySphyl);
    h.convex = ReadTArrayHeader(block + Layout::kArrayConvex);
    h.taperedCapsule = ReadTArrayHeader(block + Layout::kArrayTaperedCapsule);
    h.levelSet = ReadTArrayHeader(block + Layout::kArrayLevelSet);
    h.skinnedLevelSet = ReadTArrayHeader(block + Layout::kArraySkinnedLevelSet);
    h.mlLevelSet = ReadTArrayHeader(block + Layout::kArrayMLLevelSet);
    h.skinnedTriangleMesh = ReadTArrayHeader(block + Layout::kArraySkinnedTriangleMesh);
    return h;
}

// ── Primitive ───────────────────────────────────────────────────────────────
/**
 * One collision element in the shape's own space. Axis-aligned in the element
 * frame: `halfExtents` is the half-size of the element's bounding box seen in
 * that frame, and `rotation` carries the frame's orientation, so world space is
 * component transform applied after rotation (see PrimitiveWorldAabb).
 *
 *   Sphere: center, halfExtents = (r, r, r), radius = r, no rotation.
 *   Box:    center, halfExtents = (X/2, Y/2, Z/2)  [X/Y/Z are FULL extents],
 *           rotation from the element Rotator.
 *   Sphyl:  center, radius, halfLength = Length/2  [Length is FULL],
 *           halfExtents = (r, r, r + halfLength), rotation from the Rotator.
 *   Convex: center/extent from ElemBox mapped through the element Transform,
 *           rotation from that Transform. The vertex cloud itself is not
 *           decoded yet, so this is a deliberately coarse stand-in: counts are
 *           reported so the caller can see how much geometry is behind it.
 */
struct Primitive
{
    Shape shape = Shape::Sphere;
    Vector3 center{};
    Vector3 halfExtents{};
    FQuat rotation{ 0.0, 0.0, 0.0, 1.0 };
    double radius = 0.0;      // sphere / capsule
    double halfLength = 0.0;  // capsule, along the element frame's +Z
    int vertexCount = 0;      // convex: VertexData count (points not yet read)
    int indexCount = 0;       // convex: IndexData count
    // convex: the hull itself in COMPONENT space (element scale, rotation and
    // translation applied). Empty until the point cloud has been read.
    std::vector<Vector3> hull;
    std::vector<uint32_t> hullIndices;  // triangle list into `hull`
    uint8_t collisionEnabled = 0;  // KShapeElem::CollisionEnabled

    bool Rotated() const
    {
        return shape == Shape::Box || shape == Shape::Sphyl || shape == Shape::Convex;
    }

    void LocalAabb(Vector3& outMin, Vector3& outMax) const;
};

/** ECollisionEnabled as named by this build's dump: 0 No, 1 Query, 2 Physics, 3 Both. */
inline bool CollisionEnabledIsKnown(uint8_t v) { return v <= 3; }

/**
 * Whether a shape can answer a query. Deliberately NOT applied while reading:
 * a build where the field is stale zero would otherwise delete every shape and
 * look like "no collision in the level" instead of a bad byte. The reader
 * reports the histogram; a consumer filters only once it has seen real data.
 */
inline bool CollisionEnabledQueries(uint8_t v) { return v == 1 || v == 3; }

// ── Little readers (memcpy, so no alignment or aliasing assumptions) ────────
inline double ReadDouble(const uint8_t* p)
{
    double v = 0.0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline float ReadFloat(const uint8_t* p)
{
    float v = 0.0f;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline uint8_t ReadByte(const uint8_t* p) { return *p; }

inline Vector3 ReadVector(const uint8_t* p)
{
    return Vector3(ReadDouble(p + 0x00), ReadDouble(p + 0x08), ReadDouble(p + 0x10));
}

inline double VectorMaxAbs(const Vector3& v)
{
    return (std::max)((std::max)(std::abs(v.x), std::abs(v.y)), std::abs(v.z));
}

inline bool IsFinite(const Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/**
 * FRotator (degrees, doubles: pitch 0x0, yaw 0x8, roll 0x10) to a unit FQuat.
 *
 * This is Unreal's FRotator::Quaternion (UnrealMath), in double precision, with
 * the half-angles in radians (degrees * PI / 360). Checked against the two cases
 * that pin the convention: yaw +90 rotates local +X onto world +Y, and pitch +90
 * rotates local +X onto world +Z (UE pitch looks up).
 */
inline FQuat RotatorToQuat(double pitchDeg, double yawDeg, double rollDeg)
{
    const double p = pitchDeg * M_PI / 360.0;
    const double y = yawDeg * M_PI / 360.0;
    const double r = rollDeg * M_PI / 360.0;

    const double SP = std::sin(p), CP = std::cos(p);
    const double SY = std::sin(y), CY = std::cos(y);
    const double SR = std::sin(r), CR = std::cos(r);

    FQuat q;
    q.x = CR * SP * SY - SR * CP * CY;
    q.y = -CR * SP * CY - SR * CP * SY;
    q.z = CR * CP * SY - SR * SP * CY;
    q.w = CR * CP * CY + SR * SP * SY;
    return q;
}

inline FQuat ReadRotatorQuat(const uint8_t* e, size_t rotatorOffset)
{
    return RotatorToQuat(
        ReadDouble(e + rotatorOffset + Layout::kRotator_Pitch),
        ReadDouble(e + rotatorOffset + Layout::kRotator_Yaw),
        ReadDouble(e + rotatorOffset + Layout::kRotator_Roll));
}

/**
 * Identity FTransform: FTransform{} is NOT one. Value-initializing the struct
 * zeroes Scale3D, and this reader scales before it rotates, so an empty
 * FTransform collapses every point onto the translation.
 */
inline FTransform IdentityTransform()
{
    FTransform t{};
    t.Rotation = FQuat{ 0.0, 0.0, 0.0, 1.0 };
    t.Translation = Vector3(0.0, 0.0, 0.0);
    t.Scale3D = Vector3(1.0, 1.0, 1.0);
    return t;
}

// ── Element parsers ─────────────────────────────────────────────────────────
inline Primitive ParseSphereElem(const uint8_t* e)
{
    Primitive p;
    p.shape = Shape::Sphere;
    p.collisionEnabled = ReadByte(e + Layout::kShapeElem_CollisionEnabled);
    p.center = ReadVector(e + Layout::kSphereElem_Center);
    p.radius = static_cast<double>(ReadFloat(e + Layout::kSphereElem_Radius));
    p.halfExtents = Vector3(p.radius, p.radius, p.radius);
    return p;
}

inline Primitive ParseBoxElem(const uint8_t* e)
{
    Primitive p;
    p.shape = Shape::Box;
    p.collisionEnabled = ReadByte(e + Layout::kShapeElem_CollisionEnabled);
    p.center = ReadVector(e + Layout::kBoxElem_Center);
    p.rotation = ReadRotatorQuat(e, Layout::kBoxElem_Rotation);
    // X/Y/Z are FULL extents on this build: halve once, here, and never again.
    const double x = static_cast<double>(ReadFloat(e + Layout::kBoxElem_X));
    const double y = static_cast<double>(ReadFloat(e + Layout::kBoxElem_Y));
    const double z = static_cast<double>(ReadFloat(e + Layout::kBoxElem_Z));
    p.halfExtents = Vector3(x * 0.5, y * 0.5, z * 0.5);
    return p;
}

inline Primitive ParseSphylElem(const uint8_t* e)
{
    Primitive p;
    p.shape = Shape::Sphyl;
    p.collisionEnabled = ReadByte(e + Layout::kShapeElem_CollisionEnabled);
    p.center = ReadVector(e + Layout::kSphylElem_Center);
    p.rotation = ReadRotatorQuat(e, Layout::kSphylElem_Rotation);
    p.radius = static_cast<double>(ReadFloat(e + Layout::kSphylElem_Radius));
    // Length is the FULL cylindrical section, so the half-length is Length/2.
    p.halfLength = static_cast<double>(ReadFloat(e + Layout::kSphylElem_Length)) * 0.5;
    p.halfExtents = Vector3(p.radius, p.radius, p.radius + p.halfLength);
    return p;
}

/** A KConvexElem's Transform at 0x90: the element's own frame -> component. */
struct ConvexTransform
{
    FQuat rot;
    Vector3 trans;
    Vector3 scale;
};

/**
 * Read the element Transform. Read as doubles through memcpy: `e` points into a
 * byte buffer, so it is never dereferenced as a struct pointer (alignment is the
 * caller's buffer).
 */
inline ConvexTransform ReadConvexTransform(const uint8_t* e)
{
    const uint8_t* xf = e + Layout::kConvexElem_Transform;
    ConvexTransform t;
    t.rot = FQuat{ ReadDouble(xf + 0x00), ReadDouble(xf + 0x08),
        ReadDouble(xf + 0x10), ReadDouble(xf + 0x18) };
    t.trans = Vector3(ReadDouble(xf + 0x20), ReadDouble(xf + 0x28), ReadDouble(xf + 0x30));
    t.scale = Vector3(ReadDouble(xf + 0x40), ReadDouble(xf + 0x48), ReadDouble(xf + 0x50));
    return t;
}

inline Primitive ParseConvexElem(const uint8_t* e)
{
    Primitive p;
    p.shape = Shape::Convex;
    p.collisionEnabled = ReadByte(e + Layout::kShapeElem_CollisionEnabled);

    const TArrayHeader verts = ReadTArrayHeader(e + Layout::kConvexElem_VertexData);
    const TArrayHeader idx = ReadTArrayHeader(e + Layout::kConvexElem_IndexData);
    p.vertexCount = verts.count > 0 ? verts.count : 0;
    p.indexCount = idx.count > 0 ? idx.count : 0;

    // ElemBox is min/max in the convex element's own space, and the element
    // Transform at 0x90 maps that space into the component.
    const Vector3 mn = ReadVector(e + Layout::kConvexElem_ElemBox + Layout::kBox_Min);
    const Vector3 mx = ReadVector(e + Layout::kConvexElem_ElemBox + Layout::kBox_Max);

    const ConvexTransform xf = ReadConvexTransform(e);
    const FQuat& rot = xf.rot;
    const Vector3& trans = xf.trans;
    const Vector3& scale = xf.scale;

    const Vector3 localCenter((mn.x + mx.x) * 0.5, (mn.y + mx.y) * 0.5, (mn.z + mx.z) * 0.5);
    Vector3 localExtent((mx.x - mn.x) * 0.5, (mx.y - mn.y) * 0.5, (mx.z - mn.z) * 0.5);
    localExtent = Vector3(std::abs(localExtent.x * scale.x),
        std::abs(localExtent.y * scale.y),
        std::abs(localExtent.z * scale.z));

    const Vector3 scaled(localCenter.x * scale.x,
        localCenter.y * scale.y,
        localCenter.z * scale.z);
    p.rotation = rot;
    p.center = rot.RotateVector(scaled) + trans;
    p.halfExtents = localExtent;
    return p;
}

inline std::vector<uint32_t> BuildHullTriangles(const std::vector<Vector3>& pts);

/** Reads `size` bytes of remote memory into `buffer`; false when refused. */
using RemoteRead = std::function<bool(uintptr_t address, void* buffer, size_t size)>;

/**
 * Read a convex element's point cloud and triangle list into component space. On
 * any failure the primitive keeps no hull, so the caller falls back to ElemBox
 * rather than trusting a partial read.
 */
inline void ReadConvexHull(const uint8_t* e, const RemoteRead& readRemote, Primitive& p)
{
    const TArrayHeader verts = ReadTArrayHeader(e + Layout::kConvexElem_VertexData);
    const TArrayHeader idx = ReadTArrayHeader(e + Layout::kConvexElem_IndexData);
    // IndexData is editor-only and cooked builds strip it: with no index the hull
    // is rebuilt from its point cloud. A present index may be 32-bit (editor,
    // typical cooked) or 16-bit (compact cooked), so both are tried, and a
    // triangle whose index is out of range is dropped rather than throwing away
    // the whole hull.
    if (verts.count <= 0 || verts.count > Layout::kMaxHullVertices || verts.data < 0x10000)
        return;

    // FVector is three doubles in this build (SDK: Vector, 0x18 bytes).
    std::vector<uint8_t> vbuf(static_cast<size_t>(verts.count) * 0x18);
    if (!readRemote(verts.data, vbuf.data(), vbuf.size()))
        return;

    const ConvexTransform xf = ReadConvexTransform(e);
    std::vector<Vector3> hull;
    hull.reserve(static_cast<size_t>(verts.count));
    for (int32_t i = 0; i < verts.count; ++i) {
        const uint8_t* v = vbuf.data() + static_cast<size_t>(i) * 0x18;
        const Vector3 local(ReadDouble(v), ReadDouble(v + 0x08), ReadDouble(v + 0x10));
        const Vector3 scaled(local.x * xf.scale.x, local.y * xf.scale.y, local.z * xf.scale.z);
        const Vector3 inComponent = xf.rot.RotateVector(scaled) + xf.trans;
        if (!IsFinite(inComponent))
            return;
        hull.push_back(inComponent);
    }

    // Decode `idx.count` indices of `elemSize` bytes each, keeping only triangles
    // every vertex of which is in range. Empty when none survive or the read fails.
    const auto decode = [&](size_t elemSize) -> std::vector<uint32_t> {
        std::vector<uint32_t> tris;
        std::vector<uint8_t> ibuf(static_cast<size_t>(idx.count) * elemSize);
        if (!readRemote(idx.data, ibuf.data(), ibuf.size()))
            return tris;
        const auto at = [&](int32_t k) -> uint32_t {
            if (elemSize == 2) {
                uint16_t u = 0;
                std::memcpy(&u, ibuf.data() + static_cast<size_t>(k) * 2, 2);
                return u;
            }
            uint32_t u = 0;
            std::memcpy(&u, ibuf.data() + static_cast<size_t>(k) * 4, 4);
            return u;
        };
        for (int32_t t = 0; t + 2 < idx.count; t += 3) {
            const uint32_t a = at(t);
            const uint32_t b = at(t + 1);
            const uint32_t c = at(t + 2);
            if (a >= static_cast<uint32_t>(verts.count) || b >= static_cast<uint32_t>(verts.count)
                || c >= static_cast<uint32_t>(verts.count))
                continue;
            tris.push_back(a);
            tris.push_back(b);
            tris.push_back(c);
        }
        return tris;
    };

    std::vector<uint32_t> tris;
    if (idx.count >= 3 && idx.count <= Layout::kMaxHullIndices && idx.data >= 0x10000) {
        tris = decode(sizeof(int32_t)); // int32 first: the common editor/cooked form
        if (tris.empty())
            tris = decode(sizeof(uint16_t)); // compact cooked form
    }
    // No usable index (stripped, malformed, or entirely out of range): rebuild the
    // hull from its point cloud rather than discard it.
    if (tris.empty())
        tris = BuildHullTriangles(hull);
    if (tris.empty())
        return;

    p.hull = std::move(hull);
    p.hullIndices = std::move(tris);
}

/**
 * Triangles (indices into `pts`) of the convex hull of a point cloud. Cooked
 * builds strip KConvexElem::IndexData, so the hull is rebuilt from its points.
 * Incremental hull, O(points * faces). Empty when the cloud is degenerate.
 */
inline std::vector<uint32_t> BuildHullTriangles(const std::vector<Vector3>& pts)
{
    struct Face { uint32_t v[3]; double n[3]; double d; };
    std::vector<uint32_t> out;
    const size_t n = pts.size();
    if (n < 4 || n > static_cast<size_t>(Layout::kMaxHullVertices))
        return out;

    double maxAbs = 1.0;
    for (const Vector3& p : pts)
        maxAbs = (std::max)(maxAbs, (std::max)(std::abs(p.x), (std::max)(std::abs(p.y), std::abs(p.z))));
    const double eps = 1e-9 * maxAbs;

    const auto sub = [&](uint32_t a, uint32_t b) {
        return std::array<double, 3>{ pts[a].x - pts[b].x, pts[a].y - pts[b].y, pts[a].z - pts[b].z };
    };
    const auto cross = [](const std::array<double, 3>& u, const std::array<double, 3>& v) {
        return std::array<double, 3>{ u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    };
    const auto dot = [](const std::array<double, 3>& u, const std::array<double, 3>& v) {
        return u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    };
    const auto len = [](const std::array<double, 3>& u) { return std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); };

    // Plane of a face, normal normalised; returns false for a sliver.
    const auto makeFace = [&](uint32_t a, uint32_t b, uint32_t c, Face& f) {
        const std::array<double, 3> nn = cross(sub(b, a), sub(c, a));
        const double l = len(nn);
        if (!(l > eps))
            return false;
        f.v[0] = a; f.v[1] = b; f.v[2] = c;
        f.n[0] = nn[0] / l; f.n[1] = nn[1] / l; f.n[2] = nn[2] / l;
        f.d = f.n[0] * pts[a].x + f.n[1] * pts[a].y + f.n[2] * pts[a].z;
        return true;
    };
    const auto signedDist = [&](const Face& f, uint32_t p) {
        return f.n[0] * pts[p].x + f.n[1] * pts[p].y + f.n[2] * pts[p].z - f.d;
    };

    // Initial tetrahedron from the widest-spread points.
    uint32_t i1 = 0;
    double best = -1.0;
    for (uint32_t i = 1; i < n; ++i) {
        const double l = len(sub(i, 0));
        if (l > best) { best = l; i1 = i; }
    }
    if (!(best > eps))
        return out;
    uint32_t i2 = 0;
    best = -1.0;
    const std::array<double, 3> e1 = sub(i1, 0);
    for (uint32_t i = 0; i < n; ++i) {
        const double l = len(cross(e1, sub(i, 0)));
        if (l > best) { best = l; i2 = i; }
    }
    if (!(best > eps))
        return out;
    const std::array<double, 3> nrm = cross(e1, sub(i2, 0));
    uint32_t i3 = 0;
    best = -1.0;
    for (uint32_t i = 0; i < n; ++i) {
        const double l = std::abs(dot(nrm, sub(i, 0)));
        if (l > best) { best = l; i3 = i; }
    }
    if (!(best > eps))
        return out;

    const uint32_t seed[4] = { 0, i1, i2, i3 };
    std::vector<Face> faces;
    const int quad[4][3] = { {0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3} };
    for (const auto& q : quad) {
        Face f{};
        const int opp = 6 - q[0] - q[1] - q[2];
        if (!makeFace(seed[q[0]], seed[q[1]], seed[q[2]], f))
            return out;
        // Orient outward: the fourth vertex must be on the negative side.
        if (signedDist(f, seed[opp]) > 0.0) {
            std::swap(f.v[1], f.v[2]);
            for (double& c : f.n) c = -c;
            f.d = -f.d;
        }
        faces.push_back(f);
    }

    for (uint32_t k = 0; k < n; ++k) {
        if (k == 0 || k == i1 || k == i2 || k == i3)
            continue;
        std::vector<size_t> visible;
        for (size_t fi = 0; fi < faces.size(); ++fi) {
            if (signedDist(faces[fi], k) > eps)
                visible.push_back(fi);
        }
        if (visible.empty())
            continue;
        // Directed edges of visible faces; a horizon edge's reverse is not among them.
        std::vector<std::pair<uint32_t, uint32_t>> edges;
        for (size_t fi : visible) {
            const Face& f = faces[fi];
            for (int e = 0; e < 3; ++e)
                edges.emplace_back(f.v[e], f.v[(e + 1) % 3]);
        }
        std::vector<std::pair<uint32_t, uint32_t>> horizon;
        for (const auto& ed : edges) {
            if (std::find(edges.begin(), edges.end(), std::make_pair(ed.second, ed.first)) == edges.end())
                horizon.push_back(ed);
        }
        std::vector<Face> kept;
        kept.reserve(faces.size());
        size_t vi = 0;
        for (size_t fi = 0; fi < faces.size(); ++fi) {
            if (vi < visible.size() && visible[vi] == fi) { ++vi; continue; }
            kept.push_back(faces[fi]);
        }
        faces.swap(kept);
        for (const auto& h : horizon) {
            Face nf{};
            if (makeFace(h.first, h.second, k, nf))
                faces.push_back(nf);
        }
    }

    out.reserve(faces.size() * 3);
    for (const Face& f : faces) {
        out.push_back(f.v[0]);
        out.push_back(f.v[1]);
        out.push_back(f.v[2]);
    }
    return out;
}

/** Parse one element, or a default-initialized Shape::Count primitive. */
inline Primitive ParseElem(Shape s, const uint8_t* e)
{
    switch (s) {
    case Shape::Sphere: return ParseSphereElem(e);
    case Shape::Box: return ParseBoxElem(e);
    case Shape::Sphyl: return ParseSphylElem(e);
    case Shape::Convex: return ParseConvexElem(e);
    default: {
        Primitive p;
        p.shape = Shape::Count;
        return p;
    }
    }
}

// ── Bounded batch parse ─────────────────────────────────────────────────────
/**
 * Supplies the raw bytes for one non-empty element array (`count *
 * ElementSize(shape)` of them), or nullptr when the caller's read failed or its
 * budget refused. The pointer only has to stay valid for the ParseAggGeom call.
 */
using ElementBytes = std::function<const uint8_t*(Shape shape, uintptr_t data, int32_t count)>;

struct ParseLimits
{
    int maxPerArray = 64;    // per shape, so one boxy mesh cannot starve the rest
    int maxPrimitives = 256; // per asset, across all shapes
    // Optional. When set, convex elements also get their point cloud and triangle
    // list read; when unset they keep the ElemBox stand-in only.
    RemoteRead readRemote;
};

struct ParseTally
{
    int declared[kShapeCount]{};
    int parsed[kShapeCount]{};
    int truncated[kShapeCount]{};
    int unsupported[kShapeCount]{};  // non-empty array with no decoded layout
    int readFailed[kShapeCount]{};
    int invalid[kShapeCount]{};      // parsed but not finite
    int totalDeclared = 0;
    int totalParsed = 0;
    int collisionEnabledHistogram[4]{};  // 0..3, the named ECollisionEnabled values
    int collisionEnabledOther = 0;       // byte was something else: treat as unknown
};

/**
 * Decode every array the header claims, appending to `out`. Element data comes
 * from `bytes`, so this stays pure. Bounded by `limits` on purpose: a hostile or
 * misread count must cost a bounded read, and truncation is recorded rather than
 * hidden.
 */
inline void ParseAggGeom(const AggGeomHeader& header,
    const ElementBytes& bytes,
    const ParseLimits& limits,
    std::vector<Primitive>& out,
    ParseTally& tally)
{
    tally = ParseTally{};

    struct Slot { size_t offset; Shape shape; const TArrayHeader* array; };
    const Slot slots[] = {
        { Layout::kArraySphere, Shape::Sphere, &header.sphere },
        { Layout::kArrayBox, Shape::Box, &header.box },
        { Layout::kArraySphyl, Shape::Sphyl, &header.sphyl },
        { Layout::kArrayConvex, Shape::Convex, &header.convex },
        { Layout::kArrayTaperedCapsule, Shape::TaperedCapsule, &header.taperedCapsule },
        { Layout::kArrayLevelSet, Shape::LevelSet, &header.levelSet },
        { Layout::kArraySkinnedLevelSet, Shape::SkinnedLevelSet, &header.skinnedLevelSet },
        { Layout::kArrayMLLevelSet, Shape::MLLevelSet, &header.mlLevelSet },
        { Layout::kArraySkinnedTriangleMesh, Shape::SkinnedTriangleMesh, &header.skinnedTriangleMesh },
    };

    // Declared counts are recorded from the header alone, before any read, so
    // "the metadata says 2000 boxes" is distinguishable from "we read 64".
    for (const Slot& s : slots) {
        if (s.array->count > 0 && s.array->count <= Layout::kMaxSaneArrayCount)
            tally.declared[static_cast<int>(s.shape)] = s.array->count;
    }
    tally.totalDeclared = header.TotalElements();

    const int perShape = limits.maxPerArray > 0 ? limits.maxPerArray : 1;
    const int maxPrims = limits.maxPrimitives > 0 ? limits.maxPrimitives : 1;

    for (const Slot& s : slots) {
        const int si = static_cast<int>(s.shape);
        if (s.array->count <= 0)
            continue;

        const size_t elemSize = ElementSize(s.shape);
        if (!elemSize) {
            tally.unsupported[si] = s.array->count;
            continue;
        }
        if (!s.array->Usable()) {
            tally.readFailed[si] = s.array->count;
            continue;
        }

        const int want = (std::min)((std::min)(s.array->count, perShape),
            (std::max)(0, maxPrims - static_cast<int>(out.size())));
        if (want <= 0) {
            tally.truncated[si] = s.array->count;
            continue;
        }

        const uint8_t* data = bytes ? bytes(s.shape, s.array->data, want) : nullptr;
        if (!data) {
            tally.readFailed[si] = want;
            continue;
        }
        if (want < s.array->count)
            tally.truncated[si] = s.array->count - want;

        for (int i = 0; i < want; ++i) {
            const uint8_t* e = data + static_cast<size_t>(i) * elemSize;
            Primitive p = ParseElem(s.shape, e);
            if (p.shape == Shape::Convex && limits.readRemote)
                ReadConvexHull(e, limits.readRemote, p);
            if (p.shape == Shape::Count || !IsFinite(p.center) || !IsFinite(p.halfExtents)) {
                ++tally.invalid[si];
                continue;
            }
            if (p.collisionEnabled <= 3)
                ++tally.collisionEnabledHistogram[p.collisionEnabled];
            else
                ++tally.collisionEnabledOther;
            out.push_back(p);
            ++tally.parsed[si];
            ++tally.totalParsed;
        }
    }
}

// ── Space conversion ────────────────────────────────────────────────────────
/** Local point -> component space -> world, matching FTransform::ToMatrixWithScale. */
inline Vector3 TransformPoint(const FTransform& t, const Vector3& local)
{
    const Vector3 scaled(local.x * t.Scale3D.x, local.y * t.Scale3D.y, local.z * t.Scale3D.z);
    return t.Rotation.RotateVector(scaled) + t.Translation;
}

/** AABB of one primitive's element-frame box in world space. */
inline void PrimitiveWorldAabb(const FTransform& component, const Primitive& p,
    Vector3& outMin, Vector3& outMax)
{
    outMin = Vector3(0.0, 0.0, 0.0);
    outMax = Vector3(0.0, 0.0, 0.0);

    for (int i = 0; i < 8; ++i) {
        const Vector3 corner(
            p.center.x + ((i & 1) ? p.halfExtents.x : -p.halfExtents.x),
            p.center.y + ((i & 2) ? p.halfExtents.y : -p.halfExtents.y),
            p.center.z + ((i & 4) ? p.halfExtents.z : -p.halfExtents.z));
        // Element frame first (its own rotation), then the component transform.
        const Vector3 inComponent = p.Rotated() ? p.rotation.RotateVector(corner) : corner;
        const Vector3 w = TransformPoint(component, inComponent);
        if (i == 0) {
            outMin = w;
            outMax = w;
        } else {
            outMin = Vector3((std::min)(outMin.x, w.x), (std::min)(outMin.y, w.y), (std::min)(outMin.z, w.z));
            outMax = Vector3((std::max)(outMax.x, w.x), (std::max)(outMax.y, w.y), (std::max)(outMax.z, w.z));
        }
    }
}

/** World-space AABB of every primitive in an asset, plus the count that resolved. */
inline int AssetWorldAabb(const FTransform& component, const std::vector<Primitive>& prims,
    Vector3& outMin, Vector3& outMax)
{
    int used = 0;
    for (const Primitive& p : prims) {
        Vector3 mn, mx;
        PrimitiveWorldAabb(component, p, mn, mx);
        if (!IsFinite(mn) || !IsFinite(mx))
            continue;
        if (used == 0) {
            outMin = mn;
            outMax = mx;
        } else {
            outMin = Vector3((std::min)(outMin.x, mn.x), (std::min)(outMin.y, mn.y), (std::min)(outMin.z, mn.z));
            outMax = Vector3((std::max)(outMax.x, mx.x), (std::max)(outMax.y, mx.y), (std::max)(outMax.z, mx.z));
        }
        ++used;
    }
    if (used == 0) {
        outMin = Vector3(0.0, 0.0, 0.0);
        outMax = Vector3(0.0, 0.0, 0.0);
    }
    return used;
}

/** Radius of the sphere around a world AABB's centre that contains it. */
inline double AabbBoundingRadius(const Vector3& mn, const Vector3& mx)
{
    const double dx = (mx.x - mn.x) * 0.5;
    const double dy = (mx.y - mn.y) * 0.5;
    const double dz = (mx.z - mn.z) * 0.5;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/** Bounded primitive-local AABB, for the in-app sanity checks. */
inline void Primitive::LocalAabb(Vector3& outMin, Vector3& outMax) const
{
    PrimitiveWorldAabb(IdentityTransform(), *this, outMin, outMax);
}

} // namespace CollisionGeom
