#pragma once
// Ported from reference visibleCheck.h ? Embree 4 ray-traced bone visibility +
// FKAggregateGeom decoding + dynamic obstructions + debug wireframe overlay.

#include <vector>
#include <string>
#include <cstdint>
#include <chrono>
#include <unordered_map>
#include <cmath>
#include <array>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <algorithm>
#include <limits>
#include <cstdio>

#include "ThirdParty/ImGui/imgui.h"
#include "Core/Memory.h"
#include "Core/Vector.hpp"
#include "Core/BoneMath.hpp"
#include "Core/Offsets.h"
#include "Core/Engine.h"
#include "Interface/Utils/UpgradeOptions.hpp"

#ifndef ARC_ENABLE_EMBREE
#define ARC_ENABLE_EMBREE 1
#endif

#if ARC_ENABLE_EMBREE
#ifndef EMBREE_STATIC_LIB
#define EMBREE_STATIC_LIB 1
#endif
#include <embree4/rtcore.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace EmbreeVis {

using Vec3 = Vector3;
using Quat = FQuat;

struct Rotator {
    double pitch = 0.0;
    double yaw   = 0.0;
    double roll  = 0.0;
};

struct EVec3 { float x, y, z; };

inline EVec3 ToEVec3(const Vec3& v) { return { (float)v.x, (float)v.y, (float)v.z }; }
inline EVec3 operator+(const EVec3& a, const EVec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline EVec3 operator-(const EVec3& a, const EVec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline EVec3 operator*(const EVec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }

inline bool valid_ptr(uintptr_t p) {
    return p != 0 && p != UINTPTR_MAX && (p & 0x7u) == 0 && p >= 0x10000ULL && p < 0x800000000000ULL;
}

inline bool is_finite_pos(const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
        && std::abs(p.x) < 1e8 && std::abs(p.y) < 1e8 && std::abs(p.z) < 1e8;
}

inline Vec3 ApplyTransform(const FTransform& t, Vec3 local) {
    Vec3 scaled(local.x * t.Scale3D.x, local.y * t.Scale3D.y, local.z * t.Scale3D.z);
    double tx = 2.0 * (t.Rotation.y * scaled.z - t.Rotation.z * scaled.y);
    double ty = 2.0 * (t.Rotation.z * scaled.x - t.Rotation.x * scaled.z);
    double tz = 2.0 * (t.Rotation.x * scaled.y - t.Rotation.y * scaled.x);
    Vec3 rotated(
        scaled.x + t.Rotation.w * tx + t.Rotation.y * tz - t.Rotation.z * ty,
        scaled.y + t.Rotation.w * ty + t.Rotation.z * tx - t.Rotation.x * tz,
        scaled.z + t.Rotation.w * tz + t.Rotation.x * ty - t.Rotation.y * tx
    );
    return rotated + t.Translation;
}

inline double DistSq(const Vec3& a, const Vec3& b) {
    double dx = a.x - b.x; double dy = a.y - b.y; double dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

inline Quat RotatorToQuat(const Rotator& r) {
    double p = r.pitch * (M_PI / 180.0);
    double y = r.yaw * (M_PI / 180.0);
    double rl = r.roll * (M_PI / 180.0);
    double sp = std::sin(p * 0.5), cp = std::cos(p * 0.5);
    double sy = std::sin(y * 0.5), cy = std::cos(y * 0.5);
    double sr = std::sin(rl * 0.5), cr = std::cos(rl * 0.5);
    return {
        cr * sp * sy - sr * cp * cy,
        -(cr * sp * cy - sr * cp * sy),
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy
    };
}

// ============================================================================
//  OFFSETS (from Desktop/New Text Document (2).txt + message.txt)
// ============================================================================
namespace VisibilityOffsets
{
    constexpr int32_t UWorld_Levels = 0x2D8;
    constexpr int32_t ULevel_Actors = static_cast<int32_t>(Offsets::AActors);
    constexpr int32_t AActor_RootComponent = static_cast<int32_t>(Offsets::RootComponent);
    constexpr int32_t UPrimitiveComponent_ComponentToWorld = static_cast<int32_t>(Offsets::ComponentToWorld);
    constexpr int32_t SceneComponent_AttachChildren = 0x210;
    constexpr int32_t UStaticMeshComponent_StaticMesh = 0x6E8;
    constexpr int32_t UStaticMesh_BodySetup = 0x248;
    constexpr int32_t UBodySetup_AggGeom = 0xB0;

    constexpr int32_t AggGeom_Sphere = 0x00;
    constexpr int32_t AggGeom_Box    = 0x10;
    constexpr int32_t AggGeom_Sphyl  = 0x20;
    constexpr int32_t AggGeom_Convex = 0x30;
    // UE5 appends TaperedCapsuleElems after ConvexElems (AggregateGeom.h). We
    // ignored it, so any mesh collided only by tapered capsules read as air.
    constexpr int32_t AggGeom_TaperedCapsule = 0x40;
    constexpr int32_t ShapeElem_Base = 0x30;

    constexpr int32_t Convex_VertexData = 0x00;
    constexpr int32_t Convex_IndexData  = 0x10;
    constexpr int32_t Convex_Transform  = 0x60;

    // FKTaperedCapsuleElem: Center +0x00, Rotation +0x18, Radius0 +0x30,
    // Radius1 +0x34, Length +0x38 (segment only; total = Length + R0 + R1).
    constexpr int32_t Tapered_Radius0 = 0x30;
    constexpr int32_t Tapered_Radius1 = 0x34;
    constexpr int32_t Tapered_Length  = 0x38;

    constexpr int32_t Stride_Sphere = 0x50;
    constexpr int32_t Stride_Box    = 0x70;
    constexpr int32_t Stride_Sphyl  = 0x68;
    constexpr int32_t Stride_Convex = 0x110;
    constexpr int32_t Stride_TaperedCapsule = 0x70;
}

// ============================================================================
//  GEOMETRY UTILS & CACHING
// ============================================================================
struct CachedShape {
    std::vector<EVec3> verts;
    std::vector<uint32_t> indices;
};

struct ArrHeader { uintptr_t data; int32_t count; int32_t max; };

inline std::shared_mutex g_geom_cache_mutex;
inline std::unordered_map<uintptr_t, std::vector<CachedShape>> g_geom_cache;
inline std::atomic<uint32_t> g_vis_generation{ 0 };
inline std::atomic<bool> g_visibility_meshes_ready{ false };
inline std::atomic<int> g_last_mesh_actors{ 0 };
inline std::atomic<int> g_last_triangles{ 0 };
inline bool g_vis_log_filter_once = false;

namespace GeoUtils {
    inline void AddTriangle(std::vector<uint32_t>& indices, uint32_t base, uint32_t a, uint32_t b, uint32_t c) {
        indices.push_back(base + a); indices.push_back(base + b); indices.push_back(base + c);
    }

    inline void TessBox(const Vec3& halfExt, std::vector<EVec3>& verts, std::vector<uint32_t>& indices, const FTransform* localTransform = nullptr) {
        uint32_t base = (uint32_t)verts.size();
        float sx = (float)halfExt.x, sy = (float)halfExt.y, sz = (float)halfExt.z;
        const EVec3 corners[8] = {
            {-sx,-sy,-sz}, {+sx,-sy,-sz}, {+sx,+sy,-sz}, {-sx,+sy,-sz},
            {-sx,-sy,+sz}, {+sx,-sy,+sz}, {+sx,+sy,+sz}, {-sx,+sy,+sz}
        };
        for (int i = 0; i < 8; i++) {
            Vec3 v = { (double)corners[i].x, (double)corners[i].y, (double)corners[i].z };
            if (localTransform) v = ApplyTransform(*localTransform, v);
            verts.push_back(ToEVec3(v));
        }
        auto Q = [&](int a, int b, int c, int d) {
            AddTriangle(indices, base, a, b, c);
            AddTriangle(indices, base, a, c, d);
        };
        Q(0, 1, 2, 3); Q(4, 7, 6, 5); Q(0, 4, 5, 1);
        Q(3, 2, 6, 7); Q(0, 3, 7, 4); Q(1, 5, 6, 2);
    }

    // Icosphere, subdivided (midpoint split). The bare icosahedron (20 tris)
    // deviates from a true sphere by ~20% of the radius, which lets rays clip
    // through solid rock/barrel collision. 2 levels = 320 tris, ~1% deviation
    // -- the value Embree users converge on; more is wasted on a vischeck.
    inline void TessSphere(float radius, std::vector<EVec3>& verts, std::vector<uint32_t>& indices, int subdivisions = 2) {
        constexpr float X = 0.525731112f, Z = 0.850650808f;
        std::vector<EVec3> v = {
            {-X,0.f,Z}, {X,0.f,Z}, {-X,0.f,-Z}, {X,0.f,-Z},
            {0.f,Z,X}, {0.f,Z,-X}, {0.f,-Z,X}, {0.f,-Z,-X},
            {Z,X,0.f}, {-Z,X,0.f}, {Z,-X,0.f}, {-Z,-X,0.f}
        };
        std::vector<uint32_t> tri = {
            0,4,1, 0,9,4, 9,5,4, 4,5,8, 4,8,1, 8,10,1, 8,3,10, 5,3,8, 5,2,3, 2,7,3,
            7,10,3, 7,6,10, 7,11,6, 11,0,6, 0,1,6, 6,1,10, 9,0,11, 9,11,2, 9,2,5, 7,2,11
        };

        for (int sub = 0; sub < subdivisions; sub++) {
            std::vector<uint32_t> next;
            next.reserve(tri.size() * 4);
            std::unordered_map<uint64_t, uint32_t> mids;
            auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
                const uint64_t key = ((uint64_t)(a < b ? a : b) << 32) | (uint32_t)(a < b ? b : a);
                auto it = mids.find(key);
                if (it != mids.end()) return it->second;
                EVec3 m = v[a] + v[b];
                const float len = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
                if (len > 1e-20f) { m.x /= len; m.y /= len; m.z /= len; }
                const uint32_t idx = (uint32_t)v.size();
                v.push_back(m);
                mids.emplace(key, idx);
                return idx;
            };
            for (size_t i = 0; i + 2 < tri.size(); i += 3) {
                const uint32_t a = tri[i], b = tri[i + 1], c = tri[i + 2];
                const uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
                const uint32_t quad[12] = { a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca };
                next.insert(next.end(), quad, quad + 12);
            }
            tri.swap(next);
        }

        uint32_t base = (uint32_t)verts.size();
        for (const auto& p : v) verts.push_back(p * radius);
        for (uint32_t i : tri) indices.push_back(base + i);
    }

    // Real capsule: cylinder from -halfHeight..+halfHeight with two hemisphere
    // caps, 12 circumference segments x 6 rings per hemisphere (~288 tris).
    // The old stand-in was a plain box whose corners block rays where a real
    // capsule has only air.
    inline void TessCapsule(float radius, float halfHeight, std::vector<EVec3>& verts, std::vector<uint32_t>& indices, const FTransform* localT = nullptr) {
        if (radius <= 0.f) return;
        constexpr int SEG = 12;
        constexpr int RING = 6;
        auto push = [&](float x, float y, float z) {
            Vec3 local = { (double)x, (double)y, (double)z };
            if (localT) local = ApplyTransform(*localT, local);
            verts.push_back(ToEVec3(local));
        };
        auto ring = [&](float z, float r) {
            std::vector<uint32_t> out;
            out.reserve(SEG);
            for (int s = 0; s < SEG; s++) {
                const float th = 2.f * (float)M_PI * (float)s / (float)SEG;
                out.push_back((uint32_t)verts.size());
                push(r * std::cos(th), r * std::sin(th), z);
            }
            return out;
        };
        auto strip = [&](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
            for (int s = 0; s < SEG; s++) {
                const uint32_t a0 = a[s], a1 = a[(s + 1) % SEG];
                const uint32_t b0 = b[s], b1 = b[(s + 1) % SEG];
                const uint32_t quad[6] = { a0, b0, a1, a1, b0, b1 };
                indices.insert(indices.end(), quad, quad + 6);
            }
        };
        auto fan = [&](uint32_t pole, const std::vector<uint32_t>& r) {
            for (int s = 0; s < SEG; s++) {
                const uint32_t tri[3] = { pole, r[s], r[(s + 1) % SEG] };
                indices.insert(indices.end(), tri, tri + 3);
            }
        };

        const uint32_t topPole = (uint32_t)verts.size();
        push(0.f, 0.f, halfHeight + radius);
        std::vector<std::vector<uint32_t>> topRings;
        for (int r = 1; r < RING; r++) {
            const float phi = (float)M_PI * 0.5f * (float)r / (float)RING;
            topRings.push_back(ring(halfHeight + radius * std::cos(phi), radius * std::sin(phi)));
        }
        const std::vector<uint32_t> topEq = ring(halfHeight, radius);
        const std::vector<uint32_t> botEq = ring(-halfHeight, radius);
        std::vector<std::vector<uint32_t>> botRings;
        for (int r = RING - 1; r >= 1; r--) {
            const float phi = (float)M_PI * 0.5f * (float)r / (float)RING;
            botRings.push_back(ring(-halfHeight - radius * std::cos(phi), radius * std::sin(phi)));
        }
        const uint32_t botPole = (uint32_t)verts.size();
        push(0.f, 0.f, -halfHeight - radius);

        fan(topPole, topRings.front());
        for (size_t r = 0; r + 1 < topRings.size(); r++) strip(topRings[r], topRings[r + 1]);
        strip(topRings.back(), topEq);
        strip(topEq, botEq);
        strip(botEq, botRings.front());
        for (size_t r = 0; r + 1 < botRings.size(); r++) strip(botRings[r], botRings[r + 1]);
        fan(botPole, botRings.back());
    }

    // Tapered capsule: convex hull of two end spheres (r0 at +Z, r1 at -Z,
    // centres halfLength apart). Surface = two spherical caps plus the
    // external tangent cone between them, so no tangent-region leak like a
    // hemisphere-plus-frustum approximation would have.
    inline void TessTaperedCapsule(float r0, float r1, float halfLength, std::vector<EVec3>& verts, std::vector<uint32_t>& indices, const FTransform* localT = nullptr) {
        if (r0 <= 0.f || r1 <= 0.f) return;
        if (!(halfLength > 1e-3f)) { TessSphere((std::max)(r0, r1), verts, indices); return; }
        constexpr int SEG = 12;
        constexpr int RING = 6;
        const float L = 2.f * halfLength;
        const float cosA = std::clamp((r0 - r1) / L, -1.f, 1.f);
        const float sinA = std::sqrt((std::max)(0.f, 1.f - cosA * cosA));
        const float thetaMax = (float)M_PI - std::acos(cosA);   // pole -> contact circle

        auto push = [&](float x, float y, float z) {
            Vec3 local = { (double)x, (double)y, (double)z };
            if (localT) local = ApplyTransform(*localT, local);
            verts.push_back(ToEVec3(local));
        };
        auto ringAt = [&](float polar, float centerZ, float rad) {
            std::vector<uint32_t> out;
            out.reserve(SEG);
            const float rr = rad * std::sin(polar);
            const float zz = centerZ + rad * std::cos(polar);
            for (int s = 0; s < SEG; s++) {
                const float th = 2.f * (float)M_PI * (float)s / (float)SEG;
                out.push_back((uint32_t)verts.size());
                push(rr * std::cos(th), rr * std::sin(th), zz);
            }
            return out;
        };
        auto strip = [&](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
            for (int s = 0; s < SEG; s++) {
                const uint32_t a0 = a[s], a1 = a[(s + 1) % SEG];
                const uint32_t b0 = b[s], b1 = b[(s + 1) % SEG];
                const uint32_t quad[6] = { a0, b0, a1, a1, b0, b1 };
                indices.insert(indices.end(), quad, quad + 6);
            }
        };
        auto fan = [&](uint32_t pole, const std::vector<uint32_t>& r) {
            for (int s = 0; s < SEG; s++) {
                const uint32_t tri[3] = { pole, r[s], r[(s + 1) % SEG] };
                indices.insert(indices.end(), tri, tri + 3);
            }
        };

        const uint32_t topPole = (uint32_t)verts.size();
        push(0.f, 0.f, halfLength + r0);
        std::vector<std::vector<uint32_t>> topRings;
        for (int k = 1; k <= RING; k++)
            topRings.push_back(ringAt(thetaMax * (float)k / (float)RING, halfLength, r0));
        const uint32_t botPole = (uint32_t)verts.size();
        push(0.f, 0.f, -halfLength - r1);
        std::vector<std::vector<uint32_t>> botRings;
        for (int k = 1; k <= RING; k++)
            botRings.push_back(ringAt((float)M_PI - thetaMax * (float)k / (float)RING, -halfLength, r1));

        fan(topPole, topRings.front());
        for (size_t r = 0; r + 1 < topRings.size(); r++) strip(topRings[r], topRings[r + 1]);
        strip(topRings.back(), botRings.back());
        for (size_t r = botRings.size(); r-- > 1; ) strip(botRings[r], botRings[r - 1]);
        fan(botPole, botRings.front());
    }

    // Exact 3D incremental convex hull for cooked FKConvexElem vertex clouds
    // that dropped their triangle indices. Returns false if the point set is
    // flat/degenerate so the caller can fall back to a local bounding box.
    inline bool TessConvexHull3D(const std::vector<Vec3>& rawVerts,
                                 const FTransform& localT,
                                 std::vector<EVec3>& verts,
                                 std::vector<uint32_t>& indices) {
        if (rawVerts.size() < 4) return false;

        std::vector<Vec3> pts;
        pts.reserve((std::min)(rawVerts.size(), (size_t)256));
        const size_t step = rawVerts.size() > 256 ? (rawVerts.size() / 256) : 1;
        for (size_t i = 0; i < rawVerts.size() && pts.size() < 256; i += step) {
            const Vec3 p = ApplyTransform(localT, rawVerts[i]);
            if (!is_finite_pos(p)) continue;
            bool dup = false;
            for (const auto& q : pts) {
                if (DistSq(p, q) < 1e-2) { dup = true; break; }
            }
            if (!dup) pts.push_back(p);
        }
        if (pts.size() < 4) return false;

        auto sub = [](const Vec3& a, const Vec3& b) -> Vec3 {
            return { a.x - b.x, a.y - b.y, a.z - b.z };
        };
        auto dot = [](const Vec3& a, const Vec3& b) -> double {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        auto cross = [](const Vec3& a, const Vec3& b) -> Vec3 {
            return {
                a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x
            };
        };

        // 1. Find initial non-degenerate tetrahedron (i0, i1, i2, i3)
        size_t i0 = 0, i1 = 1;
        double bestDistSq = -1.0;
        for (size_t i = 0; i < pts.size(); ++i) {
            for (size_t j = i + 1; j < pts.size(); ++j) {
                const double d2 = DistSq(pts[i], pts[j]);
                if (d2 > bestDistSq) { bestDistSq = d2; i0 = i; i1 = j; }
            }
        }
        if (bestDistSq < 1.0) return false;

        const Vec3 edge01 = sub(pts[i1], pts[i0]);
        size_t i2 = 0;
        double bestAreaSq = -1.0;
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i == i0 || i == i1) continue;
            const Vec3 cr = cross(edge01, sub(pts[i], pts[i0]));
            const double a2 = dot(cr, cr);
            if (a2 > bestAreaSq) { bestAreaSq = a2; i2 = i; }
        }
        if (bestAreaSq < 1.0) return false;

        const Vec3 baseN = cross(edge01, sub(pts[i2], pts[i0]));
        size_t i3 = 0;
        double bestVol = -1.0;
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i == i0 || i == i1 || i == i2) continue;
            const double v = std::abs(dot(baseN, sub(pts[i], pts[i0])));
            if (v > bestVol) { bestVol = v; i3 = i; }
        }
        if (bestVol < 1.0) return false;

        const Vec3 interior{
            (pts[i0].x + pts[i1].x + pts[i2].x + pts[i3].x) * 0.25,
            (pts[i0].y + pts[i1].y + pts[i2].y + pts[i3].y) * 0.25,
            (pts[i0].z + pts[i1].z + pts[i2].z + pts[i3].z) * 0.25
        };

        struct HullFace { uint32_t a, b, c; Vec3 n; double d; };
        auto makeFace = [&](uint32_t a, uint32_t b, uint32_t c) -> HullFace {
            Vec3 n = cross(sub(pts[b], pts[a]), sub(pts[c], pts[a]));
            const double len = std::sqrt(dot(n, n));
            if (len > 1e-12) { n.x /= len; n.y /= len; n.z /= len; }
            if (dot(n, sub(interior, pts[a])) > 0.0) {
                std::swap(b, c);
                n.x = -n.x; n.y = -n.y; n.z = -n.z;
            }
            return { a, b, c, n, dot(n, pts[a]) };
        };

        std::vector<HullFace> faces;
        faces.reserve(pts.size() * 2);
        faces.push_back(makeFace((uint32_t)i0, (uint32_t)i1, (uint32_t)i2));
        faces.push_back(makeFace((uint32_t)i0, (uint32_t)i1, (uint32_t)i3));
        faces.push_back(makeFace((uint32_t)i1, (uint32_t)i2, (uint32_t)i3));
        faces.push_back(makeFace((uint32_t)i2, (uint32_t)i0, (uint32_t)i3));

        // 2. Incrementally insert remaining vertices
        std::vector<uint8_t> vis;
        std::vector<std::pair<uint32_t, uint32_t>> visEdges;
        for (size_t pi = 0; pi < pts.size(); ++pi) {
            if (pi == i0 || pi == i1 || pi == i2 || pi == i3) continue;
            const Vec3& p = pts[pi];
            vis.assign(faces.size(), 0);
            visEdges.clear();
            bool anyVis = false;
            for (size_t fi = 0; fi < faces.size(); ++fi) {
                if (dot(faces[fi].n, p) - faces[fi].d > 1e-2) {
                    vis[fi] = 1;
                    anyVis = true;
                    visEdges.push_back({ faces[fi].a, faces[fi].b });
                    visEdges.push_back({ faces[fi].b, faces[fi].c });
                    visEdges.push_back({ faces[fi].c, faces[fi].a });
                }
            }
            if (!anyVis) continue;

            // Horizon edges: directed edges (u, v) of visible faces whose reverse
            // (v, u) is not in visEdges.
            std::vector<std::pair<uint32_t, uint32_t>> horizon;
            for (const auto& e : visEdges) {
                bool revVis = false;
                for (const auto& r : visEdges) {
                    if (r.first == e.second && r.second == e.first) {
                        revVis = true;
                        break;
                    }
                }
                if (!revVis) horizon.push_back(e);
            }
            if (horizon.size() < 3) continue;

            size_t keepIdx = 0;
            for (size_t fi = 0; fi < faces.size(); ++fi) {
                if (!vis[fi]) {
                    if (keepIdx != fi) faces[keepIdx] = faces[fi];
                    ++keepIdx;
                }
            }
            faces.resize(keepIdx);

            for (const auto& he : horizon)
                faces.push_back(makeFace(he.first, he.second, (uint32_t)pi));
        }

        if (faces.size() < 4) return false;
        const uint32_t base = (uint32_t)verts.size();
        for (const auto& p : pts) verts.push_back(ToEVec3(p));
        for (const auto& f : faces) {
            indices.push_back(base + f.a);
            indices.push_back(base + f.b);
            indices.push_back(base + f.c);
        }
        return true;
    }
}

