#include "CollisionBvh.h"

#include "CollisionRead.h"
#include "../Core/CollisionTess.hpp"
#include "../Core/EntityDiagnostics.hpp"
#include "../Interface/Utils/UpgradeOptions.hpp"

// Embree 4 static. The define is what selects the static API declarations; the
// generated rtcore_config.h sets it too when the library was built static, and
// repeating it identically here is harmless and makes the intent explicit.
#ifndef EMBREE_STATIC_LIB
#define EMBREE_STATIC_LIB
#endif
#include <embree4/rtcore.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace CollisionBvh {

namespace {

// Min, median and max of each full-extent axis across the committed occluders.
OccluderSizeSummary SummariseOccluderExtents(const std::vector<std::array<double, 3>>& extents)
{
    OccluderSizeSummary s;
    if (extents.empty())
        return s;
    s.count = static_cast<int>(extents.size());
    for (int axis = 0; axis < 3; ++axis) {
        std::vector<double> column;
        column.reserve(extents.size());
        for (const auto& e : extents)
            column.push_back(e[axis]);
        std::sort(column.begin(), column.end());
        const size_t n = column.size();
        s.min[axis] = column.front();
        s.max[axis] = column.back();
        s.median[axis] = (n % 2 == 1)
            ? column[n / 2]
            : (column[n / 2 - 1] + column[n / 2]) * 0.5;
    }
    return s;
}

/** One published scene: the Embree objects a query needs, and their lifetime. */
struct Scene
{
    RTCScene scene = nullptr;
    RTCGeometry geometry = nullptr;
    RTCBuffer vertexBuffer = nullptr;
    RTCBuffer indexBuffer = nullptr;
    // Comparison-only sub-scenes (see SetOcclusionComparison); null when off or
    // when that source has no triangles. They share the buffers above.
    RTCScene meshScene = nullptr;
    RTCGeometry meshGeometry = nullptr;
    RTCScene boxScene = nullptr;
    RTCGeometry boxGeometry = nullptr;
    int triangles = 0;
    int vertices = 0;
    // Where the decoded collision soup ends and the coarse occluder boxes begin
    // (see SceneTriangles in the header): one index, exact provenance.
    int decodedTriangles = 0;
    uint64_t generation = 0;

