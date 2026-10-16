#include "CollisionRead.h"

#include "../Core/Engine.h"
#include "../Core/EntityDiagnostics.hpp"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Core/ReadBudget.hpp"
#include "../Core/SteamDecrypt.hpp"
#include "../Interface/Utils/Threads/SyncedThread.h"
#include "../Interface/Utils/UpgradeOptions.hpp"
#include "CollisionBvh.h"
#include "WorldScanCommon.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace CollisionRead {

namespace {

// ── Bounds ──────────────────────────────────────────────────────────────────
// Every read here is a full round trip, so the probe works on a prefix of the
// actor list and stores a bounded number of primitives per asset. Truncation is
// counted, not hidden: "we read 64 of 2000" must never read as "the asset has 64".
//
// The prefix length is what decides how much LEVEL geometry exists, because the
// BVH is built from this cache: a live raid measured actorsSeen 1954, walked 256,
// 132 static-mesh roots, and only 17 unique collision assets of which 16 declared
// no simple collision at all -- i.e. most of the level was never even examined.
// Raised 4x (the walk is one background job, ~0.4s per 256 actors, and each
// additional actor only costs a root resolve plus a per-asset read that the
// asset cache usually answers). Still bounded, still counted. The remaining gap
// is not this cap: it is that level meshes carry complex (tri-mesh) collision,
// which the simple-shape decoder does not read (see the assetsAggGeomEmpty note).
// Safety bound on the walk, NOT a coverage target. A live probe reported
// actorsSeen 1854 / actorsWalked 1024, so the scene silently described the first
// 55% of the roster and nothing else -- with array order deciding which half.
// Level geometry (buildings, cover) that sits later in the array was therefore
// never examined, which is one half of "the wireframes show loot and no
// buildings". Raised so a loaded level is covered end to end; the walk is a
// once-per-session background job, and walkCapHit now reports it if this ever
// binds again.
constexpr int kMaxActorsWalked = 16384;
// Raised from 192. A live probe filled the old cap exactly (171 brush + 21 mesh
// assets) and rejected 117 more with assetsOverCacheCap, so a level's brush
// geometry -- now the source of the vischeck walls -- was being dropped on the
// floor. Still a bound (this is decode time and geometry memory), just one a real
// level does not hit; the over-cap counter still reports it if it ever binds.
constexpr int kMaxAssetsStored = 1024;
constexpr int kMaxBoundsChecks = 48;
// Coarse occluders recorded across the whole walk. One per placed static mesh
// whose collision did not decode, so a level's walls are representable at all;
// bounded because this is DMA read time (one bounds read each) and because the
// scene it feeds is rebuilt from the whole list.
constexpr size_t kMaxOccluderBoxes = 4096;
// Placed copies recorded per asset. A tile reused across a level can be placed
// hundreds of times; the cap keeps one asset from eating the whole budget, and
// the count of instances beyond it is not silently dropped (see the probe's own
// truncation counters).
constexpr size_t kMaxInstancesPerAsset = 64;

// ── State ───────────────────────────────────────────────────────────────────
std::mutex g_stateMu;
ProbeResult g_result;
std::atomic<bool> g_probeRunning{ false };
std::atomic<bool> g_stopRequested{ false };
ManagedJob g_probeJob;

struct AssetRecord
{
    uintptr_t asset = 0;
    uintptr_t bodySetup = 0;
    // True when `asset` IS the BodySetup (a brush), not a UStaticMesh. Such a record
    // has no StaticMesh bounds, so the ExtendedBounds reads must be skipped for it.
    bool directBodySetup = false;
    uint64_t firstSeenMs = 0;
    int state = 0;  // 0 pending, 1 parsed, 2 no BodySetup, 3 header read failed, 4 empty
    CollisionGeom::AggGeomHeader header{};
    CollisionGeom::ParseTally tally{};
    std::vector<CollisionGeom::Primitive> primitives;
    // World placements seen for this asset, filled from the same pass that read it.
    std::vector<AssetInstance> instances;
};

std::mutex g_cacheMu;
std::unordered_map<uintptr_t, AssetRecord> g_assets;
// component -> world AABB, for instances whose asset declared no usable shapes.
// Keyed by component so a re-run replaces rather than accumulates.
std::unordered_map<uintptr_t, OccluderBox> g_occluders;

// StaticMesh::ExtendedBounds: origin + extent + radius. Declared here (not inside
// the walk) because both the decode cross-check and the actor stand-in path read
// it. (The occluder generator reads the wider AssetBoundsBlock below instead.)
struct AssetBoundsLocal
{
    Vector3 origin{};
    Vector3 extent{};
    double radius = 0.0;
};

// UStaticMesh bounds block, read in one shot: the positive and negative bounds
// extensions that UE folds into ExtendedBounds, then the extended origin/extent.
// Reading it whole keeps the occluder generator at one direct-read site (the
// sixth pinned one) while recovering the UN-extended box: the visible mesh's
// true box is the extended box minus the extensions, so the oriented occluder
// hugs the material instead of the padded bounds.
struct AssetBoundsBlock
{
    Vector3 posExt{};
    Vector3 negExt{};
    Vector3 origin{};
    Vector3 extent{};
};
// component -> asset, so a consumer that has a component pointer (the ESP paint
// pass, keyed by root component) resolves its shapes in O(1) instead of scanning
// every asset's instance list per frame.
std::unordered_map<uintptr_t, uintptr_t> g_componentToAsset;

// ── Per-actor geometry index ────────────────────────────────────────────────
// The cache above is keyed per ASSET, and the ESP paint pass asks per ENTITY.
// That works for a world item (its root IS a static mesh component, so the
// component index resolves) and fails for every player and bot: a character's
// root component is its CAPSULE, not a static mesh, so the asset path never saw
// them at all ("otherComponentRoots: 613" in a live probe). Keying by actor is
// what makes their shape outlines drawable.
//
// Three sources, one entry per actor:
//   decoded  - the shapes are in the asset cache; only the placement is here
//   capsule  - Character::CapsuleComponent: the pawn's real body collision
//   bounds   - the component's own LocalBounds box: the last resort, coarse
struct ActorGeometry
{
    int kind = 0;
    uintptr_t asset = 0;
    FTransform transform{};
    CollisionGeom::Primitive shape{};
    bool hasShape = false;
};

std::unordered_map<uintptr_t, ActorGeometry> g_actorGeometry;
// component -> actor, so a consumer holding a component (the item and bot draw
// sites) resolves the actor-keyed geometry instead of needing its own index.
std::unordered_map<uintptr_t, uintptr_t> g_componentToActor;

constexpr int kActorKindDecoded = 0;
constexpr int kActorKindCapsule = 1;
constexpr int kActorKindBounds = 2;

/** CapsuleComponent::CapsuleHalfHeight + CapsuleRadius as stored: two floats. */
struct CapsuleDims
{
    float halfHeight = 0.f;  // INCLUDES the two hemisphere caps
    float radius = 0.f;
};

uint64_t SteadyNowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Publish(const ProbeResult& r)
{
    std::lock_guard<std::mutex> lock(g_stateMu);
    g_result = r;
}

/** One budgeted bulk read. Returns false without touching `out` on failure. */
bool ReadBlock(uintptr_t address, size_t bytes, std::vector<uint8_t>& out, ProbeResult& r)
{
    if (!address || !bytes || bytes > 64 * 1024)
        return false;
    out.assign(bytes, 0);
    if (!Memory::ReadRaw(address, out.data(), bytes))
        return false;
    r.reads += 1;
    r.bytesRead += bytes;
    return true;
}

bool IsPlausiblePtr(uintptr_t p)
{
    return p != 0 && Memory::IsValidPtrFast2(p);
}

/** Same gate the occluder path uses before it trusts a component transform. */
bool PlausibleComponentTransform(const FTransform& t)
{
    const double q = t.Rotation.x * t.Rotation.x + t.Rotation.y * t.Rotation.y
        + t.Rotation.z * t.Rotation.z + t.Rotation.w * t.Rotation.w;
    return std::isfinite(q) && q > 0.9 && q < 1.1
        && std::isfinite(t.Translation.x) && std::isfinite(t.Translation.y)
        && std::isfinite(t.Translation.z)
        && std::abs(t.Scale3D.x) > 0.001 && std::abs(t.Scale3D.y) > 0.001
        && std::abs(t.Scale3D.z) > 0.001;
}

CollisionGeom::Primitive MakeBoxPrimitive(const Vector3& center, const Vector3& halfExtents)
{
    CollisionGeom::Primitive p;
    p.shape = CollisionGeom::Shape::Box;
    p.center = center;
    p.halfExtents = halfExtents;
    p.rotation = FQuat{ 0.0, 0.0, 0.0, 1.0 };
    return p;
}

CollisionGeom::Primitive MakeCapsulePrimitive(double radius, double halfLength)
{
    CollisionGeom::Primitive p;
    p.shape = CollisionGeom::Shape::Sphyl;
    p.center = Vector3(0.0, 0.0, 0.0);
    p.radius = radius;
    p.halfLength = halfLength;
    p.rotation = FQuat{ 0.0, 0.0, 0.0, 1.0 };
    return p;
}

/** The asset cache holds this actor's shapes: record only where to find them. */
/**
 * Tally a root-component class that the static-mesh path did not take.
 *
 * Bounded: three distinct names plus a count for everything else, so a level
 * whose buildings all sit on one component kind shows up as a large number
 * beside one name instead of as an unactionable total.
 */
void NoteSkippedRootClass(const std::string& name, ProbeResult& r)
{
    if (name.empty())
        return;
    for (int i = 0; i < r.skippedRootClasses; ++i) {
        if (r.skippedRootClassSample[i] == name) {
            ++r.skippedRootClassCounts[i];
            return;
        }
    }
    if (r.skippedRootClasses < 3) {
        r.skippedRootClassSample[r.skippedRootClasses] = name;
        r.skippedRootClassCounts[r.skippedRootClasses] = 1;
        ++r.skippedRootClasses;
        return;
    }
    ++r.skippedRootClassesOther;
}

/**
 * Tally the class of a static-mesh component whose StaticMesh read back unusable.
 * Bounded exactly like NoteSkippedRootClass: three distinct names plus a catch-all
 * count, and the owner actor's class captured for the first few samples so a name
 * can be read in context (a mesh-less component kind on a deco actor is benign; the
 * same kind on every building actor is the bug).
 */
void NoteUnresolvedMeshClass(const std::string& compClass, uintptr_t actor, ProbeResult& r)
{
    if (compClass.empty())
        return;
    for (int i = 0; i < r.unresolvedMeshClasses; ++i) {
        if (r.unresolvedMeshClassSample[i] == compClass) {
            ++r.unresolvedMeshClassCounts[i];
            return;
        }
    }
    if (r.unresolvedMeshClasses < 3) {
        r.unresolvedMeshClassSample[r.unresolvedMeshClasses] = compClass;
        r.unresolvedMeshClassCounts[r.unresolvedMeshClasses] = 1;
        const uintptr_t actorClass = IsPlausiblePtr(actor)
            ? steam_decrypt::GetActorClassPtr(actor) : 0;
        r.unresolvedMeshActorClassSample[r.unresolvedMeshClasses] = IsPlausiblePtr(actorClass)
            ? steam_decrypt::GetActorFNameString(actorClass) : std::string{};
        ++r.unresolvedMeshClasses;
        return;
    }
    ++r.unresolvedMeshClassesOther;
}

void RecordActorDecoded(uintptr_t actor, uintptr_t component, uintptr_t asset,
    const FTransform& ctw, ProbeResult& r)
{
    ActorGeometry g;
    g.kind = kActorKindDecoded;
    g.asset = asset;
    g.transform = ctw;
    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        g_actorGeometry[actor] = g;
        if (component)
            g_componentToActor[component] = actor;
    }
    ++r.actorPlacements;
    ++r.actorDecodedShapes;
}

