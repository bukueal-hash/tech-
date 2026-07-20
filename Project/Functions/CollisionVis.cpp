#include "CollisionVis.h"
#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Interface/Utils/Variables/index.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <iostream>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollisionVis {
namespace {

struct Tri {
    Vector3 p0{}, p1{}, p2{};
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

struct SmcEntry {
    uintptr_t root = 0;
    uintptr_t actor = 0;
};

struct CompXform {
    FQuat rot{ 0, 0, 0, 1 };
    Engine::FVector3d trans{};
    Engine::FVector3d scale{ 1, 1, 1 };
};

std::shared_ptr<const TreeHolder> g_liveTree;
std::mutex g_swapMutex;
std::atomic<int> g_rebuilding{ 0 };
std::atomic<int> g_rebuildMs{ 0 };
std::atomic<int> g_smc{ 0 };
std::atomic<int> g_queries{ 0 };
std::atomic<int> g_failOpen{ 0 };
std::atomic<int> g_hits{ 0 };
std::atomic<int> g_clears{ 0 };
std::atomic<int> g_probe{ static_cast<int>(ProbeStatus::Red) };
std::atomic<int> g_wallSmc{ 0 };
std::atomic<int> g_doorSmc{ 0 };
std::atomic<int> g_treeSmc{ 0 };
std::atomic<int> g_otherSmc{ 0 };

std::mutex g_debugMutex;
std::vector<DebugRay> g_debugRays;
std::vector<DebugTri> g_debugTris;
constexpr size_t kMaxDebugRays = 64;
constexpr size_t kMaxDebugTris = 2000;

std::unordered_map<uintptr_t, uint8_t> g_visMiss;
Vector3 g_lastRebuildPos{};
auto g_lastRebuildTime = std::chrono::steady_clock::time_point{};

constexpr int kMaxSmc = 128;
constexpr int kMaxTris = 40000;
constexpr float kRebuildMoveCm = 3000.f;
constexpr int kRebuildForceSec = 20;
constexpr float kVisHardCapM = 50.f;

Vector3 VMin(const Vector3& a, const Vector3& b)
{
    return { (std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::min)(a.z, b.z) };
}
Vector3 VMax(const Vector3& a, const Vector3& b)
{
    return { (std::max)(a.x, b.x), (std::max)(a.y, b.y), (std::max)(a.z, b.z) };
}

FQuat RotatorToQuat(double pitchDeg, double yawDeg, double rollDeg)
{
    constexpr double k = 3.14159265358979323846 / 360.0;
    const double sp = std::sin(pitchDeg * k), cp = std::cos(pitchDeg * k);
    const double sy = std::sin(yawDeg * k), cy = std::cos(yawDeg * k);
    const double sr = std::sin(rollDeg * k), cr = std::cos(rollDeg * k);
    return FQuat{
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        sr * cp * cy - cr * sp * sy,
        cr * cp * cy + sr * sp * sy };
}

Vector3 XformLocal(const CompXform& xf, const Vector3& local)
{
    const Vector3 scaled{
        local.x * xf.scale.x,
        local.y * xf.scale.y,
        local.z * xf.scale.z };
    const Vector3 rotated = xf.rot.RotateVector(scaled);
    return {
        rotated.x + xf.trans.x,
        rotated.y + xf.trans.y,
        rotated.z + xf.trans.z };
}

bool RayAabb(const Vector3& ro, const Vector3& rdInv, const Vector3& bmin, const Vector3& bmax)
{
    const Vector3 t0{ (bmin.x - ro.x) * rdInv.x, (bmin.y - ro.y) * rdInv.y, (bmin.z - ro.z) * rdInv.z };
    const Vector3 t1{ (bmax.x - ro.x) * rdInv.x, (bmax.y - ro.y) * rdInv.y, (bmax.z - ro.z) * rdInv.z };
    const float tmin = static_cast<float>(
        (std::max)((std::max)((std::min)(t0.x, t1.x), (std::min)(t0.y, t1.y)), (std::min)(t0.z, t1.z)));
    const float tmax = static_cast<float>(
        (std::min)((std::min)((std::max)(t0.x, t1.x), (std::max)(t0.y, t1.y)), (std::max)(t0.z, t1.z)));
    return tmax >= 0.f && tmin <= tmax;
}

bool RayTri(const Vector3& ro, const Vector3& rd, const Tri& t, float tMax, float& outT)
{
    constexpr float kEps = 1e-6f;
    const Vector3 e1{ t.p1.x - t.p0.x, t.p1.y - t.p0.y, t.p1.z - t.p0.z };
    const Vector3 e2{ t.p2.x - t.p0.x, t.p2.y - t.p0.y, t.p2.z - t.p0.z };
    const Vector3 h{
        rd.y * e2.z - rd.z * e2.y,
        rd.z * e2.x - rd.x * e2.z,
        rd.x * e2.y - rd.y * e2.x };
    const float a = static_cast<float>(e1.x * h.x + e1.y * h.y + e1.z * h.z);
    if (a > -kEps && a < kEps)
        return false;
    const float f = 1.f / a;
    const Vector3 s{ ro.x - t.p0.x, ro.y - t.p0.y, ro.z - t.p0.z };
    const float u = static_cast<float>(f * (s.x * h.x + s.y * h.y + s.z * h.z));
    if (u < 0.f || u > 1.f)
        return false;
    const Vector3 q{
        s.y * e1.z - s.z * e1.y,
        s.z * e1.x - s.x * e1.z,
        s.x * e1.y - s.y * e1.x };
    const float v = static_cast<float>(f * (rd.x * q.x + rd.y * q.y + rd.z * q.z));
    if (v < 0.f || u + v > 1.f)
        return false;
    const float tv = static_cast<float>(f * (e2.x * q.x + e2.y * q.y + e2.z * q.z));
    if (tv > kEps && tv < tMax - kEps) {
        outT = tv;
        return true;
    }
    return false;
}

std::unique_ptr<KdNode> BuildKd(std::vector<Tri>& tris, int depth)
{
    if (tris.empty())
        return nullptr;
    auto node = std::make_unique<KdNode>();
    node->bmin = node->bmax = tris[0].p0;
    for (const Tri& t : tris) {
        for (const Vector3& p : { t.p0, t.p1, t.p2 }) {
            node->bmin = VMin(node->bmin, p);
            node->bmax = VMax(node->bmax, p);
        }
    }
    if (tris.size() <= 8 || depth > 18) {
        node->tris = std::move(tris);
        return node;
    }
    const int axis = depth % 3;
    std::nth_element(tris.begin(), tris.begin() + tris.size() / 2, tris.end(),
        [axis](const Tri& a, const Tri& b) {
            auto axisVal = [axis](const Tri& t) -> double {
                if (axis == 0)
                    return (t.p0.x + t.p1.x + t.p2.x) / 3.0;
                if (axis == 1)
                    return (t.p0.y + t.p1.y + t.p2.y) / 3.0;
                return (t.p0.z + t.p1.z + t.p2.z) / 3.0;
            };
            return axisVal(a) < axisVal(b);
        });
    const size_t mid = tris.size() / 2;
    std::vector<Tri> left(tris.begin(), tris.begin() + mid);
    std::vector<Tri> right(tris.begin() + mid, tris.end());
    node->left = BuildKd(left, depth + 1);
    node->right = BuildKd(right, depth + 1);
    return node;
}

bool Traverse(const KdNode* node, const Vector3& ro, const Vector3& rd, const Vector3& rdInv,
    float tMax, float& bestT, BlockerClass& bestCls)
{
    if (!node)
        return false;
    if (!RayAabb(ro, rdInv, node->bmin, node->bmax))
        return false;
    bool hit = false;
    for (const Tri& t : node->tris) {
        float tv = 0.f;
        if (RayTri(ro, rd, t, tMax, tv) && tv < bestT) {
            bestT = tv;
            bestCls = t.cls;
            hit = true;
        }
    }
    if (Traverse(node->left.get(), ro, rd, rdInv, tMax, bestT, bestCls))
        hit = true;
    if (Traverse(node->right.get(), ro, rd, rdInv, tMax, bestT, bestCls))
        hit = true;
    return hit;
}

void AppendBoxTrisWorld(
    const Vector3& centerLocal,
    const FQuat& boxRot,
    float hx, float hy, float hz,
    const CompXform& xf,
    BlockerClass cls,
    std::vector<Tri>& out)
{
    const Vector3 signs[8] = {
        { -1, -1, -1 }, { +1, -1, -1 }, { +1, +1, -1 }, { -1, +1, -1 },
        { -1, -1, +1 }, { +1, -1, +1 }, { +1, +1, +1 }, { -1, +1, +1 },
    };
    Vector3 wp[8];
    for (int i = 0; i < 8; ++i) {
        const Vector3 extLocal{
            hx * signs[i].x,
            hy * signs[i].y,
            hz * signs[i].z };
        const Vector3 rotatedExt = boxRot.RotateVector(extLocal);
        const Vector3 local{
            centerLocal.x + rotatedExt.x,
            centerLocal.y + rotatedExt.y,
            centerLocal.z + rotatedExt.z };
        wp[i] = XformLocal(xf, local);
    }
    static constexpr int faces[12][3] = {
        { 0, 1, 2 }, { 0, 2, 3 }, { 4, 6, 5 }, { 4, 7, 6 }, { 0, 4, 5 }, { 0, 5, 1 },
        { 2, 6, 7 }, { 2, 7, 3 }, { 0, 3, 7 }, { 0, 7, 4 }, { 1, 5, 6 }, { 1, 6, 2 },
    };
    for (const auto& f : faces) {
        if ((int)out.size() >= kMaxTris)
            break;
        out.push_back({ wp[f[0]], wp[f[1]], wp[f[2]], cls });
    }
}

BlockerClass ClassifyFName(const std::string& fname)
{
    if (fname.empty())
        return BlockerClass::Other;
    std::string lower = fname;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("door") != std::string::npos
        || lower.find("gate") != std::string::npos
        || lower.find("hatch") != std::string::npos)
        return BlockerClass::Door;
    if (lower.find("tree") != std::string::npos
        || lower.find("foliage") != std::string::npos
        || lower.find("bush") != std::string::npos
        || lower.find("plant") != std::string::npos)
        return BlockerClass::Tree;
    if (lower.find("wall") != std::string::npos
        || lower.find("building") != std::string::npos
        || lower.find("panel") != std::string::npos
        || lower.find("fence") != std::string::npos
        || lower.find("barrier") != std::string::npos)
        return BlockerClass::Wall;
    return BlockerClass::Other;
}

CompXform ReadCompXform(uintptr_t root)
{
    CompXform xf{};
    xf.rot = Memory::read<FQuat>(root + Offsets::ComponentToWorld);
    xf.trans = Memory::read<Engine::FVector3d>(root + Offsets::WorldLocation);
    xf.scale = Memory::read<Engine::FVector3d>(root + Offsets::ComponentToWorld + 0x40);
    if (xf.scale.x == 0 && xf.scale.y == 0 && xf.scale.z == 0)
        xf.scale = { 1, 1, 1 };
    const double n = xf.rot.x * xf.rot.x + xf.rot.y * xf.rot.y
        + xf.rot.z * xf.rot.z + xf.rot.w * xf.rot.w;
    if (!std::isfinite(n) || n < 1e-8)
        xf.rot = { 0, 0, 0, 1 };
    return xf;
}

bool ReadConvexElems(uintptr_t bodySetup, const CompXform& xf, BlockerClass cls,
    std::vector<Tri>& out, int& probeHits, int& convexOk)
{
    struct TArray { uintptr_t data; int32_t num; int32_t max; };
    const uintptr_t agg = bodySetup + Offsets::AggGeom;
    const TArray conv = Memory::read<TArray>(agg + Offsets::AggGeom_ConvexElems);
    if (!conv.data || conv.num <= 0 || conv.num > 64)
        return false;
    bool any = false;
    for (int ei = 0; ei < conv.num && (int)out.size() < kMaxTris; ++ei) {
        const uintptr_t elem = conv.data + (uintptr_t)ei * Offsets::ConvexElem_Stride;
        const TArray verts = Memory::read<TArray>(elem + Offsets::ConvexElem_VertexData);
        const TArray idxs = Memory::read<TArray>(elem + Offsets::ConvexElem_IndexData);
        if (!verts.data || verts.num <= 0 || verts.num > 4096)
            continue;
        if (!idxs.data || idxs.num <= 0 || idxs.num > 65536 || (idxs.num % 3) != 0)
            continue;

        std::vector<Engine::FVector3d> raw(static_cast<size_t>(verts.num));
        if (!Memory::read(static_cast<DWORD64>(verts.data), raw.data(),
                static_cast<DWORD64>(raw.size() * sizeof(Engine::FVector3d))))
            continue;
        std::vector<Vector3> worldVerts(static_cast<size_t>(verts.num));
        bool vertsOk = true;
        for (int vi = 0; vi < verts.num; ++vi) {
            const Vector3 local{ raw[static_cast<size_t>(vi)].x, raw[static_cast<size_t>(vi)].y,
                raw[static_cast<size_t>(vi)].z };
            worldVerts[static_cast<size_t>(vi)] = XformLocal(xf, local);
            if (!std::isfinite(worldVerts[static_cast<size_t>(vi)].x)) {
                vertsOk = false;
                break;
            }
        }
        if (!vertsOk)
            continue;

        std::vector<int32_t> rawIdx(static_cast<size_t>(idxs.num));
        bool idxOk = Memory::read(static_cast<DWORD64>(idxs.data), rawIdx.data(),
            static_cast<DWORD64>(rawIdx.size() * sizeof(int32_t)));
        auto countGood = [&](const std::vector<int32_t>& idx) -> int {
            const int tc = static_cast<int>(idx.size() / 3);
            const int scan = (std::min)(tc, 48);
            int goods = 0;
            for (int ti = 0; ti < scan; ++ti) {
                const int i0 = idx[static_cast<size_t>(ti * 3)];
                const int i1 = idx[static_cast<size_t>(ti * 3 + 1)];
                const int i2 = idx[static_cast<size_t>(ti * 3 + 2)];
                if (i0 >= 0 && i0 < verts.num && i1 >= 0 && i1 < verts.num && i2 >= 0 && i2 < verts.num)
                    ++goods;
            }
            return goods;
        };
        if (!idxOk || countGood(rawIdx) <= 0) {
            std::vector<uint16_t> u16(static_cast<size_t>(idxs.num));
            if (!Memory::read(static_cast<DWORD64>(idxs.data), u16.data(),
                    static_cast<DWORD64>(u16.size() * sizeof(uint16_t))))
                continue;
            for (size_t i = 0; i < u16.size(); ++i)
                rawIdx[i] = static_cast<int32_t>(u16[i]);
            if (countGood(rawIdx) <= 0)
                continue;
        }

        const int triCount = idxs.num / 3;
        for (int ti = 0; ti < triCount && (int)out.size() < kMaxTris; ++ti) {
            const int i0 = rawIdx[static_cast<size_t>(ti * 3)];
            const int i1 = rawIdx[static_cast<size_t>(ti * 3 + 1)];
            const int i2 = rawIdx[static_cast<size_t>(ti * 3 + 2)];
            if (i0 < 0 || i0 >= verts.num || i1 < 0 || i1 >= verts.num || i2 < 0 || i2 >= verts.num)
                continue;
            out.push_back({
                worldVerts[static_cast<size_t>(i0)],
                worldVerts[static_cast<size_t>(i1)],
                worldVerts[static_cast<size_t>(i2)],
                cls });
            any = true;
        }
        if (any)
            ++convexOk;
    }
    if (any)
        ++probeHits;
    return any;
}

bool ReadBoxElems(uintptr_t bodySetup, const CompXform& xf, BlockerClass cls,
    std::vector<Tri>& out, int& probeHits, int& boxOk, int& rotatedBoxes)
{
    struct TArray { uintptr_t data; int32_t num; int32_t max; };
    const uintptr_t agg = bodySetup + Offsets::AggGeom;
    const TArray boxes = Memory::read<TArray>(agg + Offsets::AggGeom_BoxElems);
    if (!boxes.data || boxes.num <= 0 || boxes.num > 128)
        return false;
    bool any = false;
    for (int i = 0; i < boxes.num && (int)out.size() < kMaxTris; ++i) {
        const uintptr_t elem = boxes.data + (uintptr_t)i * Offsets::BoxElem_Stride;
        const Engine::FVector3d center = Memory::read<Engine::FVector3d>(elem + Offsets::BoxElem_Center);
        const double pitch = Memory::read<double>(elem + Offsets::BoxElem_Rotation + 0x00);
        const double yaw = Memory::read<double>(elem + Offsets::BoxElem_Rotation + 0x08);
        const double roll = Memory::read<double>(elem + Offsets::BoxElem_Rotation + 0x10);
        const float hx = Memory::read<float>(elem + Offsets::BoxElem_XExtent) * 0.5f;
        const float hy = Memory::read<float>(elem + Offsets::BoxElem_YExtent) * 0.5f;
        const float hz = Memory::read<float>(elem + Offsets::BoxElem_ZExtent) * 0.5f;
        if (hx <= 0.f || hy <= 0.f || hz <= 0.f)
            continue;
        if (hx > 650.f || hy > 650.f || hz > 650.f)
            continue;
        const FQuat boxRot = RotatorToQuat(pitch, yaw, roll);
        if (std::fabs(pitch) > 1e-3 || std::fabs(yaw) > 1e-3 || std::fabs(roll) > 1e-3)
            ++rotatedBoxes;
        AppendBoxTrisWorld(
            { center.x, center.y, center.z },
            boxRot, hx, hy, hz, xf, cls, out);
        any = true;
        ++probeHits;
    }
    if (any)
        ++boxOk;
    return any;
}

void CollectSmcRoots(Engine& eng, const Vector3& localPos, float radiusCm, std::vector<SmcEntry>& out)
{
    out.clear();
    uintptr_t gWorld = 0, level = 0, actors = 0;
    {
        std::shared_lock<std::shared_mutex> lock(eng.m_stateMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        gWorld = eng.GWorld;
        level = eng.PersistentLevel;
        actors = eng.Actors;
    }
    if (!gWorld || !level || !actors)
        return;
    const int actorCount = Memory::read<int>(level + Offsets::ActorsCount);
    if (actorCount <= 0 || actorCount > 65536)
        return;
    const int scanN = (actorCount > 1024) ? 1024 : actorCount;

    const float r2 = radiusCm * radiusCm;
    const uintptr_t localPawn = eng.AcknowledgedPawn;
    std::vector<uintptr_t> actorPtrs(static_cast<size_t>(scanN));
    {
        ScatterSession session;
        if (!session.isValid())
            return;
        for (int i = 0; i < scanN; ++i)
            session.prepare(actors + (uintptr_t)i * sizeof(uintptr_t), actorPtrs[static_cast<size_t>(i)]);
        if (!session.execute())
            return;
    }

    std::vector<uintptr_t> actorsOk;
    actorsOk.reserve(512);
    for (uintptr_t actor : actorPtrs) {
        if (!actor || actor == localPawn || !eng.IsValidPointer(actor))
            continue;
        actorsOk.push_back(actor);
    }
    if (actorsOk.empty())
        return;

    constexpr int kChunk = 256;
    std::vector<uintptr_t> roots(actorsOk.size(), 0);
    for (size_t off = 0; off < actorsOk.size(); off += static_cast<size_t>(kChunk)) {
        const size_t n = (std::min)(static_cast<size_t>(kChunk), actorsOk.size() - off);
        ScatterSession session;
        if (!session.isValid())
            return;
        for (size_t i = 0; i < n; ++i)
            session.prepare(actorsOk[off + i] + Offsets::RootComponent, roots[off + i]);
        if (!session.execute())
            return;
    }

    struct Row {
        uintptr_t actor = 0;
        uintptr_t root = 0;
        uintptr_t mesh = 0;
        Engine::FVector3d world{};
    };
    std::vector<Row> rows;
    rows.reserve(actorsOk.size());
    for (size_t i = 0; i < actorsOk.size(); ++i) {
        if (!roots[i] || !eng.IsValidPointer(roots[i]))
            continue;
        rows.push_back(Row{ actorsOk[i], roots[i], 0, {} });
    }
    if (rows.empty())
        return;

    for (size_t off = 0; off < rows.size(); off += static_cast<size_t>(kChunk)) {
        const size_t n = (std::min)(static_cast<size_t>(kChunk), rows.size() - off);
        ScatterSession session;
        if (!session.isValid())
            return;
        for (size_t i = 0; i < n; ++i) {
            session.prepare(rows[off + i].root + Offsets::StaticMesh, rows[off + i].mesh);
            session.prepare(rows[off + i].root + Offsets::WorldLocation, rows[off + i].world);
        }
        if (!session.execute())
            return;
    }

    struct Cand { uintptr_t root; uintptr_t actor; float dist2; };
    std::vector<Cand> cands;
    cands.reserve(512);
    int wallN = 0, doorN = 0, treeN = 0, otherN = 0;
    for (const Row& row : rows) {
        if (!row.mesh || !eng.IsValidPointer(row.mesh))
            continue;
        const Vector3 p = Engine::ToVector3(row.world);
        if (!IsPlausibleWorldPos(p))
            continue;
        const float dx = (float)(p.x - localPos.x);
        const float dy = (float)(p.y - localPos.y);
        const float dz = (float)(p.z - localPos.z);
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2)
            continue;
        cands.push_back({ row.root, row.actor, d2 });
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist2 < b.dist2; });
    for (size_t i = 0; i < cands.size() && (int)out.size() < kMaxSmc; ++i) {
        out.push_back({ cands[i].root, cands[i].actor });
        const BlockerClass cls = ClassifyFName(eng.GetActorFNameStringCached(cands[i].actor));
        if (cls == BlockerClass::Wall)
            ++wallN;
        else if (cls == BlockerClass::Door)
            ++doorN;
        else if (cls == BlockerClass::Tree)
            ++treeN;
        else
            ++otherN;
    }