    ~Scene()
    {
        // Release order matters: geometry before the buffers it points at.
        if (meshGeometry) rtcReleaseGeometry(meshGeometry);
        if (boxGeometry) rtcReleaseGeometry(boxGeometry);
        if (geometry) rtcReleaseGeometry(geometry);
        if (vertexBuffer) rtcReleaseBuffer(vertexBuffer);
        if (indexBuffer) rtcReleaseBuffer(indexBuffer);
        if (meshScene) rtcReleaseScene(meshScene);
        if (boxScene) rtcReleaseScene(boxScene);
        if (scene) rtcReleaseScene(scene);
    }
};

std::mutex g_deviceMu;
RTCDevice g_device = nullptr;

std::mutex g_sceneMu;
std::shared_ptr<Scene> g_scene;
uint64_t g_generation = 0;

std::atomic<uint64_t> g_occluded{ 0 };
std::atomic<uint64_t> g_clear{ 0 };
std::atomic<uint64_t> g_tooClose{ 0 };
std::atomic<uint64_t> g_noScene{ 0 };

std::atomic<bool> g_compareEnabled{ false };
std::atomic<uint64_t> g_cmpBoth{ 0 };
std::atomic<uint64_t> g_cmpBoxOnly{ 0 };
std::atomic<uint64_t> g_cmpMeshOnly{ 0 };
std::atomic<uint64_t> g_cmpNeither{ 0 };
std::atomic<uint64_t> g_cmpMismatch{ 0 };

uint64_t NowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

double ElapsedMs(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

/** Created once, kept for the process: device creation is not per level. */
RTCDevice Device()
{
    std::lock_guard<std::mutex> lock(g_deviceMu);
    if (!g_device) {
        g_device = rtcNewDevice(nullptr);
        if (!g_device) {
            EntityDiagnostics::LogError("CollisionBvh",
                "rtcNewDevice returned null (static Embree device unavailable)");
        }
    }
    return g_device;
}

std::shared_ptr<Scene> Snapshot()
{
    std::lock_guard<std::mutex> lock(g_sceneMu);
    return g_scene;
}

/** Any-hit on a sub-scene; an absent sub-scene has no triangles, so it cannot block. */
bool OccludedIn(RTCScene s, RTCRay ray)
{
    if (!s)
        return false;
    RTCOccludedArguments args;
    rtcInitOccludedArguments(&args);
    rtcOccluded1(s, &ray, &args);
    return !(ray.tfar >= 0.0f);
}

/**
 * A sub-scene over triangles [first, first + count) of an already-uploaded scene's
 * buffers. Comparison-only: it re-uses the vertex and index data, so the cost is
 * the BVH alone.
 */
RTCScene BuildSubScene(RTCDevice device, const Scene& owner, uint32_t first,
    uint32_t count, RTCGeometry& geometryOut)
{
    geometryOut = nullptr;
    RTCScene s = rtcNewScene(device);
    if (!s)
        return nullptr;
    rtcSetSceneBuildQuality(s, RTC_BUILD_QUALITY_MEDIUM);
    RTCGeometry g = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
    if (!g) {
        rtcReleaseScene(s);
        return nullptr;
    }
    rtcSetGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
        owner.vertexBuffer, 0, 3 * sizeof(float), owner.vertices);
    rtcSetGeometryBuffer(g, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
        owner.indexBuffer, static_cast<size_t>(first) * 3 * sizeof(uint32_t),
        3 * sizeof(uint32_t), count);
    rtcCommitGeometry(g);
    rtcAttachGeometry(s, g);
    rtcCommitScene(s);
    geometryOut = g;
    return s;
}

/** Build a scene from a triangle soup the caller already placed in world space. */
std::shared_ptr<Scene> BuildScene(const CollisionTess::Mesh& mesh, std::string& error,
    int decodedTriangles)
{
    error.clear();
    if (mesh.TriangleCount() == 0) {
        error = "no triangles";
        return nullptr;
    }
    RTCDevice device = Device();
    if (!device) {
        error = "no Embree device";
        return nullptr;
    }

    auto scene = std::make_shared<Scene>();
    scene->scene = rtcNewScene(device);
    if (!scene->scene) {
        error = "rtcNewScene failed";
        return nullptr;
    }
    rtcSetSceneBuildQuality(scene->scene, RTC_BUILD_QUALITY_MEDIUM);

    scene->geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
    if (!scene->geometry) {
        error = "rtcNewGeometry failed";
        return nullptr;
    }

    const size_t vertexBytes = mesh.vertices.size() * sizeof(float);
    const size_t indexBytes = mesh.indices.size() * sizeof(uint32_t);
    scene->vertexBuffer = rtcNewBuffer(device, vertexBytes);
    scene->indexBuffer = rtcNewBuffer(device, indexBytes);
    if (!scene->vertexBuffer || !scene->indexBuffer) {
        error = "rtcNewBuffer failed";
        return nullptr;
    }

    // rtcNewBuffer hands back writable memory: copy the mesh straight in.
    std::memcpy(rtcGetBufferData(scene->vertexBuffer), mesh.vertices.data(), vertexBytes);
    std::memcpy(rtcGetBufferData(scene->indexBuffer), mesh.indices.data(), indexBytes);

    rtcSetGeometryBuffer(scene->geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
        scene->vertexBuffer, 0, 3 * sizeof(float), mesh.vertices.size() / 3);
    rtcSetGeometryBuffer(scene->geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
        scene->indexBuffer, 0, 3 * sizeof(uint32_t), mesh.indices.size() / 3);

    rtcCommitGeometry(scene->geometry);
    if (rtcAttachGeometry(scene->scene, scene->geometry) == RTC_INVALID_GEOMETRY_ID) {
        error = "rtcAttachGeometry failed";
        return nullptr;
    }
    rtcCommitScene(scene->scene);
    scene->triangles = static_cast<int>(mesh.TriangleCount());
    scene->vertices = static_cast<int>(mesh.vertices.size() / 3);
    scene->decodedTriangles = (std::max)(0, (std::min)(decodedTriangles, scene->triangles));

    if (g_compareEnabled.load(std::memory_order_relaxed)) {
        const uint32_t decoded = static_cast<uint32_t>(scene->decodedTriangles);
        const uint32_t total = static_cast<uint32_t>(scene->triangles);
        if (decoded > 0)
            scene->meshScene = BuildSubScene(device, *scene, 0, decoded, scene->meshGeometry);
        if (total > decoded)
            scene->boxScene = BuildSubScene(device, *scene, decoded, total - decoded, scene->boxGeometry);
    }
    return scene;
}

void Publish(std::shared_ptr<Scene> scene)
{
    std::lock_guard<std::mutex> lock(g_sceneMu);
    scene->generation = ++g_generation;
    g_scene = std::move(scene);
}

/**
 * Shared ray setup, so the two query paths cannot disagree about the segment.
 * tfar is always initialised to the full span here and only ever shrunk by the
 * caller: passing a 0 through from the caller (which is what made rtcIntersect1
 * find nothing while rtcOccluded1 worked) is not expressible any more.
 */
bool MakeRay(const Vector3& eye, const Vector3& target, double tnearCm,
    RTCRay& ray, double& spanCm)
{
    const Vector3 dir(target.x - eye.x, target.y - eye.y, target.z - eye.z);
    spanCm = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(spanCm > 0.0) || !std::isfinite(spanCm))
        return false;
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
        return false;

