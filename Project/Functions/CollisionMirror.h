#pragma once
// CollisionMirror — world static-collision LOS queries.
//
// Builds a KD-tree of world-space triangles read from UBodySetup::AggGeom
// (convex hulls / boxes / spheres / capsules), then answers
// is_visible(from, to) with a segment raycast.
//
// This header is pure math — no DMA, no locks, no engine state — so the
// tree build and raycast are testable standalone and safe on any thread.
// The DMA reads and rebuild scheduling live in CollisionMirror.cpp.
//
// Offsets are from this repo's own SDK dump (build 24710327) — see
// CollisionMirror.cpp for the full chain. Do not mix in forum-source offsets.

#include "../../Core/Vector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace CollisionMirror {

// ── Geometry primitives ─────────────────────────────────────────────────────

struct Tri {
    Vector3 p0, p1, p2;
};

struct KDNode {
    Vector3 bbMin, bbMax;
    std::vector<Tri> triangles;
    KDNode* left = nullptr;
    KDNode* right = nullptr;
    ~KDNode() { delete left; delete right; }
};

// ── Small vec helpers (Vector3 lacks cross / component min-max) ────────────

inline Vector3 VCross(const Vector3& a, const Vector3& b)
{
    return Vector3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
}

inline Vector3 VMin(const Vector3& a, const Vector3& b)
{
    return Vector3((std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::min)(a.z, b.z));
}

inline Vector3 VMax(const Vector3& a, const Vector3& b)
{
    return Vector3((std::max)(a.x, b.x), (std::max)(a.y, b.y), (std::max)(a.z, b.z));
}

inline Vector3 VMul(const Vector3& a, const Vector3& b)
{
    return Vector3(a.x * b.x, a.y * b.y, a.z * b.z);
}

inline Vector3 VTriCentroid(const Tri& t)
{
    return Vector3(
        (t.p0.x + t.p1.x + t.p2.x) / 3.0,
        (t.p0.y + t.p1.y + t.p2.y) / 3.0,
        (t.p0.z + t.p1.z + t.p2.z) / 3.0);
}

// ── KD-tree build ───────────────────────────────────────────────────────────

inline KDNode* BuildTree(std::vector<Tri>& tris, int depth = 0)
{
    if (tris.empty())
        return nullptr;

    auto* node = new KDNode();
    node->bbMin = node->bbMax = tris[0].p0;
    for (const auto& t : tris) {
        node->bbMin = VMin(node->bbMin, VMin(t.p0, VMin(t.p1, t.p2)));
        node->bbMax = VMax(node->bbMax, VMax(t.p0, VMax(t.p1, t.p2)));
    }

    // Leaf: small enough to test directly.
    if (static_cast<int>(tris.size()) <= 8) {
        node->triangles = std::move(tris);
        return node;
    }

    const int axis = depth % 3;
    auto axisComp = [axis](const Vector3& v) -> double {
        return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
    };
    std::nth_element(tris.begin(), tris.begin() + tris.size() / 2, tris.end(),
        [&](const Tri& a, const Tri& b) {
            const double ac = (axisComp(a.p0) + axisComp(a.p1) + axisComp(a.p2)) / 3.0;
            const double bc = (axisComp(b.p0) + axisComp(b.p1) + axisComp(b.p2)) / 3.0;
            return ac < bc;
        });

    const size_t mid = tris.size() / 2;
    std::vector<Tri> left(tris.begin(), tris.begin() + mid);
    std::vector<Tri> right(tris.begin() + mid, tris.end());
    node->left = BuildTree(left, depth + 1);
    node->right = BuildTree(right, depth + 1);
    return node;
}

inline void FreeTree(KDNode* node)
{
    delete node;
}

// ── Raycast ─────────────────────────────────────────────────────────────────

inline Vector3 SafeInvDir(const Vector3& rd)
{
    constexpr double kEps = 1e-12;
    return Vector3(
        (std::fabs(rd.x) > kEps) ? 1.0 / rd.x : (rd.x >= 0.0 ? 1e12 : -1e12),
        (std::fabs(rd.y) > kEps) ? 1.0 / rd.y : (rd.y >= 0.0 ? 1e12 : -1e12),
        (std::fabs(rd.z) > kEps) ? 1.0 / rd.z : (rd.z >= 0.0 ? 1e12 : -1e12));
}

