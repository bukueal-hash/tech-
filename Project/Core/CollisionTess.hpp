#pragma once
// Collision primitive -> triangle mesh, in world space.
//
// Stage 2 of the visibility pipeline: the shapes Core/CollisionGeom.hpp decoded
// are analytic (a box, a capsule, a sphere), and a BVH needs triangles. This is
// the bridge, and it is pure: no DMA, no Embree, no Windows. The caller hands in
// a primitive and the component transform it was read through.
//
// Order matters and is the point: the element's own rotation is applied first
// (element space), then the component transform (rotation, scale, translation).
// Doing it the other way round rotates the collision box in the mesh's frame.
//
// Triangle budgets are deliberate, not incidental:
//   box      12   - exact, an oriented box IS 12 triangles
//   sphere  ~320   - icosahedron with 2 subdivisions (20 * 4^2)
//   capsule ~240   - 12 radial segments, 5 rings per hemisphere, so ~2% under
//                    the 264 the writeup quotes; the count is reported, not assumed
//   convex   ~2x  - the hull's own triangles when its point cloud decoded; only
//                    a hull that did not decode falls back to its ElemBox
//
// A hull that stands in for its box is counted apart (convexAsBox) so the
// fallback stays visible. A box is never smaller than the hull inside it, so it
// is the conservative fallback, but it can block open air, which is why the real
// point cloud is preferred.

#include "CollisionGeom.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace CollisionTess {

/** Unindexed world-space triangle soup: 3 floats per vertex, 3 indices per tri. */
struct Mesh
{
    std::vector<float> vertices;
    std::vector<uint32_t> indices;

    size_t TriangleCount() const { return indices.size() / 3; }
    void Clear() { vertices.clear(); indices.clear(); }
};

struct Counts
{
    int boxes = 0;
    int spheres = 0;
    int capsules = 0;
    int convexHulls = 0;
    int convexAsBox = 0;
    // Why a hull fell back to its box, from header counts alone (no extra reads).
    int convexNoIndex = 0;     // IndexData absent or under one triangle
    int convexTooLarge = 0;    // point cloud over the decode cap
    int convexNoVerts = 0;     // VertexData empty
    int convexReadFailed = 0;  // counts sane, the point or index read failed
    int skipped = 0;
    int triangles = 0;
};

namespace detail {

inline void PushTriangle(Mesh& mesh, const Vector3& a, const Vector3& b, const Vector3& c)
{
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size() / 3);
    const Vector3 pts[3] = { a, b, c };
    for (const Vector3& v : pts) {
        mesh.vertices.push_back(static_cast<float>(v.x));
        mesh.vertices.push_back(static_cast<float>(v.y));
        mesh.vertices.push_back(static_cast<float>(v.z));
    }
    mesh.indices.push_back(base);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
}

inline Vector3 ToWorld(const FTransform& component, const Vector3& local)
{
    return CollisionGeom::TransformPoint(component, local);
}

/** One element-space point, rotated by the element's own frame then placed. */
inline Vector3 ElementToWorld(const CollisionGeom::Primitive& p, const FTransform& component,
    const Vector3& elementLocal)
{
    const Vector3 inComponent = p.Rotated() ? p.rotation.RotateVector(elementLocal) : elementLocal;
    return ToWorld(component, inComponent);
}

inline void AppendBox(const Vector3& center, const Vector3& half,
    const CollisionGeom::Primitive& p, const FTransform& component, Mesh& mesh)
{
    Vector3 corner[8];
    for (int i = 0; i < 8; ++i) {
        const Vector3 local(
            center.x + ((i & 1) ? half.x : -half.x),
            center.y + ((i & 2) ? half.y : -half.y),
            center.z + ((i & 4) ? half.z : -half.z));
        corner[i] = ElementToWorld(p, component, local);
    }

    // Six faces, two triangles each. Winding is not oriented on purpose: the BVH
    // answers occlusion, and Embree only needs a consistent hit either side.
    static const int kFace[6][4] = {
        {0, 2, 3, 1}, // -Z
        {4, 5, 7, 6}, // +Z
        {0, 1, 5, 4}, // -Y
        {2, 6, 7, 3}, // +Y
        {0, 4, 6, 2}, // -X
        {1, 3, 7, 5}, // +X
    };
    for (const auto& f : kFace) {
        PushTriangle(mesh, corner[f[0]], corner[f[1]], corner[f[2]]);
        PushTriangle(mesh, corner[f[0]], corner[f[2]], corner[f[3]]);
    }
}