    g_wallSmc.store(wallN, std::memory_order_relaxed);
    g_doorSmc.store(doorN, std::memory_order_relaxed);
    g_treeSmc.store(treeN, std::memory_order_relaxed);
    g_otherSmc.store(otherN, std::memory_order_relaxed);
}

void SnapshotDebugTris(const std::vector<Tri>& all)
{
    std::vector<DebugTri> snap;
    snap.reserve((std::min)(all.size(), kMaxDebugTris));
    for (size_t i = 0; i < all.size() && snap.size() < kMaxDebugTris; ++i)
        snap.push_back({ all[i].p0, all[i].p1, all[i].p2, all[i].cls });
    std::lock_guard<std::mutex> lk(g_debugMutex);
    g_debugTris = std::move(snap);
}

void RebuildFromSmc(Engine& eng, const std::vector<SmcEntry>& smc, ProbeStatus& probeOut)
{
    std::vector<Tri> all;
    all.reserve(8192);
    int probeHits = 0;
    int missMesh = 0, missBody = 0, boxOk = 0, convexOk = 0, boundsOk = 0, boundsBad = 0;
    int rotatedBoxes = 0;

    for (const SmcEntry& e : smc) {
        if ((int)all.size() >= kMaxTris)
            break;
        const uintptr_t root = e.root;
        uintptr_t mesh = Memory::read<uintptr_t>(root + Offsets::StaticMesh);
        if (!mesh || !Memory::IsValidPtrFast2(mesh))
            mesh = Memory::read<uintptr_t>(root + Offsets::StaticMeshLegacy);
        if (!mesh || !Memory::IsValidPtrFast2(mesh)) {
            ++missMesh;
            continue;
        }

        const CompXform xf = ReadCompXform(root);
        const BlockerClass cls = ClassifyFName(eng.GetActorFNameStringCached(e.actor));

        const uintptr_t body = Memory::read<uintptr_t>(mesh + Offsets::BodySetup);
        const bool bodyOk = body && Memory::IsValidPtrFast2(body);
        if (!bodyOk) {
            ++missBody;
        } else {
            if (ReadConvexElems(body, xf, cls, all, probeHits, convexOk))
                continue;
            if (ReadBoxElems(body, xf, cls, all, probeHits, boxOk, rotatedBoxes))
                continue;
        }

        const Engine::FVector3d ext = Memory::read<Engine::FVector3d>(mesh + Offsets::ExtendedBounds + 0x18);
        float hx = (float)ext.x, hy = (float)ext.y, hz = (float)ext.z;
        if (hx < 5.f || hy < 5.f || hz < 5.f || hx > 650.f || hy > 650.f || hz > 650.f) {
            ++boundsBad;
            continue;
        }
        AppendBoxTrisWorld({ 0, 0, 0 }, { 0, 0, 0, 1 }, hx, hy, hz, xf, cls, all);
        ++probeHits;
        ++boundsOk;
    }

    if (probeHits <= 0)
        probeOut = ProbeStatus::Red;
    else if (probeHits < 3)
        probeOut = ProbeStatus::Yellow;
    else
        probeOut = ProbeStatus::Green;

    SnapshotDebugTris(all);

    auto holder = std::make_shared<TreeHolder>();
    holder->triCount = (int)all.size();
    if (!all.empty())
        holder->root = BuildKd(all, 0);

    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        g_liveTree = std::move(holder);
    }
    g_probe.store(static_cast<int>(probeOut), std::memory_order_relaxed);
    g_smc.store((int)smc.size(), std::memory_order_relaxed);
}

} // namespace

