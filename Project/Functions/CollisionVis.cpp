#include "CollisionVis.h"
#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Core/SteamDecrypt.hpp"
#include "../Interface/Utils/Variables/index.h"
#include "WorldScanCommon.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace CollisionVis {

// ═══════════════════════════════════════════════════════════════════════════
// Internal types
// ═══════════════════════════════════════════════════════════════════════════

struct Tri {
    Vector3 p0, p1, p2;
    BlockerClass cls = BlockerClass::Other;
};

struct KdNode {
    Vector3 bmin{}, bmax{};
    std::vector<Tri> tris;
    std::unique_ptr<KdNode> left;
    std::unique_ptr<KdNode> right;
};

struct TreeHolder {
    std::unique_ptr<KdNode> root;
    int triCount = 0;
};

struct ConvexElemCache {
    std::vector<Engine::FVector3d> verts;
    std::vector<int32_t> indices;
};

struct BoxElemCache {
    Engine::FVector3d center{};
    double pitch = 0, yaw = 0, roll = 0;
    float hx = 0, hy = 0, hz = 0;
};

struct CachedMeshGeom {
    BlockerClass cls = BlockerClass::Other;
    bool bodyValid = false;
    bool hasConvex = false;
    bool hasBox = false;
    bool hasBounds = false;
    Engine::FVector3d boundsExtent{};
    std::vector<ConvexElemCache> convexElems;
    std::vector<BoxElemCache> boxElems;
};

struct SmcEntry {
    uintptr_t root = 0;
    uintptr_t actor = 0;
};

struct CompXform {
    FQuat rot{ 0, 0, 0, 1 };
    Engine::FVector3d trans{};
    Engine::FVector3d scale{ 1, 1, 1 };
};

// ═══════════════════════════════════════════════════════════════════════════
// Globals
// ═══════════════════════════════════════════════════════════════════════════

static std::mutex g_swapMutex;
static std::shared_ptr<TreeHolder> g_liveTree;

static std::atomic<int> g_rebuilding{ 0 };
static std::atomic<int> g_rebuildMs{ 0 };
static std::atomic<int> g_probe{ static_cast<int>(ProbeStatus::Red) };
static std::atomic<int> g_smc{ 0 };
static std::atomic<int> g_queries{ 0 };
static std::atomic<int> g_failOpen{ 0 };
static std::atomic<int> g_hits{ 0 };
static std::atomic<int> g_clears{ 0 };
static std::atomic<int> g_wallSmc{ 0 };
static std::atomic<int> g_doorSmc{ 0 };
static std::atomic<int> g_treeSmc{ 0 };
static std::atomic<int> g_otherSmc{ 0 };

static std::atomic<bool> g_bodySetupProbed{ false };
static std::atomic<std::ptrdiff_t> g_bodySetupOffset{ Offsets::BodySetup };

static std::unordered_map<uintptr_t, std::shared_ptr<CachedMeshGeom>> g_geomCache;
static std::atomic<int64_t> g_cacheHits{ 0 };
static std::atomic<int64_t> g_cacheMisses{ 0 };

static std::mutex g_debugMutex;
static std::vector<DebugRay> g_debugRays;
static std::vector<DebugTri> g_debugTris;
static constexpr size_t kMaxDebugRays = 64;
static constexpr size_t kMaxDebugTris = 2000;
static constexpr int kMaxSmc = 128;
static constexpr int kMaxTris = 40000;
static constexpr float kRebuildMoveCm = 3000.f;
static constexpr int kRebuildForceSec = 20;
static constexpr float kVisHardCapM = 50.f;

static Vector3 g_lastRebuildPos{};
static auto g_lastRebuildTime = std::chrono::steady_clock::time_point{};

// ═══════════════════════════════════════════════════════════════════════════
// Geometry helpers
// ═══════════════════════════════════════════════════════════════════════════

static Vector3 VMin(const Vector3& a, const Vector3& b) {
    return { (std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::min)(a.z, b.z) };
}

static Vector3 VMax(const Vector3& a, const Vector3& b) {
    return { (std::max)(a.x, b.x), (std::max)(a.y, b.y), (std::max)(a.z, b.z) };
}

static FQuat RotatorToQuat(double pitchDeg, double yawDeg, double rollDeg) {
    constexpr double k = 3.14159265358979323846 / 360.0;
    const double sp = std::sin(pitchDeg * k), cp = std::cos(pitchDeg * k);
    const double sy = std::sin(yawDeg * k),   cy = std::cos(yawDeg * k);
    const double sr = std::sin(rollDeg * k),  cr = std::cos(rollDeg * k);
    return FQuat{
        cr * cp * cy + sr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        sr * cp * cy - cr * sp * sy
    };
}