bool RecordActorBoundsBox(uintptr_t actor, uintptr_t component, const Vector3& origin,
    const Vector3& extent, const FTransform& ctw, ProbeResult& r);

/**
 * Record a stand-in shape for an actor whose root is NOT a static mesh component.
 *
 * A character's root component IS its collision capsule (ACharacter makes it so),
 * which is why the class name already resolved for the static-mesh test is also
 * what says a real capsule exists to read here. Everything else that is a
 * primitive component falls back to its own LocalBounds box - coarse, but the
 * only geometry a skeletal mesh can offer.
 */
void RecordActorStandIn(uintptr_t actor, uintptr_t root, const std::string& componentClass,
    ProbeResult& r)
{
    if (!IsPlausiblePtr(root)) {
        ++r.actorGeometryUnusable;
        ++r.standInRootReject;
        return;
    }
    const FTransform ctw = Engine::ReadComponentToWorld(root);
    r.reads += 1;
    if (!PlausibleComponentTransform(ctw)) {
        ++r.actorGeometryUnusable;
        ++r.standInTransformReject;
        return;
    }

    if (componentClass.find("CapsuleComponent") != std::string::npos) {
        const CapsuleDims dims = Memory::read_nocache<CapsuleDims>(
            root + Offsets::CapsuleComponent_CapsuleHalfHeight);
        r.reads += 1;
        const bool dimsOk = std::isfinite(dims.halfHeight) && std::isfinite(dims.radius)
            && dims.radius > 0.5f && dims.radius < 500.f
            && dims.halfHeight > dims.radius && dims.halfHeight < 500.f;
        if (dimsOk) {
            ActorGeometry g;
            g.kind = kActorKindCapsule;
            g.transform = ctw;
            // HalfHeight carries the caps: the cylinder half-length the outline
            // and the tessellator both want is the difference, not the raw field.
            g.shape = MakeCapsulePrimitive(dims.radius,
                static_cast<double>(dims.halfHeight) - static_cast<double>(dims.radius));
            g.hasShape = true;
            {
                std::lock_guard<std::mutex> lock(g_cacheMu);
                g_actorGeometry[actor] = g;
                g_componentToActor[root] = actor;
            }
            ++r.actorPlacements;
            ++r.actorCapsules;
            return;
        }
    }

    // UPrimitiveComponent::LocalBounds (FBoxSphereBounds), local space.
    const AssetBoundsLocal lb = Memory::read_nocache<AssetBoundsLocal>(
        root + Offsets::PrimitiveComponent_LocalBounds);
    r.reads += 1;
    if (!RecordActorBoundsBox(actor, root, lb.origin, lb.extent, ctw, r)) {
        ++r.actorGeometryUnusable;
        ++r.standInBoundsReject;
        // A brush IS a primitive component, so its bounds are supposed to be here.
        // Record the raw numbers for the first couple of them: all-zero means the
        // offset does not hold for this class on this build, and sane-but-small
        // means the validation is rejecting a usable box.
        if (componentClass.find("Brush") != std::string::npos) {
            ++r.brushBoundsRejects;
            if (r.brushBoundsSample[1].empty()) {
                char s[160];
                std::snprintf(s, sizeof(s),
                    "o(%.1f,%.1f,%.1f) e(%.2f,%.2f,%.2f) r%.2f",
                    lb.origin.x, lb.origin.y, lb.origin.z,
                    lb.extent.x, lb.extent.y, lb.extent.z, lb.radius);
                r.brushBoundsSample[r.brushBoundsSample[0].empty() ? 0 : 1] = s;
            }
        }
    }
}

/** Shared by the two box stand-in sources: validate, then record. */
bool RecordActorBoundsBox(uintptr_t actor, uintptr_t component, const Vector3& origin,
    const Vector3& extent, const FTransform& ctw, ProbeResult& r)
{
    const bool ok = std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z)
        && std::isfinite(extent.x) && std::isfinite(extent.y) && std::isfinite(extent.z)
        && extent.x > 0.5 && extent.y > 0.5 && extent.z > 0.5
        && CollisionGeom::VectorMaxAbs(extent) < 100000.0;
    if (!ok)
        return false;
    ActorGeometry g;
    g.kind = kActorKindBounds;
    g.transform = ctw;
    g.shape = MakeBoxPrimitive(origin, extent);
    g.hasShape = true;
    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        g_actorGeometry[actor] = g;
        if (component)
            g_componentToActor[component] = actor;
    }
    ++r.actorPlacements;
    ++r.actorBoundsBoxes;
    return true;
}

/**
 * Resolve one actor's static-mesh component. Returns 0 and records why not.
 * The class check is what keeps skeletal-mesh actors out of a static-mesh path
 * instead of misreading their component bytes.
 */
uintptr_t ResolveStaticMeshComponent(uintptr_t actor, ProbeResult& r,
    std::string& outClassName, uintptr_t& outRoot)
{
    const uintptr_t root = Engine::ResolveActorRoot(actor);
    outRoot = root;
    if (!IsPlausiblePtr(root)) {
        ++r.rootsMissing;
        return 0;
    }
    ++r.rootsResolved;

    const uintptr_t classPtr = steam_decrypt::GetActorClassPtr(root);
    outClassName = IsPlausiblePtr(classPtr) ? steam_decrypt::GetActorFNameString(classPtr)
        : std::string{};
    if (outClassName.find("StaticMeshComponent") == std::string::npos) {
        ++r.otherComponentRoots;
        return 0;
    }
    ++r.staticMeshComponentRoots;
    return root;
}