    ray.org_x = static_cast<float>(eye.x);
    ray.org_y = static_cast<float>(eye.y);
    ray.org_z = static_cast<float>(eye.z);
    ray.tnear = static_cast<float>(tnearCm);
    // Unit direction with distances carried in tnear/tfar, which is what Embree
    // expects: a ray parameter in the same units as the world.
    ray.dir_x = static_cast<float>(dir.x / spanCm);
    ray.dir_y = static_cast<float>(dir.y / spanCm);
    ray.dir_z = static_cast<float>(dir.z / spanCm);
    ray.time = 0.0f;
    ray.tfar = static_cast<float>(spanCm);
    ray.mask = 0xFFFFFFFFu;
    ray.id = 0;
    ray.flags = 0;
    return true;
}

} // namespace

bool Ready()
{
    return Snapshot() != nullptr;
}

SceneTriangles AcquireSceneTriangles()
{
    SceneTriangles out;
    // Held for the whole call and handed back in `lifetime`: the buffers belong
    // to the scene, so a rebuild publishing a new one must not free this while a
    // frame is still reading it.
    std::shared_ptr<Scene> scene = Snapshot();
    if (!scene)
        return out;

    out.generation = scene->generation;
    out.triangles = scene->triangles;
    out.vertices = scene->vertices;
    out.decodedTriangles = scene->decodedTriangles;
    // The buffers were filled with rtcNewBuffer + memcpy at build time and the
    // scene sets no RTC_SCENE_FLAG_COMPACT, so the committed data is still here
    // and is what the BVH references. Nothing writes them after the commit.
    out.vertexData = static_cast<const float*>(rtcGetBufferData(scene->vertexBuffer));
    out.indexData = static_cast<const uint32_t*>(rtcGetBufferData(scene->indexBuffer));
    out.readable = out.vertexData && out.indexData && out.triangles > 0 && out.vertices > 0;
    out.lifetime = scene;
    return out;
}