// ============================================================================
//  EMBREE MANAGER
// ============================================================================

#if ARC_ENABLE_EMBREE
class EmbreeSceneManager {
public:
    EmbreeSceneManager() { device = rtcNewDevice("verbose=0"); }
    ~EmbreeSceneManager() {
        if (scenes[0]) rtcReleaseScene(scenes[0]);
        if (scenes[1]) rtcReleaseScene(scenes[1]);
        if (device) rtcReleaseDevice(device);
    }

    struct ScopedScene {
        RTCScene scene;
        std::shared_lock<std::shared_mutex> lock;
        operator bool() const { return scene != nullptr; }
    };
    ScopedScene AcquireScene() {
        std::shared_lock<std::shared_mutex> lock(scene_lifetime_mutex);
        RTCScene s = scenes[active_index.load(std::memory_order_acquire)];
        return { s, std::move(lock) };
    }

    // Debug draw reads the active scene's buffers directly (no second copy kept).
    void GetDebugGeometry(std::vector<EVec3>& out_v, std::vector<uint32_t>& out_i) {
        std::shared_lock<std::shared_mutex> life(scene_lifetime_mutex);
        const int idx = active_index.load(std::memory_order_acquire);
        if (!scenes[idx]) { out_v.clear(); out_i.clear(); return; }
        out_v = scene_verts[idx]; out_i = scene_indices[idx];
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(build_mutex);
        std::unique_lock<std::shared_mutex> life(scene_lifetime_mutex);
        int idx = active_index.load(std::memory_order_relaxed);
        if (scenes[idx]) { rtcReleaseScene(scenes[idx]); scenes[idx] = nullptr; }
        int other = 1 - idx;
        if (scenes[other]) { rtcReleaseScene(scenes[other]); scenes[other] = nullptr; }
        scene_verts[0].clear(); scene_verts[1].clear();
        scene_indices[0].clear(); scene_indices[1].clear();
    }