/** Decode one asset's AggGeom, using `scratch` as the element read buffer. */
void DecodeAsset(AssetRecord& rec, std::vector<uint8_t>& scratch, ProbeResult& r)
{
    if (rec.directBodySetup) {
        rec.bodySetup = rec.asset;
    } else {
        rec.bodySetup = Memory::read_nocache<uintptr_t>(rec.asset + Offsets::StaticMesh_BodySetup);
        r.reads += 1;
    }
    if (!IsPlausiblePtr(rec.bodySetup)) {
        rec.state = 2;  // BodySetup is Transient on this build: null is a real outcome
        return;
    }

    std::vector<uint8_t> headers;
    if (!ReadBlock(rec.bodySetup + Offsets::BodySetup_AggGeom,
            CollisionGeom::Layout::kHeaderBytes, headers, r)) {
        rec.state = 3;
        return;
    }

    rec.header = CollisionGeom::ParseAggGeomHeader(headers.data());
    if (rec.header.TotalElements() <= 0) {
        rec.state = 4;
        return;
    }

    const CollisionGeom::ElementBytes bytes =
        [&scratch](CollisionGeom::Shape shape, uintptr_t data, int32_t count) -> const uint8_t* {
            const size_t elemSize = CollisionGeom::ElementSize(shape);
            if (!elemSize || count <= 0)
                return nullptr;
            const size_t total = static_cast<size_t>(count) * elemSize;
            if (total > 64 * 1024)
                return nullptr;
            scratch.assign(total, 0);
            if (!Memory::ReadRaw(data, scratch.data(), total))
                return nullptr;
            return scratch.data();
        };

    CollisionGeom::ParseLimits limits;
    limits.maxPerArray = 64;
    limits.maxPrimitives = 256;
    // Hull point clouds go through the budgeted ReadBlock, so they are counted the
    // same way as every other read in this decode.
    limits.readRemote = [&r](uintptr_t address, void* buffer, size_t size) {
        std::vector<uint8_t> tmp;
        if (!ReadBlock(address, size, tmp, r))
            return false;
        std::memcpy(buffer, tmp.data(), size);
        return true;
    };
    CollisionGeom::ParseAggGeom(rec.header, bytes, limits, rec.primitives, rec.tally);
    rec.state = rec.primitives.empty() ? 4 : 1;
}

/** Append `n` little-endian 8-byte words from `p` as "\"off=0xval\"" entries. */
void AppendWordDump(std::ostringstream& f, const uint8_t* p, size_t n)
{
    for (size_t o = 0; o + 8 <= n * 8; o += 8) {
        if (o)
            f << ",";
        uintptr_t v = 0;
        std::memcpy(&v, p + o, sizeof(v));
        f << "\"" << std::hex << o << "=0x" << v << std::dec << "\"";
    }
}

/**
 * Diagnostic only, nothing here feeds the BVH. Anchored at UStaticMesh::RenderData
 * (0x168, verified live -- it sits inside the dump's unnamed StaticMesh pad block),
 * this walks RenderData -> LODResources -> LOD0 -> PositionVertexBuffer +
 * IndexBuffer and reads the real mesh geometry. Every field and offset is
 * VALIDATED, never trusted: the LOD/buffer layout comes from SDK CL-1424961, which
 * differs from this build (its BodySetup 0x250 vs this build's dumped 0x248), so a
 * live run is what decides. It logs the raw word dumps of each struct plus the
 * decoded position range, so the true layout can be pinned from the log. Reads go
 * through ReadBlock, so they are budgeted like the rest of this probe.
 */
void ProbeRenderBuffers(uintptr_t asset, ProbeResult& r)
{
    constexpr std::ptrdiff_t kRenderData = 0x168;   // UStaticMesh::RenderData
    constexpr std::ptrdiff_t kLodArrayData = 0x00;  // RenderData.LODResources (TArray)
    constexpr std::ptrdiff_t kLodArrayCount = 0x08;
    constexpr std::ptrdiff_t kLodVertexBuffers = 0x00; // FStaticMeshLODResources
    constexpr std::ptrdiff_t kLodIndexBuffer = 0x58;
    constexpr std::ptrdiff_t kVBufPosition = 0x58;  // FStaticMeshVertexBuffers
    constexpr std::ptrdiff_t kPosData = 0x08;       // FPositionVertexBuffer
    constexpr std::ptrdiff_t kPosNum = 0x10;
    constexpr std::ptrdiff_t kIBufData = 0x00;      // FRawStaticIndexBuffer
    constexpr std::ptrdiff_t kIBufBytes = 0x08;
    constexpr std::ptrdiff_t kIBuf16 = 0x10;

    const auto rd64 = [](const uint8_t* p) { uintptr_t v = 0; std::memcpy(&v, p, 8); return v; };
    const auto rd32 = [](const uint8_t* p) { int32_t v = 0; std::memcpy(&v, p, 4); return v; };

    std::ostringstream f;
    f << "{\"location\":\"CollisionRead.cpp\",\"message\":\"render_probe\",\"timestamp\":"
      << EntityDiagnostics::NowMs() << ",\"asset\":\"0x" << std::hex << asset << std::dec << "\"";

    std::vector<uint8_t> b;
    if (!ReadBlock(asset + kRenderData, 8, b, r)) {
        f << ",\"ok\":0,\"why\":\"renderData unreadable\"}\n";
        EntityDiagnostics::LogRaw(f.str());
        return;
    }
    const uintptr_t renderData = rd64(b.data());
    f << ",\"renderData\":\"0x" << std::hex << renderData << std::dec << "\"";
    if (!IsPlausiblePtr(renderData)) {
        f << ",\"ok\":0,\"why\":\"renderData not a pointer\"}\n";
        EntityDiagnostics::LogRaw(f.str());
        return;
    }

    if (!ReadBlock(renderData, 0x30, b, r)) {
        f << ",\"ok\":0,\"why\":\"renderData header unreadable\"}\n";
        EntityDiagnostics::LogRaw(f.str());
        return;
    }
    const uintptr_t lodData = rd64(b.data() + kLodArrayData);
    const int32_t lodCount = rd32(b.data() + kLodArrayCount);
    f << ",\"lodData\":\"0x" << std::hex << lodData << "\",\"lodCount\":" << std::dec << lodCount
      << ",\"renderDataWords\":[";
    AppendWordDump(f, b.data(), 0x30 / 8);
    f << "]";
    if (lodCount < 1 || lodCount > 16 || !IsPlausiblePtr(lodData)) {
        f << ",\"ok\":0,\"why\":\"lod array implausible\"}\n";
        EntityDiagnostics::LogRaw(f.str());
        return;
    }

    // LODResources is either a value array (element 0 == lodData) or an indirect
    // array (element 0 == *(lodData)); dump the first LOD's words either way.
    std::vector<uint8_t> e;
    if (!ReadBlock(lodData, 0xC0, e, r)) {
        f << ",\"ok\":0,\"why\":\"LOD0 unreadable\"}\n";
        EntityDiagnostics::LogRaw(f.str());
        return;
    }
    f << ",\"lod0FirstWord\":\"0x" << std::hex << rd64(e.data()) << std::dec << "\",\"lod0Words\":[";
    AppendWordDump(f, e.data(), 0xC0 / 8);
    f << "]";

    // Best-effort decode: value-array LOD0, position buffer at +0x58, index at +0x58.
    const uintptr_t lod0 = lodData;
    std::vector<uint8_t> pb;
    if (ReadBlock(lod0 + kVBufPosition, 0x20, pb, r)) {
        const uintptr_t posData = rd64(pb.data() + kPosData);
        const int32_t posNum = rd32(pb.data() + kPosNum);
        f << ",\"posBufWords\":[";
        AppendWordDump(f, pb.data(), 0x20 / 8);
        f << "],\"posData\":\"0x" << std::hex << posData << "\",\"posNum\":" << std::dec << posNum;
        if (IsPlausiblePtr(posData) && posNum > 2 && posNum < 4000000) {
            std::vector<uint8_t> vb;
            const size_t sample = (std::min)(static_cast<size_t>(posNum), static_cast<size_t>(256)) * 12;
            if (ReadBlock(posData, sample, vb, r)) {
                double mn[3] = { 1e30, 1e30, 1e30 };
                double mx[3] = { -1e30, -1e30, -1e30 };
                bool finite = true;
                for (size_t o = 0; o + 12 <= vb.size(); o += 12) {
                    float vx;
                    float vy;
                    float vz;
                    std::memcpy(&vx, vb.data() + o, 4);
                    std::memcpy(&vy, vb.data() + o + 4, 4);
                    std::memcpy(&vz, vb.data() + o + 8, 4);
                    if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz)) {
                        finite = false;
                        break;
                    }
                    mn[0] = (std::min)(mn[0], static_cast<double>(vx));
                    mx[0] = (std::max)(mx[0], static_cast<double>(vx));
                    mn[1] = (std::min)(mn[1], static_cast<double>(vy));
                    mx[1] = (std::max)(mx[1], static_cast<double>(vy));
                    mn[2] = (std::min)(mn[2], static_cast<double>(vz));
                    mx[2] = (std::max)(mx[2], static_cast<double>(vz));
                }
                f << ",\"posFinite\":" << (finite ? 1 : 0)
                  << ",\"posMin\":[" << mn[0] << "," << mn[1] << "," << mn[2] << "]"
                  << ",\"posMax\":[" << mx[0] << "," << mx[1] << "," << mx[2] << "]";
            }
        }
    }
    std::vector<uint8_t> ib;
    if (ReadBlock(lod0 + kLodIndexBuffer, 0x20, ib, r)) {
        const uintptr_t ibData = rd64(ib.data() + kIBufData);
        const int32_t ibBytes = rd32(ib.data() + kIBufBytes);
        const int32_t ib16 = rd32(ib.data() + kIBuf16);
        f << ",\"idxBufWords\":[";
        AppendWordDump(f, ib.data(), 0x20 / 8);
        f << "],\"idxData\":\"0x" << std::hex << ibData << "\",\"idxBytes\":" << std::dec << ibBytes
          << ",\"idx16\":" << ib16;
    }
    f << ",\"ok\":1}\n";
    EntityDiagnostics::LogRaw(f.str());
}