BuildStats RebuildFromCache()
{
    BuildStats stats;
    const auto start = std::chrono::steady_clock::now();

    // Copy the cache out under its lock, then tessellate outside it: the reader's
    // lock must never be held for seconds of geometry work.
    struct AssetView
    {
        uintptr_t asset = 0;
        std::vector<CollisionGeom::Primitive> primitives;
        std::vector<CollisionRead::AssetInstance> instances;
    };
    std::vector<AssetView> snapshot;
    CollisionRead::ForEachParsedAsset(
        [&snapshot](uintptr_t asset, const std::vector<CollisionGeom::Primitive>& primitives,
            const std::vector<CollisionRead::AssetInstance>& instances) {
            snapshot.push_back(AssetView{ asset, primitives, instances });
        });

    // Coarse occluders come from the same walk but a different list: the placed
    // static meshes whose collision did NOT decode. They are copied out first so
    // the reader's lock is never held across the tessellation below. Gated on the
    // config flag: a bounds box is the mesh's full extent, so a target nearer than
    // a building can sit inside the building's own box and read as blocked. Off by
    // default now that brushes supply the level's real walls (UpgradeOptions).
    std::vector<CollisionRead::OccluderBox> occluders;
    if (UpgradeOptions::collisionBvhBoxOccluders) {
        CollisionRead::ForEachOccluderBox([&occluders](const CollisionRead::OccluderBox& box) {
            occluders.push_back(box);
        });
    }

    // Neither list is required on its own: a level whose meshes are all
    // complex-collision has an empty snapshot and every occluder, while a prop-only
    // cache is the reverse. Bailing on an empty snapshot (as this did) meant the
    // one case that most needs a scene -- a level that decoded nothing -- was the
    // one case that produced none.
    if (snapshot.empty() && occluders.empty()) {
        stats.note = "collision cache is empty - run the collision probe first";
        stats.totalMs = ElapsedMs(start);
        return stats;
    }

    const auto tessStart = std::chrono::steady_clock::now();
    CollisionTess::Mesh mesh;
    CollisionTess::Counts counts;
    for (const AssetView& view : snapshot) {
        if (view.instances.empty())
            continue;
        bool contributed = false;
        for (const CollisionRead::AssetInstance& inst : view.instances) {
            if (!inst.component)
                continue;
            // Counted only when it actually placed triangles, so "instances"
            // cannot overstate how much of the level is really in the BVH.
            if (CollisionTess::AppendAsset(view.primitives, inst.transform, mesh, counts) > 0) {
                ++stats.instances;
                contributed = true;
            }
        }
        if (contributed)
            ++stats.assets;
    }

    // One box per coarse occluder, kept in the mesh's own frame and placed through
    // its component transform, so the box is ORIENTED with the mesh. A world AABB
    // (what this used to be) inflates a rotated mesh on every diagonal and blocks
    // the air beside the building. Provenance boundary for the debug draw: every
    // triangle above this index is decoded collision, every one below it belongs
    // to a coarse occluder box.
    const int decodedTriangles = static_cast<int>(mesh.TriangleCount());

    int occluderBoxes = 0;
    std::vector<std::array<double, 3>> occluderExtents;
    {
        const double maxHalfCm = static_cast<double>(UpgradeOptions::collisionBvhBoxMaxHalfCm);
        const double maxCornerCm = static_cast<double>(UpgradeOptions::collisionBvhBoxMaxCornerCm);
        const double minWorldHeightCm = static_cast<double>(UpgradeOptions::collisionBvhBoxMinWorldHeightCm);
        const double verticalThinFactor = static_cast<double>(UpgradeOptions::collisionBvhBoxVerticalThin);
        std::vector<CollisionGeom::Primitive> one(1);
        for (const CollisionRead::OccluderBox& box : occluders) {
            Vector3 half = box.halfExtents;
            // A degenerate box would add triangles with no volume: skip it, and
            // count it, rather than committing geometry that can never block.
            if (!(half.x > 0.5) || !(half.y > 0.5) || !(half.z > 0.5)) {
                ++stats.boxOccludersSkipped;
                continue;
            }
            // Buildings-only filters, ported from the reference reader. A bounds
            // box for a wall, floor slab, pipe or small prop is thin on some axis
            // and blocks air a target can stand in, so it is not cover:
            //  - reject an axis longer than maxHalfCm (a whole building, not a wall);
            //  - reject a scaled corner longer than maxCornerCm (a huge diagonal);
            //  - reject a box whose WORLD vertical extent is under minWorldHeightCm
            //    (a floor, or a wall lying flat);
            //  - otherwise thin the most-vertical local axis by verticalThinFactor,
            //    so a slab that only just clears the height test hugs the surface
            //    instead of standing as a full-height wall.
            // A cm threshold of 0 disables that test; a thin factor of 0 (or >= 1)
            // disables the thinning.
            const FQuat& rot = box.transform.Rotation;
            const Vector3 scaledHalf(std::abs(box.transform.Scale3D.x) * half.x,
                std::abs(box.transform.Scale3D.y) * half.y,
                std::abs(box.transform.Scale3D.z) * half.z);
            if (maxHalfCm > 0.0
                && (std::max)(half.x, (std::max)(half.y, half.z)) > maxHalfCm) {
                ++stats.boxOccludersSkipped;
                continue;
            }
            const double corner = std::sqrt(scaledHalf.x * scaledHalf.x
                + scaledHalf.y * scaledHalf.y + scaledHalf.z * scaledHalf.z);
            if (maxCornerCm > 0.0 && corner > maxCornerCm) {
                ++stats.boxOccludersSkipped;
                continue;
            }
            // Each local axis' contribution to world space and to world Z, from the
            // rotation alone: the world AABB half extents are the sum of the
            // absolute axis contributions, and |z| is the vertical footprint.
            const Vector3 ax = rot.RotateVector(Vector3(scaledHalf.x, 0.0, 0.0));
            const Vector3 ay = rot.RotateVector(Vector3(0.0, scaledHalf.y, 0.0));
            const Vector3 az = rot.RotateVector(Vector3(0.0, 0.0, scaledHalf.z));
            const double worldHalfZ = std::abs(ax.z) + std::abs(ay.z) + std::abs(az.z);
            if (minWorldHeightCm > 0.0 && worldHalfZ * 2.0 < minWorldHeightCm) {
                ++stats.boxOccludersSkipped;
                continue;
            }
            if (verticalThinFactor > 0.0 && verticalThinFactor < 1.0) {
                const double vx = std::abs(ax.z);
                const double vy = std::abs(ay.z);
                const double vz = std::abs(az.z);
                if (vx >= vy && vx >= vz && vx > 1e-9) half.x *= verticalThinFactor;
                else if (vy >= vx && vy >= vz && vy > 1e-9) half.y *= verticalThinFactor;
                else if (vz > 1e-9) half.z *= verticalThinFactor;
            }
            CollisionGeom::Primitive& p = one[0];
            p = CollisionGeom::Primitive{};
            p.shape = CollisionGeom::Shape::Box;
            p.halfExtents = half;
            p.center = box.center;
            p.rotation = FQuat{ 0.0, 0.0, 0.0, 1.0 };
            if (CollisionTess::AppendAsset(one, box.transform, mesh, counts) > 0) {
                ++occluderBoxes;
                // Recorded for the size histogram as the box's world AABB size.
                const double worldHalfX = std::abs(ax.x) + std::abs(ay.x) + std::abs(az.x);
                const double worldHalfY = std::abs(ax.y) + std::abs(ay.y) + std::abs(az.y);
                occluderExtents.push_back({ worldHalfX * 2.0, worldHalfY * 2.0, worldHalfZ * 2.0 });
            }
        }
    }

    stats.tessMs = ElapsedMs(tessStart);
    // counts.boxes includes the occluder boxes just appended (they go through the
    // same tessellator), so the decoded-box count is the difference.
    stats.boxes = counts.boxes - occluderBoxes;
    stats.boxOccluders = occluderBoxes;
    stats.occluderSize = SummariseOccluderExtents(occluderExtents);
    stats.spheres = counts.spheres;
    stats.capsules = counts.capsules;
    stats.convexHulls = counts.convexHulls;
    stats.convexAsBox = counts.convexAsBox;
    stats.convexNoIndex = counts.convexNoIndex;
    stats.convexTooLarge = counts.convexTooLarge;
    stats.convexNoVerts = counts.convexNoVerts;
    stats.convexReadFailed = counts.convexReadFailed;
    stats.skipped = counts.skipped;
    stats.triangles = static_cast<int>(mesh.TriangleCount());
    stats.decodedTriangles = decodedTriangles;
    stats.occluderTriangles = stats.triangles - decodedTriangles;

    if (stats.triangles == 0) {
        stats.note = snapshot.empty()
            ? "no decoded shapes and no usable bounds occluders - nothing to query against"
            : "cache had geometry but no usable instance transform";
        stats.totalMs = ElapsedMs(start);
        return stats;
    }

    std::string error;
    const auto commitStart = std::chrono::steady_clock::now();
    std::shared_ptr<Scene> built = BuildScene(mesh, error, decodedTriangles);
    stats.commitMs = ElapsedMs(commitStart);
    if (!built) {
        stats.note = error.empty() ? "scene build failed" : error;
        stats.totalMs = ElapsedMs(start);
        return stats;
    }

    Publish(std::move(built));
    stats.built = true;
    {
        std::lock_guard<std::mutex> lock(g_sceneMu);
        stats.generation = g_generation;
    }
    stats.totalMs = ElapsedMs(start);

    std::ostringstream f;
    f << "{\"location\":\"CollisionBvh.cpp\",\"message\":\"collision_bvh\","
      << "\"timestamp\":" << EntityDiagnostics::NowMs()
      << ",\"assets\":" << stats.assets
      << ",\"instances\":" << stats.instances
      << ",\"triangles\":" << stats.triangles
      << ",\"decodedTriangles\":" << stats.decodedTriangles
      << ",\"occluderTriangles\":" << stats.occluderTriangles
      << ",\"boxOccluders\":" << stats.boxOccluders
      << ",\"boxOccludersSkipped\":" << stats.boxOccludersSkipped
      << ",\"boxes\":" << stats.boxes
      << ",\"spheres\":" << stats.spheres
      << ",\"capsules\":" << stats.capsules
      << ",\"convexHulls\":" << stats.convexHulls
      << ",\"convexAsBox\":" << stats.convexAsBox
      << ",\"convexNoIndex\":" << stats.convexNoIndex
      << ",\"convexTooLarge\":" << stats.convexTooLarge
      << ",\"convexNoVerts\":" << stats.convexNoVerts
      << ",\"convexReadFailed\":" << stats.convexReadFailed
      << ",\"skipped\":" << stats.skipped
      << ",\"tessMs\":" << stats.tessMs
      << ",\"commitMs\":" << stats.commitMs
      << ",\"totalMs\":" << stats.totalMs
      << ",\"generation\":" << stats.generation
      << "}\n";
    EntityDiagnostics::LogRaw(f.str());
    return stats;
}