static Vector3 RotateVectorByQuat(const Vector3& v, const FQuat& q) {
    const double vx = v.x, vy = v.y, vz = v.z;
    const double qx = q.x, qy = q.y, qz = q.z, qw = q.w;
    const double tx = 2.0 * (qy * vz - qz * vy);
    const double ty = 2.0 * (qz * vx - qx * vz);
    const double tz = 2.0 * (qx * vy - qy * vx);
    return {
        static_cast<float>(vx + qw * tx + (qy * tz - qz * ty)),
        static_cast<float>(vy + qw * ty + (qz * tx - qx * tz)),
        static_cast<float>(vz + qw * tz + (qx * ty - qy * tx))
    };
}

static Vector3 XformLocal(const CompXform& xf, const Vector3& local) {
    const Vector3 scaled{ local.x * xf.scale.x, local.y * xf.scale.y, local.z * xf.scale.z };
    const Vector3 rotated = RotateVectorByQuat(scaled, xf.rot);
    return { rotated.x + xf.trans.x, rotated.y + xf.trans.y, rotated.z + xf.trans.z };
}

static CompXform ReadCompXform(uintptr_t comp) {
    CompXform xf{};
    xf.rot = Memory::read<FQuat>(comp + Offsets::ComponentToWorld);
    xf.trans = Memory::read<Engine::FVector3d>(comp + Offsets::WorldLocation);
    xf.scale = Memory::read<Engine::FVector3d>(comp + Offsets::ComponentToWorld + 0x40);
    if (xf.scale.x == 0 || xf.scale.y == 0 || xf.scale.z == 0)
        xf.scale = { 1, 1, 1 };
    const double len2 = xf.rot.x * xf.rot.x + xf.rot.y * xf.rot.y
        + xf.rot.z * xf.rot.z + xf.rot.w * xf.rot.w;
    if (!std::isfinite(len2) || len2 < 0.5)
        xf.rot = FQuat{ 0, 0, 0, 1 };
    return xf;
}

// ═══════════════════════════════════════════════════════════════════════════
// KD-tree
// ═══════════════════════════════════════════════════════════════════════════

static bool RayAabb(const Vector3& ro, const Vector3& rdInv,
    const Vector3& bmin, const Vector3& bmax)
{
    const Vector3 t0{ (bmin.x - ro.x) * rdInv.x, (bmin.y - ro.y) * rdInv.y, (bmin.z - ro.z) * rdInv.z };
    const Vector3 t1{ (bmax.x - ro.x) * rdInv.x, (bmax.y - ro.y) * rdInv.y, (bmax.z - ro.z) * rdInv.z };
    const Vector3 tmin{ VMin(t0, t1) };
    const Vector3 tmax{ VMax(t0, t1) };
    const float enter = (std::max)({ tmin.x, tmin.y, tmin.z });
    const float leave = (std::min)({ tmax.x, tmax.y, tmax.z });
    return leave >= 0.f && enter <= leave;
}

static bool RayTri(const Vector3& ro, const Vector3& rd, const Tri& t, float tMax, float& outT) {
    constexpr float kEps = 1e-6f;
    const Vector3 e1{ t.p1.x - t.p0.x, t.p1.y - t.p0.y, t.p1.z - t.p0.z };
    const Vector3 e2{ t.p2.x - t.p0.x, t.p2.y - t.p0.y, t.p2.z - t.p0.z };
    const Vector3 h{ rd.y * e2.z - rd.z * e2.y, rd.z * e2.x - rd.x * e2.z, rd.x * e2.y - rd.y * e2.x };
    const float a = e1.x * h.x + e1.y * h.y + e1.z * h.z;
    if (a > -kEps && a < kEps) return false;
    const float f = 1.f / a;
    const Vector3 s{ ro.x - t.p0.x, ro.y - t.p0.y, ro.z - t.p0.z };
    const float u = f * (s.x * h.x + s.y * h.y + s.z * h.z);
    if (u < 0.f || u > 1.f) return false;
    const Vector3 q{ s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x };
    const float v = f * (rd.x * q.x + rd.y * q.y + rd.z * q.z);
    if (v < 0.f || u + v > 1.f) return false;
    const float tv = f * (e2.x * q.x + e2.y * q.y + e2.z * q.z);
    if (tv > kEps && tv < tMax - kEps) {
        outT = tv;
        return true;
    }
    return false;
}

static bool Traverse(const KdNode* node, const Vector3& ro, const Vector3& rd,
    const Vector3& rdInv, float tMax, BlockerClass& outCls)
{
    if (!node) return false;
    if (!RayAabb(ro, rdInv, node->bmin, node->bmax)) return false;
    for (const auto& t : node->tris) {
        float tv = 0.f;
        if (RayTri(ro, rd, t, tMax, tv)) {
            outCls = t.cls;
            return true;
        }
    }
    bool hit = false;
    if (Traverse(node->left.get(), ro, rd, rdInv, tMax, outCls))
        return true;
    if (Traverse(node->right.get(), ro, rd, rdInv, tMax, outCls))
        return true;
    return false;
}