void EmitAssetLine(const AssetRecord& rec, const std::string& className)
{
    std::ostringstream f;
    f << "{\"location\":\"CollisionRead.cpp\",\"message\":\"collision_asset\","
      << "\"timestamp\":" << EntityDiagnostics::NowMs()
      << ",\"asset\":\"0x" << std::hex << rec.asset << std::dec << "\""
      << ",\"bodySetup\":\"0x" << std::hex << rec.bodySetup << std::dec << "\""
      << ",\"state\":" << rec.state
      << ",\"rootClass\":\"" << EntityDiagnostics::Sanitize(className, 40) << "\""
      << ",\"declared\":{\"sphere\":" << rec.tally.declared[0]
      << ",\"box\":" << rec.tally.declared[1]
      << ",\"capsule\":" << rec.tally.declared[2]
      << ",\"convex\":" << rec.tally.declared[3]
      << ",\"tapered\":" << rec.tally.declared[4]
      << ",\"levelSet\":" << rec.tally.declared[5]
      << ",\"skinnedLevelSet\":" << rec.tally.declared[6]
      << ",\"mlLevelSet\":" << rec.tally.declared[7]
      << ",\"skinnedTri\":" << rec.tally.declared[8] << "}"
      << ",\"parsed\":" << rec.tally.totalParsed
      << ",\"stored\":" << rec.primitives.size()
      << ",\"collisionEnabled\":[" << rec.tally.collisionEnabledHistogram[0] << ","
      << rec.tally.collisionEnabledHistogram[1] << ","
      << rec.tally.collisionEnabledHistogram[2] << ","
      << rec.tally.collisionEnabledHistogram[3] << "]"
      << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}

void EmitSummaryLine(const ProbeResult& r)
{
    std::ostringstream f;
    f << "{\"location\":\"CollisionRead.cpp\",\"message\":\"collision_probe\","
      << "\"timestamp\":" << EntityDiagnostics::NowMs()
      << ",\"actorsSeen\":" << r.actorsSeen
      << ",\"actorsWalked\":" << r.actorsWalked
      << ",\"rootsResolved\":" << r.rootsResolved
      << ",\"rootsMissing\":" << r.rootsMissing
      << ",\"staticMeshRoots\":" << r.staticMeshComponentRoots
      << ",\"otherComponentRoots\":" << r.otherComponentRoots
      << ",\"actorPlacements\":" << r.actorPlacements
      << ",\"actorDecodedShapes\":" << r.actorDecodedShapes
      << ",\"actorCapsules\":" << r.actorCapsules
      << ",\"actorBoundsBoxes\":" << r.actorBoundsBoxes
      << ",\"actorGeometryUnusable\":" << r.actorGeometryUnusable
      << ",\"standInRootReject\":" << r.standInRootReject
      << ",\"standInTransformReject\":" << r.standInTransformReject
      << ",\"standInBoundsReject\":" << r.standInBoundsReject
      << ",\"brushRoots\":" << r.brushRoots
      << ",\"brushNoBodySetup\":" << r.brushNoBodySetup
      << ",\"brushBoundsRejects\":" << r.brushBoundsRejects
      << ",\"brushBoundsSample\":[\"" << r.brushBoundsSample[0] << "\",\""
      << r.brushBoundsSample[1] << "\"]"
      << ",\"assetsUnresolved\":" << r.assetsUnresolved
      << ",\"assetsUnique\":" << r.assetsUnique
      << ",\"assetsParsed\":" << r.assetsParsed
      << ",\"assetsBodySetupNull\":" << r.assetsBodySetupNull
      << ",\"assetsHeaderReadFailed\":" << r.assetsHeaderReadFailed
      << ",\"assetsAggGeomEmpty\":" << r.assetsAggGeomEmpty
      << ",\"occluderBoxes\":" << r.occluderBoxes
      << ",\"assetsFromCache\":" << r.assetsFromCache
      << ",\"assetsOverCacheCap\":" << r.assetsOverCacheCap
      << ",\"primitivesStored\":" << r.primitivesStored
      << ",\"declaredTotal\":{\"sphere\":" << r.declaredByShape[0]
      << ",\"box\":" << r.declaredByShape[1]
      << ",\"capsule\":" << r.declaredByShape[2]
      << ",\"convex\":" << r.declaredByShape[3]
      << ",\"tapered\":" << r.declaredByShape[4]
      << ",\"levelSet\":" << r.declaredByShape[5]
      << ",\"skinnedLevelSet\":" << r.declaredByShape[6]
      << ",\"mlLevelSet\":" << r.declaredByShape[7]
      << ",\"skinnedTri\":" << r.declaredByShape[8] << "}"
      << ",\"unsupportedTotal\":{\"tapered\":" << r.unsupportedByShape[4]
      << ",\"levelSet\":" << r.unsupportedByShape[5]
      << ",\"skinnedLevelSet\":" << r.unsupportedByShape[6]
      << ",\"mlLevelSet\":" << r.unsupportedByShape[7]
      << ",\"skinnedTri\":" << r.unsupportedByShape[8] << "}"
      << ",\"truncatedTotal\":{\"sphere\":" << r.truncatedByShape[0]
      << ",\"box\":" << r.truncatedByShape[1]
      << ",\"capsule\":" << r.truncatedByShape[2]
      << ",\"convex\":" << r.truncatedByShape[3] << "}"
      << ",\"readFailedTotal\":{\"sphere\":" << r.readFailedByShape[0]
      << ",\"box\":" << r.readFailedByShape[1]
      << ",\"capsule\":" << r.readFailedByShape[2]
      << ",\"convex\":" << r.readFailedByShape[3] << "}"
      << ",\"collisionEnabled\":[" << r.collisionEnabledHistogram[0] << ","
      << r.collisionEnabledHistogram[1] << ","
      << r.collisionEnabledHistogram[2] << ","
      << r.collisionEnabledHistogram[3] << "]"
      << ",\"collisionEnabledOther\":" << r.collisionEnabledOther
      << ",\"boundsChecked\":" << r.boundsChecked
      << ",\"boundsAgree25cm\":" << r.boundsAgree25cm
      << ",\"boundsAgree100cm\":" << r.boundsAgree100cm
      << ",\"worstBoundsDeltaCm\":" << r.worstBoundsDeltaCm
      << ",\"medianBoundsDeltaCm\":" << r.medianBoundsDeltaCm
      << ",\"actorsSeen\":" << r.actorsSeen
      << ",\"actorsWalked\":" << r.actorsWalked
      << ",\"walkCapHit\":" << (r.walkCapHit ? "true" : "false")
      << ",\"skippedRootClasses\":" << r.skippedRootClasses
      // class AND count per entry: the count is what says whether a skipped kind
      // is a curiosity or the kind the level's buildings are built from.
      << ",\"skippedRootClassSample\":[\""
      << EntityDiagnostics::Sanitize(r.skippedRootClassSample[0], 40) << " x"
      << r.skippedRootClassCounts[0] << "\",\""
      << EntityDiagnostics::Sanitize(r.skippedRootClassSample[1], 40) << " x"
      << r.skippedRootClassCounts[1] << "\",\""
      << EntityDiagnostics::Sanitize(r.skippedRootClassSample[2], 40) << " x"
      << r.skippedRootClassCounts[2] << "\"]"
      << ",\"skippedRootClassesOther\":" << r.skippedRootClassesOther
      // What the unusable StaticMesh slots actually were: class name + count per
      // sampled kind, the owner actor class beside it, and the null/garbage split.
      << ",\"unresolvedMeshClasses\":" << r.unresolvedMeshClasses
      << ",\"unresolvedMeshClassSample\":[\""
      << EntityDiagnostics::Sanitize(r.unresolvedMeshClassSample[0], 40) << " on "
      << EntityDiagnostics::Sanitize(r.unresolvedMeshActorClassSample[0], 40) << " x"
      << r.unresolvedMeshClassCounts[0] << "\",\""
      << EntityDiagnostics::Sanitize(r.unresolvedMeshClassSample[1], 40) << " on "
      << EntityDiagnostics::Sanitize(r.unresolvedMeshActorClassSample[1], 40) << " x"
      << r.unresolvedMeshClassCounts[1] << "\",\""
      << EntityDiagnostics::Sanitize(r.unresolvedMeshClassSample[2], 40) << " on "
      << EntityDiagnostics::Sanitize(r.unresolvedMeshActorClassSample[2], 40) << " x"
      << r.unresolvedMeshClassCounts[2] << "\"]"
      << ",\"unresolvedMeshClassesOther\":" << r.unresolvedMeshClassesOther
      << ",\"unresolvedMeshNull\":" << r.unresolvedMeshNull
      << ",\"unresolvedMeshGarbage\":" << r.unresolvedMeshGarbage
      // Cache split by kind: brushes vs meshes, each with its primitive total, plus
      // the placed-instance count of the mesh assets that can actually reach the BVH.
      << ",\"cachedBrushAssets\":" << r.cachedBrushAssets
      << ",\"cachedMeshAssets\":" << r.cachedMeshAssets
      << ",\"cachedBrushPrimitives\":" << r.cachedBrushPrimitives
      << ",\"cachedMeshPrimitives\":" << r.cachedMeshPrimitives
      << ",\"bvhEligibleInstances\":" << r.bvhEligibleInstances
      << ",\"reads\":" << r.reads
      << ",\"bytesRead\":" << r.bytesRead
      << ",\"elapsedMs\":" << r.elapsedMs
      << ",\"note\":\"" << EntityDiagnostics::Sanitize(r.note, 120) << "\""
      << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}