    // Takes ownership of the buffers (no extra copies before the scene is built).
    void UpdateFromBuffers(std::vector<EVec3> verts, std::vector<uint32_t> indices) {
        if (!device || verts.empty() || indices.size() < 3) return;
        std::lock_guard<std::mutex> clock(build_mutex);
        int write = 1 - active_index.load(std::memory_order_relaxed);

        RTCScene old_scene = scenes[write];
        if (old_scene) {
            std::unique_lock<std::shared_mutex> life(scene_lifetime_mutex);
            scenes[write] = nullptr;
            life.unlock();
            rtcReleaseScene(old_scene);
        }

        scene_verts[write] = std::move(verts);
        scene_indices[write] = std::move(indices);

        const size_t vcount = scene_verts[write].size();
        {
            auto& idx = scene_indices[write];
            size_t out = 0;
            for (size_t i = 0; i + 2 < idx.size(); i += 3) {
                uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
                if (a >= vcount || b >= vcount || c >= vcount) continue;
                idx[out++] = a; idx[out++] = b; idx[out++] = c;
            }
            idx.resize(out);
            if (idx.size() < 3) return;
        }

        RTCScene new_scene = rtcNewScene(device);
        if (!new_scene) return;
        // WATERTIGHT traversal. Without ROBUST the fast mode lets a ray slip
        // through the shared edge of two neighbouring triangles and a solid
        // wall reads as an opening (the "wall shows invisible as visible"
        // report). The scene is static and committed once, so HIGH build
        // quality (spatial splits) is a one-time cost with the best traversal.
        rtcSetSceneFlags(new_scene, RTC_SCENE_FLAG_ROBUST);
        rtcSetSceneBuildQuality(new_scene, RTC_BUILD_QUALITY_HIGH);
        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        if (!geom) { rtcReleaseScene(new_scene); return; }
        rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, scene_verts[write].data(), 0, 3 * sizeof(float), scene_verts[write].size());
        rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, scene_indices[write].data(), 0, 3 * sizeof(uint32_t), scene_indices[write].size() / 3);
        rtcCommitGeometry(geom);
        rtcAttachGeometry(new_scene, geom);
        rtcReleaseGeometry(geom);
        rtcCommitScene(new_scene);

        {
            std::unique_lock<std::shared_mutex> life(scene_lifetime_mutex);
            scenes[write] = new_scene;
            active_index.store(write, std::memory_order_release);
        }
        g_last_triangles.store(static_cast<int>(scene_indices[write].size() / 3), std::memory_order_relaxed);
    }

private:
    RTCDevice device = nullptr;
    RTCScene scenes[2] = { nullptr, nullptr };
    std::atomic<int> active_index{ 0 };
    std::mutex build_mutex;
    std::shared_mutex scene_lifetime_mutex;
    std::vector<EVec3> scene_verts[2];
    std::vector<uint32_t> scene_indices[2];
};
#else
class EmbreeSceneManager {
public:
    struct ScopedScene { void* scene = nullptr; operator bool() const { return false; } };
    ScopedScene AcquireScene() { return {}; }
    void Clear() {}
    void UpdateFromBuffers(const std::vector<EVec3>&, const std::vector<uint32_t>&) {}
    void GetDebugGeometry(std::vector<EVec3>& out_v, std::vector<uint32_t>& out_i) { out_v.clear(); out_i.clear(); }
};
#endif

inline EmbreeSceneManager g_embree_manager;

// ============================================================================
//  DYNAMIC OBSTRUCTIONS ? smokes, shields, doors, abilities
// ============================================================================

enum ObsType : uint8_t { OBS_SPHERE, OBS_BOX };

struct ObstructionShape {
    Vec3      center;      // world space
    ObsType   type;
    float     radius;      // sphere radius (or bounding radius for quick cull)
    Vec3      half_ext;    // box half-extents (local space, OBS_BOX only)
    Quat      rotation;    // world rotation  (OBS_BOX only)
    uintptr_t actor;
    uint32_t  last_frame;
};

inline std::vector<ObstructionShape> g_obstruction_buf[2];
inline std::atomic<int>  g_obstruction_active{0};
inline std::mutex        g_obstruction_build_mutex;
inline std::unordered_map<uintptr_t, ObstructionShape> g_obstruction_map;
inline std::atomic<uint32_t> g_obstruction_frame{0};

struct GeomInfo {
    ObsType type;
    Vec3    local_center;
    Vec3    half_ext;
    Rotator local_rot;
    float   radius;
};

struct ObstructionClass { bool match; float fallback_r; GeomInfo geom; };
inline std::unordered_map<uintptr_t, ObstructionClass> g_obs_fname_cache;