static std::unique_ptr<KdNode> BuildKd(std::vector<Tri>& tris, int depth) {
    if (tris.empty()) return nullptr;
    auto node = std::make_unique<KdNode>();
    node->bmin = node->bmax = tris[0].p0;
    for (const auto& t : tris) {
        for (const auto& p : { t.p0, t.p1, t.p2 }) {
            node->bmin = VMin(node->bmin, p);
            node->bmax = VMax(node->bmax, p);
        }
    }
    if ((int)tris.size() <= 8) {
        node->tris = std::move(tris);
        return node;
    }
    const int axis = depth % 3;
    std::nth_element(tris.begin(), tris.begin() + tris.size() / 2, tris.end(),
        [axis](const Tri& a, const Tri& b) {
            auto axisVal = [](const Vector3& v, int ax) -> float {
                return (ax == 0) ? v.x : (ax == 1) ? v.y : v.z;
            };
            float ac = (axisVal(a.p0, axis) + axisVal(a.p1, axis) + axisVal(a.p2, axis)) / 3.0f;
            float bc = (axisVal(b.p0, axis) + axisVal(b.p1, axis) + axisVal(b.p2, axis)) / 3.0f;
            return ac < bc;
        });
    const size_t mid = tris.size() / 2;
    std::vector<Tri> left(tris.begin(), tris.begin() + mid);
    std::vector<Tri> right(tris.begin() + mid, tris.end());
    node->left = BuildKd(left, depth + 1);
    node->right = BuildKd(right, depth + 1);
    return node;
}

// ═══════════════════════════════════════════════════════════════════════════
// Blocker classification
// ═══════════════════════════════════════════════════════════════════════════

static BlockerClass ClassifyBlocker(uintptr_t actor) {
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return BlockerClass::Other;
    std::string fname = steam_decrypt::GetActorFNameString(actor);
    if (fname.empty()) return BlockerClass::Other;
    std::string lower = fname;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("door") != std::string::npos)
        return BlockerClass::Door;
    if (lower.find("tree") != std::string::npos || lower.find("foliage") != std::string::npos)
        return BlockerClass::Tree;
    if (lower.find("wall") != std::string::npos || lower.find("building") != std::string::npos
        || lower.find("concrete") != std::string::npos || lower.find("brick") != std::string::npos)
        return BlockerClass::Wall;
    return BlockerClass::Other;
}

static BlockerClass ClassifyByMeshFName(uintptr_t mesh) {
    if (!mesh || !Memory::IsValidPtrFast2(mesh)) return BlockerClass::Other;
    std::string fn = steam_decrypt::GetActorFNameString(mesh);
    if (fn.empty()) return BlockerClass::Other;
    std::string lower = fn;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("hatch") != std::string::npos)
        return BlockerClass::Door;
    if (lower.find("tree") != std::string::npos || lower.find("foliage") != std::string::npos)
        return BlockerClass::Tree;
    if (lower.find("wall") != std::string::npos || lower.find("building") != std::string::npos)
        return BlockerClass::Wall;
    return BlockerClass::Other;
}

// ═══════════════════════════════════════════════════════════════════════════
// Geometry reading (convex + box)
// ═══════════════════════════════════════════════════════════════════════════

static std::ptrdiff_t FindBodySetupOffset(uintptr_t mesh) {
    if (g_bodySetupProbed)
        return g_bodySetupOffset.load(std::memory_order_relaxed);
    static const std::ptrdiff_t kCandidates[] = {
        Offsets::BodySetup, 0x1E8, 0x1F8, 0x200, 0x1D8, 0x208
    };
    for (std::ptrdiff_t off : kCandidates) {
        const uintptr_t body = Memory::read<uintptr_t>(mesh + off);
        if (body && Memory::IsValidPtrFast2(body)) {
            g_bodySetupOffset.store(off, std::memory_order_relaxed);
            g_bodySetupProbed = true;
            return off;
        }
    }
    g_bodySetupProbed = true;
    return Offsets::BodySetup;
}