// The BVH is built from this cache, so its size is the honest answer to "why do
// the rays see nothing": triangles near zero with assets > 0 means the level's
// meshes carry complex (tri-mesh) collision the simple-shape decoder cannot read.
void EmitBvhBuildLine(const CollisionBvh::BuildStats& b)
{
    std::ostringstream f;
    f << "{\"location\":\"CollisionRead.cpp\",\"message\":\"collision_bvh_build\","
      << "\"timestamp\":" << EntityDiagnostics::NowMs()
      << ",\"built\":" << (b.built ? 1 : 0)
      << ",\"assets\":" << b.assets
      << ",\"instances\":" << b.instances
      << ",\"triangles\":" << b.triangles
      << ",\"boxes\":" << b.boxes
      << ",\"spheres\":" << b.spheres
      << ",\"capsules\":" << b.capsules
      << ",\"convexAsBox\":" << b.convexAsBox
      << ",\"skipped\":" << b.skipped
      << ",\"tessMs\":" << b.tessMs
      << ",\"commitMs\":" << b.commitMs
      << ",\"totalMs\":" << b.totalMs
      << ",\"generation\":" << b.generation
      << ",\"boxOccluders\":" << b.boxOccluders
      // Full extents of the occluder boxes (min / median / max per X,Y,Z): what
      // size they actually are, without one line per box.
      << ",\"occluderCount\":" << b.occluderSize.count
      << ",\"occluderExtentMin\":[" << b.occluderSize.min[0] << "," << b.occluderSize.min[1] << "," << b.occluderSize.min[2] << "]"
      << ",\"occluderExtentMedian\":[" << b.occluderSize.median[0] << "," << b.occluderSize.median[1] << "," << b.occluderSize.median[2] << "]"
      << ",\"occluderExtentMax\":[" << b.occluderSize.max[0] << "," << b.occluderSize.max[1] << "," << b.occluderSize.max[2] << "]"
      << ",\"note\":\"" << EntityDiagnostics::Sanitize(b.note, 120) << "\""
      << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}