const char* BlockerClassName(BlockerClass c)
{
    switch (c) {
    case BlockerClass::Wall: return "wall";
    case BlockerClass::Door: return "door";
    case BlockerClass::Tree: return "tree";
    default: return "other";
    }
}

float EffectivePlayerRangeM()
{
    float r = var::vis_max_range_m > 0.f ? var::vis_max_range_m : kVisHardCapM;
    if (var::vis_use_player_esp_dist && var::esp_distance > 0.f)
        r = (std::min)(r, var::esp_distance);
    return (std::min)(r, kVisHardCapM);
}

float EffectiveBotRangeM()
{
    float r = var::vis_max_range_m > 0.f ? var::vis_max_range_m : kVisHardCapM;
    if (var::vis_use_bot_esp_dist && var::bot_esp_distance > 0.f)
        r = (std::min)(r, var::bot_esp_distance);
    return (std::min)(r, kVisHardCapM);
}

float RebuildRadiusM()
{
    // First-pass prove: rebuild ignores ESP distance, hard-capped at 50m.
    float r = var::vis_max_range_m > 0.f ? var::vis_max_range_m : kVisHardCapM;
    return (std::min)(r, kVisHardCapM);
}

void TickRebuild(Engine& eng)
{
    if (!var::vis_enabled)
        return;
    if (g_rebuilding.exchange(1) != 0)
        return;

    Vector3 localPos{};
    {
        std::shared_lock<std::shared_mutex> lock(eng.m_cameraMutex, std::try_to_lock);
        if (lock.owns_lock())
            localPos = eng.g_Camera.Location;
    }
    if (!IsPlausibleWorldPos(localPos)) {
        g_rebuilding.store(0);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float dx = (float)(localPos.x - g_lastRebuildPos.x);
    const float dy = (float)(localPos.y - g_lastRebuildPos.y);
    const float dz = (float)(localPos.z - g_lastRebuildPos.z);
    const float moved2 = dx * dx + dy * dy + dz * dz;
    const bool forced = g_lastRebuildTime.time_since_epoch().count() == 0
        || now - g_lastRebuildTime >= std::chrono::seconds(kRebuildForceSec);
    if (!forced && moved2 < kRebuildMoveCm * kRebuildMoveCm) {
        g_rebuilding.store(0);
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<SmcEntry> smc;
    CollectSmcRoots(eng, localPos, RebuildRadiusM() * 100.f, smc);
    ProbeStatus probe = ProbeStatus::Red;
    RebuildFromSmc(eng, smc, probe);
    g_lastRebuildPos = localPos;
    g_lastRebuildTime = now;
    const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    g_rebuildMs.store(ms, std::memory_order_relaxed);
    g_rebuilding.store(0);

    if (var::show_debug_overlay || var::vis_debug) {
        static auto s_lastLog = std::chrono::steady_clock::time_point{};
        if (s_lastLog.time_since_epoch().count() == 0 || now - s_lastLog >= std::chrono::seconds(1)) {
            s_lastLog = now;
            const Stats st = GetStats();
            std::cout << "[debugVis] smc=" << st.smc
                << " tris=" << st.tris
                << " rebuilding=" << st.rebuilding
                << " rebuildMs=" << st.rebuildMs
                << " probe=" << (int)st.probe
                << " wall=" << st.wallSmc
                << " door=" << st.doorSmc
                << " tree=" << st.treeSmc
                << " queries=" << st.queries
                << " hits=" << st.hits
                << " clears=" << st.clears
                << " failOpen=" << st.failOpen
                << std::endl;
        }
    }
}

RayHit RaycastLos(const Vector3& from, const Vector3& to)
{
    g_queries.fetch_add(1, std::memory_order_relaxed);
    RayHit hit{};
    hit.clear = true;
    hit.valid = true;

    if (!var::vis_enabled) {
        g_failOpen.fetch_add(1, std::memory_order_relaxed);
        return hit;
    }
    if (g_probe.load(std::memory_order_relaxed) == (int)ProbeStatus::Red) {
        g_failOpen.fetch_add(1, std::memory_order_relaxed);
        return hit;
    }

    std::shared_ptr<const TreeHolder> tree;
    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        tree = g_liveTree;
    }
    if (!tree || !tree->root) {
        g_failOpen.fetch_add(1, std::memory_order_relaxed);
        return hit;
    }

    Vector3 rd{ (float)(to.x - from.x), (float)(to.y - from.y), (float)(to.z - from.z) };
    const float len2 = static_cast<float>(rd.x * rd.x + rd.y * rd.y + rd.z * rd.z);
    if (len2 < 1e-4f) {
        g_clears.fetch_add(1, std::memory_order_relaxed);
        return hit;
    }

    const float len = std::sqrt(len2);
    const float bias = (std::min)(40.f, len * 0.02f);
    Vector3 ro{
        (float)from.x + rd.x / len * bias,
        (float)from.y + rd.y / len * bias,
        (float)from.z + rd.z / len * bias };
    rd = { (float)(to.x - ro.x), (float)(to.y - ro.y), (float)(to.z - ro.z) };

    auto inv = [](float v) {
        constexpr float e = 1e-12f;
        if (std::fabs(v) < e)
            return v >= 0.f ? 1e12f : -1e12f;
        return 1.f / v;
    };
    const Vector3 rdInv{
        inv(static_cast<float>(rd.x)),
        inv(static_cast<float>(rd.y)),
        inv(static_cast<float>(rd.z)) };
    float bestT = 1.f;
    BlockerClass bestCls = BlockerClass::Other;
    const bool blocked = Traverse(tree->root.get(), ro, rd, rdInv, 1.f, bestT, bestCls);
    if (blocked) {
        hit.clear = false;
        hit.hitT = bestT;
        hit.blocker = bestCls;
        hit.hitPos = {
            ro.x + rd.x * bestT,
            ro.y + rd.y * bestT,
            ro.z + rd.z * bestT };
        g_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_clears.fetch_add(1, std::memory_order_relaxed);
    }

    if (var::vis_debug && var::vis_debug_rays) {
        DebugRay dr{ from, blocked ? hit.hitPos : to, blocked, hit.hitPos, bestCls };
        RecordDebugRay(dr);
    }
    return hit;
}

RayHit RaycastLosMulti(const Vector3& from, const Vector3* ends, int endCount)
{
    RayHit anyClear{};
    anyClear.clear = true;
    anyClear.valid = true;
    if (!ends || endCount <= 0)
        return anyClear;
    RayHit firstBlocked{};
    bool sawBlock = false;
    for (int i = 0; i < endCount; ++i) {
        RayHit h = RaycastLos(from, ends[i]);
        if (h.clear)
            return h;
        if (!sawBlock) {
            firstBlocked = h;
            sawBlock = true;
        }
    }
    return sawBlock ? firstBlocked : anyClear;
}

Stats GetStats()
{
    Stats s{};
    s.probe = static_cast<ProbeStatus>(g_probe.load(std::memory_order_relaxed));
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
    std::shared_ptr<const TreeHolder> tree;
    {
        std::lock_guard<std::mutex> lk(g_swapMutex);
        tree = g_liveTree;
    }
    s.tris = tree ? tree->triCount : 0;
    return s;
}

void RecordDebugRay(const DebugRay& ray)
{
    std::lock_guard<std::mutex> lk(g_debugMutex);
    if (g_debugRays.size() >= kMaxDebugRays)
        g_debugRays.erase(g_debugRays.begin());
    g_debugRays.push_back(ray);
}

void CopyDebugRays(std::vector<DebugRay>& out)
{
    std::lock_guard<std::mutex> lk(g_debugMutex);
    out = g_debugRays;
}

void CopyDebugTris(std::vector<DebugTri>& out)
{
    std::lock_guard<std::mutex> lk(g_debugMutex);
    out = g_debugTris;
}

void ApplyVisToEspCaches(Engine& eng)
{
    if (!var::vis_enabled || !var::vis_use_esp_colors)
        return;
    if (g_probe.load(std::memory_order_relaxed) == (int)ProbeStatus::Red)
        return;

    Vector3 cam{};
    {
        std::shared_lock<std::shared_mutex> lock(eng.m_cameraMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        cam = eng.g_Camera.Location;
    }
    if (!IsPlausibleWorldPos(cam))
        return;

    const float playerR = EffectivePlayerRangeM() * 100.f;
    const float botR = EffectiveBotRangeM() * 100.f;
    const int needMiss = (std::max)(1, var::vis_hysteresis_frames);

    auto applyActor = [&](uintptr_t key, Vector3 worldPos, float distM, bool multi,
        bool& isVisibleOut) {
        const float maxCm = multi ? botR : playerR;
        if (distM * 100.f > maxCm) {
            isVisibleOut = true;
            g_visMiss.erase(key);
            return;
        }
        Vector3 ends[3];
        int n = 0;
        ends[n++] = worldPos;
        if (var::vis_multi_bone || multi) {
            Vector3 head = worldPos; head.z += 160.0;
            Vector3 chest = worldPos; chest.z += 100.0;
            ends[n++] = head;
            ends[n++] = chest;
        }
        const RayHit h = RaycastLosMulti(cam, ends, n);
        if (h.clear) {
            g_visMiss[key] = 0;
            isVisibleOut = true;
        } else {
            uint8_t& m = g_visMiss[key];
            if (m < 255)
                ++m;
            isVisibleOut = m < needMiss;
        }
    };

    {
        std::unique_lock<std::shared_mutex> lock(eng.m_playerCacheMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (auto& [key, actor] : eng.playerCache) {
                if (!actor.Drawing)
                    continue;
                applyActor(key, actor.WorldPos, actor.Distance, false, actor.isVisible);
            }
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(eng.m_robotCacheMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (auto& [key, actor] : eng.robotCache) {
                if (!actor.Drawing)
                    continue;
                applyActor(key, actor.WorldPos, actor.Distance, true, actor.isVisible);
            }
        }
    }
}

bool AimLosAllows(const Vector3& from, const Vector3& aimBoneWorld)
{
    if (!var::vis_enabled || !var::vis_use_aim)
        return true;
    if (g_probe.load(std::memory_order_relaxed) == (int)ProbeStatus::Red)
        return true;
    return RaycastLos(from, aimBoneWorld).clear;
}

} // namespace CollisionVis