void Clear()
{
    std::lock_guard<std::mutex> lock(g_sceneMu);
    g_scene.reset();
}

bool BlockedForConsumer(const Vector3& eye, const Vector3& target, double clearanceCm)
{
    // No scene, no opinion. Checked before the query so the counters this path
    // shares with the debug overlay cannot even be touched by a fail-open miss.
    if (!Ready())
        return false;

    const QueryStats before = Stats();
    const bool blocked = Occluded(eye, target, 5.0, clearanceCm);
    const QueryStats after = Stats();

    // A query that was not judged is not evidence of cover: noScene means the
    // scene vanished between the check and the query, tooClose means the segment
    // is shorter than the biases and the target's own body is the likely blocker.
    if (after.noScene != before.noScene || after.tooClose != before.tooClose)
        return false;
    return blocked;
}

void SetOcclusionComparison(bool on)
{
    g_cmpBoth = 0;
    g_cmpBoxOnly = 0;
    g_cmpMeshOnly = 0;
    g_cmpNeither = 0;
    g_cmpMismatch = 0;
    g_compareEnabled.store(on, std::memory_order_relaxed);
}

OcclusionCompare CompareStats()
{
    OcclusionCompare c;
    c.bothBlock = g_cmpBoth.load(std::memory_order_relaxed);
    c.boxOnly = g_cmpBoxOnly.load(std::memory_order_relaxed);
    c.meshOnly = g_cmpMeshOnly.load(std::memory_order_relaxed);
    c.neither = g_cmpNeither.load(std::memory_order_relaxed);
    c.mismatch = g_cmpMismatch.load(std::memory_order_relaxed);
    return c;
}

