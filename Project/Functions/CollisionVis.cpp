#include "CollisionVis.h"
#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Interface/Utils/Variables/index.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace CollisionVis {
namespace {

struct Tri { Vector3 p0, p1, p2; };

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

std::mutex g_debugMutex;
std::vector<DebugRay> g_debugRays;
constexpr size_t kMaxDebugRays = 64;

std::unordered_map<uintptr_t, uint8_t> g_visMiss;
Vector3 g_lastRebuildPos{};
auto g_lastRebuildTime = std::chrono::steady_clock::time_point{};

constexpr int kMaxSmc = 256;
constexpr int kMaxTris = 20000;
constexpr float kRebuildMoveCm = 3000.f;
constexpr int kRebuildForceSec = 20;

Vector3 VMin(const Vector3& a, const Vector3& b)
{
    return { (std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::min)(a.z, b.z) };
}
Vector3 VMax(const Vector3& a, const Vector3& b)
{
    return { (std::max)(a.x, b.x), (std::max)(a.y, b.y), (std::max)(a.z, b.z) };
}

bool RayAabb(const Vector3& ro, const Vector3& rdInv, const Vector3& bmin, const Vector3& bmax)
{
    const Vector3 t0{ (bmin.x - ro.x) * rdInv.x, (bmin.y - ro.y) * rdInv.y, (bmin.z - ro.z) * rdInv.z };
    const Vector3 t1{ (bmax.x - ro.x) * rdInv.x, (bmax.y - ro.y) * rdInv.y, (bmax.z - ro.z) * rdInv.z };
    const float tmin = (std::max)((std::max)((std::min)(t0.x, t1.x), (std::min)(t0.y, t1.y)), (std::min)(t0.z, t1.z));
    const float tmax = (std::min)((std::min)((std::max)(t0.x, t1.x), (std::max)(t0.y, t1.y)), (std::max)(t0.z, t1.z));
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
    const float a = e1.x * h.x + e1.y * h.y + e1.z * h.z;
    if (a > -kEps && a < kEps)
        return false;
    const float f = 1.f / a;
    const Vector3 s{ ro.x - t.p0.x, ro.y - t.p0.y, ro.z - t.p0.z };
    const float u = f * (s.x * h.x + s.y * h.y + s.z * h.z);
    if (u < 0.f || u > 1.f)
        return false;
    const Vector3 q{
        s.y * e1.z - s.z * e1.y,
        s.z * e1.x - s.x * e1.z,
        s.x * e1.y - s.y * e1.x };
    const float v = f * (rd.x * q.x + rd.y * q.y + rd.z * q.z);
    if (v < 0.f || u + v > 1.f)
        return false;
    const float tv = f * (e2.x * q.x + e2.y * q.y + e2.z * q.z);
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
    float tMax, float& bestT)
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
            hit = true;
        }
    }
    if (Traverse(node->left.get(), ro, rd, rdInv, tMax, bestT))
        hit = true;
    if (Traverse(node->right.get(), ro, rd, rdInv, tMax, bestT))
        hit = true;
    return hit;
}

void AppendBoxTris(const Vector3& c, float hx, float hy, float hz, const Engine::FVector3d& trans,
    const Engine::FVector3d& /*scale*/, std::vector<Tri>& out)
{
    // Axis-aligned box in world after translation (simplified — extents already world-ish).
    const Vector3 o{ c.x + (float)trans.x, c.y + (float)trans.y, c.z + (float)trans.z };
    const Vector3 corners[8] = {
        { o.x - hx, o.y - hy, o.z - hz }, { o.x + hx, o.y - hy, o.z - hz },
        { o.x + hx, o.y + hy, o.z - hz }, { o.x - hx, o.y + hy, o.z - hz },
        { o.x - hx, o.y - hy, o.z + hz }, { o.x + hx, o.y - hy, o.z + hz },
        { o.x + hx, o.y + hy, o.z + hz }, { o.x - hx, o.y + hy, o.z + hz },
    };
    static constexpr int faces[12][3] = {
        {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},
        {2,6,7},{2,7,3},{0,3,7},{0,7,4},{1,5,6},{1,6,2},
    };
    for (const auto& f : faces)
        out.push_back({ corners[f[0]], corners[f[1]], corners[f[2]] });
}