/** Icosahedron, subdivided twice and projected to the sphere: 320 triangles. */
inline void AppendSphere(const Vector3& center, double radius,
    const CollisionGeom::Primitive& p, const FTransform& component, Mesh& mesh)
{
    if (!(radius > 0.0) || !std::isfinite(radius))
        return;

    const double t = (1.0 + std::sqrt(5.0)) * 0.5;
    std::vector<Vector3> verts = {
        Vector3(-1, t, 0), Vector3(1, t, 0), Vector3(-1, -t, 0), Vector3(1, -t, 0),
        Vector3(0, -1, t), Vector3(0, 1, t), Vector3(0, -1, -t), Vector3(0, 1, -t),
        Vector3(t, 0, -1), Vector3(t, 0, 1), Vector3(-t, 0, -1), Vector3(-t, 0, 1),
    };
    std::vector<uint32_t> faces = {
        0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11,
        1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
        3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9,
        4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1,
    };

    const auto normalize = [](const Vector3& v) {
        const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (!(len > 0.0))
            return Vector3(0.0, 0.0, 1.0);
        return Vector3(v.x / len, v.y / len, v.z / len);
    };
    for (int sub = 0; sub < 2; ++sub) {
        std::vector<uint32_t> next;
        next.reserve(faces.size() * 4);
        for (size_t i = 0; i + 2 < faces.size(); i += 3) {
            const uint32_t a = faces[i], b = faces[i + 1], c = faces[i + 2];
            const Vector3 va = normalize(verts[a]);
            const Vector3 vb = normalize(verts[b]);
            const Vector3 vc = normalize(verts[c]);
            const uint32_t ab = static_cast<uint32_t>(verts.size());
            verts.push_back(normalize(Vector3((va.x + vb.x) * 0.5, (va.y + vb.y) * 0.5, (va.z + vb.z) * 0.5)));
            const uint32_t bc = static_cast<uint32_t>(verts.size());
            verts.push_back(normalize(Vector3((vb.x + vc.x) * 0.5, (vb.y + vc.y) * 0.5, (vb.z + vc.z) * 0.5)));
            const uint32_t ca = static_cast<uint32_t>(verts.size());
            verts.push_back(normalize(Vector3((vc.x + va.x) * 0.5, (vc.y + va.y) * 0.5, (vc.z + va.z) * 0.5)));
            next.insert(next.end(), { a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca });
        }
        faces.swap(next);
    }

    for (size_t i = 0; i + 2 < faces.size(); i += 3) {
        Vector3 tri[3];
        for (int k = 0; k < 3; ++k) {
            const Vector3 unit = normalize(verts[faces[i + k]]);
            const Vector3 local(center.x + unit.x * radius, center.y + unit.y * radius,
                center.z + unit.z * radius);
            tri[k] = ElementToWorld(p, component, local);
        }
        PushTriangle(mesh, tri[0], tri[1], tri[2]);
    }
}

/**
 * Capsule along the element frame's +Z: a cylinder band and two hemispherical
 * caps, each cap ending in a triangle fan at its pole.
 */
inline void AppendCapsule(const Vector3& center, double radius, double halfLength,
    const CollisionGeom::Primitive& p, const FTransform& component, Mesh& mesh)
{
    if (!(radius > 0.0) || !std::isfinite(radius) || !std::isfinite(halfLength))
        return;

    constexpr int kRadial = 12;
    constexpr int kRings = 5; // per hemisphere, including the pole ring
    const double twoPi = 6.283185307179586;
    const double halfPi = 1.5707963267948966;

    const auto point = [&](double angle, double phi, double axial) {
        const double r = radius * std::cos(phi);
        const Vector3 local(center.x + r * std::cos(angle),
            center.y + r * std::sin(angle),
            center.z + axial + radius * std::sin(phi));
        return ElementToWorld(p, component, local);
    };

    // Cylinder band.
    for (int s = 0; s < kRadial; ++s) {
        const double a0 = twoPi * s / kRadial;
        const double a1 = twoPi * (s + 1) / kRadial;
        const Vector3 lo0 = point(a0, 0.0, -halfLength);
        const Vector3 lo1 = point(a1, 0.0, -halfLength);
        const Vector3 hi0 = point(a0, 0.0, halfLength);
        const Vector3 hi1 = point(a1, 0.0, halfLength);
        PushTriangle(mesh, lo0, lo1, hi1);
        PushTriangle(mesh, lo0, hi1, hi0);
    }

    // Two caps: rings from the equator to the pole, then a fan at the pole.
    for (int cap = 0; cap < 2; ++cap) {
        const double axial = cap == 0 ? -halfLength : halfLength;
        const double sign = cap == 0 ? -1.0 : 1.0;
        for (int ring = 0; ring + 1 < kRings; ++ring) {
            const double phi0 = sign * halfPi * ring / kRings;
            const double phi1 = sign * halfPi * (ring + 1) / kRings;
            for (int s = 0; s < kRadial; ++s) {
                const double a0 = twoPi * s / kRadial;
                const double a1 = twoPi * (s + 1) / kRadial;
                const Vector3 p00 = point(a0, phi0, axial);
                const Vector3 p01 = point(a1, phi0, axial);
                const Vector3 p10 = point(a0, phi1, axial);
                const Vector3 p11 = point(a1, phi1, axial);
                PushTriangle(mesh, p00, p01, p11);
                PushTriangle(mesh, p00, p11, p10);
            }
        }
        const Vector3 pole = ElementToWorld(p, component,
            Vector3(center.x, center.y, center.z + axial + sign * radius));
        const double phiLast = sign * halfPi * (kRings - 1) / kRings;
        for (int s = 0; s < kRadial; ++s) {
            const double a0 = twoPi * s / kRadial;
            const double a1 = twoPi * (s + 1) / kRadial;
            const Vector3 r0 = point(a0, phiLast, axial);
            const Vector3 r1 = point(a1, phiLast, axial);
            PushTriangle(mesh, r0, r1, pole);
        }
    }
}

} // namespace detail