inline bool RayAabb(const Vector3& ro, const Vector3& rdInv,
    const Vector3& bmin, const Vector3& bmax)
{
    const Vector3 t0 = VMul(bmin - ro, rdInv);
    const Vector3 t1 = VMul(bmax - ro, rdInv);
    const Vector3 tmin = VMin(t0, t1);
    const Vector3 tmax = VMax(t0, t1);
    const double enter = (std::max)((std::max)(tmin.x, tmin.y), tmin.z);
    const double leave = (std::min)((std::min)(tmax.x, tmax.y), tmax.z);
    return leave >= 0.0 && enter <= leave;
}

inline bool RayTriSegment(const Vector3& ro, const Vector3& rd,
    const Tri& t, const double tMax)
{
    constexpr double kEps = 1e-6;
    const Vector3 e1 = t.p1 - t.p0;
    const Vector3 e2 = t.p2 - t.p0;
    const Vector3 h = VCross(rd, e2);
    const double a = e1.Dot(h);
    if (a > -kEps && a < kEps)
        return false;
    const double f = 1.0 / a;
    const Vector3 s = ro - t.p0;
    const double u = f * s.Dot(h);
    if (u < 0.0 || u > 1.0)
        return false;
    const Vector3 q = VCross(s, e1);
    const double v = f * rd.Dot(q);
    if (v < 0.0 || u + v > 1.0)
        return false;
    const double tv = f * e2.Dot(q);
    return tv > kEps && tv < tMax - kEps;
}

inline bool Traverse(const KDNode* node,
    const Vector3& ro, const Vector3& rd, const Vector3& rdInv,
    const double tMax, const Tri** outHit = nullptr)
{
    if (!node)
        return false;
    if (!RayAabb(ro, rdInv, node->bbMin, node->bbMax))
        return false;
    for (const auto& t : node->triangles) {
        if (RayTriSegment(ro, rd, t, tMax)) {
            if (outHit)
                *outHit = &t;
            return true;
        }
    }
    if (Traverse(node->left, ro, rd, rdInv, tMax, outHit))
        return true;
    return Traverse(node->right, ro, rd, rdInv, tMax, outHit);
}

/**
 * Segment visibility: true if nothing between `from` and `to` blocks.
 * rd = to - from, so t in (eps, 1) = strictly inside the segment.
 * No tree (null) → true (fail-open, same as LRTS warmup).
 */
inline bool IsVisible(const KDNode* tree,
    const Vector3& from, const Vector3& to, const Tri** outHit = nullptr)
{
    if (!tree)
        return true;
    const Vector3 rd = to - from;
    const double len2 = rd.Dot(rd);
    if (len2 < 1e-8)
        return true;
    const Vector3 rdInv = SafeInvDir(rd);
    return !Traverse(tree, from, rd, rdInv, 1.0, outHit);
}

// ── Public API (implemented in CollisionMirror.cpp) ────────────────────────
// Rebuild the world collision tree. Throttled internally (movement / 20s
// force / first build); the heavy DMA runs on a spawned worker thread, so
// this returns immediately and is safe to call from Update.
void ScheduleRebuild(uintptr_t uworld, uintptr_t persistentLevel,
    const Vector3& localPos);

// Segment visibility against the published tree. try_lock + fail-open: never
// blocks the caller; no tree yet → true. Safe from paint or workers.
bool QueryVisible(const Vector3& from, const Vector3& to);

// Raycast that also returns the first triangle hit (copied out — valid after
// the lock is released). Visible=true means no hit. Safe from paint.
struct RayHit {
    bool visible = true;   // true when nothing blocked the segment
    bool hit = false;      // false when visible or no tree
    Tri tri{};             // first blocking triangle (world space)
};
RayHit QueryRay(const Vector3& from, const Vector3& to);

// Walk published triangles within `radius` of `center`, calling fn per tri.
// Holds the internal lock for the whole walk (tree cannot be swapped/deleted
// mid-iteration). try_lock: if a rebuild is mid-swap, returns false and skips
// the frame rather than blocking. Returns number of triangles visited.
size_t ForEachTriNear(const Vector3& center, double radius,
    void (*fn)(const Tri&, void*), void* ctx);

bool IsReady();
size_t TriangleCount();
int MeshCount();
int RebuildCount();
int LastRebuildMs();
void Clear();

} // namespace CollisionMirror
