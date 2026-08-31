#include "CollisionMirror.h"

#include "../Core/Engine.h"
#include "../Core/AgentLog.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "WorldScanCommon.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

// ── Offsets (from this repo's SDK dump, build 24710327 — NOT forum source) ──
//   UStaticMesh::BodySetup          0x1F0   (SDK Class.cpp)
//   UBodySetup::AggGeom             0xB8    (inline FKAggregateGeom, 0x78)
//   KAggregateGeom TArray order     0x0..0x60 (7 arrays, 16B headers)
//   KBoxElem:    Center 0x30, Rotation 0x48, X 0x60, Y 0x64, Z 0x68 (size 0x70)
//   KConvexElem: VertexData 0x30, IndexData 0x40, Transform 0x90 (size 0x110)
//   KSphereElem: Center 0x30, Radius 0x48 (size 0x50)
//   KSphylElem:  Center 0x30, Rotation 0x48, Radius 0x60, Length 0x64 (size 0x68)
// FVector = 3×double (0x18); FTransform = 4×double quat + 3×double trans +
// 3×double scale (0x60). LevelSet/tapered elems skipped (no portable layout).

namespace CollisionMirror {

namespace {

constexpr uintptr_t kStaticMesh_BodySetup = 0x1F0;
constexpr uintptr_t kStaticMesh_ExtendedBounds = 0x308;    // FBoxSphereBounds
constexpr uintptr_t kStaticMesh_PositiveBoundsExt = 0x2D8; // FVector
constexpr uintptr_t kStaticMesh_NegativeBoundsExt = 0x2F0; // FVector
constexpr uintptr_t kBodySetup_AggGeom = 0xB8;

constexpr uintptr_t kAggGeom_SphereElems = 0x00;
constexpr uintptr_t kAggGeom_BoxElems = 0x10;
constexpr uintptr_t kAggGeom_SphylElems = 0x20;
constexpr uintptr_t kAggGeom_ConvexElems = 0x30;

constexpr uintptr_t kBoxElem_Center = 0x30;
constexpr uintptr_t kBoxElem_Rotation = 0x48;
constexpr uintptr_t kBoxElem_X = 0x60;
constexpr uintptr_t kBoxElem_Y = 0x64;
constexpr uintptr_t kBoxElem_Z = 0x68;

constexpr uintptr_t kConvexElem_VertexData = 0x30;  // TArray<FVector>
constexpr uintptr_t kConvexElem_IndexData = 0x40;   // TArray<int32>

constexpr uintptr_t kSphereElem_Center = 0x30;
constexpr uintptr_t kSphereElem_Radius = 0x48;

constexpr uintptr_t kSphylElem_Center = 0x30;
constexpr uintptr_t kSphylElem_Rotation = 0x48;
constexpr uintptr_t kSphylElem_Radius = 0x60;
constexpr uintptr_t kSphylElem_Length = 0x64;

// Bounds caps (world cm). Per-TRIANGLE sanity, not per-mesh reject: the map's
// occlusion geometry (roads, floors, tunnels, elevators) is exactly what must
// survive, so the old whole-mesh caps (and the "skip thin floors" heuristic)
// threw away most of the map. Only absurd individual triangles — the symptom
// of a misread element — get dropped.
constexpr double kMaxTriEdge = 60000.0;    // single triangle max edge (cm)
constexpr double kMaxCollectRadiusSq = 60000.0 * 60000.0;  // 600m
constexpr int kMaxVerts = 65536;
constexpr int kMaxTrisPerElem = 65536 * 3;
constexpr int kMaxElems = 2048;

// Rebuild gating
constexpr float kRebuildMoveSq = 3000.f * 3000.f;
constexpr auto kRebuildForceInterval = std::chrono::seconds(60);
// Even when the player moves beyond the trigger distance, never rebuild more
// than once per cooldown — the full-scan re-reads every nearby mesh over DMA
// and caused in-game latency icons. Movement now just shortens the window, not
// the rate; the 60s force interval guarantees stale coverage eventually.
constexpr auto kRebuildCooldown = std::chrono::seconds(20);

// ── Double-precision game structs (LWC) ────────────────────────────────────

struct FVec3d { double x, y, z; };
struct FQuat4d { double x, y, z, w; };
struct TArrayHeader { uintptr_t data = 0; int32_t num = 0; int32_t max = 0; };
static_assert(sizeof(FVec3d) == 24, "FVec3d must be 3 doubles");
static_assert(sizeof(TArrayHeader) == 16, "TArrayHeader must be 16 bytes");

// Component transform. Translation comes from the battle-tested WorldLocation
// (0x390, the same slot the whole ESP uses for positions — never trust the
// forum-pinned CTW layout for the anchor). Quat @ CTW+0x00, scale @ CTW+0x38
// (UE LWC) with an identity fallback: static-mesh scale is ~1.0 and the
// element rotator carries rotation, so a sanitized identity is safe and
// cannot poison the world-space transform the way a garbage scale would.
struct C2W {
    FQuat4d q{};
    FVec3d t{};
    FVec3d s{};
    bool ok = false;
};

C2W ReadC2W(uintptr_t sceneComponent)
{
    C2W out;
    if (!sceneComponent || !Memory::IsValidPtrFast2(sceneComponent))
        return out;

    // Anchor: WorldLocation (proven path — ESP boxes land on it). Fall back to
    // RelativeLocation only if world is zero.
    out.t = Memory::read<FVec3d>(sceneComponent + Offsets::WorldLocation);
    bool anchorOk = std::isfinite(out.t.x) && std::isfinite(out.t.y) && std::isfinite(out.t.z)
        && (out.t.x != 0.0 || out.t.y != 0.0 || out.t.z != 0.0);
    if (!anchorOk) {
        const FVec3d rel = Memory::read<FVec3d>(sceneComponent + Offsets::RelativeLocation);
        if (std::isfinite(rel.x) && std::isfinite(rel.y) && std::isfinite(rel.z)
            && (rel.x != 0.0 || rel.y != 0.0 || rel.z != 0.0)) {
            out.t = rel;
            anchorOk = true;
        }
    }
    if (!anchorOk)
        return out;

    // Quat @ CTW+0x00; scale @ CTW+0x40 (FTransform: Quat4d 0x00 (0x20),
    // FVector3d 0x20 (0x18), Scale3D 0x40 (0x18), pad @0x38 — help.txt
    // Struct/Transform, size 0x60). Reading the pad slot misaligned the
    // scale so most static-mesh triangles blew up past FilterSanity's cap
    // and every body setup capsSkipped.
    out.q = Memory::read<FQuat4d>(sceneComponent + Offsets::ComponentToWorld);
    const FVec3d scale = Memory::read<FVec3d>(
        sceneComponent + Offsets::ComponentToWorld + 0x40);
    const bool qFinite = std::isfinite(out.q.x) && std::isfinite(out.q.y)
        && std::isfinite(out.q.z) && std::isfinite(out.q.w);
    const bool sFinite = std::isfinite(scale.x) && std::isfinite(scale.y)
        && std::isfinite(scale.z)
        && scale.x > 1e-4 && scale.x < 1e4
        && scale.y > 1e-4 && scale.y < 1e4
        && scale.z > 1e-4 && scale.z < 1e4;
    if (!qFinite) {
        out.q = FQuat4d{ 0.0, 0.0, 0.0, 1.0 };
    }
    if (sFinite) {
        out.s = scale;
    } else {
        out.s = FVec3d{ 1.0, 1.0, 1.0 };
    }
    out.ok = true;
    return out;
}

Vector3 TransformPoint(const C2W& c, const FVec3d& local)
{
    // w = rot * (scale * v) + trans  (double precision, matches UC source)
    const double sx = local.x * c.s.x;
    const double sy = local.y * c.s.y;
    const double sz = local.z * c.s.z;
    // Quat rotate
    const double qx = c.q.x, qy = c.q.y, qz = c.q.z, qw = c.q.w;
    const double tx = 2.0 * (qy * sz - qz * sy);
    const double ty = 2.0 * (qz * sx - qx * sz);
    const double tz = 2.0 * (qx * sy - qy * sx);
    return Vector3(
        sx + qw * tx + (qy * tz - qz * ty) + c.t.x,
        sy + qw * ty + (qz * tx - qx * tz) + c.t.y,
        sz + qw * tz + (qx * ty - qy * tx) + c.t.z);
}

// ── Fingerprint: dedupe identical mesh+transform instances ─────────────────

uint64_t InstanceFingerprint(uintptr_t mesh, const C2W& c)
{
    auto qi = [](double v) -> uint64_t {
        return static_cast<uint64_t>(static_cast<int64_t>(std::llround(v * 10000.0)));
    };
    auto ti = [](double v) -> uint64_t {
        return static_cast<uint64_t>(static_cast<int64_t>(std::llround(v)));
    };
    auto si = [](double v) -> uint64_t {
        return static_cast<uint64_t>(static_cast<int64_t>(std::llround(v * 1000.0)));
    };
    uint64_t h = static_cast<uint64_t>(mesh);
    auto mix = [&](uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    };
    if (c.ok) {
        mix(qi(c.q.x)); mix(qi(c.q.y)); mix(qi(c.q.z)); mix(qi(c.q.w));
        mix(ti(c.t.x)); mix(ti(c.t.y)); mix(ti(c.t.z));
        mix(si(c.s.x)); mix(si(c.s.y)); mix(si(c.s.z));
    }
    return h;
}

bool ReadConvexElem(uintptr_t elem, const C2W& c, std::vector<Tri>& out)
{
    // Cached reads throughout — nocache reads silently return zeros under DMA
    // bus contention, and a convex with 64K verts is the biggest single read in
    // the rebuild. The probe's 0x70 header block read proved the cached path.
    const TArrayHeader vh = Memory::read<TArrayHeader>(
        elem + kConvexElem_VertexData);
    if (!Memory::IsValidPtrFast2(vh.data) || vh.num <= 0 || vh.num > kMaxVerts)
        return false;
    const TArrayHeader ih = Memory::read<TArrayHeader>(
        elem + kConvexElem_IndexData);
    // NOTE: do NOT require ih.num % 3 == 0 — Chaos index buffers are not
    // guaranteed clean triangle lists, and rejecting on that killed every
    // convex (101 of them) while boxes passed. Floor to the last full triple.
    if (!Memory::IsValidPtrFast2(ih.data) || ih.num < 3 || ih.num > kMaxTrisPerElem)
        return false;

    std::vector<FVec3d> verts(static_cast<size_t>(vh.num));
    if (!Memory::ReadRaw(vh.data, verts.data(),
            static_cast<size_t>(vh.num) * sizeof(FVec3d)))
        return false;

    // Load indices. UC reads the buffer as int32 first, and if that yields no
    // in-range triangles, retries as uint16 (some collision index buffers are
    // stored 16-bit). Returns the good layout via the out vector.
    const auto readIdx = [&](const TArrayHeader& hdr, std::vector<int32_t>& outIdx) -> bool {
        const size_t count = static_cast<size_t>(hdr.num);
        std::vector<uint16_t> u16(count);
        std::vector<int32_t> tmp(count);
        // int32 attempt
        if (Memory::ReadRaw(hdr.data, tmp.data(), count * sizeof(int32_t))) {
            // require at least one good tri in the first 48
            int goods = 0;
            const size_t triProbe = count / 3;
            const size_t scan = triProbe < 48 ? triProbe : 48;
            for (size_t ti = 0; ti < scan; ++ti) {
                const int32_t i0 = tmp[ti * 3 + 0];
                const int32_t i1 = tmp[ti * 3 + 1];
                const int32_t i2 = tmp[ti * 3 + 2];
                if (i0 >= 0 && i0 < vh.num && i1 >= 0 && i1 < vh.num
                    && i2 >= 0 && i2 < vh.num)
                    ++goods;
            }
            if (goods > 0) {
                outIdx.swap(tmp);
                return true;
            }
        }
        // uint16 attempt
        if (Memory::ReadRaw(hdr.data, u16.data(), count * sizeof(uint16_t))) {
            int goods = 0;
            const size_t triProbe = count / 3;
            const size_t scan = triProbe < 48 ? triProbe : 48;
            for (size_t ti = 0; ti < scan && ti * 3 + 2 < count; ++ti) {
                const int32_t i0 = u16[ti * 3 + 0];
                const int32_t i1 = u16[ti * 3 + 1];
                const int32_t i2 = u16[ti * 3 + 2];
                if (i0 >= 0 && i0 < vh.num && i1 >= 0 && i1 < vh.num
                    && i2 >= 0 && i2 < vh.num)
                    ++goods;
            }
            if (goods > 0) {
                outIdx.resize(count);
                for (size_t i = 0; i < count; ++i)
                    outIdx[i] = u16[i];
                return true;
            }
        }
        return false;
    };

    std::vector<int32_t> idx;
    if (!readIdx(ih, idx))
        return false;

    const size_t triCount = static_cast<size_t>(ih.num / 3);
    const size_t safeTri = (std::min)(triCount, idx.size() / 3);
    for (size_t ti = 0; ti < safeTri; ++ti) {
        const int32_t i0 = idx[ti * 3 + 0];
        const int32_t i1 = idx[ti * 3 + 1];
        const int32_t i2 = idx[ti * 3 + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0
            || i0 >= vh.num || i1 >= vh.num || i2 >= vh.num)
            continue;
        const Vector3 p0 = TransformPoint(c, verts[static_cast<size_t>(i0)]);
        const Vector3 p1 = TransformPoint(c, verts[static_cast<size_t>(i1)]);
        const Vector3 p2 = TransformPoint(c, verts[static_cast<size_t>(i2)]);
        if (!std::isfinite(p0.x) || !std::isfinite(p1.x) || !std::isfinite(p2.x))
            continue;
        out.push_back({ p0, p1, p2 });
    }
    return !out.empty();
}

// Rotator (3×double pitch/yaw/roll degrees) → quat. Same math as UC source.
FQuat4d RotatorToQuat(double pitchDeg, double yawDeg, double rollDeg)
{
    constexpr double k = 3.14159265358979323846 / 360.0;
    const double sp = std::sin(pitchDeg * k), cp = std::cos(pitchDeg * k);
    const double sy = std::sin(yawDeg * k), cy = std::cos(yawDeg * k);
    const double sr = std::sin(rollDeg * k), cr = std::cos(rollDeg * k);
    return { cr * sp * cy + sr * cp * sy,
             cr * cp * sy - sr * sp * cy,
             cr * cp * cy + sr * sp * sy,
             sr * cp * cy - cr * sp * sy };
}

bool ReadBoxElem(uintptr_t elem, const C2W& c, std::vector<Tri>& out)
{
    const FVec3d center = Memory::read<FVec3d>(elem + kBoxElem_Center);
    const FVec3d rot = Memory::read<FVec3d>(elem + kBoxElem_Rotation);
    const float hx = Memory::read<float>(elem + kBoxElem_X) * 0.5f;
    const float hy = Memory::read<float>(elem + kBoxElem_Y) * 0.5f;
    const float hz = Memory::read<float>(elem + kBoxElem_Z) * 0.5f;
    if (hx <= 0.f || hy <= 0.f || hz <= 0.f)
        return false;

    // Build box in local space, rotate by element rotator, then component transform.
    const FQuat4d br = RotatorToQuat(rot.x, rot.y, rot.z);
    constexpr double signs[8][3] = {
        {-1,-1,-1},{+1,-1,-1},{+1,+1,-1},{-1,+1,-1},
        {-1,-1,+1},{+1,-1,+1},{+1,+1,+1},{-1,+1,+1}
    };
    Vector3 wp[8];
    for (int i = 0; i < 8; ++i) {
        const double lx = center.x + signs[i][0] * hx;
        const double ly = center.y + signs[i][1] * hy;
        const double lz = center.z + signs[i][2] * hz;
        // Apply element rotation
        const double qx = br.x, qy = br.y, qz = br.z, qw = br.w;
        const double tx = 2.0 * (qy * lz - qz * ly);
        const double ty = 2.0 * (qz * lx - qx * lz);
        const double tz = 2.0 * (qx * ly - qy * lx);
        const FVec3d rl{ lx + qw * tx + (qy * tz - qz * ty),
                         ly + qw * ty + (qz * tx - qx * tz),
                         lz + qw * tz + (qx * ty - qy * tx) };
        wp[i] = TransformPoint(c, rl);
    }
    static constexpr int faces[12][3] = {
        {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},
        {2,6,7},{2,7,3},{0,3,7},{0,7,4},{1,5,6},{1,6,2},
    };
    for (const auto& f : faces) {
        out.push_back({ wp[f[0]], wp[f[1]], wp[f[2]] });
    }
    return true;
}

bool ReadSphereElem(uintptr_t elem, const C2W& c, std::vector<Tri>& out)
{
    const FVec3d center = Memory::read<FVec3d>(elem + kSphereElem_Center);
    const float radius = Memory::read<float>(elem + kSphereElem_Radius);
    if (radius <= 0.f)
        return false;
    // 6-axis octahedron (8 triangles)
    const double r = radius;
    const Vector3 lv[6] = {
        Vector3(center.x + r, center.y, center.z), Vector3(center.x - r, center.y, center.z),
        Vector3(center.x, center.y + r, center.z), Vector3(center.x, center.y - r, center.z),
        Vector3(center.x, center.y, center.z + r), Vector3(center.x, center.y, center.z - r),
    };
    Vector3 wp[6];
    for (int i = 0; i < 6; ++i) {
        wp[i] = TransformPoint(c, FVec3d{ lv[i].x, lv[i].y, lv[i].z });
    }
    static constexpr int oct[8][3] = {
        {0,2,4},{2,1,4},{1,3,4},{3,0,4},
        {0,5,2},{2,5,1},{1,5,3},{3,5,0},
    };
    for (const auto& f : oct) {
        out.push_back({ wp[f[0]], wp[f[1]], wp[f[2]] });
    }
    return true;
}

bool ReadSphylElem(uintptr_t elem, const C2W& c, std::vector<Tri>& out)
{
    const FVec3d center = Memory::read<FVec3d>(elem + kSphylElem_Center);
    const FVec3d rot = Memory::read<FVec3d>(elem + kSphylElem_Rotation);
    const float radius = Memory::read<float>(elem + kSphylElem_Radius);
    const float length = Memory::read<float>(elem + kSphylElem_Length);
    if (radius <= 0.f)
        return false;
    // Approximate as a box (radius, radius, halfLen+radius) like the UC source.
    const float hh = (length * 0.5f) + radius;
    // Reuse box path with the sphyl center/rotation.
    constexpr double signs[8][3] = {
        {-1,-1,-1},{+1,-1,-1},{+1,+1,-1},{-1,+1,-1},
        {-1,-1,+1},{+1,-1,+1},{+1,+1,+1},{-1,+1,+1}
    };
    const FQuat4d br = RotatorToQuat(rot.x, rot.y, rot.z);
    Vector3 wp[8];
    for (int i = 0; i < 8; ++i) {
        const double lx = center.x + signs[i][0] * radius;
        const double ly = center.y + signs[i][1] * radius;
        const double lz = center.z + signs[i][2] * hh;
        const double qx = br.x, qy = br.y, qz = br.z, qw = br.w;
        const double tx = 2.0 * (qy * lz - qz * ly);
        const double ty = 2.0 * (qz * lx - qx * lz);
        const double tz = 2.0 * (qx * ly - qy * lx);
        const FVec3d rl{ lx + qw * tx + (qy * tz - qz * ty),
                         ly + qw * ty + (qz * tx - qx * tz),
                         lz + qw * tz + (qx * ty - qy * tx) };
        wp[i] = TransformPoint(c, rl);
    }
    static constexpr int faces[12][3] = {
        {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},
        {2,6,7},{2,7,3},{0,3,7},{0,7,4},{1,5,6},{1,6,2},
    };
    for (const auto& f : faces) {
        out.push_back({ wp[f[0]], wp[f[1]], wp[f[2]] });
    }
    return true;
}

// Per-rejection accounting for the collision_rebuild NDJSON row. Lets us see
// WHICH stage rejects the 300+ capsSkips instead of guessing (counts are per
// body setup, aggregated over the job).
struct GeoReject {
    int caps = 0;         // element count cap exceeded
    int elemFail = 0;     // every element read yielded nothing (per element type)
    int sanity = 0;       // FilterSanity dropped everything
    int convOk = 0; int convFail = 0;   // convex element reads
    int boxOk = 0;  int boxFail = 0;    // box element reads
    int sphOk = 0;  int sphFail = 0;    // sphere element reads
    int sphylOk = 0; int sphylFail = 0; // sphyl element reads
    int boundsOk = 0; int boundsFail = 0; // ExtendedBounds box fallback reads
    // WHY a fallback attempt failed (only reachable when we got to the box path)
    int bFree = 0;    // failure before filters: bad ptr / non-finite / negative dims
    int bEps = 0;     // half < k_eps
    int bLos = 0;     // any half > k_max_los (650)
    int bCorner = 0;  // |scale|·half corner length > k_corner_half (850)
    int bZ = 0;       // world-Z mass < k_min_world_z (80)
    // Element presence (header num > 0), counted once per read geometry call
    int convPres = 0; int boxPres = 0; int sphPres = 0; int sphylPres = 0;
};

// ── ExtendedBounds box fallback (UC rebuild-style) ─────────────────────────
//
// When an instance has no parseable collision primitives, draw a box from the
// mesh's ExtendedBounds (FBoxSphereBounds: Origin @+0x00, BoxExtent @+0x18),
// corrected by Positive/NegativeBoundsExtension. This is what makes map pieces
// visible at all — only a handful of props carry well-formed primitives.
bool ReadAssetBoundsBox(uintptr_t mesh, const C2W& c, std::vector<Tri>& out,
    GeoReject* reject = nullptr)
{
    if (!mesh || !Memory::IsValidPtrFast2(mesh))
        return false;

    const FVec3d extOrg = Memory::read<FVec3d>(mesh + kStaticMesh_ExtendedBounds + 0x00);
    const FVec3d extExt = Memory::read<FVec3d>(mesh + kStaticMesh_ExtendedBounds + 0x18);
    const FVec3d posExt = Memory::read<FVec3d>(mesh + kStaticMesh_PositiveBoundsExt);
    const FVec3d negExt = Memory::read<FVec3d>(mesh + kStaticMesh_NegativeBoundsExt);

    const bool finite = std::isfinite(extOrg.x) && std::isfinite(extOrg.y)
        && std::isfinite(extOrg.z) && std::isfinite(extExt.x)
        && std::isfinite(extExt.y) && std::isfinite(extExt.z)
        && std::isfinite(posExt.x) && std::isfinite(posExt.y)
        && std::isfinite(posExt.z) && std::isfinite(negExt.x)
        && std::isfinite(negExt.y) && std::isfinite(negExt.z);
    if (!finite) {
        if (reject) ++reject->bFree;
        return false;
    }

    // Correct the box origin/half-extents by the extensions (UC math).
    const double halfX = extExt.x - (posExt.x + negExt.x) * 0.5;
    const double halfY = extExt.y - (posExt.y + negExt.y) * 0.5;
    const double halfZ = extExt.z - (posExt.z + negExt.z) * 0.5;
    const double orgX  = extOrg.x - (posExt.x - negExt.x) * 0.5;
    const double orgY  = extOrg.y - (posExt.y - negExt.y) * 0.5;
    const double orgZ  = extOrg.z - (posExt.z - negExt.z) * 0.5;

    // Filters from the UC source (k_eps/k_max_los/k_corner_half/k_min_world_z).
    constexpr double kEps = 1e-4;
    constexpr double kMaxLos = 2000.0;
    constexpr double kCornerHalf = 3000.0;
    constexpr double kMinWorldZ = 10.0;
    if (!std::isfinite(halfX) || !std::isfinite(halfY) || !std::isfinite(halfZ)) {
        if (reject) ++reject->bFree;
        return false;
    }
    if (halfX < kEps || halfY < kEps || halfZ < kEps) {
        if (reject) ++reject->bEps;
        return false;
    }
    if (halfX > kMaxLos || halfY > kMaxLos || halfZ > kMaxLos) {
        if (reject) ++reject->bLos;
        return false;
    }
    {
        // |scale|·half corner length cap.
        const Vector3 ax(std::abs(c.s.x) * halfX, std::abs(c.s.y) * halfY,
                         std::abs(c.s.z) * halfZ);
        const double axLen = std::sqrt(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z);
        if (axLen > kCornerHalf) {
            if (reject) ++reject->bCorner;
            return false;
        }
    }
    {
        // World-Z extent: the rotated "up" axis contribution of each local axis
        // (R[0][2],R[1][2],R[2][2] = z of each basis column).
        const double qx = c.q.x, qy = c.q.y, qz = c.q.z, qw = c.q.w;
        const double r02 = 2.0 * (qx * qz - qw * qy);
        const double r12 = 2.0 * (qy * qz + qw * qx);
        const double r22 = 1.0 - 2.0 * (qx * qx + qy * qy);
        const double wz = std::abs(r02) * std::abs(c.s.x) * halfX
            + std::abs(r12) * std::abs(c.s.y) * halfY
            + std::abs(r22) * std::abs(c.s.z) * halfZ;
        if (wz < kMinWorldZ) {
            if (reject) ++reject->bZ;
            return false;
        }
    }

    // 8 corners, identity rotator (UC passes dquat(1,0,0,0)), component
    // transform applied. Same topology as ReadBoxElem's faces.
    constexpr double signs[8][3] = {
        {-1,-1,-1},{+1,-1,-1},{+1,+1,-1},{-1,+1,-1},
        {-1,-1,+1},{+1,-1,+1},{+1,+1,+1},{-1,+1,+1}
    };
    Vector3 wp[8];
    for (int i = 0; i < 8; ++i) {
        const FVec3d local{ orgX + signs[i][0] * halfX,
                            orgY + signs[i][1] * halfY,
                            orgZ + signs[i][2] * halfZ };
        wp[i] = TransformPoint(c, local);
        if (!std::isfinite(wp[i].x) || !std::isfinite(wp[i].y) || !std::isfinite(wp[i].z))
            return false;
    }
    static constexpr int faces[12][3] = {
        {0,1,2},{0,2,3},{4,6,5},{4,7,6},{0,4,5},{0,5,1},
        {2,6,7},{2,7,3},{0,3,7},{0,7,4},{1,5,6},{1,6,2},
    };
    for (const auto& f : faces) {
        out.push_back({ wp[f[0]], wp[f[1]], wp[f[2]] });
    }
    if (reject) ++reject->boundsOk;
    return true;
}

// Drop individual garbage triangles (absurd edge lengths from misread data)
// but keep everything sane — floors and roads occlude LOS and must survive.
bool FilterSanity(std::vector<Tri>& tris)
{
    std::vector<Tri> keep;
    keep.reserve(tris.size());
    const double edge2 = kMaxTriEdge * kMaxTriEdge;
    for (const auto& t : tris) {
        const Vector3 d01 = t.p1 - t.p0;
        const Vector3 d12 = t.p2 - t.p1;
        const Vector3 d20 = t.p0 - t.p2;
        if (d01.Dot(d01) > edge2 || d12.Dot(d12) > edge2 || d20.Dot(d20) > edge2)
            continue;
        keep.push_back(t);
    }
    tris.swap(keep);
    return !tris.empty();
}

// ── Published tree state ────────────────────────────────────────────────────

std::mutex g_treeMu;
KDNode* g_tree = nullptr;
std::atomic<bool> g_ready{ false };
std::atomic<size_t> g_triCount{ 0 };
std::atomic<int> g_meshCount{ 0 };
std::atomic<int> g_rebuildCount{ 0 };
std::atomic<int> g_lastRebuildMs{ 0 };

} // namespace

// ── Query (paint/worker safe: try_lock, fail-open) ─────────────────────────

bool QueryVisible(const Vector3& from, const Vector3& to)
{
    // Hold the lock for the whole traversal: the rebuild swaps AND deletes the
    // old tree under the same mutex, so traversing a copied pointer after
    // release would be a use-after-free. try_lock keeps paint non-blocking —
    // if a rebuild is mid-swap (rare, every 20s+) we fail open instead of
    // waiting. The raycast itself is pure math, sub-ms on the published tree.
    std::unique_lock<std::mutex> lk(g_treeMu, std::defer_lock);
    if (!lk.try_lock())
        return true;
    return IsVisible(g_tree, from, to);
}

bool IsReady()
{
    return g_ready.load(std::memory_order_acquire);
}

size_t TriangleCount()
{
    return g_triCount.load(std::memory_order_acquire);
}

int MeshCount()
{
    return g_meshCount.load(std::memory_order_acquire);
}

int RebuildCount()
{
    return g_rebuildCount.load(std::memory_order_acquire);
}

int LastRebuildMs()
{
    return g_lastRebuildMs.load(std::memory_order_acquire);
}

// ── Debug walk / hit ray (paint-safe: try_lock, hold lock during iteration) ──

RayHit QueryRay(const Vector3& from, const Vector3& to)
{
    RayHit out;
    std::unique_lock<std::mutex> lk(g_treeMu, std::defer_lock);
    if (!lk.try_lock() || !g_tree)
        return out;
    const Tri* hit = nullptr;
    const bool vis = IsVisible(g_tree, from, to, &hit);
    out.visible = vis;
    if (hit) {
        out.hit = true;
        out.tri = *hit;
    }
    return out;
}

size_t ForEachTriNear(const Vector3& center, double radius,
    void (*fn)(const Tri&, void*), void* ctx)
{
    if (!fn)
        return 0;
    std::unique_lock<std::mutex> lk(g_treeMu, std::defer_lock);
    if (!lk.try_lock() || !g_tree)
        return 0;

    const double r2 = radius * radius;
    size_t visited = 0;

    // Iterative walk with AABB distance culling (no recursion depth worries).
    struct StackItem { const KDNode* node; };
    std::vector<StackItem> stack;
    stack.push_back({ g_tree });
    while (!stack.empty()) {
        const KDNode* node = stack.back().node;
        stack.pop_back();
        if (!node)
            continue;
        // AABB vs sphere: closest point on box to center is clamp(center, min, max).
        const Vector3 closest(
            (std::max)(node->bbMin.x, (std::min)(center.x, node->bbMax.x)),
            (std::max)(node->bbMin.y, (std::min)(center.y, node->bbMax.y)),
            (std::max)(node->bbMin.z, (std::min)(center.z, node->bbMax.z)));
        const Vector3 d = closest - center;
        if (d.Dot(d) > r2)
            continue;
        for (const auto& t : node->triangles) {
            fn(t, ctx);
            ++visited;
        }
        if (node->left)
            stack.push_back({ node->left });
        if (node->right)
            stack.push_back({ node->right });
    }
    return visited;
}

// ── Rebuild ─────────────────────────────────────────────────────────────────

namespace {

std::atomic<bool> s_rebuilding{ false };
std::mutex s_stateMu;
Vector3 s_lastPos{};
std::chrono::steady_clock::time_point s_lastTime{};

bool ReadAssetGeometry(uintptr_t mesh, uintptr_t bodySetup, const C2W& c,
    std::vector<Tri>& out, GeoReject* reject = nullptr)
{
    // Parse per instance: the primitives live in body-setup local space, so
    // each instance needs its own C2W transform. Asset-level caching would
    // need a separate local-space parse pass (Stage 5) — the rebuild is
    // throttled, so parsing per instance is fine for now.
    // `mesh` is passed through to the ExtendedBounds box fallback.

    // Cached reads, exactly like the probe's 0x70 block read — nocache reads
    // silently return zeros under DMA bus contention, which zeroes every mesh.
    const TArrayHeader convexHdr = Memory::read<TArrayHeader>(
        bodySetup + kBodySetup_AggGeom + kAggGeom_ConvexElems);
    const TArrayHeader boxHdr = Memory::read<TArrayHeader>(
        bodySetup + kBodySetup_AggGeom + kAggGeom_BoxElems);
    const TArrayHeader sphHdr = Memory::read<TArrayHeader>(
        bodySetup + kBodySetup_AggGeom + kAggGeom_SphereElems);
    const TArrayHeader sphylHdr = Memory::read<TArrayHeader>(
        bodySetup + kBodySetup_AggGeom + kAggGeom_SphylElems);

    if (reject) {
        if (convexHdr.num > 0) ++reject->convPres;
        if (boxHdr.num > 0) ++reject->boxPres;
        if (sphHdr.num > 0) ++reject->sphPres;
        if (sphylHdr.num > 0) ++reject->sphylPres;
    }

    if (convexHdr.num > kMaxElems || boxHdr.num > kMaxElems
        || sphHdr.num > kMaxElems || sphylHdr.num > kMaxElems) {
        if (reject) ++reject->caps;
        return false;
    }

    std::vector<Tri> tris;
    tris.reserve(1024);

    if (Memory::IsValidPtrFast2(convexHdr.data) && convexHdr.num > 0) {
        for (int32_t ei = 0; ei < convexHdr.num; ++ei) {
            const uintptr_t elem = convexHdr.data + static_cast<uintptr_t>(ei) * 0x110;
            std::vector<Tri> et;
            if (reject) ++reject->convFail;
            if (ReadConvexElem(elem, c, et)) {
                tris.insert(tris.end(), et.begin(), et.end());
                if (reject) ++reject->convOk;
            }
        }
    }
    if (Memory::IsValidPtrFast2(boxHdr.data) && boxHdr.num > 0) {
        for (int32_t ei = 0; ei < boxHdr.num; ++ei) {
            const uintptr_t elem = boxHdr.data + static_cast<uintptr_t>(ei) * 0x70;
            std::vector<Tri> et;
            if (reject) ++reject->boxFail;
            if (ReadBoxElem(elem, c, et)) {
                tris.insert(tris.end(), et.begin(), et.end());
                if (reject) ++reject->boxOk;
            }
        }
    }
    if (Memory::IsValidPtrFast2(sphHdr.data) && sphHdr.num > 0) {
        for (int32_t ei = 0; ei < sphHdr.num; ++ei) {
            const uintptr_t elem = sphHdr.data + static_cast<uintptr_t>(ei) * 0x50;
            std::vector<Tri> et;
            if (reject) ++reject->sphFail;
            if (ReadSphereElem(elem, c, et)) {
                tris.insert(tris.end(), et.begin(), et.end());
                if (reject) ++reject->sphOk;
            }
        }
    }
    if (Memory::IsValidPtrFast2(sphylHdr.data) && sphylHdr.num > 0) {
        for (int32_t ei = 0; ei < sphylHdr.num; ++ei) {
            const uintptr_t elem = sphylHdr.data + static_cast<uintptr_t>(ei) * 0x68;
            std::vector<Tri> et;
            if (reject) ++reject->sphylFail;
            if (ReadSphylElem(elem, c, et)) {
                tris.insert(tris.end(), et.begin(), et.end());
                if (reject) ++reject->sphylOk;
            }
        }
    }

    // Per-triangle sanity filter — drops misread garbage, keeps map geometry.
    if (tris.empty()) {
        if (reject) ++reject->elemFail;
        // UC falls back to a box from the mesh's ExtendedBounds when no
        // primitive survives. We do the same inside ReadAssetGeometry so the
        // caller (RunRebuildJob) sees a single code path.
        return ReadAssetBoundsBox(mesh, c, out, reject);
    }
    if (!FilterSanity(tris)) {
        if (reject) ++reject->sanity;
        return ReadAssetBoundsBox(mesh, c, out, reject);
    }
    out.insert(out.end(), tris.begin(), tris.end());
    return true;
}

void RunRebuildJob(uintptr_t uworld, uintptr_t persistentLevel,
    const Vector3& localPos)
{
    std::vector<uint64_t> actors;
    WorldScan::CollectLevelActors(uworld, persistentLevel, actors);

    std::unordered_set<uint64_t> seenFp;
    std::vector<Tri> worldTris;
    worldTris.reserve(8192);
    int meshesUsed = 0;

    // Stage counters for the NDJSON tap — the probe proved the chain works with
    // cached reads + IsPlausibleObjPtr, so mirror it exactly and surface where
    // (if anywhere) this reader diverges.
    int cRoots = 0, cMeshes = 0, cBodySetups = 0, cC2w = 0, cDistSkip = 0,
        cCapsSkip = 0;
    GeoReject geo;

    for (uint64_t a : actors) {
        const uintptr_t actor = static_cast<uintptr_t>(a);
        if (!actor)
            continue;
        const uintptr_t root = Memory::read<uintptr_t>(actor + Offsets::RootComponent);
        if (!Engine::IsPlausibleObjPtr(root))
            continue;
        ++cRoots;
        uintptr_t mesh = Memory::read<uintptr_t>(root + Offsets::StaticMesh);
        if (!Engine::IsPlausibleObjPtr(mesh))
            mesh = Memory::read<uintptr_t>(root + Offsets::StaticMeshLegacy);
        if (!Engine::IsPlausibleObjPtr(mesh))
            continue;
        ++cMeshes;
        const uintptr_t bodySetup = Memory::read<uintptr_t>(mesh + kStaticMesh_BodySetup);
        const bool haveBody = Engine::IsPlausibleObjPtr(bodySetup);
        if (haveBody)
            ++cBodySetups;

        const C2W c = ReadC2W(root);
        if (!c.ok)
            continue;
        ++cC2w;
        // Distance gate to local player
        const Vector3 wp(c.t.x, c.t.y, c.t.z);
        const Vector3 d = wp - localPos;
        if (d.Dot(d) > kMaxCollectRadiusSq) {
            ++cDistSkip;
            continue;
        }
        const uint64_t fp = InstanceFingerprint(mesh, c);
        if (!seenFp.insert(fp).second)
            continue;

        std::vector<Tri> tris;
        bool got = false;
        if (haveBody) {
            got = ReadAssetGeometry(mesh, bodySetup, c, tris, &geo);
        } else {
            // No body setup: still box the mesh from ExtendedBounds, exactly
            // like the UC rebuild does after its primitive pass turns up empty.
            got = ReadAssetBoundsBox(mesh, c, tris, &geo);
        }
        if (got) {
            worldTris.insert(worldTris.end(), tris.begin(), tris.end());
            ++meshesUsed;
        } else {
            ++cCapsSkip;
            ++geo.boundsFail;
        }
        // Element presence is counted inside ReadAssetGeometry (single header
        // read per mesh, no duplicate DMA) — no trailing header re-read here.
    }

    KDNode* newTree = worldTris.empty() ? nullptr : BuildTree(worldTris);
    {
        std::lock_guard<std::mutex> lk(g_treeMu);
        delete g_tree;
        g_tree = newTree;
        g_ready.store(newTree != nullptr, std::memory_order_release);
        g_triCount.store(worldTris.size(), std::memory_order_release);
        g_meshCount.store(meshesUsed, std::memory_order_release);
        g_rebuildCount.fetch_add(1, std::memory_order_relaxed);
    }

    // NDJSON stats tap (worker thread, throttled by rebuild cadence).
    {
        std::ofstream f(kArcVerifyPath, std::ios::app);
        if (f) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            f << "{\"sessionId\":\"c190fb\",\"location\":\"CollisionMirror.cpp\","
              << "\"message\":\"collision_rebuild\","
              << "\"data\":{\"actors\":" << actors.size()
              << ",\"roots\":" << cRoots
              << ",\"meshes\":" << cMeshes
              << ",\"bodySetups\":" << cBodySetups
              << ",\"c2w\":" << cC2w
              << ",\"distSkip\":" << cDistSkip
              << ",\"capsSkip\":" << cCapsSkip
              << ",\"conv\":" << geo.convPres << ",\"box\":" << geo.boxPres
              << ",\"sph\":" << geo.sphPres << ",\"sphyl\":" << geo.sphylPres
              << ",\"rejCaps\":" << geo.caps
              << ",\"rejElem\":" << geo.elemFail
              << ",\"rejSanity\":" << geo.sanity
              << ",\"convOk\":" << geo.convOk << ",\"convFail\":" << geo.convFail
              << ",\"boxOk\":" << geo.boxOk << ",\"boxFail\":" << geo.boxFail
              << ",\"sphOk\":" << geo.sphOk << ",\"sphFail\":" << geo.sphFail
              << ",\"sphylOk\":" << geo.sphylOk << ",\"sphylFail\":" << geo.sphylFail
              << ",\"boundsOk\":" << geo.boundsOk << ",\"boundsFail\":" << geo.boundsFail
              << ",\"bFree\":" << geo.bFree << ",\"bEps\":" << geo.bEps
              << ",\"bLos\":" << geo.bLos << ",\"bCorner\":" << geo.bCorner
              << ",\"bZ\":" << geo.bZ
              << ",\"trisUsed\":" << meshesUsed
              << ",\"tris\":" << worldTris.size()
              << ",\"ready\":" << (newTree ? 1 : 0)
              << ",\"rebuilds\":" << g_rebuildCount.load(std::memory_order_relaxed)
              << "}" << ",\"timestamp\":" << ms << "}\n";
        }
    }
}

} // namespace