void RunProbeJob()
{
    // Attribute every read to this thread in the read ledger, under the purpose
    // the geometry already belongs to.
    ReadBudget::SetCurrentRequester("collision_probe");
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Bounds);

    const auto start = std::chrono::steady_clock::now();
    ProbeResult r;
    std::vector<uint8_t> scratch;
    int renderProbeLeft = 4;  // diagnostic: a few undecoded assets per run
    std::vector<double> boundsDeltas;

    if (Memory::getProcessID() == 0) {
        r.note = "not attached - run the probe in raid with the target attached";
        r.elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        Publish(r);
        EmitSummaryLine(r);
        return;
    }

    const std::vector<uintptr_t>& cached = WorldScan::CachedActorPtrs();
    r.actorsSeen = static_cast<int>(cached.size());

    for (const uintptr_t actor : cached) {
        if (g_stopRequested.load(std::memory_order_relaxed))
            break;
        if (r.actorsWalked >= kMaxActorsWalked) {
            r.walkCapHit = true;
            break;
        }
        if (!IsPlausiblePtr(actor))
            continue;
        ++r.actorsWalked;

        std::string className;
        uintptr_t rootComponent = 0;
        uintptr_t component = ResolveStaticMeshComponent(actor, r, className, rootComponent);
        // A level brush (walls, floors) is a BrushComponent root. Its collision lives
        // in its own BodySetup, so that pointer is the asset to decode - not a
        // StaticMesh. Without this branch every brush fell into the stand-in path.
        const bool isBrush = !component && IsPlausiblePtr(rootComponent)
            && className.find("BrushComponent") != std::string::npos;
        uintptr_t brushBodySetup = 0;
        if (isBrush) {
            brushBodySetup = Memory::read_nocache<uintptr_t>(
                rootComponent + Offsets::BrushComponent_BrushBodySetup);
            r.reads += 1;
            if (IsPlausiblePtr(brushBodySetup)) {
                component = rootComponent;
                ++r.brushRoots;
            } else {
                ++r.brushNoBodySetup;
            }
        }
        if (!component) {
            // Not a static mesh component: a character's root is its CAPSULE and
            // everything else still has LocalBounds. Recording here is what gives
            // players and bots an actor-keyed shape at all; without it a live
            // probe resolved 913 roots, kept 300, and skipped the 613 that are
            // exactly the entities the shape outlines are drawn for.
            if (IsPlausiblePtr(rootComponent))
                RecordActorStandIn(actor, rootComponent, className, r);
            else {
                ++r.actorGeometryUnusable;
                ++r.standInRootReject;
            }
            // Tally what the non-static-mesh roots actually are: without it
            // "otherComponentRoots: 589" is a number nobody can act on.
            NoteSkippedRootClass(className, r);
            continue;
        }

        const uintptr_t asset = brushBodySetup ? brushBodySetup
            : Memory::read_nocache<uintptr_t>(component + Offsets::StaticMeshComponent_StaticMesh);
        if (!brushBodySetup)
            r.reads += 1;
        if (!IsPlausiblePtr(asset)) {
            // No StaticMesh pointer to decode: fall back to the component's own
            // bounds so the actor still gets a shape, instead of vanishing.
            ++r.assetsUnresolved;
            if (!brushBodySetup) {
                if (asset == 0)
                    ++r.unresolvedMeshNull;
                else
                    ++r.unresolvedMeshGarbage;
                NoteUnresolvedMeshClass(className, actor, r);
                RecordActorStandIn(actor, component, className, r);
            }
            continue;
        }

        // Per-asset cache: many components share one UStaticMesh, and the read
        // cost is what the dedup exists to remove. A hit is still a data point
        // for the world-space agreement check below.
        // The component transform is read for every qualifying actor: it is what
        // places this asset's shapes in the world, which both the bounds check and
        // the BVH stage need. One read, not one per consumer.
        const FTransform ctw = Engine::ReadComponentToWorld(component);
        r.reads += 1;

        // One lock serves the lookup and the size bound. Only this job thread
        // ever inserts, and `rec` is used before the next iteration, so the
        // pointer stays valid for this iteration even across a rehash.
        AssetRecord* rec = nullptr;
        bool cacheFull = false;
        {
            std::lock_guard<std::mutex> lock(g_cacheMu);
            const auto it = g_assets.find(asset);
            if (it != g_assets.end()) {
                rec = &it->second;
                ++r.assetsFromCache;
            } else if (static_cast<int>(g_assets.size()) >= kMaxAssetsStored) {
                cacheFull = true;  // bounded, and counted below
            }
        }
        if (cacheFull) {
            ++r.assetsOverCacheCap;
            continue;
        }
        if (!rec) {
            AssetRecord asset_rec;
            asset_rec.asset = asset;
            asset_rec.directBodySetup = brushBodySetup != 0;
            asset_rec.firstSeenMs = SteadyNowMs();
            DecodeAsset(asset_rec, scratch, r);
            if (asset_rec.state != 1 && renderProbeLeft > 0) {
                --renderProbeLeft;
                ProbeRenderBuffers(asset, r);
            }
            {
                std::lock_guard<std::mutex> lock(g_cacheMu);
                rec = &g_assets.emplace(asset, std::move(asset_rec)).first->second;
            }
            ++r.assetsUnique;
            switch (rec->state) {
            case 1: ++r.assetsParsed; break;
            case 2: ++r.assetsBodySetupNull; break;
            case 3: ++r.assetsHeaderReadFailed; break;
            default: ++r.assetsAggGeomEmpty; break;
            }
            for (int s = 0; s < CollisionGeom::kShapeCount; ++s) {
                r.declaredByShape[s] += rec->tally.declared[s];
                r.parsedByShape[s] += rec->tally.parsed[s];
                r.unsupportedByShape[s] += rec->tally.unsupported[s];
                r.truncatedByShape[s] += rec->tally.truncated[s];
                r.readFailedByShape[s] += rec->tally.readFailed[s];
            }
            for (int b = 0; b < 4; ++b)
                r.collisionEnabledHistogram[b] += rec->tally.collisionEnabledHistogram[b];
            r.collisionEnabledOther += rec->tally.collisionEnabledOther;
            r.primitivesStored += static_cast<int>(rec->primitives.size());
            EmitAssetLine(*rec, className);
        }

        // Coarse occluder for this placement, when there is nothing else to
        // represent it with. See OccluderBox in the header: an empty AggGeom is
        // complex-only collision, which is most of a level's walls, and without
        // this the visibility scene has no cover to find. It reads the bounds
        // block (positive/negative extensions + ExtendedBounds) in one shot — a
        // deliberately recorded sixth direct-read site in direct-read-baseline.txt
        // — and it happens here instead of under the cache lock.
        if (rec->state != 1 && !rec->directBodySetup && g_occluders.size() < kMaxOccluderBoxes) {
            const double qOc = ctw.Rotation.x * ctw.Rotation.x
                + ctw.Rotation.y * ctw.Rotation.y
                + ctw.Rotation.z * ctw.Rotation.z
                + ctw.Rotation.w * ctw.Rotation.w;
            const bool ocTransformOk = std::isfinite(qOc) && qOc > 0.9 && qOc < 1.1
                && std::isfinite(ctw.Translation.x) && std::isfinite(ctw.Translation.y)
                && std::isfinite(ctw.Translation.z)
                && std::abs(ctw.Scale3D.x) > 0.001 && std::abs(ctw.Scale3D.y) > 0.001
                && std::abs(ctw.Scale3D.z) > 0.001;
            if (ocTransformOk) {
                const AssetBoundsBlock bb = Memory::read_nocache<AssetBoundsBlock>(
                    asset + Offsets::StaticMesh_PositiveBoundsExtension);
                r.reads += 1;
                // Fold the extensions back out: UE stores ExtendedBounds as the
                // mesh box grown by the positive/negative extensions, so the mesh's
                // own half extent is the stored one minus their mean and the centre
                // shifts by the mean of the two. Zero extensions are a no-op.
                const Vector3 half(
                    bb.extent.x - (bb.posExt.x + bb.negExt.x) * 0.5,
                    bb.extent.y - (bb.posExt.y + bb.negExt.y) * 0.5,
                    bb.extent.z - (bb.posExt.z + bb.negExt.z) * 0.5);
                Vector3 org(
                    bb.origin.x - (bb.posExt.x - bb.negExt.x) * 0.5,
                    bb.origin.y - (bb.posExt.y - bb.negExt.y) * 0.5,
                    bb.origin.z - (bb.posExt.z - bb.negExt.z) * 0.5);
                const bool ocBoundsOk = std::isfinite(half.x) && std::isfinite(half.y)
                    && std::isfinite(half.z) && half.x > 0.0 && half.y > 0.0 && half.z > 0.0
                    && CollisionGeom::VectorMaxAbs(half) < 100000.0
                    && std::isfinite(org.x) && std::isfinite(org.y) && std::isfinite(org.z);
                if (ocBoundsOk) {
                    // A centre flung far from its own box is a misread; snap it
                    // back rather than place a wall the mesh does not occupy.
                    if (CollisionGeom::VectorMaxAbs(org)
                        > CollisionGeom::VectorMaxAbs(half) * 2.35)
                        org = Vector3(0.0, 0.0, 0.0);
                    {
                        std::lock_guard<std::mutex> lock(g_cacheMu);
                        g_occluders[component] = OccluderBox{ org, half, ctw };
                    }
                    // The same read, kept for the actor index: an item standing on
                    // a mesh whose collision did not decode still gets its box, and
                    // the box is the asset's own bounds in the component's frame.
                    RecordActorBoundsBox(actor, component, bb.origin, bb.extent, ctw, r);
                }
            }
        }

        // Record the placement (deduped by component, capped per asset) so the
        // BVH stage can build world space from this same pass.
        if (rec->state == 1 && rec->instances.size() < kMaxInstancesPerAsset) {
            bool alreadyPlaced = false;
            for (const AssetInstance& inst : rec->instances) {
                if (inst.component == component) {
                    alreadyPlaced = true;
                    break;
                }
            }
            if (!alreadyPlaced) {
                rec->instances.push_back(AssetInstance{ component, ctw });
                std::lock_guard<std::mutex> lock(g_cacheMu);
                g_componentToAsset[component] = asset;
            }
        }

        // Actor index: the decoded shapes stay in the asset cache, so this entry
        // only says where to find them. This is the difference between "the cache
        // has no geometry for this actor" and "the caller had the wrong key".
        if (rec->state == 1)
            RecordActorDecoded(actor, component, asset, ctw, r);

        // Cross-validation: decode the shapes into world space with the same
        // component transform the ESP uses, and compare that AABB against the
        // asset's own ExtendedBounds box under the same transform. Both are
        // geometry-derived, so the delta measures THIS decoder, not a difference
        // in origin convention.
        if (rec->state == 1 && !rec->directBodySetup && r.boundsChecked < kMaxBoundsChecks) {
            const double q = ctw.Rotation.x * ctw.Rotation.x + ctw.Rotation.y * ctw.Rotation.y
                + ctw.Rotation.z * ctw.Rotation.z + ctw.Rotation.w * ctw.Rotation.w;
            const bool scaleUsable = std::isfinite(ctw.Scale3D.x) && std::isfinite(ctw.Scale3D.y)
                && std::isfinite(ctw.Scale3D.z) && std::abs(ctw.Scale3D.x) > 0.001
                && std::abs(ctw.Scale3D.y) > 0.001 && std::abs(ctw.Scale3D.z) > 0.001;
            const bool originUsable = std::isfinite(ctw.Translation.x)
                && std::isfinite(ctw.Translation.y) && std::isfinite(ctw.Translation.z);
            if (std::isfinite(q) && q > 0.9 && q < 1.1 && scaleUsable && originUsable) {
                const AssetBoundsLocal b = Memory::read_nocache<AssetBoundsLocal>(
                    rec->asset + Offsets::StaticMesh_ExtendedBounds);
                r.reads += 1;
                const bool boundsUsable = std::isfinite(b.extent.x) && std::isfinite(b.extent.y)
                    && std::isfinite(b.extent.z) && b.extent.x > 0.0 && b.extent.y > 0.0
                    && b.extent.z > 0.0 && CollisionGeom::VectorMaxAbs(b.extent) < 100000.0;

                Vector3 aggMin, aggMax;
                const int used = CollisionGeom::AssetWorldAabb(ctw, rec->primitives, aggMin, aggMax);
                if (used > 0 && boundsUsable) {
                    // The asset box goes through the same transform, corner by corner.
                    Vector3 boxMin(0.0, 0.0, 0.0), boxMax(0.0, 0.0, 0.0);
                    for (int i = 0; i < 8; ++i) {
                        const Vector3 local(
                            b.origin.x + ((i & 1) ? b.extent.x : -b.extent.x),
                            b.origin.y + ((i & 2) ? b.extent.y : -b.extent.y),
                            b.origin.z + ((i & 4) ? b.extent.z : -b.extent.z));
                        const Vector3 w = CollisionGeom::TransformPoint(ctw, local);
                        if (i == 0) {
                            boxMin = w;
                            boxMax = w;
                        } else {
                            boxMin = Vector3((std::min)(boxMin.x, w.x), (std::min)(boxMin.y, w.y),
                                (std::min)(boxMin.z, w.z));
                            boxMax = Vector3((std::max)(boxMax.x, w.x), (std::max)(boxMax.y, w.y),
                                (std::max)(boxMax.z, w.z));
                        }
                    }
                    const double d = (std::max)((std::max)(
                        (std::max)(std::abs(aggMin.x - boxMin.x), std::abs(aggMax.x - boxMax.x)),
                        (std::max)(std::abs(aggMin.y - boxMin.y), std::abs(aggMax.y - boxMax.y))),
                        (std::max)(std::abs(aggMin.z - boxMin.z), std::abs(aggMax.z - boxMax.z)));
                    if (std::isfinite(d)) {
                        ++r.boundsChecked;
                        boundsDeltas.push_back(d);
                        if (d <= 25.0) ++r.boundsAgree25cm;
                        if (d <= 100.0) ++r.boundsAgree100cm;
                        if (d > r.worstBoundsDeltaCm) r.worstBoundsDeltaCm = d;
                    }
                }
            }
        }
    }

    if (!boundsDeltas.empty()) {
        std::sort(boundsDeltas.begin(), boundsDeltas.end());
        r.medianBoundsDeltaCm = boundsDeltas[boundsDeltas.size() / 2];
    }

    if (r.actorsSeen == 0)
        r.note = "actor cache empty - the world scan has not populated yet";
    else if (r.assetsUnique == 0 && r.assetsFromCache == 0 && r.staticMeshComponentRoots > 0)
        r.note = std::to_string(r.assetsUnresolved) + " of "
            + std::to_string(r.staticMeshComponentRoots)
            + " static-mesh components had no readable StaticMesh pointer - nothing to decode";
    else if (r.assetsUnique == 0 && r.assetsFromCache == 0)
        r.note = "no StaticMeshComponent actors in the cached prefix";
    else if (r.assetsBodySetupNull > 0 && r.assetsParsed == 0)
        r.note = "every BodySetup read null - the Transient slot is not populated on this build";
    // The one gap a count cannot express on its own: an asset whose AggGeom is
    // empty is not a decode failure, it is a mesh whose collision is complex per-poly
    // (cooked tri-mesh) -- so the visibility scene legitimately has no geometry for
    // it. Said out loud, because "no geometry drawn" is otherwise indistinguishable
    // from "the decoder found nothing".
    else if (r.assetsParsed == 0 && r.assetsAggGeomEmpty > 0)
        r.note = "every asset declares empty AggGeom (complex/tri-mesh collision only) - no geometry to build";
    else if (r.assetsAggGeomEmpty > r.assetsParsed)
        r.note = std::to_string(r.assetsAggGeomEmpty) + " of "
            + std::to_string(r.assetsUnique)
            + " assets declare no simple collision (complex only); "
            + std::to_string(r.occluderBoxes) + " coarse bounds occluders stand in";

    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        r.occluderBoxes = static_cast<int>(g_occluders.size());
        // Split the cache by kind. This is the number that says whether the BVH is
        // near-empty because brushes filled every slot (they are dropped from the
        // scene) or because the meshes themselves carry no simple collision.
        for (const auto& kv : g_assets) {
            const AssetRecord& a = kv.second;
            const int prims = static_cast<int>(a.primitives.size());
            const int insts = static_cast<int>(a.instances.size());
            if (a.directBodySetup) {
                ++r.cachedBrushAssets;
                r.cachedBrushPrimitives += prims;
            } else {
                ++r.cachedMeshAssets;
                r.cachedMeshPrimitives += prims;
                if (a.state == 1 && prims > 0)
                    r.bvhEligibleInstances += insts;
            }
        }
    }

    r.ran = true;
    r.elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    Publish(r);
    EmitSummaryLine(r);
}

} // namespace