bool ReadBoxElems(uintptr_t bodySetup, const Engine::FVector3d& trans, const Engine::FVector3d& scale,
    std::vector<Tri>& out, int& probeHits)
{
    const uintptr_t agg = bodySetup + Offsets::AggGeom;
    struct TArray { uintptr_t data; int32_t num; int32_t max; };
    const TArray boxes = Memory::read<TArray>(agg + Offsets::AggGeom_BoxElems);
    if (!boxes.data || boxes.num <= 0 || boxes.num > 64)
        return false;
    bool any = false;
    for (int i = 0; i < boxes.num && (int)out.size() < kMaxTris; ++i) {
        const uintptr_t elem = boxes.data + (uintptr_t)i * Offsets::BoxElem_Stride;
        const Engine::FVector3d center = Memory::read<Engine::FVector3d>(elem + Offsets::BoxElem_Center);
        const float hx = Memory::read<float>(elem + Offsets::BoxElem_XExtent) * 0.5f;
        const float hy = Memory::read<float>(elem + Offsets::BoxElem_YExtent) * 0.5f;
        const float hz = Memory::read<float>(elem + Offsets::BoxElem_ZExtent) * 0.5f;
        if (hx <= 0.f || hy <= 0.f || hz <= 0.f)
            continue;
        if (hx > 650.f || hy > 650.f || hz > 650.f)
            continue;
        AppendBoxTris(
            { (float)center.x * (float)scale.x, (float)center.y * (float)scale.y, (float)center.z * (float)scale.z },
            hx * (float)std::abs(scale.x), hy * (float)std::abs(scale.y), hz * (float)std::abs(scale.z),
            trans, scale, out);
        any = true;
        ++probeHits;
    }
    return any;
}

void CollectSmcRoots(Engine& eng, const Vector3& localPos, float radiusCm, std::vector<uintptr_t>& out)
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

    const float r2 = radiusCm * radiusCm;
    const uintptr_t localPawn = eng.AcknowledgedPawn;
    std::vector<uintptr_t> actorPtrs(static_cast<size_t>(actorCount));
    ScatterSession session;
    if (!session.isValid())
        return;
    for (int i = 0; i < actorCount; ++i)
        session.prepare(actors + (uintptr_t)i * sizeof(uintptr_t), actorPtrs[static_cast<size_t>(i)]);
    if (!session.execute())
        return;

    struct Cand { uintptr_t root; float dist2; };
    std::vector<Cand> cands;
    cands.reserve(512);

    for (uintptr_t actor : actorPtrs) {
        if (!actor || actor == localPawn || !eng.IsValidPointer(actor))
            continue;
        const uintptr_t root = Memory::read<uintptr_t>(actor + Offsets::RootComponent);
        if (!root || !eng.IsValidPointer(root))
            continue;
        const uintptr_t mesh = Memory::read<uintptr_t>(root + Offsets::StaticMesh);
        if (!mesh || !eng.IsValidPointer(mesh))
            continue;
        const Engine::FVector3d w = Memory::read_nocache<Engine::FVector3d>(root + Offsets::WorldLocation);
        const Vector3 p = Engine::ToVector3(w);
        if (!IsPlausibleWorldPos(p))
            continue;
        const float dx = (float)(p.x - localPos.x);
        const float dy = (float)(p.y - localPos.y);
        const float dz = (float)(p.z - localPos.z);
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2)
            continue;
        cands.push_back({ root, d2 });
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist2 < b.dist2; });
    for (size_t i = 0; i < cands.size() && (int)out.size() < kMaxSmc; ++i)
        out.push_back(cands[i].root);
}