static std::shared_ptr<CachedMeshGeom> PopulateGeomCache(
    uintptr_t mesh, BlockerClass cls, int& missBodyOut,
    int& convexOkOut, int& boxOkOut, int& boundsOkOut, int& boundsBadOut)
{
    auto entry = std::make_shared<CachedMeshGeom>();
    entry->cls = cls;
    const std::ptrdiff_t bodyOff = FindBodySetupOffset(mesh);
    const uintptr_t body = Memory::read<uintptr_t>(mesh + bodyOff);
    entry->bodyValid = body && Memory::IsValidPtrFast2(body);
    if (!entry->bodyValid) {
        ++missBodyOut;
        return entry;
    }

    // Convex elements
    struct TArray { uintptr_t data; int32_t num; int32_t max; };
    const uintptr_t agg = body + Offsets::AggGeom;
    const TArray conv = Memory::read<TArray>(agg + Offsets::AggGeom_ConvexElems);
    if (conv.data && conv.num > 0 && conv.num <= 64) {
        entry->hasConvex = true;
        entry->convexElems.resize((size_t)conv.num);
        for (int ei = 0; ei < conv.num; ++ei) {
            auto& ce = entry->convexElems[(size_t)ei];
            const uintptr_t elem = conv.data + (uintptr_t)ei * Offsets::ConvexElem_Stride;
            const TArray verts = Memory::read<TArray>(elem + Offsets::ConvexElem_VertexData);
            const TArray idxs = Memory::read<TArray>(elem + Offsets::ConvexElem_IndexData);
            if (!verts.data || verts.num <= 0 || verts.num > 4096) continue;
            if (!idxs.data || idxs.num <= 0 || idxs.num > 65536 || (idxs.num % 3) != 0) continue;
            ce.verts.resize((size_t)verts.num);
            if (!Memory::read((DWORD64)verts.data, ce.verts.data(),
                (DWORD64)(ce.verts.size() * sizeof(Engine::FVector3d)))) {
                ce.verts.clear(); continue;
            }
            ce.indices.resize((size_t)idxs.num);
            if (!Memory::read((DWORD64)idxs.data, ce.indices.data(),
                (DWORD64)(ce.indices.size() * sizeof(int32_t)))) {
                ce.verts.clear(); ce.indices.clear(); continue;
            }
            ++convexOkOut;
        }
    }

    // Box elements
    const TArray boxes = Memory::read<TArray>(agg + Offsets::AggGeom_BoxElems);
    if (boxes.data && boxes.num > 0 && boxes.num <= 128) {
        entry->hasBox = true;
        entry->boxElems.resize((size_t)boxes.num);
        for (int i = 0; i < boxes.num; ++i) {
            auto& be = entry->boxElems[(size_t)i];
            const uintptr_t elem = boxes.data + (uintptr_t)i * Offsets::BoxElem_Stride;
            be.center = Memory::read<Engine::FVector3d>(elem + Offsets::BoxElem_Center);
            be.pitch = Memory::read<double>(elem + Offsets::BoxElem_Rotation + 0x00);
            be.yaw = Memory::read<double>(elem + Offsets::BoxElem_Rotation + 0x08);
            be.roll = Memory::read<double>(elem + Offsets::BoxElem_Rotation + 0x10);
            be.hx = Memory::read<float>(elem + Offsets::BoxElem_XExtent) * 0.5f;
            be.hy = Memory::read<float>(elem + Offsets::BoxElem_YExtent) * 0.5f;
            be.hz = Memory::read<float>(elem + Offsets::BoxElem_ZExtent) * 0.5f;
            ++boxOkOut;
        }
    }

    // Bounds
    const Engine::FVector3d ext = Memory::read<Engine::FVector3d>(
        mesh + Offsets::ExtendedBounds);
    const Engine::FVector3d posExt = Memory::read<Engine::FVector3d>(
        mesh + Offsets::PositiveBoundsExt);
    const Engine::FVector3d negExt = Memory::read<Engine::FVector3d>(
        mesh + Offsets::NegativeBoundsExt);
    const Engine::FVector3d extExt = Memory::read<Engine::FVector3d>(
        mesh + Offsets::ExtendedBounds + 0x18);
    float bx = (float)(extExt.x - (posExt.x + negExt.x) * 0.5);
    float by = (float)(extExt.y - (posExt.y + negExt.y) * 0.5);
    float bz = (float)(extExt.z - (posExt.z + negExt.z) * 0.5);
    if (bx >= 5.f && by >= 5.f && bz >= 5.f && bx < 800.f && by < 800.f && bz < 800.f) {
        entry->hasBounds = true;
        entry->boundsExtent = ext;
        ++boundsOkOut;
    } else {
        ++boundsBadOut;
    }

    return entry;
}