inline GeomInfo ReadActorGeom(uintptr_t actor) {
    GeomInfo out = { OBS_SPHERE, {0,0,0}, {0,0,0}, {0,0,0}, 0 };

    uintptr_t root = Memory::read<uintptr_t>(actor + VisibilityOffsets::AActor_RootComponent);
    if (!valid_ptr(root)) return out;
    uintptr_t mesh = Memory::read<uintptr_t>(root + VisibilityOffsets::UStaticMeshComponent_StaticMesh);
    if (!valid_ptr(mesh)) return out;
    uintptr_t body = Memory::read<uintptr_t>(mesh + VisibilityOffsets::UStaticMesh_BodySetup);
    if (!valid_ptr(body)) return out;

    uintptr_t aggBase = body + VisibilityOffsets::UBodySetup_AggGeom;
    ArrHeader boxes   = Memory::read<ArrHeader>(aggBase + VisibilityOffsets::AggGeom_Box);
    ArrHeader spheres = Memory::read<ArrHeader>(aggBase + VisibilityOffsets::AggGeom_Sphere);

    if (boxes.count > 0 && boxes.count < 64 && valid_ptr(boxes.data)) {
        std::vector<uint8_t> buf(boxes.count * VisibilityOffsets::Stride_Box);
        Memory::ReadRaw(boxes.data, buf.data(), buf.size());
        float bestVol = 0;
        for (int i = 0; i < boxes.count; i++) {
            uint8_t* p = buf.data() + i * VisibilityOffsets::Stride_Box + VisibilityOffsets::ShapeElem_Base;
            Vec3 center  = *(Vec3*)p;
            Rotator rot  = *(Rotator*)(p + 0x18);
            float hx = *(float*)(p + 0x30), hy = *(float*)(p + 0x34), hz = *(float*)(p + 0x38);
            float vol = hx * hy * hz;
            if (vol > bestVol) {
                bestVol = vol;
                out.type         = OBS_BOX;
                out.local_center = center;
                out.half_ext     = { (double)hx, (double)hy, (double)hz };
                out.local_rot    = rot;
                out.radius       = std::sqrt(hx*hx + hy*hy + hz*hz);
            }
        }
        if (bestVol > 0) return out;
    }

    if (spheres.count > 0 && spheres.count < 64 && valid_ptr(spheres.data)) {
        std::vector<uint8_t> buf(spheres.count * VisibilityOffsets::Stride_Sphere);
        Memory::ReadRaw(spheres.data, buf.data(), buf.size());
        float bestR = 0;
        for (int i = 0; i < spheres.count; i++) {
            uint8_t* p = buf.data() + i * VisibilityOffsets::Stride_Sphere + VisibilityOffsets::ShapeElem_Base;
            Vec3 center = *(Vec3*)p;
            float rad   = *(float*)(p + 0x18);
            if (rad > bestR) {
                bestR = rad;
                out.type         = OBS_SPHERE;
                out.local_center = center;
                out.radius       = rad;
            }
        }
    }

    return out;
}

inline Quat QuatInverse(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }

inline Vec3 QuatRotateVec(const Quat& q, const Vec3& v) {
    double tx = 2.0 * (q.y * v.z - q.z * v.y);
    double ty = 2.0 * (q.z * v.x - q.x * v.z);
    double tz = 2.0 * (q.x * v.y - q.y * v.x);
    return {
        v.x + q.w * tx + q.y * tz - q.z * ty,
        v.y + q.w * ty + q.z * tx - q.x * tz,
        v.z + q.w * tz + q.x * ty - q.y * tx
    };
}

inline Quat QuatMul(const Quat& a, const Quat& b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}