void RebuildFromSmc(const std::vector<uintptr_t>& smc, ProbeStatus& probeOut)
{
    std::vector<Tri> all;
    all.reserve(4096);
    int probeHits = 0;
    for (uintptr_t root : smc) {
        if ((int)all.size() >= kMaxTris)
            break;
        const uintptr_t mesh = Memory::read<uintptr_t>(root + Offsets::StaticMesh);
        if (!mesh)
            continue;
        const uintptr_t body = Memory::read<uintptr_t>(mesh + Offsets::BodySetup);
        if (!body || !Memory::IsValidPtrFast2(body))
            continue;
        const Engine::FVector3d trans = Memory::read<Engine::FVector3d>(root + Offsets::ComponentToWorld + 0x20);
        Engine::FVector3d scale = Memory::read<Engine::FVector3d>(root + Offsets::ComponentToWorld + 0x40);
        if (scale.x == 0 && scale.y == 0 && scale.z == 0)
            scale = { 1, 1, 1 };
        if (ReadBoxElems(body, trans, scale, all, probeHits))
            continue;
        // Bounds fallback box
        const Engine::FVector3d ext = Memory::read<Engine::FVector3d>(mesh + Offsets::ExtendedBounds + 0x18);
        float hx = (float)ext.x, hy = (float)ext.y, hz = (float)ext.z;
        if (hx < 5.f || hy < 5.f || hz < 5.f || hx > 650.f || hy > 650.f || hz > 650.f)
            continue;
        AppendBoxTris({ 0, 0, 0 }, hx, hy, hz, trans, scale, all);
        ++probeHits;
    }

    if (probeHits <= 0)
        probeOut = ProbeStatus::Red;
    else if (probeHits < 3)
        probeOut = ProbeStatus::Yellow;
    else
        probeOut = ProbeStatus::Green;

    auto holder = std::make_shared<TreeHolder>();
    holder->triCount = (int)all.size();
    if (!all.empty())
        holder->root = BuildKd(all, 0);

    std::lock_guard<std::mutex> lk(g_swapMutex);
    g_liveTree = std::move(holder);
    g_probe.store(static_cast<int>(probeOut), std::memory_order_relaxed);
    g_smc.store((int)smc.size(), std::memory_order_relaxed);
}

} // namespace

float EffectivePlayerRangeM()
{
    float r = var::vis_max_range_m > 0.f ? var::vis_max_range_m : 200.f;
    if (var::vis_use_player_esp_dist && var::esp_distance > 0.f)
        r = (std::min)(r, var::esp_distance);
    return r;
}

float EffectiveBotRangeM()
{
    float r = var::vis_max_range_m > 0.f ? var::vis_max_range_m : 200.f;
    if (var::vis_use_bot_esp_dist && var::bot_esp_distance > 0.f)
        r = (std::min)(r, var::bot_esp_distance);
    return r;
}

float RebuildRadiusM()
{
    return (std::max)(EffectivePlayerRangeM(), EffectiveBotRangeM());
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
    std::vector<uintptr_t> smc;
    CollectSmcRoots(eng, localPos, RebuildRadiusM() * 100.f, smc);
    ProbeStatus probe = ProbeStatus::Red;
    RebuildFromSmc(smc, probe);
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
    const float len2 = rd.x * rd.x + rd.y * rd.y + rd.z * rd.z;
    if (len2 < 1e-4f) {
        g_clears.fetch_add(1, std::memory_order_relaxed);
        return hit;
    }

    // Bias origin slightly forward to reduce self-hit.
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
    const Vector3 rdInv{ inv(rd.x), inv(rd.y), inv(rd.z) };
    float bestT = 1.f;
    const bool blocked = Traverse(tree->root.get(), ro, rd, rdInv, 1.f, bestT);
    if (blocked) {
        hit.clear = false;
        hit.hitT = bestT;
        hit.hitPos = {
            ro.x + rd.x * bestT,
            ro.y + rd.y * bestT,
            ro.z + rd.z * bestT };
        g_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_clears.fetch_add(1, std::memory_order_relaxed);
    }

    if (var::vis_debug && var::vis_debug_rays) {
        DebugRay dr{ from, blocked ? hit.hitPos : to, blocked, hit.hitPos };
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

void ApplyVisToEspCaches(Engine& eng)
{
    if (!var::vis_enabled || !var::vis_use_esp_colors)
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
            isVisibleOut = true; // out of vis range: don't darken
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
            isVisibleOut = m < needMiss; // fail-open until hysteresis reached
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