static bool EmitCachedMesh(const CachedMeshGeom& cached, const CompXform& xf,
    BlockerClass cls, std::vector<Tri>& all,
    int& probeHits, int& convexOk, int& boxOk, int& boundsOk)
{
    bool any = false;

    if (cached.hasConvex) {
        for (const auto& ce : cached.convexElems) {
            if (ce.verts.empty() || ce.indices.empty()) continue;
            const int vCount = (int)ce.verts.size();
            std::vector<Vector3> worldVerts(ce.verts.size());
            bool vertsOk = true;
            for (size_t vi = 0; vi < ce.verts.size(); ++vi) {
                const Vector3 local{ (float)ce.verts[vi].x, (float)ce.verts[vi].y, (float)ce.verts[vi].z };
                worldVerts[vi] = XformLocal(xf, local);
                if (!std::isfinite(worldVerts[vi].x) || !std::isfinite(worldVerts[vi].y)
                    || !std::isfinite(worldVerts[vi].z)) { vertsOk = false; break; }
            }
            if (!vertsOk) continue;
            const int triCount = (int)(ce.indices.size() / 3);
            for (int ti = 0; ti < triCount && (int)all.size() < kMaxTris; ++ti) {
                const int i0 = ce.indices[(size_t)ti * 3];
                const int i1 = ce.indices[(size_t)ti * 3 + 1];
                const int i2 = ce.indices[(size_t)ti * 3 + 2];
                if (i0 < 0 || i0 >= vCount || i1 < 0 || i1 >= vCount || i2 < 0 || i2 >= vCount) continue;
                all.push_back({ worldVerts[(size_t)i0], worldVerts[(size_t)i1], worldVerts[(size_t)i2], cls });
                any = true;
            }
            ++convexOk;
        }
        ++probeHits;
    }

    if (cached.hasBox) {
        bool boxAny = false;
        for (const auto& be : cached.boxElems) {
            if (be.hx <= 0 || be.hy <= 0 || be.hz <= 0) continue;
            const FQuat boxRot = RotatorToQuat(be.pitch, be.yaw, be.roll);
            const Vector3 center{ (float)be.center.x, (float)be.center.y, (float)be.center.z };
            const Vector3 signs[8] = {
                {-1,-1,-1},{+1,-1,-1},{+1,+1,-1},{-1,+1,-1},
                {-1,-1,+1},{+1,-1,+1},{+1,+1,+1},{-1,+1,+1}
            };
            Vector3 wp[8];
            for (int i = 0; i < 8; ++i) {
                const Vector3 local{
                    center.x + be.hx * signs[i].x,
                    center.y + be.hy * signs[i].y,
                    center.z + be.hz * signs[i].z
                };
                wp[i] = XformLocal(xf, local);
            }
            static constexpr int faces[12][3] = {
                {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},
                {2,6,7},{2,7,3},{0,3,7},{0,7,4},{1,5,6},{1,6,2}
            };
            for (const auto& f : faces) {
                if ((int)all.size() >= kMaxTris) break;
                all.push_back({ wp[f[0]], wp[f[1]], wp[f[2]], cls });
            }
            boxAny = true;
            ++probeHits;
        }
        if (boxAny) { any = true; ++boxOk; }
    }

    if (cached.hasBounds) {
        const float hx = cached.boundsExtent.x;
        const float hy = cached.boundsExtent.y;
        const float hz = cached.boundsExtent.z;
        const Vector3 signs[8] = {
            {-1,-1,-1},{+1,-1,-1},{+1,+1,-1},{-1,+1,-1},
            {-1,-1,+1},{+1,-1,+1},{+1,+1,+1},{-1,+1,+1}
        };
        Vector3 wp[8];
        for (int i = 0; i < 8; ++i) {
            wp[i] = XformLocal(xf, { hx * signs[i].x, hy * signs[i].y, hz * signs[i].z });
        }
        static constexpr int faces[12][3] = {
            {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},
            {2,6,7},{2,7,3},{0,3,7},{0,7,4},{1,5,6},{1,6,2}
        };
        for (const auto& f : faces) {
            if ((int)all.size() >= kMaxTris) break;
            all.push_back({ wp[f[0]], wp[f[1]], wp[f[2]], cls });
        }
        ++probeHits;
        ++boundsOk;
        any = true;
    }

    return any;
}

// ═══════════════════════════════════════════════════════════════════════════
// Collect SMC roots (from forum: collect_static_mesh_roots)
// ═══════════════════════════════════════════════════════════════════════════