inline bool RaySegIntersectsOBB(const Vec3& org, const Vec3& dir, double len,
                                const Vec3& box_center, const Vec3& half_ext, const Quat& box_rot) {
    Quat inv = QuatInverse(box_rot);
    Vec3 local_org = QuatRotateVec(inv, Vec3{ org.x - box_center.x, org.y - box_center.y, org.z - box_center.z });
    Vec3 local_dir = QuatRotateVec(inv, dir);

    double tmin = 0.0, tmax = len;
    double axes[3] = { local_org.x, local_org.y, local_org.z };
    double dirs[3] = { local_dir.x, local_dir.y, local_dir.z };
    double exts[3] = { half_ext.x,  half_ext.y,  half_ext.z  };

    for (int i = 0; i < 3; i++) {
        if (std::abs(dirs[i]) < 1e-12) {
            if (axes[i] < -exts[i] || axes[i] > exts[i]) return false;
        } else {
            double inv_d = 1.0 / dirs[i];
            double t1 = (-exts[i] - axes[i]) * inv_d;
            double t2 = ( exts[i] - axes[i]) * inv_d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = (std::max)(tmin, t1);
            tmax = (std::min)(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    return true;
}

inline bool ClassifyObstruction(const std::string& name, float& out_r) {
    std::string lo; lo.reserve(name.size());
    for (char c : name) lo += (char)std::tolower((unsigned char)c);

    if (lo.find("smoke") != std::string::npos ||
        lo.find("gas")   != std::string::npos ||
        lo.find("fog")   != std::string::npos) {
        out_r = 500.0f; return true;
    }
    if (
        lo.find("barrier")   != std::string::npos ||
        lo.find("barricade") != std::string::npos) {
        out_r = 200.0f; return true;
    }
    if (lo.find("door") != std::string::npos) {
        return false;
    }
    if (lo.find("goo") != std::string::npos) {
        out_r = 300.0f; return true;
    }
    return false;
}

inline bool RaySegIntersectsSphere(const Vec3& org, const Vec3& dir, double len,
                                   const Vec3& c, float r) {
    double ox = org.x - c.x, oy = org.y - c.y, oz = org.z - c.z;
    double b = ox * dir.x + oy * dir.y + oz * dir.z;
    double rr = (double)r * (double)r;
    double disc = b * b - (ox * ox + oy * oy + oz * oz - rr);
    if (disc < 0) return false;
    double sq = std::sqrt(disc);
    double t0 = -b - sq, t1 = -b + sq;
    return !(t0 > len || t1 < 0.0);
}

inline bool CheckObstructions(Vec3 start, Vec3 end) {
    if (!UpgradeOptions::visibilityObstructionSpheres)
        return true;
    int idx = g_obstruction_active.load(std::memory_order_acquire);
    const auto& shapes = g_obstruction_buf[idx];
    if (shapes.empty()) return true;

    double dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z;
    double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6) return true;
    Vec3 dir = { dx / len, dy / len, dz / len };

    for (const auto& s : shapes) {
        double tx = s.center.x - start.x, ty = s.center.y - start.y,
               tz = s.center.z - start.z;
        double dc_sq = tx * tx + ty * ty + tz * tz;
        double cull = len + s.radius;
        if (dc_sq > cull * cull) continue;

        if (s.type == OBS_BOX) {
            if (RaySegIntersectsOBB(start, dir, len, s.center, s.half_ext, s.rotation))
                return false;
        } else {
            if (RaySegIntersectsSphere(start, dir, len, s.center, s.radius))
                return false;
        }
    }
    return true;
}

inline void ClearObstructions() {
    std::lock_guard<std::mutex> lock(g_obstruction_build_mutex);
    g_obstruction_map.clear();
    g_obs_fname_cache.clear();
    int wr = 1 - g_obstruction_active.load(std::memory_order_relaxed);
    g_obstruction_buf[wr].clear();
    g_obstruction_active.store(wr, std::memory_order_release);
}

inline bool SafeActorFName_(uintptr_t actor, std::string& out) {
    out = engine.GetActorFNameStringCached(actor);
    return !out.empty();
}

inline void CacheObstructions(uintptr_t uworld) {
    if (!valid_ptr(uworld)) return;

    ArrHeader levels = Memory::read<ArrHeader>(uworld + VisibilityOffsets::UWorld_Levels);
    if (!levels.data || levels.count <= 0 || levels.count > 512) return;
    static std::vector<uintptr_t> levelPtrs;
    static std::vector<uintptr_t> actorPtrs;
    levelPtrs.assign(levels.count, 0);
    Memory::ReadRaw(levels.data, levelPtrs.data(), levels.count * sizeof(uintptr_t));

    g_obstruction_frame.fetch_add(1, std::memory_order_relaxed);
    uint32_t frame = g_obstruction_frame.load(std::memory_order_relaxed);

    for (uintptr_t lvl : levelPtrs) {
        if (!valid_ptr(lvl)) continue;
        ArrHeader actors = Memory::read<ArrHeader>(lvl + VisibilityOffsets::ULevel_Actors);
        if (!actors.data || actors.count <= 0 || actors.count > 100000) continue;
        actorPtrs.assign(actors.count, 0);
        Memory::ReadRaw(actors.data, actorPtrs.data(), actors.count * sizeof(uintptr_t));

        for (uintptr_t actor : actorPtrs) {
            if (!valid_ptr(actor)) continue;

            auto cached = g_obs_fname_cache.find(actor);
            float    fallback_r = 0;
            GeomInfo gi = { OBS_SPHERE, {0,0,0}, {0,0,0}, {0,0,0}, 0 };
            if (cached != g_obs_fname_cache.end()) {
                if (!cached->second.match) continue;
                fallback_r = cached->second.fallback_r;
                gi         = cached->second.geom;
            } else {
                std::string name;
                if (!SafeActorFName_(actor, name) || name.empty()) {
                    g_obs_fname_cache[actor] = { false, 0, {} };
                    continue;
                }
                if (!ClassifyObstruction(name, fallback_r)) {
                    g_obs_fname_cache[actor] = { false, 0, {} };
                    continue;
                }
                gi = ReadActorGeom(actor);
                g_obs_fname_cache[actor] = { true, fallback_r, gi };
            }

            uintptr_t root = Memory::read<uintptr_t>(actor + VisibilityOffsets::AActor_RootComponent);
            if (!valid_ptr(root)) continue;

            FTransform ctw = Memory::read<FTransform>(
                root + VisibilityOffsets::UPrimitiveComponent_ComponentToWorld);
            Vec3 pos = ApplyTransform(ctw, gi.local_center);
            if (!is_finite_pos(pos)) continue;

            float base_r = (gi.radius > 1.0f) ? gi.radius : fallback_r;

            ObstructionShape shape = {};
            shape.center     = pos;
            shape.type       = gi.type;
            shape.actor      = actor;
            shape.last_frame = frame;

            if (gi.type == OBS_BOX && gi.radius > 1.0f) {
                Quat local_q = RotatorToQuat(gi.local_rot);
                shape.rotation = QuatMul(ctw.Rotation, local_q);
                shape.half_ext = { gi.half_ext.x * std::abs(ctw.Scale3D.x),
                                   gi.half_ext.y * std::abs(ctw.Scale3D.y),
                                   gi.half_ext.z * std::abs(ctw.Scale3D.z) };
                float hx = (float)shape.half_ext.x, hy = (float)shape.half_ext.y, hz = (float)shape.half_ext.z;
                shape.radius = std::sqrt(hx*hx + hy*hy + hz*hz);
            } else {
                shape.radius = base_r;
            }

            std::lock_guard<std::mutex> lock(g_obstruction_build_mutex);
            auto it = g_obstruction_map.find(actor);
            if (it != g_obstruction_map.end()) {
                shape.last_frame = frame;
                it->second = shape;
            } else {
                g_obstruction_map[actor] = shape;
            }
        }
    }

    int cur = g_obstruction_active.load(std::memory_order_acquire);
    int wr  = 1 - cur;
    auto& buf = g_obstruction_buf[wr];
    buf.clear();

    {
        std::lock_guard<std::mutex> lock(g_obstruction_build_mutex);
        buf.reserve(g_obstruction_map.size());
        for (auto it = g_obstruction_map.begin(); it != g_obstruction_map.end(); ) {
            if (frame - it->second.last_frame > 20) {
                g_obs_fname_cache.erase(it->second.actor);
                it = g_obstruction_map.erase(it);
            } else {
                buf.push_back(it->second);
                ++it;
            }
        }
    }
    g_obstruction_active.store(wr, std::memory_order_release);
}

// ============================================================================
//  MAIN LOGIC ? CheckVisibility (ray from start ? end)
// ============================================================================
#define ARC_DISABLE_VIS_CHECK 0

// Debug copy of recent camera-to-bone rays. CheckVisibility fills it; the
// overlay takes and clears it once per paint. actorKey lets the paint re-resolve
// the endpoint to the target's LIVE position (the ray itself was cast at
// collect time; without this the line points at where the bot stood back then
// and never follows it).
struct DebugRaySeg {
    Vec3 a, b;
    bool visible;
    uintptr_t actorKey = 0; // 0 = no identity, draw the recorded point
    bool isBot = false;
    int boneIndex = -1;     // UniBone for players; -1 = bot body ray
};
inline std::mutex g_debug_ray_mutex;
inline std::vector<DebugRaySeg> g_debug_rays;
constexpr size_t kDebugRayCap = 512;

inline void RecordDebugRay(const Vec3& a, const Vec3& b, bool visible,
                           uintptr_t actorKey = 0, bool isBot = false,
                           int boneIndex = -1) {
    std::lock_guard<std::mutex> lk(g_debug_ray_mutex);
    if (g_debug_rays.size() < kDebugRayCap)
        g_debug_rays.push_back({ a, b, visible, actorKey, isBot, boneIndex });
}

// Ray bias, in UE centimetres. These decide how thin a wall can be before a
// camera-to-bone ray slips past it:
//   near   - geometry closer than this to the eye is ignored (stops the ray
//            self-hitting when the camera is pressed into a surface).
//   back   - the ray stops this far short of the bone. Larger = walls the
//            target is hugging are missed (a hidden target reads "visible").
//   minDist- targets closer than this always count as visible (point blank).
// Smaller near/back = tighter walls, at the cost of a little noise at the eye.
constexpr float kVisRayNearCm    = 8.0f;
constexpr float kVisRayBackCm    = 6.0f;
constexpr float kVisRayMinDistCm = 20.0f;

inline bool CheckVisibilityImpl(Vec3 start, Vec3 end) {
#if ARC_DISABLE_VIS_CHECK
    (void)start; (void)end;
    return true;
#else
    if (!CheckObstructions(start, end)) return false;

#if ARC_ENABLE_EMBREE
    auto guarded = g_embree_manager.AcquireScene();
    if (!guarded) return true; // Fail safe to visible until scene commits
    RTCScene scene = guarded.scene;

    EVec3 org = ToEVec3(start);
    EVec3 dir_vec = ToEVec3(end) - org;
    float dist = std::sqrt(dir_vec.x * dir_vec.x + dir_vec.y * dir_vec.y + dir_vec.z * dir_vec.z);
    if (dist < kVisRayMinDistCm) return true;

    alignas(16) RTCRay ray{};
    ray.org_x = org.x; ray.org_y = org.y; ray.org_z = org.z;
    float inv_dist = 1.0f / dist;
    ray.dir_x = dir_vec.x * inv_dist;
    ray.dir_y = dir_vec.y * inv_dist;
    ray.dir_z = dir_vec.z * inv_dist;
    ray.tnear = kVisRayNearCm;
    ray.tfar = dist - kVisRayBackCm;
    if (ray.tfar <= ray.tnear) return true;
    ray.mask = -1; ray.flags = 0; ray.time = 0.0f; ray.id = 0;

    RTCOccludedArguments args;
    rtcInitOccludedArguments(&args);
    args.flags = RTC_RAY_QUERY_FLAG_COHERENT;
    rtcOccluded1(scene, &ray, &args);

    return (ray.tfar >= 0.0f);
#else
    (void)start; (void)end;
    return true;
#endif
#endif
}

struct VisHoldEntry {
    bool shown = true;
    uint64_t pendingSinceMs = 0;
    bool initialized = false;
};
inline std::mutex g_vis_hold_mutex;
inline std::unordered_map<uintptr_t, VisHoldEntry> g_vis_hold_map;

// Visibility hold: a visible<->invisible change only takes effect after the
// raw result has stayed the same for kVisHoldMs. Keyed by actorKey so per-frame
// snapshots (CollectEspRenderFrame -> GetBones) keep their hold state across
// frames instead of resetting every 12 ms, and initialize immediately on first
// sight instead of starting stuck on false.
inline bool ApplyVisHold(bool& shown, uint64_t& pendingSinceMs, bool raw,
                         uintptr_t actorKey = 0) {
    constexpr uint64_t kVisHoldMs = 2000;
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (actorKey != 0) {
        std::lock_guard<std::mutex> lk(g_vis_hold_mutex);
        if (g_vis_hold_map.size() > 4096)
            g_vis_hold_map.clear();
        VisHoldEntry& st = g_vis_hold_map[actorKey];
        if (!st.initialized) {
            st.initialized = true;
            st.shown = raw;
            st.pendingSinceMs = 0;
        } else if (raw == st.shown) {
            st.pendingSinceMs = 0;
        } else {
            if (st.pendingSinceMs == 0)
                st.pendingSinceMs = nowMs;
            if (nowMs - st.pendingSinceMs >= kVisHoldMs) {
                st.shown = raw;
                st.pendingSinceMs = 0;
            }
        }
        shown = st.shown;
        pendingSinceMs = st.pendingSinceMs;
        return shown;
    }
    if (raw == shown) {
        pendingSinceMs = 0;
        return shown;
    }
    if (pendingSinceMs == 0)
        pendingSinceMs = nowMs;
    if (nowMs - pendingSinceMs >= kVisHoldMs) {
        shown = raw;
        pendingSinceMs = 0;
    }
    return shown;
}

// Reads the held visibility for an actor without feeding a dummy raw value
// (used by EntityList, where GetBones does not run).
inline bool PeekVisHold(uintptr_t actorKey, bool& shown, uint64_t& pendingSinceMs,
                        bool fallback = true) {
    if (actorKey == 0) return fallback;
    std::lock_guard<std::mutex> lk(g_vis_hold_mutex);
    auto it = g_vis_hold_map.find(actorKey);
    if (it == g_vis_hold_map.end() || !it->second.initialized) {
        shown = fallback;
        pendingSinceMs = 0;
        return fallback;
    }
    shown = it->second.shown;
    pendingSinceMs = it->second.pendingSinceMs;
    return shown;
}

inline bool CheckVisibility(Vec3 start, Vec3 end,
                            uintptr_t actorKey = 0, bool isBot = false,
                            int boneIndex = -1) {
    // Reach is capped by the menu's Ray range slider: a target farther than
    // that is not traced at all (no ray cast, no ray drawn) and counts as
    // visible. 0 = unlimited.
    if (UpgradeOptions::rayRangeM > 0.f) {
        const EVec3 a = ToEVec3(start);
        const EVec3 d = ToEVec3(end) - a;
        const float distCm = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (distCm > UpgradeOptions::rayRangeM * 100.0f)
            return true;
    }

    const bool visible = CheckVisibilityImpl(start, end);
    if (UpgradeOptions::collisionDebugRays)
        RecordDebugRay(start, end, visible, actorKey, isBot, boneIndex);
    return visible;
}

// ============================================================================
//  BODY GEOMETRY EXTRACTION
// ============================================================================
inline std::vector<CachedShape> RetrieveOrCacheBodyGeom(uintptr_t bodySetup) {
    {
        std::shared_lock<std::shared_mutex> rlk(g_geom_cache_mutex);
        auto it = g_geom_cache.find(bodySetup);
        if (it != g_geom_cache.end()) return it->second;
    }

    std::vector<CachedShape> shapes;
    uintptr_t aggBase = bodySetup + VisibilityOffsets::UBodySetup_AggGeom;
    // One 80-byte read for all five TArray headers: they sit contiguously in
    // FKAggregateGeom (+0x00 sphere, +0x10 box, +0x20 sphyl, +0x30 convex,
    // +0x40 tapered capsule). Separate reads cost one DMA round-trip each.
    uint8_t aggHdr[0x50] = {};
    Memory::ReadRaw(aggBase, aggHdr, sizeof(aggHdr));
    ArrHeader spheres = *reinterpret_cast<ArrHeader*>(aggHdr + VisibilityOffsets::AggGeom_Sphere);
    ArrHeader boxes   = *reinterpret_cast<ArrHeader*>(aggHdr + VisibilityOffsets::AggGeom_Box);
    ArrHeader sphyls  = *reinterpret_cast<ArrHeader*>(aggHdr + VisibilityOffsets::AggGeom_Sphyl);
    ArrHeader convex  = *reinterpret_cast<ArrHeader*>(aggHdr + VisibilityOffsets::AggGeom_Convex);
    ArrHeader tapered = *reinterpret_cast<ArrHeader*>(aggHdr + VisibilityOffsets::AggGeom_TaperedCapsule);

    // Decode every simple shape AND any convex hulls. Skipping boxes when a
    // hull existed left gaps where the hull alone did not cover the mesh.
    if (boxes.count > 0 && boxes.count < 64 && valid_ptr(boxes.data)) {
        std::vector<uint8_t> buf(boxes.count * VisibilityOffsets::Stride_Box);
        Memory::ReadRaw(boxes.data, buf.data(), buf.size());
        for (int i = 0; i < boxes.count; i++) {
            uint8_t* p = buf.data() + i * VisibilityOffsets::Stride_Box + VisibilityOffsets::ShapeElem_Base;
            Vec3 center = *(Vec3*)p;
            Rotator rot = *(Rotator*)(p + 0x18);
            float x = *(float*)(p + 0x30); float y = *(float*)(p + 0x34); float z = *(float*)(p + 0x38);
            CachedShape s;
            FTransform t{};
            t.Rotation = RotatorToQuat(rot);
            t.Translation = center;
            t.Scale3D = Vec3(1.0, 1.0, 1.0);
            GeoUtils::TessBox(Vec3(x * 0.5, y * 0.5, z * 0.5), s.verts, s.indices, &t);
            shapes.push_back(std::move(s));
        }
    }

    if (spheres.count > 0 && spheres.count < 64 && valid_ptr(spheres.data)) {
        std::vector<uint8_t> buf(spheres.count * VisibilityOffsets::Stride_Sphere);
        Memory::ReadRaw(spheres.data, buf.data(), buf.size());
        for (int i = 0; i < spheres.count; i++) {
            uint8_t* p = buf.data() + i * VisibilityOffsets::Stride_Sphere + VisibilityOffsets::ShapeElem_Base;
            Vec3 center = *(Vec3*)p;
            float rad = *(float*)(p + 0x18);
            CachedShape s;
            GeoUtils::TessSphere(rad, s.verts, s.indices);
            for (auto& v : s.verts) v = v + ToEVec3(center);
            shapes.push_back(std::move(s));
        }
    }

    if (sphyls.count > 0 && sphyls.count < 64 && valid_ptr(sphyls.data)) {
        std::vector<uint8_t> buf(sphyls.count * VisibilityOffsets::Stride_Sphyl);
        Memory::ReadRaw(sphyls.data, buf.data(), buf.size());
        for (int i = 0; i < sphyls.count; i++) {
            uint8_t* p = buf.data() + i * VisibilityOffsets::Stride_Sphyl + VisibilityOffsets::ShapeElem_Base;
            Vec3 center = *(Vec3*)p;
            Rotator rot = *(Rotator*)(p + 0x18);
            float r = *(float*)(p + 0x30); float l = *(float*)(p + 0x34);
            CachedShape s;
            FTransform t{};
            t.Rotation = RotatorToQuat(rot);
            t.Translation = center;
            t.Scale3D = Vec3(1.0, 1.0, 1.0);
            GeoUtils::TessCapsule(r, l * 0.5f, s.verts, s.indices, &t);
            shapes.push_back(std::move(s));
        }
    }

    // Tapered capsules (UE5): a shape we never decoded, so a mesh that uses
    // only these (common for tree trunks, poles, legs) read as air.
    if (tapered.count > 0 && tapered.count < 64 && valid_ptr(tapered.data)) {
        std::vector<uint8_t> buf(tapered.count * VisibilityOffsets::Stride_TaperedCapsule);
        Memory::ReadRaw(tapered.data, buf.data(), buf.size());
        for (int i = 0; i < tapered.count; i++) {
            uint8_t* p = buf.data() + i * VisibilityOffsets::Stride_TaperedCapsule + VisibilityOffsets::ShapeElem_Base;
            Vec3 center = *(Vec3*)p;
            Rotator rot = *(Rotator*)(p + 0x18);
            float r0 = *(float*)(p + VisibilityOffsets::Tapered_Radius0);
            float r1 = *(float*)(p + VisibilityOffsets::Tapered_Radius1);
            float len = *(float*)(p + VisibilityOffsets::Tapered_Length);
            // Layout guard: a wrong offset would read garbage. Physical
            // capsule bounds are small positive numbers in cm.
            if (!std::isfinite(r0) || !std::isfinite(r1) || !std::isfinite(len)) continue;
            if (r0 < 0.01f || r0 > 100000.f || r1 < 0.01f || r1 > 100000.f) continue;
            if (len < 0.f || len > 100000.f) continue;
            CachedShape s;
            FTransform t{};
            t.Rotation = RotatorToQuat(rot);
            t.Translation = center;
            t.Scale3D = Vec3(1.0, 1.0, 1.0);
            GeoUtils::TessTaperedCapsule(r0, r1, len * 0.5f, s.verts, s.indices, &t);
            shapes.push_back(std::move(s));
        }
    }

    if (convex.count > 0 && convex.count < 32 && valid_ptr(convex.data)) {
        for (int i = 0; i < convex.count; i++) {
            uintptr_t elAddr = convex.data + (i * VisibilityOffsets::Stride_Convex);
            uintptr_t shapeBase = elAddr + VisibilityOffsets::ShapeElem_Base;
            FTransform localT = Memory::read<FTransform>(shapeBase + VisibilityOffsets::Convex_Transform);
            uintptr_t vData = Memory::read<uintptr_t>(shapeBase + VisibilityOffsets::Convex_VertexData);
            int vCount = Memory::read<int>(shapeBase + VisibilityOffsets::Convex_VertexData + 0x8);
            uintptr_t iData = Memory::read<uintptr_t>(shapeBase + VisibilityOffsets::Convex_IndexData);
            int iCount = Memory::read<int>(shapeBase + VisibilityOffsets::Convex_IndexData + 0x8);

            if (vCount > 0 && vCount < 5000 && valid_ptr(vData)) {
                CachedShape s;
                std::vector<Vec3> rawVerts(vCount);
                Memory::ReadRaw(vData, rawVerts.data(), vCount * sizeof(Vec3));

                const uint32_t vCountU = (uint32_t)vCount;
                if (iCount > 0 && iCount < 15000 && valid_ptr(iData)) {
                    std::vector<int32_t> rawIndices(iCount);
                    Memory::ReadRaw(iData, rawIndices.data(), iCount * sizeof(int32_t));
                    const int tri_count = iCount / 3;
                    for (int t = 0; t < tri_count; ++t) {
                        int32_t a = rawIndices[t * 3];
                        int32_t b = rawIndices[t * 3 + 1];
                        int32_t c = rawIndices[t * 3 + 2];
                        if (a < 0 || b < 0 || c < 0) continue;
                        if ((uint32_t)a >= vCountU || (uint32_t)b >= vCountU || (uint32_t)c >= vCountU) continue;
                        s.indices.push_back((uint32_t)a);
                        s.indices.push_back((uint32_t)b);
                        s.indices.push_back((uint32_t)c);
                    }
                    for (const auto& v : rawVerts)
                        s.verts.push_back(ToEVec3(ApplyTransform(localT, v)));
                } else if (!GeoUtils::TessConvexHull3D(rawVerts, localT, s.verts, s.indices)) {
                    // Coplanar/degenerate hull fallback: fill local bounds with a box.
                    Vec3 lo = rawVerts[0];
                    Vec3 hi = rawVerts[0];
                    for (const auto& v : rawVerts) {
                        lo.x = (std::min)(lo.x, v.x); lo.y = (std::min)(lo.y, v.y); lo.z = (std::min)(lo.z, v.z);
                        hi.x = (std::max)(hi.x, v.x); hi.y = (std::max)(hi.y, v.y); hi.z = (std::max)(hi.z, v.z);
                    }
                    const Vec3 ctr{ (lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5 };
                    const Vec3 half{ (hi.x - lo.x) * 0.5, (hi.y - lo.y) * 0.5, (hi.z - lo.z) * 0.5 };
                    FTransform boxT = localT;
                    boxT.Translation = ApplyTransform(localT, ctr);
                    GeoUtils::TessBox(half, s.verts, s.indices, &boxT);
                }
                shapes.push_back(std::move(s));
            }
        }
    }

    {
        std::unique_lock<std::shared_mutex> wlk(g_geom_cache_mutex);
        g_geom_cache[bodySetup] = shapes;
    }
    return shapes;
}

// ============================================================================
//  WORLD COLLECTION ? walk levels, gather StaticMesh actors, feed Embree
// ============================================================================
inline bool IsPassThroughMeshName(const std::string& lower_name) {
    // Glass, windows, and foliage should not stop visibility rays
    return lower_name.find("glass") != std::string::npos
        || lower_name.find("window_pane") != std::string::npos
        || lower_name.find("windowpane") != std::string::npos;
}

// One pending game-memory read, used to batch many reads into one DMA scatter.
struct VisPendingRead { uintptr_t addr; void* buf; size_t size; bool ok = false; };

// Runs all reads in one scatter. Sets `ok` on each read that succeeded.
// If the batch as a whole fails, retries each read on its own so one bad read
// only costs that one actor.
inline void VisReadBatch(std::vector<VisPendingRead>& reads) {
    if (reads.empty()) return;
    ScatterSession session;
    if (session.isValid()) {
        for (auto& r : reads)
            r.ok = session.prepare(r.addr, r.buf, r.size);
        if (session.execute())
            return; // prepared reads succeeded; unprepared ones keep ok=false
    }
    for (auto& r : reads)
        r.ok = Memory::ReadRaw(r.addr, r.buf, r.size);
}

struct VisActorCandidate {
    uintptr_t actor = 0;
    size_t    slot = 0;        // index into the per-level stats list
    std::string name;
    uintptr_t root = 0;
    FTransform ctw{};
    uintptr_t mesh = 0;
    uintptr_t body = 0;
};

// Reads one field for every candidate in a single batch, then keeps the ones `keep` accepts.
template <typename T, typename AddrFn, typename KeepFn>
inline bool VisStageRead(std::vector<VisActorCandidate>& cands, T VisActorCandidate::* out,
                         AddrFn addrOf, KeepFn keep) {
    std::vector<VisPendingRead> reads;
    reads.reserve(cands.size());
    for (auto& c : cands)
        reads.push_back({ addrOf(c), &(c.*out), sizeof(T) });
    VisReadBatch(reads);

    size_t n = 0;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (!reads[i].ok || !keep(cands[i])) continue;
        if (n != i) cands[n] = std::move(cands[i]);
        ++n;
    }
    cands.resize(n);
    return true;
}

inline bool CollectVisibilityMeshesFromWorld(uintptr_t uworld, Vec3 origin, float range, uint32_t my_gen) {
    if (!valid_ptr(uworld)) return false;

    std::vector<EVec3> all_verts;
    std::vector<uint32_t> all_indices;
    all_verts.reserve(500000); all_indices.reserve(1500000);

    int total_processed = 0;
    int total_kept = 0;
    // Actor type names the filter skipped / kept (for finding where walls are)
    std::unordered_map<std::string, int> skipped_names;
    std::unordered_map<std::string, int> kept_names;

    ArrHeader levels = Memory::read<ArrHeader>(uworld + VisibilityOffsets::UWorld_Levels);
    if (!levels.data || levels.count <= 0 || levels.count > 512) return false;
    std::vector<uintptr_t> levelPtrs(levels.count);
    Memory::ReadRaw(levels.data, levelPtrs.data(), levels.count * sizeof(uintptr_t));

    // Per-level stats for the one-time log: (level index, actors read, actors kept)
    struct LevelStat { int index; int actors; int kept; };
    std::vector<LevelStat> per_level;

    // Pass 1: gather actor pointers from every level in UWorld::Levels.
    // Defer FName resolution until AFTER the batched scatter stages so we do
    // not run per-actor name reads on out-of-range or non-mesh actors.
    std::vector<VisActorCandidate> cands;
    std::vector<uintptr_t> actorPtrs;
    for (size_t li = 0; li < levelPtrs.size(); ++li) {
        const uintptr_t lvl = levelPtrs[li];
        if (!valid_ptr(lvl)) continue;
        ArrHeader actors = Memory::read<ArrHeader>(lvl + VisibilityOffsets::ULevel_Actors);
        if (!actors.data || actors.count <= 0 || actors.count > 100000) continue;
        actorPtrs.assign(actors.count, 0);
        Memory::ReadRaw(actors.data, actorPtrs.data(), actors.count * sizeof(uintptr_t));
        const int seen_before = total_processed;
        const size_t slot = per_level.size();
        per_level.push_back({ static_cast<int>(li), 0, 0 });

        for (uintptr_t actor : actorPtrs) {
            if (!valid_ptr(actor)) continue;
            total_processed++;
            VisActorCandidate c;
            c.actor = actor;
            c.slot = slot;
            cands.push_back(std::move(c));
        }
        per_level[slot].actors = total_processed - seen_before;
    }

    // Pass 2: follow actor -> root -> transform (with range cull) -> mesh -> body.
    // Each step is ONE batched scatter read across all candidates.
    if (!VisStageRead(cands, &VisActorCandidate::root,
            [](const VisActorCandidate& c) { return c.actor + VisibilityOffsets::AActor_RootComponent; },
            [](const VisActorCandidate& c) { return valid_ptr(c.root); }))
        return false;

    if (!VisStageRead(cands, &VisActorCandidate::ctw,
            [](const VisActorCandidate& c) { return c.root + VisibilityOffsets::UPrimitiveComponent_ComponentToWorld; },
            [range, origin](const VisActorCandidate& c) {
                if (!is_finite_pos(c.ctw.Translation)) return false;
                return !(range > 0 && DistSq(c.ctw.Translation, origin) > (double)range * range);
            }))
        return false;

    if (!VisStageRead(cands, &VisActorCandidate::mesh,
            [](const VisActorCandidate& c) { return c.root + VisibilityOffsets::UStaticMeshComponent_StaticMesh; },
            [](const VisActorCandidate& c) { return valid_ptr(c.mesh); }))
        return false;

    if (!VisStageRead(cands, &VisActorCandidate::body,
            [](const VisActorCandidate& c) { return c.mesh + VisibilityOffsets::UStaticMesh_BodySetup; },
            [](const VisActorCandidate& c) { return valid_ptr(c.body); }))
        return false;

    // Pass 3: filter surviving candidates by actor/mesh FName and build triangles.
    std::unordered_map<uintptr_t, bool> meshPassThroughCache;
    int built = 0;
    for (auto& c : cands) {
        if (((++built) & 63) == 0 &&
            g_vis_generation.load(std::memory_order_acquire) != my_gen)
            return false;

        if (!SafeActorFName_(c.actor, c.name) || c.name.empty()) continue;
        std::string lower_name = c.name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        if (lower_name.find("staticmesh") == std::string::npos &&
            lower_name.rfind("sm_", 0) != 0) {
            skipped_names[c.name]++;
            continue;
        }
        if (IsPassThroughMeshName(lower_name)) {
            skipped_names[c.name + " [pass-through]"]++;
            continue;
        }
        auto pit = meshPassThroughCache.find(c.mesh);
        if (pit == meshPassThroughCache.end()) {
            std::string meshName;
            bool passThrough = false;
            if (SafeActorFName_(c.mesh, meshName) && !meshName.empty()) {
                std::transform(meshName.begin(), meshName.end(), meshName.begin(),
                               [](unsigned char ch) { return (char)std::tolower(ch); });
                passThrough = IsPassThroughMeshName(meshName);
            }
            pit = meshPassThroughCache.emplace(c.mesh, passThrough).first;
        }
        if (pit->second) {
            skipped_names[c.name + " [mesh pass-through]"]++;
            continue;
        }

        const auto& shapes = RetrieveOrCacheBodyGeom(c.body);
        if (shapes.empty()) continue;
        total_kept++;
        kept_names[c.name]++;
        per_level[c.slot].kept++;

        for (const auto& sh : shapes) {
            uint32_t idx_base = (uint32_t)all_verts.size();
            for (const auto& localV : sh.verts) {
                Vec3 lv = { (double)localV.x, (double)localV.y, (double)localV.z };
                Vec3 wv = ApplyTransform(c.ctw, lv);
                all_verts.push_back(ToEVec3(wv));
            }
            for (uint32_t idx : sh.indices) {
                all_indices.push_back(idx_base + idx);
            }
        }
    }

    g_last_mesh_actors.store(total_kept, std::memory_order_relaxed);
    if (!g_vis_log_filter_once && total_processed > 0) {
        std::printf("[VISIBILITY] Filter Results: Levels=%d Checked %d Actors -> Kept %d StaticMesh Actors (%zu tris)\n",
            (int)levelPtrs.size(), total_processed, total_kept, all_indices.size() / 3);

        auto print_top = [](const char* title, const std::unordered_map<std::string, int>& m) {
            std::vector<std::pair<std::string, int>> sorted(m.begin(), m.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::printf("[VISIBILITY] %s (top 30):\n", title);
            for (size_t n = 0; n < sorted.size() && n < 30; n++)
                std::printf("[VISIBILITY]   %6d  %s\n", sorted.at(n).second, sorted.at(n).first.c_str());
        };
        for (const LevelStat& ls : per_level)
            std::printf("[VISIBILITY]   level[%d]: actors=%d kept=%d\n", ls.index, ls.actors, ls.kept);
        print_top("KEPT actor types", kept_names);
        print_top("SKIPPED actor types", skipped_names);

        g_vis_log_filter_once = true;
    }

    if (all_verts.empty() || all_indices.size() < 3) return false;

    if (g_vis_generation.load(std::memory_order_acquire) == my_gen)
        g_embree_manager.UpdateFromBuffers(std::move(all_verts), std::move(all_indices));
    return true;
}

inline void ResetVisibilityWorld() {
    g_vis_generation.fetch_add(1, std::memory_order_release);
    g_visibility_meshes_ready.store(false, std::memory_order_release);
    g_last_mesh_actors.store(0, std::memory_order_relaxed);
    g_last_triangles.store(0, std::memory_order_relaxed);
    g_vis_log_filter_once = false;
    g_embree_manager.Clear();
    ClearObstructions();
    {
        std::unique_lock<std::shared_mutex> wlk(g_geom_cache_mutex);
        g_geom_cache.clear();
    }
    {
        std::lock_guard<std::mutex> hlk(g_vis_hold_mutex);
        g_vis_hold_map.clear();
    }
}

inline void RequestRebuildWorldMeshes() {
    g_vis_generation.fetch_add(1, std::memory_order_release);
    g_visibility_meshes_ready.store(false, std::memory_order_release);
    g_vis_log_filter_once = false;
}

// ============================================================================
//  DEBUG OVERLAY ? draws the collected collision-mesh triangles & obstructions
// ============================================================================
inline void RenderVisibilityDebug(ImDrawList* draw,
                                  const Engine::CameraCache& cam,
                                  int sw, int sh,
                                  bool draw_embree = true,
                                  bool draw_obstructions = true)
{
    if (!draw) return;

    auto w2s = [&](const Vec3& p, ImVec2& out) -> bool {
        Vector3 scr{};
        if (!engine.ProjectWorldLocationToScreen(p, scr, cam))
            return false;
        if (!std::isfinite(scr.x) || !std::isfinite(scr.y))
            return false;
        out = ImVec2((float)scr.x, (float)scr.y);
        return true;
    };

    const double maxDistCm = (double)UpgradeOptions::collisionDebugRadiusM * 100.0;
    const double maxDistSq = maxDistCm * maxDistCm;

    // --- Embree static triangles (white wireframe) ---
    // Wall edges: clip at the camera's near plane, then project without the
    // screen-bounds test so walls running past the screen edge still draw.
    Vector3 camX{}, camY{}, camZ{};
    EngineProjection::RotationGetAxes(cam.Rotation, camX, camY, camZ);
    constexpr double kWallNearCm = 1.0;
    auto depthOf = [&](const Vec3& p) {
        return (p.x - cam.Location.x) * camX.x + (p.y - cam.Location.y) * camX.y + (p.z - cam.Location.z) * camX.z;
    };
    auto drawWallEdge = [&](Vec3 a, Vec3 b) {
        const double da = depthOf(a), db = depthOf(b);
        if (da < kWallNearCm && db < kWallNearCm) return;
        if (da < kWallNearCm) {
            const double t = (kWallNearCm - da) / (db - da);
            a = Vec3{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
        } else if (db < kWallNearCm) {
            const double t = (kWallNearCm - db) / (da - db);
            b = Vec3{ b.x + (a.x - b.x) * t, b.y + (a.y - b.y) * t, b.z + (a.z - b.z) * t };
        }
        Vector3 sa{}, sb{};
        if (!engine.ProjectWorldLocationToScreenUnclipped(a, sa, cam)) return;
        if (!engine.ProjectWorldLocationToScreenUnclipped(b, sb, cam)) return;
        if (!std::isfinite(sa.x) || !std::isfinite(sa.y) || !std::isfinite(sb.x) || !std::isfinite(sb.y)) return;
        draw->AddLine(ImVec2((float)sa.x, (float)sa.y), ImVec2((float)sb.x, (float)sb.y), IM_COL32(255, 255, 255, 230), 1.2f);
    };

    if (draw_embree) {
        std::vector<EVec3>    verts;
        std::vector<uint32_t> indices;
        g_embree_manager.GetDebugGeometry(verts, indices);

        const int triCap = (std::max)(500, UpgradeOptions::collisionDebugSceneTriCap);

        // Collect triangles in range, with their distance from the camera
        std::vector<std::pair<double, size_t>> near_tris;
        near_tris.reserve(indices.size() / 3);
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
            if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;

            // Use the closest corner, so a big wall is kept if any part of it is near
            Vec3 p0 = { (double)verts[i0].x, (double)verts[i0].y, (double)verts[i0].z };
            Vec3 p1 = { (double)verts[i1].x, (double)verts[i1].y, (double)verts[i1].z };
            Vec3 p2 = { (double)verts[i2].x, (double)verts[i2].y, (double)verts[i2].z };
            double d = (std::min)(DistSq(p0, cam.Location), (std::min)(DistSq(p1, cam.Location), DistSq(p2, cam.Location)));
            if (d > maxDistSq) continue;
            near_tris.push_back({ d, i });
        }

        // Keep only the closest triangles, then sort nearest-first so nearby walls draw first
        auto by_dist = [](const std::pair<double, size_t>& a, const std::pair<double, size_t>& b) {
            return a.first < b.first;
        };
        if ((int)near_tris.size() > triCap) {
            std::nth_element(near_tris.begin(), near_tris.begin() + triCap, near_tris.end(), by_dist);
            near_tris.resize(triCap);
        }
        std::sort(near_tris.begin(), near_tris.end(), by_dist);

        int drawnTris = 0;
        for (const auto& entry : near_tris) {
            size_t i = entry.second;
            uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];

            Vec3 p0 = { (double)verts[i0].x, (double)verts[i0].y, (double)verts[i0].z };
            Vec3 p1 = { (double)verts[i1].x, (double)verts[i1].y, (double)verts[i1].z };
            Vec3 p2 = { (double)verts[i2].x, (double)verts[i2].y, (double)verts[i2].z };

            drawWallEdge(p0, p1);
            drawWallEdge(p1, p2);
            drawWallEdge(p2, p0);
            ++drawnTris;
        }
    }

    // --- Dynamic obstructions (wireframe boxes + spheres) ---
    if (!draw_obstructions) return;
    int obs_idx = g_obstruction_active.load(std::memory_order_acquire);
    const auto& obs = g_obstruction_buf[obs_idx];

    ImU32 obs_col = IM_COL32(255, 120, 0, 180);

    for (const auto& sp : obs) {
        if (DistSq(sp.center, cam.Location) > maxDistSq) continue;

        if (sp.type == OBS_BOX) {
            float hx = (float)sp.half_ext.x, hy = (float)sp.half_ext.y, hz = (float)sp.half_ext.z;
            Vec3 local_corners[8] = {
                {-hx,-hy,-hz}, {+hx,-hy,-hz}, {+hx,+hy,-hz}, {-hx,+hy,-hz},
                {-hx,-hy,+hz}, {+hx,-hy,+hz}, {+hx,+hy,+hz}, {-hx,+hy,+hz}
            };
            ImVec2 sc[8];
            bool ok[8];
            for (int i = 0; i < 8; i++) {
                Vec3 world = QuatRotateVec(sp.rotation, local_corners[i]);
                world.x += sp.center.x; world.y += sp.center.y; world.z += sp.center.z;
                ok[i] = w2s(world, sc[i]);
            }
            constexpr int edges[24] = { 0,1, 1,2, 2,3, 3,0, 4,5, 5,6, 6,7, 7,4, 0,4, 1,5, 2,6, 3,7 };
            for (int i = 0; i < 24; i += 2) {
                int a = edges[i], b = edges[i+1];
                if (ok[a] && ok[b])
                    draw->AddLine(sc[a], sc[b], obs_col, 1.5f);
            }
        } else {
            constexpr int   RING_SEGS = 24;
            constexpr float TWO_PI    = 6.283185307f;
            float r = sp.radius;
            for (int ri = 0; ri < 3; ri++) {
                ImVec2 prev{}; bool prev_ok = false;
                for (int seg = 0; seg <= RING_SEGS; seg++) {
                    float t = (float)seg / (float)RING_SEGS * TWO_PI;
                    float ct = r * std::cos(t), st = r * std::sin(t);
                    Vec3 pt;
                    if      (ri == 0) pt = { sp.center.x + ct, sp.center.y + st, sp.center.z };
                    else if (ri == 1) pt = { sp.center.x + ct, sp.center.y,      sp.center.z + st };
                    else              pt = { sp.center.x,      sp.center.y + ct, sp.center.z + st };
                    ImVec2 s{};
                    bool cur_ok = w2s(pt, s);
                    if (cur_ok && prev_ok)
                        draw->AddLine(prev, s, obs_col, 1.5f);
                    prev = s; prev_ok = cur_ok;
                }
            }
        }

        ImVec2 c2d{};
        if (w2s(sp.center, c2d))
            draw->AddCircleFilled(c2d, 3.0f, obs_col);
    }
}

// Draws the rays recorded since the last paint. Colors come only from the caller (ESP picks).
// Red dots mark blocked rays when hit markers are on. When the paint frame is
// given, each ray's endpoint is re-resolved to the target's CURRENT position
// (same data the ESP box uses) so the line tracks the moving target instead of
// freezing at the collect-time sample. Rays whose target left the frame are
// dropped, which also clears ghost lines after a kill.
// Ray colours, passed in from the ESP settings so rays use the same picks as the boxes.
struct RayColors {
    ImU32 playerVisible;
    ImU32 playerHidden;
    ImU32 botVisible;
    ImU32 botHidden;
};

inline void RenderDebugRays(ImDrawList* draw, const Engine::CameraCache& cam,
                            const Engine::EspRenderFrame* frame,
                            const RayColors& colors)
{
    if (!draw) return;

    std::vector<DebugRaySeg> rays;
    {
        std::lock_guard<std::mutex> lk(g_debug_ray_mutex);
        // Keep the last non-empty set for paints between recordings, but CLEAR
        // the recorder too: swapping alone handed the previous set straight
        // back to g_debug_rays, so every historical ray re-entered it, the
        // 512-cap filled with minutes-old rays, and fresh recordings stopped.
        static std::vector<DebugRaySeg> lastRays;
        if (!g_debug_rays.empty()) {
            lastRays = std::move(g_debug_rays);
            g_debug_rays.clear();
        }
        rays = lastRays;
    }
    if (rays.empty()) return;

    auto w2s = [&](const Vec3& p, ImVec2& out) -> bool {
        Vector3 scr{};
        if (!engine.ProjectWorldLocationToScreen(p, scr, cam))
            return false;
        if (!std::isfinite(scr.x) || !std::isfinite(scr.y))
            return false;
        out = ImVec2((float)scr.x, (float)scr.y);
        return true;
    };

    const double maxDistCm = (double)UpgradeOptions::collisionDebugRadiusM * 100.0;
    const double maxDistSq = maxDistCm * maxDistCm;
    const size_t maxRays = (size_t)(std::max)(1, UpgradeOptions::collisionDebugMaxRays);
    // Keep the newest rays when there are more than the cap.
    const size_t first = rays.size() > maxRays ? rays.size() - maxRays : 0;

    
    

    for (size_t i = first; i < rays.size(); ++i) {
        const DebugRaySeg& r = rays[i];
        if (DistSq(r.a, cam.Location) > maxDistSq) continue;

        // Live endpoint: the same world point the ESP box is built from,
        // refreshed every paint. The ray was CAST at collect time (its colour
        // is still the old result); only where the line POINTS is updated.
        // Not found = target gone (dead, evicted): the recorded point stays,
        // so lines never vanish wholesale; the next set replaces the ghost.
        // Colour follows the target's live isVisible (same flag as its ESP box), so
        // a ray always matches the box it touches. Falls back to the cast result.
        bool visibleNow = r.visible;
        Vec3 endPt = r.b;
        if (frame && r.actorKey != 0) {
            if (r.isBot) {
                for (const auto& item : frame->robots) {
                    if (item.actorKey != r.actorKey) continue;
                    visibleNow = item.entry.isVisible;
                    const Vector3& wp = item.entry.WorldPos;
                    endPt = Vec3{ wp.x, wp.y, wp.z + 90.0f };
                    break;
                }
            } else {
                for (const auto& item : frame->players) {
                    if (item.actorKey != r.actorKey) continue;
                    visibleNow = item.entry.isVisible;
                    const size_t bi = static_cast<size_t>(r.boneIndex);
                    if (r.boneIndex >= 0
                        && bi < item.entry.boneData.valid.size()
                        && item.entry.boneData.valid.test(bi)) {
                        const Vector3& bp = item.entry.boneData.bonesWorldDouble[bi];
                        // Each bone's ray follows that bone's own visibility.
                        visibleNow = item.entry.boneData.visible.test(bi);
                        endPt = Vec3{ bp.x, bp.y, bp.z };
                    } else {
                        const Vector3& wp = item.entry.WorldPos;
                        endPt = Vec3{ wp.x, wp.y, wp.z };
                    }
                    break;
                }
            }
        }

        ImVec2 sa{}, sb{};
        // The camera origin projects to nothing, so start lines at the crosshair (screen center).
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        sa = ImVec2(ds.x * 0.5f, ds.y * 0.5f);
        const bool okB = w2s(endPt, sb);
        if (!okB) continue;

        const ImU32 col = r.isBot
            ? (visibleNow ? colors.botVisible : colors.botHidden)
            : (visibleNow ? colors.playerVisible : colors.playerHidden);
        draw->AddLine(sa, sb, col, 1.2f);
        if (!visibleNow && UpgradeOptions::collisionDebugHitMarkers)
            draw->AddCircleFilled(sb, 4.0f, col);
    }
}

} // namespace EmbreeVis