/**
 * Why the probe should arm itself, or nullptr when nothing needs a scene.
 *
 * The probe used to arm on UpgradeOptions::collisionProbe alone — a flag the menu
 * button sets and that the shipped auto_config.ini never carried, so the vischeck
 * and the scene overlays could be enabled with NO scene to work against. Both
 * vischeck features fail open without one (they report clear), which makes
 * "vischeck on, scene absent" look exactly like "nothing is ever occluded" —
 * i.e. like a broken vischeck. Everything listed here reads the BVH, so asking
 * for any of it is asking for a scene, once per session.
 */
static const char* ProbeArmReason()
{
    if (UpgradeOptions::collisionProbe)
        return "collision_probe";
    if (UpgradeOptions::visCheckAim)
        return "vischeck_aim";
    if (UpgradeOptions::collisionDebugShapes || UpgradeOptions::collisionDebugRays
        || UpgradeOptions::collisionDebugSceneDecoded
        || UpgradeOptions::collisionDebugSceneBoxes)
        return "collision_debug";
    return nullptr;
}

// Auto-rebuild bookkeeping. Touched only from TickAutoArm (one thread).
constexpr int kMaxAutoRebuilds = 4;
constexpr int kRebuildRosterPermille = 250;   // 25% change in roster size
int g_autoBuilds = 0;
size_t g_lastBuildRoster = 0;

void TickAutoArm()
{
    const char* armReason = ProbeArmReason();
    if (!armReason)
        return;

    // One record naming what asked for the scene. Arming is not free (it walks
    // the level and commits a BVH), so a silent auto-arm would read as a stray
    // hitch at session start with nothing to attribute it to.
    static bool s_armReasonLogged = false;
    if (!s_armReasonLogged) {
        s_armReasonLogged = true;
        char rec[192];
        std::snprintf(rec, sizeof(rec),
            "{\"location\":\"CollisionRead.cpp:TickAutoArm\","
            "\"message\":\"collision_probe_autoarm\",\"timestamp\":%llu,"
            "\"reason\":\"%.32s\"}\n",
            static_cast<unsigned long long>(EntityDiagnostics::NowMs()), armReason);
        EntityDiagnostics::LogRaw(rec);
    }

    // With the probe armed, prove the ray pipeline once per launch on synthetic
    // geometry and log the verdict. It costs milliseconds, needs no target, and
    // it is the only part of this feature that can be checked without a raid.
    // RunSelfTest leaves no scene published (its last case clears it), so this
    // cannot leave a fake wall standing in front of the real BVH.
    static bool s_selfTestRan = false;
    if (!s_selfTestRan) {
        s_selfTestRan = true;
        CollisionBvh::RunSelfTest();
        // Same reason as the scene self-test: the keying the shape outlines depend
        // on is provable without a target, and a bug in it looks exactly like "no
        // geometry for this actor" - which is what it was mistaken for.
        RunIndexSelfTest();
    }

    if (g_probeRunning.load(std::memory_order_acquire))
        return;

    // Throttle FIRST, before any decision is taken or recorded. Polling the
    // roster is a vector copy under the actor-cache lock, so this asks at most
    // four times a second rather than on every painted frame — and, more
    // importantly, a decision recorded before the gate was a decision recorded
    // hundreds of times per second.
    static std::atomic<uint64_t> s_lastCheckMs{ 0 };
    const uint64_t nowMs = SteadyNowMs();
    const uint64_t last = s_lastCheckMs.load(std::memory_order_relaxed);
    if (last != 0 && nowMs < last + 250)
        return;
    s_lastCheckMs.store(nowMs, std::memory_order_relaxed);

    // Roster size read OUTSIDE g_stateMu: CachedActorPtrs() takes the actor-cache
    // lock, and holding both would set up a lock-order argument with the scans.
    const size_t rosterNow = WorldScan::CachedActorPtrs().size();
    // An empty roster is a world that has not loaded (or has unloaded), not a
    // world that changed. Treating it as a change made every unload look like a
    // reason to rebuild: the count flipped 1171 -> 0 and a rebuild was announced
    // on each frame while nothing could be built from it.
    if (rosterNow == 0)
        return;

    bool needBuild = false;
    bool isRebuild = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMu);
        if (g_result.running)
            return;
        if (!g_result.ran) {
            needBuild = true;
        } else if (g_autoBuilds < kMaxAutoRebuilds) {
            // The scene used to be built once and never revisited, so it kept
            // describing whatever was resident at that instant. Travelling into a
            // level that streamed in afterwards therefore left the wireframes
            // showing the old, emptier world -- loot boxes with no buildings.
            // Bounded: at most kMaxAutoRebuilds rebuilds per session, and only
            // after the roster has moved by kRebuildRosterPermille, so this can
            // never become a rebuild loop.
            const size_t base = g_lastBuildRoster ? g_lastBuildRoster : 1;
            const size_t delta = rosterNow > base ? rosterNow - base : base - rosterNow;
            if (delta * 1000 >= base * kRebuildRosterPermille) {
                needBuild = true;
                isRebuild = true;
            }
        }
    }
    if (!needBuild)
        return;
    if (isRebuild) {
        char rec[224];
        std::snprintf(rec, sizeof(rec),
            "{\"location\":\"CollisionRead.cpp:TickAutoArm\","
            "\"message\":\"collision_probe_rebuild\",\"timestamp\":%llu,"
            "\"why\":\"roster_changed\",\"rosterWas\":%zu,\"rosterNow\":%zu,"
            "\"build\":%d}\n",
            static_cast<unsigned long long>(EntityDiagnostics::NowMs()),
            g_lastBuildRoster, rosterNow, g_autoBuilds + 1);
        EntityDiagnostics::LogRaw(rec);
    }

    // Count the build BEFORE starting it, so the bound holds even if the job
    // refuses to start (a counter advanced afterwards is a counter a failed start
    // can escape, and this one bounds how often a whole level is re-walked).
    g_lastBuildRoster = rosterNow;
    ++g_autoBuilds;
    StartProbe();
}

void StartProbe()
{
    bool expected = false;
    if (!g_probeRunning.compare_exchange_strong(expected, true))
        return;

    g_stopRequested.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_stateMu);
        g_result = ProbeResult{};
        g_result.running = true;
    }

    const bool started = g_probeJob.start([]() {
        bool probeCompleted = false;
        try {
            RunProbeJob();
            probeCompleted = true;
        } catch (const std::exception& ex) {
            ProbeResult r;
            r.ran = true;
            r.note = std::string("probe threw: ") + ex.what();
            Publish(r);
        } catch (...) {
            ProbeResult r;
            r.ran = true;
            r.note = "probe threw";
            Publish(r);
        }
        // Build the visibility scene from what the walk just decoded, here on the
        // owned background job -- never on the paint or aim tick, because a level
        // build is seconds of work. This is the other half of "no rays":
        // RebuildFromCache was reachable only from a menu button, so a session
        // that never pressed it had NO scene, and every visibility ray answered
        // not-judged (CollisionBvh::QueryStats::noScene) -- the rays were traced
        // and drawn, they just had nothing to hit and no answer to colour by.
        if (probeCompleted && !g_stopRequested.load(std::memory_order_relaxed)) {
            const CollisionBvh::BuildStats built = CollisionBvh::RebuildFromCache();
            // A published build already logs its own record (CollisionBvh.cpp);
            // this covers what that one cannot report -- a build that published
            // nothing, which is what "the vischeck never says blocked" looks like
            // from the log side.
            if (!built.built)
                EmitBvhBuildLine(built);
        }
        {
            std::lock_guard<std::mutex> lock(g_stateMu);
            g_result.running = false;
        }
        g_probeRunning.store(false, std::memory_order_release);
    });

    if (!started) {
        std::lock_guard<std::mutex> lock(g_stateMu);
        g_result.running = false;
        g_result.ran = true;
        g_result.note = "job already running";
        g_probeRunning.store(false, std::memory_order_release);
    }
}