static uint64_t SmcFingerprint(uintptr_t root) {
    if (!root || !Memory::IsValidPtrFast2(root)) return 0;
    const uintptr_t mesh = Memory::read<uintptr_t>(root + Offsets::StaticMesh);
    if (!mesh || !Memory::IsValidPtrFast2(mesh)) return 0;
    const uintptr_t xform = root + Offsets::ComponentToWorld;
    const FQuat q = Memory::read<FQuat>(xform);
    const Engine::FVector3d t = Memory::read<Engine::FVector3d>(xform + 0x20);
    const Engine::FVector3d s = Memory::read<Engine::FVector3d>(xform + 0x40);
    double qw = q.w, qx = q.x, qy = q.y, qz = q.z;
    if (qw < 0) { qw = -qw; qx = -qx; qy = -qy; qz = -qz; }
    auto qi = [](double v) -> uint64_t { return (uint64_t)(int64_t)std::llround(v * 10000.0); };
    auto ti = [](double v) -> uint64_t { return (uint64_t)(int64_t)std::llround(v); };
    auto si = [](double v) -> uint64_t { return (uint64_t)(int64_t)std::llround(v * 1000.0); };
    uint64_t h = (uint64_t)mesh;
    auto mix = [&](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(qi(qx)); mix(qi(qy)); mix(qi(qz)); mix(qi(qw));
    mix(ti(t.x)); mix(ti(t.y)); mix(ti(t.z));
    mix(si(s.x)); mix(si(s.y)); mix(si(s.z));
    return h;
}

static void CollectSmcRoots(Engine& eng, const Vector3& localPos,
    float radiusCm, std::vector<SmcEntry>& out)
{
    out.clear();
    uintptr_t gWorld = 0, level = 0, actors = 0;
    {
        std::shared_lock<std::shared_mutex> lock(eng.m_stateMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        gWorld = eng.GWorld;
        level = eng.PersistentLevel;
        actors = eng.Actors;
    }
    if (!gWorld || !level || !actors) return;

    int32_t actorCount = 0;
    uintptr_t actorData = 0;
    if (!WorldScan::ReadLevelActors(level, actorData, actorCount)) return;
    if (actorCount <= 0 || actorCount > 65536) return;

    const int scanN = (actorCount > 1024) ? 1024 : actorCount;
    const float r2 = radiusCm * radiusCm;
    const uintptr_t pawn = eng.AcknowledgedPawn;

    std::vector<uintptr_t> roots((size_t)scanN);
    {
        ScatterSession session;
        for (int i = 0; i < scanN; ++i) {
            session.prepare(actorData + (uintptr_t)(i * sizeof(uintptr_t)), &roots[i], sizeof(uintptr_t));
        }
        if (!session.execute()) return;
    }

    std::vector<uintptr_t> actorsOk;
    actorsOk.reserve(512);
    for (uintptr_t a : roots) {
        if (!a || !Memory::IsValidPtrFast2(a)) continue;
        actorsOk.push_back(a);
    }
    if (actorsOk.empty()) return;

    // Read roots in batch
    constexpr int kChunk = 256;
    std::vector<uintptr_t> rootPtrs(actorsOk.size(), 0);
    for (size_t off = 0; off < actorsOk.size(); off += (size_t)kChunk) {
        const size_t n = (std::min)((size_t)kChunk, actorsOk.size() - off);
        ScatterSession session;
        for (size_t i = 0; i < n; ++i) {
            session.prepare(actorsOk[off + i] + Offsets::RootComponent,
                &rootPtrs[off + i], sizeof(uintptr_t));
        }
        if (!session.execute()) return;
    }

    struct Row { uintptr_t root; uintptr_t actor; };
    std::vector<Row> rows;
    rows.reserve(actorsOk.size());
    for (size_t i = 0; i < actorsOk.size(); ++i) {
        if (!rootPtrs[i] || !Memory::IsValidPtrFast2(rootPtrs[i])) continue;
        rows.push_back({ rootPtrs[i], actorsOk[i] });
    }
    if (rows.empty()) return;

    // Classify and distance filter
    std::unordered_set<uint64_t> seenFp;
    seenFp.reserve(512);
    for (auto& row : rows) {
        const Vector3 p = Engine::ToVector3(
            Memory::read<Engine::FVector3d>(row.root + Offsets::ComponentToWorld + 0x20));
        const float dx = p.x - localPos.x;
        const float dy = p.y - localPos.y;
        const float dz = p.z - localPos.z;
        if (dx * dx + dy * dy + dz * dz > r2) continue;
        const uint64_t fp = SmcFingerprint(row.root);
        if (fp == 0 || !seenFp.insert(fp).second) continue;
        out.push_back({ row.root, row.actor });
        if ((int)out.size() >= kMaxSmc) break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Rebuild (from forum: rebuild)
// ═══════════════════════════════════════════════════════════════════════════

static void RebuildFromSmc(Engine& eng, const std::vector<SmcEntry>& smc, ProbeStatus& probeOut) {
    std::vector<Tri> all;
    all.reserve(8192);
    int probeHits = 0, missMesh = 0;
    int convexOk = 0, boxOk = 0, boundsOk = 0, boundsBad = 0;

    for (const auto& e : smc) {
        if ((int)all.size() >= kMaxTris) break;
        const uintptr_t root = e.root;
        uintptr_t mesh = Memory::read<uintptr_t>(root + Offsets::StaticMesh);
        if (!mesh || !Memory::IsValidPtrFast2(mesh))
            mesh = Memory::read<uintptr_t>(root + Offsets::StaticMeshLegacy);
        if (!mesh || !Memory::IsValidPtrFast2(mesh)) { ++missMesh; continue; }

        const CompXform xf = ReadCompXform(root);
        const BlockerClass cls = ClassifyByMeshFName(mesh);

        auto it = g_geomCache.find(mesh);
        if (it != g_geomCache.end() && it->second) {
            ++g_cacheHits;
            EmitCachedMesh(*it->second, xf, cls, all, probeHits, convexOk, boxOk, boundsOk);
            continue;
        }
        ++g_cacheMisses;
        auto cached = PopulateGeomCache(mesh, cls, missMesh, convexOk, boxOk, boundsOk, boundsBad);
        g_geomCache[mesh] = cached;
        EmitCachedMesh(*cached, xf, cls, all, probeHits, convexOk, boxOk, boundsOk);
    }

    if (probeHits == 0)
        probeOut = ProbeStatus::Red;
    else if (probeHits < (int)smc.size() / 2)
        probeOut = ProbeStatus::Yellow;
    else
        probeOut = ProbeStatus::Green;

    auto holder = std::make_shared<TreeHolder>();
    holder->triCount = (int)all.size();
    if (!all.empty())
        holder->root = BuildKd(all, 0);

    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        g_liveTree = std::move(holder);
    }
    g_probe.store((int)probeOut, std::memory_order_relaxed);
    g_smc.store((int)smc.size(), std::memory_order_relaxed);
}

// ═══════════════════════════════════════════════════════════════════════════
// TickRebuild
// ═══════════════════════════════════════════════════════════════════════════

static float RangeForVar(float baseM) {
    float r = baseM;
    if (var::vis_use_player_esp_dist)
        r = (std::min)(r, var::esp_distance);
    return (std::min)(r, kVisHardCapM);
}

void TickRebuild(Engine& eng) {
    if (g_rebuilding.load(std::memory_order_relaxed) != 0)
        return;

    Vector3 localPos{};
    {
        std::shared_lock<std::shared_mutex> lock(eng.m_stateMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        localPos = eng.g_Camera.Location;
    }
    if (!IsPlausibleWorldPos(localPos)) return;

    const auto now = std::chrono::steady_clock::now();
    const float dx = localPos.x - g_lastRebuildPos.x;
    const float dy = localPos.y - g_lastRebuildPos.y;
    const float dz = localPos.z - g_lastRebuildPos.z;
    const float moved2 = dx * dx + dy * dy + dz * dz;
    const bool forced = g_lastRebuildTime.time_since_epoch().count() == 0
        || now - g_lastRebuildTime >= std::chrono::seconds(kRebuildForceSec);
    if (!forced && moved2 < kRebuildMoveCm * kRebuildMoveCm)
        return;

    g_rebuilding.store(1);
    std::thread([&eng, localPos]() {
        try {
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<SmcEntry> smc;
            CollectSmcRoots(eng, localPos, RebuildRadiusM() * 100.f, smc);
            ProbeStatus probe = ProbeStatus::Red;
            RebuildFromSmc(eng, smc, probe);
            g_lastRebuildPos = localPos;
            g_lastRebuildTime = std::chrono::steady_clock::now();
            g_rebuildMs.store((int)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        } catch (...) {}
        g_rebuilding.store(0);
    }).detach();
}

// ═══════════════════════════════════════════════════════════════════════════
// Public API
// ═══════════════════════════════════════════════════════════════════════════

RayHit RaycastLos(const Vector3& from, const Vector3& to) {
    g_queries.fetch_add(1, std::memory_order_relaxed);
    RayHit hit{};
    hit.clear = true;
    hit.valid = true;

    if (!var::vis_enabled) { g_failOpen.fetch_add(1, std::memory_order_relaxed); return hit; }
    if (g_probe.load(std::memory_order_relaxed) == (int)ProbeStatus::Red)
        return hit;

    std::shared_ptr<TreeHolder> tree;
    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        tree = g_liveTree;
    }
    if (!tree || !tree->root) { g_failOpen.fetch_add(1, std::memory_order_relaxed); return hit; }

    const Vector3 rd{ to.x - from.x, to.y - from.y, to.z - from.z };
    const float len2 = rd.x * rd.x + rd.y * rd.y + rd.z * rd.z;
    if (len2 < 1e-8f) return hit;

    const float len = std::sqrt(len2);
    const float bias = (std::max)(40.f, len * 0.02f);
    const Vector3 ro{
        from.x + rd.x / len * bias,
        from.y + rd.y / len * bias,
        from.z + rd.z / len * bias
    };
    const Vector3 rdNorm{ (to.x - ro.x) / len, (to.y - ro.y) / len, (to.z - ro.z) / len };

    auto safeInv = [](float v) -> float {
        constexpr float e = 1e-12f;
        return (std::fabs(v) > e) ? 1.f / v : (v >= 0.f ? 1e12f : -1e12f);
    };
    const Vector3 rdInv{ safeInv(rdNorm.x), safeInv(rdNorm.y), safeInv(rdNorm.z) };

    float bestT = 1.f;
    BlockerClass bestCls = BlockerClass::Other;
    BlockerClass hitCls = BlockerClass::Other;
    if (Traverse(tree->root.get(), ro, rdNorm, rdInv, 1.f, hitCls)) {
        hit.clear = false;
        hit.blocker = hitCls;
        g_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_clears.fetch_add(1, std::memory_order_relaxed);
    }

    return hit;
}

RayHit RaycastLosMulti(const Vector3& from, const Vector3* ends, int endCount) {
    RayHit anyClear{};
    anyClear.clear = true;
    anyClear.valid = true;
    if (!ends || endCount <= 0) return anyClear;
    RayHit firstBlocked{};
    bool sawBlock = false;
    for (int i = 0; i < endCount; ++i) {
        RayHit h = RaycastLos(from, ends[i]);
        if (h.clear) return h;
        if (!sawBlock) {
            firstBlocked = h;
            sawBlock = true;
        }
    }
    return sawBlock ? firstBlocked : anyClear;
}

float EffectivePlayerRangeM() {
    return RangeForVar(var::esp_distance);
}

float EffectiveBotRangeM() {
    return RangeForVar(var::bot_esp_distance);
}

float RebuildRadiusM() {
    float r = (std::max)(50.f, (std::max)(EffectivePlayerRangeM(), EffectiveBotRangeM()));
    return (std::min)(r, kVisHardCapM);
}

Stats GetStats() {
    Stats s{};
    s.probe = (ProbeStatus)g_probe.load(std::memory_order_relaxed);
    s.smc = g_smc.load(std::memory_order_relaxed);
    s.rebuilding = g_rebuilding.load(std::memory_order_relaxed);
    s.rebuildMs = g_rebuildMs.load(std::memory_order_relaxed);
    s.queries = g_queries.load(std::memory_order_relaxed);
    s.failOpen = g_failOpen.load(std::memory_order_relaxed);
    s.hits = g_hits.load(std::memory_order_relaxed);
    s.clears = g_clears.load(std::memory_order_relaxed);
    s.wallSmc = g_wallSmc.load(std::memory_order_relaxed);
    s.doorSmc = g_doorSmc.load(std::memory_order_relaxed);
    s.treeSmc = g_treeSmc.load(std::memory_order_relaxed);
    s.otherSmc = g_otherSmc.load(std::memory_order_relaxed);
    std::shared_ptr<TreeHolder> tree;
    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        tree = g_liveTree;
    }
    s.tris = tree ? tree->triCount : 0;
    return s;
}

void RecordDebugRay(const DebugRay& ray) {
    std::lock_guard<std::mutex> lk(g_debugMutex);
    if (g_debugRays.size() >= kMaxDebugRays)
        g_debugRays.erase(g_debugRays.begin());
    g_debugRays.push_back(ray);
}

void CopyDebugRays(std::vector<DebugRay>& out) {
    std::lock_guard<std::mutex> lk(g_debugMutex);
    out = g_debugRays;
}

void CopyDebugTris(std::vector<DebugTri>& out) {
    std::lock_guard<std::mutex> lk(g_debugMutex);
    out = g_debugTris;
}

void ApplyVisToEspCaches(Engine& eng) {
    if (!var::vis_enabled) return;
    if (g_probe.load(std::memory_order_relaxed) == (int)ProbeStatus::Red) return;

    Vector3 cam{};
    {
        std::shared_lock<std::shared_mutex> lock(eng.m_stateMutex, std::try_to_lock);
        if (!lock.owns_lock()) return;
        cam = eng.g_Camera.Location;
    }
    if (!IsPlausibleWorldPos(cam)) return;

    const float playerR = EffectivePlayerRangeM() * 100.f;
    const float botR = EffectiveBotRangeM() * 100.f;
    const int needMiss = var::vis_hysteresis_frames;

    auto applyActor = [&](uintptr_t actor, uintptr_t root, bool isVisible) {
        if (!root || !Memory::IsValidPtrFast2(root)) return;
        const Vector3 worldPos = Engine::ToVector3(
            Memory::read<Engine::FVector3d>(root + Offsets::ComponentToWorld + 0x20));
        if (!IsPlausibleWorldPos(worldPos)) return;
        const Vector3 d{ worldPos.x - cam.x, worldPos.y - cam.y, worldPos.z - cam.z };
        const float distSq = d.x * d.x + d.y * d.y + d.z * d.z;
        const float rangeSq = (var::vis_multi_bone) ? botR : playerR;
        if (distSq > rangeSq * rangeSq) return;

        Vector3 ends[3];
        int n = 0;
        ends[n++] = worldPos;
        if (var::vis_multi_bone) {
            Vector3 head = worldPos; head.z += 160.f;
            ends[n++] = head;
        }
        RayHit h = RaycastLosMulti(cam, ends, n);
        const uint64_t key = (uint64_t)actor;
        if (h.clear) {
            eng.m_playerCacheMutex.try_lock();
            eng.m_playerCacheMutex.unlock();
            // visibility tracking omitted for brevity
        }
    };

    // Player cache
    {
        std::shared_lock<std::shared_mutex> lock(eng.m_playerCacheMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (auto& [key, actor] : eng.playerCache) {
                if (!actor.isVisible) continue;
                applyActor(key, actor.rootComponent, true);
            }
        }
    }
    // Robot cache
    {
        std::shared_lock<std::shared_mutex> lock(eng.m_robotCacheMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (auto& [key, actor] : eng.robotCache) {
                if (!actor.isVisible) continue;
                applyActor(key, actor.rootComponent, true);
            }
        }
    }
}

bool AimLosAllows(const Vector3& from, const Vector3& aimBoneWorld) {
    if (!var::vis_enabled) return true;
    if (g_probe.load(std::memory_order_relaxed) == (int)ProbeStatus::Red)
        return true;
    return RaycastLos(from, aimBoneWorld).clear;
}

void ClearGeomCache() {
    g_geomCache.clear();
    g_cacheHits.store(0, std::memory_order_relaxed);
    g_cacheMisses.store(0, std::memory_order_relaxed);
    g_bodySetupProbed = false;
    g_bodySetupOffset.store(Offsets::BodySetup, std::memory_order_relaxed);
}

const char* BlockerClassName(BlockerClass c) {
    switch (c) {
    case BlockerClass::Wall: return "wall";
    case BlockerClass::Door: return "door";
    case BlockerClass::Tree: return "tree";
    default: return "other";
    }
}

} // namespace CollisionVis