/**
 * Append one primitive, placed by `component`, to `mesh`. Returns the triangles
 * added. Primitive types with no layout in this build's dump are skipped and
 * counted, never guessed.
 */
/** Triangles of a convex hull's real point cloud. False when it has none. */
inline bool AppendHull(const CollisionGeom::Primitive& p, const FTransform& component, Mesh& mesh)
{
    if (p.hull.empty() || p.hullIndices.size() < 3 || p.hullIndices.size() % 3 != 0)
        return false;
    for (const uint32_t vi : p.hullIndices) {
        if (vi >= p.hull.size())
            return false;
    }
    for (size_t t = 0; t < p.hullIndices.size(); t += 3) {
        detail::PushTriangle(mesh,
            detail::ToWorld(component, p.hull[p.hullIndices[t]]),
            detail::ToWorld(component, p.hull[p.hullIndices[t + 1]]),
            detail::ToWorld(component, p.hull[p.hullIndices[t + 2]]));
    }
    return true;
}

inline int AppendPrimitive(const CollisionGeom::Primitive& p, const FTransform& component,
    Mesh& mesh, Counts& counts)
{
    const size_t before = mesh.TriangleCount();

    if (!CollisionGeom::IsFinite(p.center) || !CollisionGeom::IsFinite(p.halfExtents)) {
        ++counts.skipped;
        return 0;
    }
    // A scaled-to-nothing component would collapse the mesh onto the origin and
    // silently make every ray hit. Refuse it instead.
    if (!std::isfinite(component.Scale3D.x) || !std::isfinite(component.Scale3D.y)
        || !std::isfinite(component.Scale3D.z)
        || std::abs(component.Scale3D.x) < 1e-4 || std::abs(component.Scale3D.y) < 1e-4
        || std::abs(component.Scale3D.z) < 1e-4) {
        ++counts.skipped;
        return 0;
    }

    switch (p.shape) {
    case CollisionGeom::Shape::Sphere:
        if (p.radius > 0.0) {
            detail::AppendSphere(p.center, p.radius, p, component, mesh);
            ++counts.spheres;
        } else {
            ++counts.skipped;
        }
        break;
    case CollisionGeom::Shape::Box:
        if (p.halfExtents.x > 0.0 && p.halfExtents.y > 0.0 && p.halfExtents.z > 0.0) {
            detail::AppendBox(p.center, p.halfExtents, p, component, mesh);
            ++counts.boxes;
        } else {
            ++counts.skipped;
        }
        break;
    case CollisionGeom::Shape::Sphyl:
        if (p.radius > 0.0) {
            detail::AppendCapsule(p.center, p.radius, p.halfLength, p, component, mesh);
            ++counts.capsules;
        } else {
            ++counts.skipped;
        }
        break;
    case CollisionGeom::Shape::Convex:
        // The hull's own triangles when its point cloud decoded. The ElemBox stand-in
        // is only the fallback, and it is counted apart so it stays visible.
        if (AppendHull(p, component, mesh)) {
            ++counts.convexHulls;
        } else {
            if (p.indexCount < 3)
                ++counts.convexNoIndex;
            else if (p.vertexCount > CollisionGeom::Layout::kMaxHullVertices)
                ++counts.convexTooLarge;
            else if (p.vertexCount <= 0)
                ++counts.convexNoVerts;
            else
                ++counts.convexReadFailed;
            if (p.halfExtents.x > 0.0 && p.halfExtents.y > 0.0 && p.halfExtents.z > 0.0) {
                detail::AppendBox(p.center, p.halfExtents, p, component, mesh);
                ++counts.convexAsBox;
            } else {
                ++counts.skipped;
            }
        }
        break;
    default:
        ++counts.skipped;
        break;
    }

    const int added = static_cast<int>(mesh.TriangleCount() - before);
    counts.triangles += added;
    return added;
}

/** Append every primitive of one asset instance. */
inline int AppendAsset(const std::vector<CollisionGeom::Primitive>& primitives,
    const FTransform& component, Mesh& mesh, Counts& counts)
{
    const size_t before = mesh.TriangleCount();
    for (const CollisionGeom::Primitive& p : primitives)
        AppendPrimitive(p, component, mesh, counts);
    return static_cast<int>(mesh.TriangleCount() - before);
}

} // namespace CollisionTess