QueryStats Stats()
{
    QueryStats s;
    s.occluded = g_occluded.load(std::memory_order_relaxed);
    s.clear = g_clear.load(std::memory_order_relaxed);
    s.tooClose = g_tooClose.load(std::memory_order_relaxed);
    s.noScene = g_noScene.load(std::memory_order_relaxed);
    return s;
}

bool Occluded(const Vector3& eye, const Vector3& target, double tnearCm, double clearanceCm)
{
    std::shared_ptr<Scene> scene = Snapshot();
    if (!scene || !scene->scene) {
        g_noScene.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    RTCRay ray{};
    double span = 0.0;
    if (!MakeRay(eye, target, tnearCm, ray, span)) {
        g_clear.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const double tfar = span - clearanceCm;
    if (tfar <= tnearCm) {
        // Shorter than the two biases together: the answer would be a coin flip
        // on the target's own collision, so it is reported as not judgeable.
        g_tooClose.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ray.tfar = static_cast<float>(tfar);

    const RTCRay original = ray;  // rtcOccluded1 overwrites tfar on a hit
    RTCOccludedArguments args;
    rtcInitOccludedArguments(&args);
    rtcOccluded1(scene->scene, &ray, &args);

    // rtcOccluded1 signals a hit by setting tfar to -inf.
    const bool hit = !(ray.tfar >= 0.0f);

    // Only judge against sub-scenes this scene was built with: the toggle takes
    // effect on the next build, so a flip since then has nothing to compare against.
    if (g_compareEnabled.load(std::memory_order_relaxed) && (scene->boxScene || scene->meshScene)) {
        const bool boxHit = OccludedIn(scene->boxScene, original);
        const bool meshHit = OccludedIn(scene->meshScene, original);
        if (hit != (boxHit || meshHit))
            g_cmpMismatch.fetch_add(1, std::memory_order_relaxed);
        if (boxHit && meshHit)
            g_cmpBoth.fetch_add(1, std::memory_order_relaxed);
        else if (boxHit)
            g_cmpBoxOnly.fetch_add(1, std::memory_order_relaxed);
        else if (meshHit)
            g_cmpMeshOnly.fetch_add(1, std::memory_order_relaxed);
        else
            g_cmpNeither.fetch_add(1, std::memory_order_relaxed);
    }
    if (hit)
        g_occluded.fetch_add(1, std::memory_order_relaxed);
    else
        g_clear.fetch_add(1, std::memory_order_relaxed);
    return hit;
}

Hit ClosestHit(const Vector3& eye, const Vector3& target, double tnearCm)
{
    Hit out;
    std::shared_ptr<Scene> scene = Snapshot();
    if (!scene || !scene->scene) {
        g_noScene.fetch_add(1, std::memory_order_relaxed);
        return out;
    }

    RTCRayHit rayHit{};
    double span = 0.0;
    if (!MakeRay(eye, target, tnearCm, rayHit.ray, span))
        return out;
    // Embree expects the caller to mark "no hit yet" before the call; a zeroed
    // geomID is a valid geometry id, not the sentinel.
    rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

    RTCRayQueryContext context;
    rtcInitRayQueryContext(&context);
    RTCIntersectArguments args;
    rtcInitIntersectArguments(&args);
    args.context = &context;

    rtcIntersect1(scene->scene, &rayHit, &args);
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID)
        return out;

    out.hit = true;
    out.distanceCm = static_cast<double>(rayHit.ray.tfar);
    out.fraction = span > 0.0 ? out.distanceCm / span : 0.0;
    return out;
}

SelfTestResult RunSelfTest()
{
    SelfTestResult r;
    const auto start = std::chrono::steady_clock::now();

    // A 200 cm cube centred at x = 300, so it occupies x in [200, 400].
    CollisionGeom::Primitive wall;
    wall.shape = CollisionGeom::Shape::Box;
    wall.center = Vector3(300.0, 0.0, 0.0);
    wall.halfExtents = Vector3(100.0, 100.0, 100.0);

    CollisionTess::Mesh mesh;
    CollisionTess::Counts counts;
    CollisionTess::AppendPrimitive(wall, CollisionGeom::IdentityTransform(), mesh, counts);

    std::string error;
    std::shared_ptr<Scene> scene = BuildScene(mesh, error,
        static_cast<int>(mesh.TriangleCount()));
    r.buildMs = ElapsedMs(start);
    if (!scene) {
        r.detail = "scene build failed: " + error;
        return r;
    }
    Publish(scene);
    r.ran = true;

    const Vector3 eye(0.0, 0.0, 0.0);
    const Vector3 throughWall(600.0, 0.0, 0.0);
    const Vector3 aroundWall(600.0, 600.0, 0.0);
    const Vector3 tooClose(30.0, 0.0, 0.0);

    std::ostringstream detail;

    // 1. The box is 12 triangles, which is what "exact" means for a box.
    const bool boxTris = (mesh.TriangleCount() == 12);
    detail << "box_is_12_tris=" << (boxTris ? "pass" : "fail")
           << "(" << mesh.TriangleCount() << ") ";

    // 2. A ray through the wall must be blocked.
    const bool blocked = Occluded(eye, throughWall);
    detail << "through_wall_occluded=" << (blocked ? "pass" : "fail") << " ";

    // 3. A ray that goes around it must not be.
    const bool clear = !Occluded(eye, aroundWall);
    detail << "around_wall_clear=" << (clear ? "pass" : "fail") << " ";

    // 4. Closest hit lands on the near face: x = 200, so 200 cm along the ray.
    const Hit hit = ClosestHit(eye, throughWall);
    const bool hitOk = hit.hit && std::abs(hit.distanceCm - 200.0) <= 1.0;
    detail << "closest_hit_200cm=" << (hitOk ? "pass" : "fail")
           << "(" << hit.distanceCm << ") ";

    // 5. A span shorter than the clearance is not judgeable, and must not read
    //    as blocked just because the target sits inside geometry.
    const bool tooCloseOk = !Occluded(eye, tooClose);
    detail << "too_close_not_judged=" << (tooCloseOk ? "pass" : "fail") << " ";

    // 6. The scene's OWN triangle buffers are readable through the accessor the
    //    debug draw uses, and say what the scene actually holds: 12 triangles, 36
    //    vertices (the soup is unindexed), the whole of it decoded rather than an
    //    occluder box, and the cube's committed x range. This is the difference
    //    between "the overlay drew nothing" and "the scene held nothing", which
    //    no counter can tell apart from the outside.
    const SceneTriangles tris = AcquireSceneTriangles();
    bool trisOk = tris.readable && tris.triangles == 12 && tris.vertices == 36
        && tris.decodedTriangles == 12 && tris.generation != 0;
    if (trisOk) {
        double minX = 1e30;
        double maxX = -1e30;
        for (int t = 0; t < tris.triangles; ++t) {
            for (int e = 0; e < 3; ++e) {
                const uint32_t idx = tris.indexData[3 * t + e];
                if (idx >= static_cast<uint32_t>(tris.vertices)) {
                    trisOk = false;
                    break;
                }
                const double x = tris.vertexData[3 * idx + 0];
                minX = (std::min)(minX, x);
                maxX = (std::max)(maxX, x);
            }
        }
        trisOk = trisOk && minX > 199.0 && minX < 201.0 && maxX > 399.0 && maxX < 401.0;
    }
    detail << "scene_triangles_readable=" << (trisOk ? "pass" : "fail")
           << "(" << tris.triangles << ") ";

    // 7. An emptied scene answers clear instead of using a stale BVH.
    Clear();
    const bool emptyOk = !Occluded(eye, throughWall) && !Ready();
    detail << "cleared_scene_clear=" << (emptyOk ? "pass" : "fail") << " ";

    // 8. A hull rebuilt from its point cloud (cooked builds keep no index) must be
    //    the cube's 12 faces, and the interior point must not be a hull vertex.
    std::vector<Vector3> cloud;
    for (int i = 0; i < 8; ++i)
        cloud.push_back(Vector3((i & 1) ? 100.0 : -100.0, (i & 2) ? 100.0 : -100.0,
            (i & 4) ? 100.0 : -100.0));
    cloud.push_back(Vector3(0.0, 0.0, 0.0));
    const std::vector<uint32_t> hullTris = CollisionGeom::BuildHullTriangles(cloud);
    bool hullOk = hullTris.size() == 36;
    for (const uint32_t vi : hullTris) {
        if (vi == 8)
            hullOk = false;
    }
    detail << "hull_cloud_is_cube=" << (hullOk ? "pass" : "fail")
           << "(" << hullTris.size() / 3 << " tris) ";

    const bool cases[8] = { boxTris, blocked, clear, hitOk, tooCloseOk, trisOk, emptyOk, hullOk };
    r.cases = 8;
    for (const bool ok : cases) {
        if (!ok)
            ++r.failed;
    }
    r.passed = (r.failed == 0);
    r.detail = detail.str();

    std::ostringstream f;
    f << "{\"location\":\"CollisionBvh.cpp\",\"message\":\"collision_bvh_selftest\","
      << "\"timestamp\":" << EntityDiagnostics::NowMs()
      << ",\"passed\":" << (r.passed ? "true" : "false")
      << ",\"cases\":" << r.cases
      << ",\"failed\":" << r.failed
      << ",\"triangles\":" << mesh.TriangleCount()
      << ",\"buildMs\":" << r.buildMs
      << ",\"detail\":\"" << EntityDiagnostics::Sanitize(r.detail, 600) << "\""
      << "}\n";
    EntityDiagnostics::LogRaw(f.str());
    return r;
}

std::string Summary()
{
    std::shared_ptr<Scene> scene = Snapshot();
    const QueryStats q = Stats();
    char buf[256]{};
    if (!scene) {
        std::snprintf(buf, sizeof(buf), "no BVH (queries answered clear: %llu)",
            static_cast<unsigned long long>(q.noScene));
        return std::string(buf);
    }
    std::snprintf(buf, sizeof(buf),
        "gen %llu, %d tris (decoded %d, occluder %d) | occluded %llu, clear %llu, too-close %llu",
        static_cast<unsigned long long>(scene->generation), scene->triangles,
        scene->decodedTriangles, scene->triangles - scene->decodedTriangles,
        static_cast<unsigned long long>(q.occluded),
        static_cast<unsigned long long>(q.clear),
        static_cast<unsigned long long>(q.tooClose));
    return std::string(buf);
}

} // namespace CollisionBvh