ProbeResult GetProbeResult()
{
    std::lock_guard<std::mutex> lock(g_stateMu);
    return g_result;
}

int CachedAssetCount()
{
    std::lock_guard<std::mutex> lock(g_cacheMu);
    return static_cast<int>(g_assets.size());
}

void ForEachParsedAsset(
    const std::function<void(uintptr_t asset,
        const std::vector<CollisionGeom::Primitive>& primitives,
        const std::vector<AssetInstance>& instances)>& fn)
{
    if (!fn)
        return;
    std::lock_guard<std::mutex> lock(g_cacheMu);
    for (const auto& kv : g_assets) {
        // state 1 means the asset decoded geometry; anything else has nothing to
        // place and is skipped rather than handed over empty.
        if (kv.second.state != 1 || kv.second.primitives.empty())
            continue;
        // Brushes are BodySetups, not UStaticMesh, and were once kept out of the
        // scene on the assumption that the level's walls are static meshes. A live
        // probe disproved that: the mesh cache held 21 assets for 1 primitive total
        // (their collision is complex tri-mesh, which this decoder cannot read),
        // while 171 brush BodySetups carried 233 primitives -- the level's real
        // simple collision. Dropping them left the vischeck scene with one mesh and
        // 30 coarse boxes, which is why the rays never saw a wall. Brush geometry is
        // simple shapes in a BodySetup exactly like a mesh's, so it goes in the same
        // way; the paint paths have always consumed it through the same index.
        fn(kv.first, kv.second.primitives, kv.second.instances);
    }
}

bool ForEachPrimitiveAtComponent(uintptr_t component,
    const std::function<void(const CollisionGeom::Primitive&, const FTransform&)>& fn)
{
    if (!fn || !component)
        return false;
    std::lock_guard<std::mutex> lock(g_cacheMu);
    const auto link = g_componentToAsset.find(component);
    if (link == g_componentToAsset.end())
        return false;
    const auto it = g_assets.find(link->second);
    if (it == g_assets.end() || it->second.state != 1)
        return false;
    const AssetRecord& rec = it->second;
    FTransform transform = CollisionGeom::IdentityTransform();
    for (const AssetInstance& inst : rec.instances) {
        if (inst.component == component) {
            transform = inst.transform;
            break;
        }
    }
    for (const CollisionGeom::Primitive& p : rec.primitives)
        fn(p, transform);
    return true;
}

bool ForEachPrimitiveAtActor(uintptr_t key,
    const std::function<void(const CollisionGeom::Primitive&, const FTransform&, bool)>& fn)
{
    if (!fn || !key)
        return false;

    std::lock_guard<std::mutex> lock(g_cacheMu);
    auto it = g_actorGeometry.find(key);
    if (it == g_actorGeometry.end()) {
        // The caller may hold a component of the actor rather than the actor
        // itself, which is what the item and bot paint paths do.
        const auto link = g_componentToActor.find(key);
        if (link == g_componentToActor.end())
            return false;
        it = g_actorGeometry.find(link->second);
        if (it == g_actorGeometry.end())
            return false;
    }

    const ActorGeometry& g = it->second;
    if (g.kind == kActorKindDecoded) {
        const auto asset = g_assets.find(g.asset);
        if (asset == g_assets.end() || asset->second.state != 1)
            return false;
        for (const CollisionGeom::Primitive& p : asset->second.primitives)
            fn(p, g.transform, /*standIn=*/false);
        return true;
    }
    if (!g.hasShape)
        return false;
    fn(g.shape, g.transform, /*standIn=*/true);
    return true;
}

IndexSelfTestResult RunIndexSelfTest()
{
    IndexSelfTestResult r;
    r.ran = true;

    // Keys that cannot be real pointers: no real actor or component address is 1,
    // 2 or 3, so a probe running concurrently is never reached by this test.
    const uintptr_t actorKey = 1;
    const uintptr_t componentKey = 2;
    const uintptr_t unknownKey = 3;

    ActorGeometry g;
    g.kind = kActorKindCapsule;
    g.transform = CollisionGeom::IdentityTransform();
    g.shape = MakeCapsulePrimitive(34.0, 88.0);
    g.hasShape = true;
    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        g_actorGeometry[actorKey] = g;
        g_componentToActor[componentKey] = actorKey;
    }

    int seen = 0;
    bool lastStandIn = false;
    CollisionGeom::Primitive lastShape{};
    const auto visit = [&](const CollisionGeom::Primitive& p, const FTransform&, bool standIn) {
        ++seen;
        lastStandIn = standIn;
        lastShape = p;
    };

    // 1. The actor key resolves, and the shape and its stand-in flag survive.
    const bool byActor = ForEachPrimitiveAtActor(actorKey, visit);
    const bool actorOk = byActor && seen == 1 && lastStandIn
        && lastShape.shape == CollisionGeom::Shape::Sphyl
        && lastShape.radius > 33.9 && lastShape.radius < 34.1
        && lastShape.halfLength > 87.9 && lastShape.halfLength < 88.1;

    // 2. A COMPONENT of that actor resolves to the same geometry. This is the
    //    case the bot and item paint paths rely on: they hold a root component.
    seen = 0;
    const bool byComponent = ForEachPrimitiveAtActor(componentKey, visit);
    const bool componentOk = byComponent && seen == 1 && lastStandIn;

    // 3. An unknown key resolves to nothing. A lookup that answered here would
    //    draw one entity's geometry at another entity's position.
    seen = 0;
    const bool unknownOk = !ForEachPrimitiveAtActor(unknownKey, visit) && seen == 0;

    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        g_actorGeometry.erase(actorKey);
        g_componentToActor.erase(componentKey);
    }

    const bool cases[3] = { actorOk, componentOk, unknownOk };
    r.cases = 3;
    for (const bool ok : cases) {
        if (!ok)
            ++r.failed;
    }
    r.passed = r.failed == 0;

    std::ostringstream detail;
    detail << "actor_key_resolves=" << (actorOk ? "pass" : "fail") << " ";
    detail << "component_key_resolves=" << (componentOk ? "pass" : "fail") << " ";
    detail << "unknown_key_empty=" << (unknownOk ? "pass" : "fail") << " ";
    r.detail = detail.str();

    std::ostringstream f;
    f << "{\"location\":\"CollisionRead.cpp\",\"message\":\"collision_actor_index_selftest\","
      << "\"timestamp\":" << EntityDiagnostics::NowMs()
      << ",\"passed\":" << (r.passed ? "true" : "false")
      << ",\"cases\":" << r.cases
      << ",\"failed\":" << r.failed
      << ",\"detail\":\"" << EntityDiagnostics::Sanitize(r.detail, 160) << "\""
      << "}\n";
    EntityDiagnostics::LogRaw(f.str());
    return r;
}

void ForEachOccluderBox(const std::function<void(const OccluderBox&)>& fn)
{
    if (!fn)
        return;
    std::lock_guard<std::mutex> lock(g_cacheMu);
    for (const auto& kv : g_occluders)
        fn(kv.second);
}

void ClearCache()
{
    // A running job holds a pointer into this map between iterations, so a clear
    // under its feet would dangle it. The menu already disables the button while
    // a run is in flight; this is the invariant, not just the UI state.
    if (g_probeRunning.load(std::memory_order_acquire))
        return;
    // The scene is geometry taken from this cache; keeping it after the cache is
    // gone would answer occlusion from a level the user just asked to forget.
    // A re-run of the probe rebuilds and republishes (see StartProbe).
    CollisionBvh::Clear();
    std::lock_guard<std::mutex> lock(g_cacheMu);
    g_assets.clear();
    g_componentToAsset.clear();
    // The actor index holds the same geometry by a different key; a stale entry
    // would keep outlining a level the user just asked to forget.
    g_actorGeometry.clear();
    g_componentToActor.clear();
    g_occluders.clear();
}

std::string CacheSummary()
{
    std::lock_guard<std::mutex> lock(g_cacheMu);
    int parsed = 0, totalPrims = 0;
    for (const auto& kv : g_assets) {
        if (kv.second.state == 1)
            ++parsed;
        totalPrims += static_cast<int>(kv.second.primitives.size());
    }
    char buf[160]{};
    std::snprintf(buf, sizeof(buf), "%zu assets, %d with geometry, %d primitives",
        g_assets.size(), parsed, totalPrims);
    return std::string(buf);
}

void StopJobs()
{
    g_stopRequested.store(true, std::memory_order_release);
    g_probeJob.stop();
    g_probeRunning.store(false, std::memory_order_release);
}

} // namespace CollisionRead