// Called from a low-cadence worker; spawns the actual rebuild on a thread.
void ScheduleRebuild(uintptr_t uworld, uintptr_t persistentLevel,
    const Vector3& localPos)
{
    if (!uworld || !persistentLevel)
        return;
    if (s_rebuilding.load(std::memory_order_acquire))
        return;

    bool should = false;
    {
        std::lock_guard<std::mutex> lk(s_stateMu);
        const Vector3 delta = localPos - s_lastPos;
        const bool moved = delta.Dot(delta) > kRebuildMoveSq;
        const bool forced = (std::chrono::steady_clock::now() - s_lastTime)
            >= kRebuildForceInterval;
        // Require the cooldown to have elapsed for movement-triggered rebuilds
        // (forced already implies it). Radial coverage degrades while sprinting,
        // but the 1Hz LOS taps still work against the stale tree.
        const auto sinceLast = std::chrono::steady_clock::now() - s_lastTime;
        should = (moved && sinceLast >= kRebuildCooldown) || forced
            || !g_ready.load(std::memory_order_acquire);
    }
    if (!should)
        return;

    if (s_rebuilding.exchange(true))
        return;

    std::thread([uworld, persistentLevel, localPos]() {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            RunRebuildJob(uworld, persistentLevel, localPos);
        } catch (...) {
        }
        g_lastRebuildMs.store(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(s_stateMu);
            s_lastPos = localPos;
            s_lastTime = std::chrono::steady_clock::now();
        }
        s_rebuilding.store(false, std::memory_order_release);
    }).detach();
}

void Clear()
{
    std::lock_guard<std::mutex> lk(g_treeMu);
    delete g_tree;
    g_tree = nullptr;
    g_ready.store(false, std::memory_order_release);
    g_triCount.store(0, std::memory_order_release);
    g_meshCount.store(0, std::memory_order_release);
}

} // namespace CollisionMirror
