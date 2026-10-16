#include "CollisionDebug.h"

#include "CollisionRead.h"
#include "CollisionBvh.h"
#include "../Core/CollisionGeom.hpp"
#include "../Core/EntityDiagnostics.hpp"
#include "../Interface/Utils/UpgradeOptions.hpp"
#include "../ThirdParty/ImGui/imgui.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace CollisionDebug {

namespace {

Stats g_frame;
bool g_worldRayBudgetHit = false;
bool g_entityRayBudgetHit = false;

// Muted palette: these overlays must never be mistaken for real ESP drawing.
constexpr ImU32 kBoxColor = IM_COL32(90, 200, 255, 130);
constexpr ImU32 kSphereColor = IM_COL32(120, 255, 160, 130);
constexpr ImU32 kCapsuleColor = IM_COL32(255, 210, 110, 130);
constexpr ImU32 kConvexColor = IM_COL32(230, 130, 255, 130);
constexpr ImU32 kClearRay = IM_COL32(70, 240, 120, 190);
// Minimum per-frame budget for player/bot rays (the entity lane).
constexpr int kEntityRayFloor = 64;
constexpr ImU32 kBlockedRay = IM_COL32(255, 80, 80, 190);
constexpr ImU32 kTooCloseRay = IM_COL32(170, 170, 170, 160);
// No BVH published is its own colour, not the grey of an unjudgeable span: the
// segment is fine and the ANSWER is missing, which is a different thing to fix.
constexpr ImU32 kNoSceneRay = IM_COL32(110, 150, 230, 170);
constexpr ImU32 kHitMarker = IM_COL32(255, 255, 255, 220);
// A shape that is NOT decoded collision: a character's capsule read from
// CapsuleComponent, or a component's own LocalBounds box. Its own colour, because
// calling a bounds box "the decoded geometry" is the one thing this overlay must
// never do - the crate that is really 12 triangles and the wall that is really an
// AABB have to look different on screen.
constexpr ImU32 kStandInColor = IM_COL32(255, 120, 220, 150);
// Scene triangles. Two flat colours, not a normal-based shade: the question this
// overlay answers is WHICH SOURCE a triangle came from (a real decoded shape or a
// stand-in bounds box), and a shade ramp would blur exactly that.
constexpr ImU32 kSceneDecodedTri = IM_COL32(150, 235, 255, 80);
constexpr ImU32 kSceneBoxTri = IM_COL32(255, 150, 70, 80);
// Thinner than the shape outlines: a scene is thousands of edges and the overlay
// must stay readable underneath the boxes and rays it is explaining.
constexpr float kSceneTriThickness = 0.8f;

constexpr int kCircleSegments = 16;

/**
 * What one traced ray got back.
 *
 * Kept as an enum rather than a colour chosen inline so the two kinds of "no
 * answer" cannot be conflated again: no BVH published means the collision probe
 * has not run and every query answers clear by contract, while a span shorter
 * than the biases is a property of the two points being asked about. They are
 * different failures with different fixes, so they get different counters and
 * different colours.
 */
enum class RayVerdict { NoScene, TooClose, Blocked, Clear };

/** Pure, so the classification is testable without a target or a draw list. */
RayVerdict ClassifyRay(const CollisionBvh::QueryStats& before,
    const CollisionBvh::QueryStats& after, bool blocked)
{
    // No scene first: when the scene is missing the query cannot have been
    // judged at all, and a level with no BVH would otherwise report every ray as
    // "clear" - the answer that looks like a working vischeck.
    if (after.noScene > before.noScene)
        return RayVerdict::NoScene;
    if (after.tooClose > before.tooClose)
        return RayVerdict::TooClose;
    return blocked ? RayVerdict::Blocked : RayVerdict::Clear;
}
constexpr double kPi = 3.14159265358979;

/** Project one world point; false when it left the view (skip, never clamp). */
bool Project(Engine& engine, const Engine::CameraCache& cam, const Vector3& world, ImVec2& out)
{
    Vector3 screen{};
    if (!engine.ProjectWorldLocationToScreen(world, screen, cam))
        return false;
    if (!std::isfinite(screen.x) || !std::isfinite(screen.y))
        return false;
    out = ImVec2(static_cast<float>(screen.x), static_cast<float>(screen.y));
    return true;
}

/** Draw a world-space loop. One unprojectable point drops the whole loop. */
void DrawLoop(ImDrawList* dl, Engine& engine, const Engine::CameraCache& cam,
    const Vector3* points, int count, ImU32 color, float thickness, bool closed)
{
    if (count < 2)
        return;
    std::vector<ImVec2> screen(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (!Project(engine, cam, points[i], screen[static_cast<size_t>(i)])) {
            ++g_frame.projectFailed;
            return;
        }
    }
    const int last = closed ? count : count - 1;
    for (int i = 0; i < last; ++i) {
        const int j = (i + 1) % count;
        dl->AddLine(screen[static_cast<size_t>(i)], screen[static_cast<size_t>(j)], color, thickness);
        ++g_frame.segments;
    }
}

void DrawSegment(ImDrawList* dl, Engine& engine, const Engine::CameraCache& cam,
    const Vector3& a, const Vector3& b, ImU32 color, float thickness)
{
    ImVec2 sa, sb;
    // Same near-plane pull as the start point below, mirrored: a target that
    // cannot project (behind the camera or at its origin) is pulled back toward
    // the eye instead of dropping the whole ray.
    constexpr double kClipCm = 150.0;
    Vector3 pb = b;
    if (!Project(engine, cam, pb, sb)) {
        const double ex = a.x - b.x;
        const double ey = a.y - b.y;
        const double ez = a.z - b.z;
        const double len = std::sqrt(ex * ex + ey * ey + ez * ez);
        if (!(len > kClipCm)) {
            ++g_frame.projectFailed;
            return;
        }
        const double t = kClipCm / len;
        pb = Vector3(b.x + ex * t, b.y + ey * t, b.z + ez * t);
        if (!Project(engine, cam, pb, sb)) {
            ++g_frame.projectFailed;
            return;
        }
    }
    if (!Project(engine, cam, a, sa)) {
        // A ray starts at the camera origin, and that point cannot project (its
        // depth is zero), so every ray used to be dropped here. Start it just past
        // the near plane instead. A segment too short to reach past that point is
        // not drawn.
        constexpr double kNearCm = 150.0;
        const double dx = pb.x - a.x;
        const double dy = pb.y - a.y;
        const double dz = pb.z - a.z;
        const double lenCm = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(lenCm > kNearCm)) {
            ++g_frame.projectFailed;
            return;
        }
        const double t = kNearCm / lenCm;
        const Vector3 start(a.x + dx * t, a.y + dy * t, a.z + dz * t);
        if (!Project(engine, cam, start, sa)) {
            ++g_frame.projectFailed;
            return;
        }
    }
    dl->AddLine(sa, sb, color, thickness);
    ++g_frame.segments;
}

/** Two unit vectors spanning the plane perpendicular to a unit axis. */
void OrthonormalBasis(const Vector3& unitAxis, Vector3& u, Vector3& v)
{
    const Vector3 ref = (std::abs(unitAxis.z) < 0.9) ? Vector3(0.0, 0.0, 1.0) : Vector3(1.0, 0.0, 0.0);
    u = Vector3(unitAxis.y * ref.z - unitAxis.z * ref.y,
        unitAxis.z * ref.x - unitAxis.x * ref.z,
        unitAxis.x * ref.y - unitAxis.y * ref.x);
    const double uLen = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
    u = uLen > 1e-9 ? Vector3(u.x / uLen, u.y / uLen, u.z / uLen) : Vector3(1.0, 0.0, 0.0);
    v = Vector3(unitAxis.y * u.z - unitAxis.z * u.y,
        unitAxis.z * u.x - unitAxis.x * u.z,
        unitAxis.x * u.y - unitAxis.y * u.x);
    // unitAxis is unit and u is unit and perpendicular, so v already is; the
    // guard is for a degenerate axis, not for normalisation of v.
    const double vLen = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (vLen > 1e-9)
        v = Vector3(v.x / vLen, v.y / vLen, v.z / vLen);
}

void DrawCircle(ImDrawList* dl, Engine& engine, const Engine::CameraCache& cam,
    const Vector3& center, const Vector3& unitAxis, double radius, ImU32 color)
{
    Vector3 u, v;
    OrthonormalBasis(unitAxis, u, v);
    std::vector<Vector3> ring;
    ring.reserve(kCircleSegments);
    for (int i = 0; i < kCircleSegments; ++i) {
        const double a = (2.0 * kPi * i) / kCircleSegments;
        const double cx = std::cos(a) * radius;
        const double sy = std::sin(a) * radius;
        ring.push_back(Vector3(center.x + u.x * cx + v.x * sy,
            center.y + u.y * cx + v.y * sy,
            center.z + u.z * cx + v.z * sy));
    }
    DrawLoop(dl, engine, cam, ring.data(), static_cast<int>(ring.size()), color, 1.f, true);
}

Vector3 ElementToWorld(const CollisionGeom::Primitive& p, const FTransform& transform,
    const Vector3& elementLocal)
{
    const Vector3 inComponent = p.Rotated() ? p.rotation.RotateVector(elementLocal) : elementLocal;
    return CollisionGeom::TransformPoint(transform, inComponent);
}

/** Colour of one primitive, stand-ins reported as such rather than as a decode. */
ImU32 PrimitiveColor(const CollisionGeom::Primitive& p, bool standIn)
{
    if (standIn)
        return kStandInColor;
    switch (p.shape) {
    case CollisionGeom::Shape::Sphere: return kSphereColor;
    case CollisionGeom::Shape::Box: return kBoxColor;
    case CollisionGeom::Shape::Sphyl: return kCapsuleColor;
    case CollisionGeom::Shape::Convex: return kConvexColor;
    default: return kBoxColor;
    }
}

/** One primitive's outline: boxes are the exact 12 edges, curved shapes circles. */
void DrawPrimitive(ImDrawList* dl, Engine& engine, const Engine::CameraCache& cam,
    const CollisionGeom::Primitive& p, const FTransform& transform, bool standIn)
{
    const ImU32 color = PrimitiveColor(p, standIn);
    switch (p.shape) {
    case CollisionGeom::Shape::Box:
    case CollisionGeom::Shape::Convex: {
        Vector3 corner[8];
        for (int i = 0; i < 8; ++i) {
            const Vector3 local(p.center.x + ((i & 1) ? p.halfExtents.x : -p.halfExtents.x),
                p.center.y + ((i & 2) ? p.halfExtents.y : -p.halfExtents.y),
                p.center.z + ((i & 4) ? p.halfExtents.z : -p.halfExtents.z));
            corner[i] = ElementToWorld(p, transform, local);
        }
        for (int i = 0; i < 8; ++i) {
            for (const int bit : { 1, 2, 4 }) {
                if (!(i & bit))
                    DrawSegment(dl, engine, cam, corner[i], corner[i | bit], color, 1.f);
            }
        }
        break;
    }
    case CollisionGeom::Shape::Sphere: {
        const Vector3 center = ElementToWorld(p, transform, p.center);
        const Vector3 axes[3] = { Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1) };
        for (const Vector3& axis : axes)
            DrawCircle(dl, engine, cam, center, axis, p.radius, color);
        break;
    }
    case CollisionGeom::Shape::Sphyl: {
        // The element frame's +Z is the capsule axis; the circles sit at the two
        // cap centres, in the plane perpendicular to it.
        const Vector3 top = ElementToWorld(p, transform,
            Vector3(p.center.x, p.center.y, p.center.z + p.halfLength));
        const Vector3 bottom = ElementToWorld(p, transform,
            Vector3(p.center.x, p.center.y, p.center.z - p.halfLength));
        const Vector3 axisWorld = CollisionGeom::TransformPoint(transform, p.rotation.RotateVector(Vector3(0.0, 0.0, 1.0)))
            - CollisionGeom::TransformPoint(transform, Vector3(0.0, 0.0, 0.0));
        const double axisLen = std::sqrt(axisWorld.x * axisWorld.x + axisWorld.y * axisWorld.y
            + axisWorld.z * axisWorld.z);
        const Vector3 unitAxis = axisLen > 1e-9
            ? Vector3(axisWorld.x / axisLen, axisWorld.y / axisLen, axisWorld.z / axisLen)
            : Vector3(0.0, 0.0, 1.0);

        DrawCircle(dl, engine, cam, top, unitAxis, p.radius, color);
        DrawCircle(dl, engine, cam, bottom, unitAxis, p.radius, color);

        Vector3 u, v;
        OrthonormalBasis(unitAxis, u, v);
        for (int i = 0; i < 4; ++i) {
            const double a = (2.0 * kPi * i) / 4.0;
            const double cx = std::cos(a) * p.radius;
            const double sy = std::sin(a) * p.radius;
            const Vector3 offset(u.x * cx + v.x * sy, u.y * cx + v.y * sy, u.z * cx + v.z * sy);
            DrawSegment(dl, engine, cam,
                Vector3(top.x + offset.x, top.y + offset.y, top.z + offset.z),
                Vector3(bottom.x + offset.x, bottom.y + offset.y, bottom.z + offset.z),
                color, 1.f);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

Options CurrentOptions()
{
    Options opt;
    opt.drawShapes = UpgradeOptions::collisionDebugShapes;
    opt.drawRays = UpgradeOptions::collisionDebugRays;
    opt.drawHitMarkers = UpgradeOptions::collisionDebugHitMarkers;
    opt.drawSceneDecoded = UpgradeOptions::collisionDebugSceneDecoded;
    opt.drawSceneBoxes = UpgradeOptions::collisionDebugSceneBoxes;
    opt.maxSceneTrisPerLane = UpgradeOptions::collisionDebugSceneTriCap;
    opt.radiusM = UpgradeOptions::collisionDebugRadiusM;
    opt.maxPrimitives = UpgradeOptions::collisionDebugMaxPrims;
    opt.maxRays = UpgradeOptions::collisionDebugMaxRays;
    return opt;
}

void BeginFrame()
{
    g_frame = Stats{};
    g_worldRayBudgetHit = false;
    g_entityRayBudgetHit = false;
}

Stats LastFrame()
{
    Stats s = g_frame;
    s.raysBudgetExhausted = g_worldRayBudgetHit;
    s.entityRaysBudgetExhausted = g_entityRayBudgetHit;
    return s;
}

int DrawShapeOutlines(ImDrawList* drawList, Engine& engine,
    const Engine::CameraCache& cam, const Vector3& eye, uintptr_t component, uintptr_t actor)
{
    if (!drawList || (!component && !actor))
        return 0;

    const Options opt = CurrentOptions();
    const double radiusMaxCm = static_cast<double>(opt.radiusM) * 100.0;  // m -> cm

    int drawn = 0;
    bool any = false;
    const auto visit = [&](const CollisionGeom::Primitive& p, const FTransform& transform,
        bool standIn) {
        if (drawn >= opt.maxPrimitives) {
            ++g_frame.culled;
            return;
        }
        // Cull on the element centre, which is what carries the shape close
        // enough to matter; a large shape just inside the radius may appear,
        // one far outside it never does.
        const Vector3 world = CollisionGeom::TransformPoint(transform, p.center);
        const double dx = world.x - eye.x;
        const double dy = world.y - eye.y;
        const double dz = world.z - eye.z;
        const double distCm = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(distCm) || distCm > radiusMaxCm) {
            ++g_frame.culled;
            return;
        }
        DrawPrimitive(drawList, engine, cam, p, transform, standIn);
        if (standIn)
            ++g_frame.standIns;
        else
            ++g_frame.decoded;
        ++drawn;
        ++g_frame.primitives;
        any = true;
    };

    // Actor key first. It is the only key that can resolve for a player or a bot:
    // their root component is a capsule or a skeletal mesh, never a static mesh
    // component, so nothing was ever stored under a component for them. The
    // component key stays as the fallback the world-item path has always used,
    // and the last call is the original asset lookup, untouched.
    bool found = false;
    if (actor)
        found = CollisionRead::ForEachPrimitiveAtActor(actor, visit);
    if (!found && component) {
        found = CollisionRead::ForEachPrimitiveAtActor(component, visit);
        if (!found) {
            CollisionRead::ForEachPrimitiveAtComponent(component,
                [&](const CollisionGeom::Primitive& p, const FTransform& transform) {
                    visit(p, transform, /*standIn=*/false);
                });
        }
    }

    if (any)
        ++g_frame.components;
    return drawn;
}

void EnsureSelfTestsRan()
{
    // Called from the overlay loop, NOT from BeginFrame: the paint pass returns
    // early when no live camera resolved, so a check hooked there would silently
    // never run on exactly the sessions that have no target to draw.
    static bool s_ran = false;
    if (s_ran)
        return;
    s_ran = true;
    RunVerdictSelfTest();
}

VerdictSelfTest RunVerdictSelfTest()
{
    VerdictSelfTest r;
    r.ran = true;

    const CollisionBvh::QueryStats base{};          // a judged ray: nothing moves
    CollisionBvh::QueryStats after{};

    // 1. No BVH published, asked of a ray whose answer came back false. Must read
    //    as "no scene", never as clear - that is the shape of a vischeck that is
    //    inert while the panel claims it answers.
    after = base;
    after.noScene = base.noScene + 1;
    const bool noSceneOk = ClassifyRay(base, after, false) == RayVerdict::NoScene;

    // 2. Span too short, also answering false. Must NOT be read as no-scene.
    after = base;
    after.tooClose = base.tooClose + 1;
    const bool tooCloseOk = ClassifyRay(base, after, false) == RayVerdict::TooClose;

    // 3. Judged rays still classify by the boolean.
    const bool blockedOk = ClassifyRay(base, base, true) == RayVerdict::Blocked;
    const bool clearOk = ClassifyRay(base, base, false) == RayVerdict::Clear;

    // 4. Both counters moved - a no-scene query that also reported a short span.
    //    No scene wins: with nothing published there is no judgeable span either.
    after = base;
    after.noScene = base.noScene + 1;
    after.tooClose = base.tooClose + 1;
    const bool bothOk = ClassifyRay(base, after, true) == RayVerdict::NoScene;

    const bool cases[5] = { noSceneOk, tooCloseOk, blockedOk, clearOk, bothOk };
    r.cases = 5;
    for (const bool ok : cases) {
        if (!ok)
            ++r.failed;
    }
    r.passed = (r.failed == 0);

    std::ostringstream detail;
    detail << "no_scene_distinct=" << (noSceneOk ? "pass" : "fail") << " ";
    detail << "too_close_distinct=" << (tooCloseOk ? "pass" : "fail") << " ";
    detail << "blocked=" << (blockedOk ? "pass" : "fail") << " ";
    detail << "clear=" << (clearOk ? "pass" : "fail") << " ";
    detail << "no_scene_wins=" << (bothOk ? "pass" : "fail") << " ";
    r.detail = detail.str();

    std::ostringstream f;
    f << "{\"location\":\"CollisionDebug.cpp\",\"message\":\"collision_ray_verdict_selftest\","
      << "\"timestamp\":" << EntityDiagnostics::NowMs()
      << ",\"passed\":" << (r.passed ? "true" : "false")
      << ",\"cases\":" << r.cases
      << ",\"failed\":" << r.failed
      << ",\"detail\":\"" << EntityDiagnostics::Sanitize(r.detail, 160) << "\""
      << "}\n";
    EntityDiagnostics::LogRaw(f.str());
    return r;
}

int DrawSceneWireframe(ImDrawList* drawList, Engine& engine,
    const Engine::CameraCache& cam, const Vector3& eye)
{
    if (!drawList)
        return 0;

    const Options opt = CurrentOptions();
    if (!opt.drawSceneDecoded && !opt.drawSceneBoxes)
        return 0;

    const CollisionBvh::SceneTriangles scene = CollisionBvh::AcquireSceneTriangles();
    g_frame.sceneGeneration = scene.generation;
    g_frame.sceneTriangles = scene.triangles;
    if (!scene.readable) {
        // Reported, never guessed at: an overlay that silently drew nothing would
        // be indistinguishable from a scene whose geometry is somewhere else.
        g_frame.sceneNoScene = true;
        return 0;
    }

    const double radiusMaxCm = static_cast<double>(opt.radiusM) * 100.0;  // m -> cm
    const int laneCap = (std::max)(0, opt.maxSceneTrisPerLane);

    // The provenance boundary is an index, so each lane is a contiguous range and
    // an off lane costs nothing: no walking of a range that is not being drawn.
    const int decodedEnd = (std::max)(0, (std::min)(scene.decodedTriangles, scene.triangles));
    struct Lane { int begin; int end; bool on; ImU32 color; int drawn; };
    Lane lanes[2] = {
        { 0, decodedEnd, opt.drawSceneDecoded, kSceneDecodedTri, 0 },
        { decodedEnd, scene.triangles, opt.drawSceneBoxes, kSceneBoxTri, 0 },
    };

    int drawn = 0;
    for (Lane& lane : lanes) {
        if (!lane.on)
            continue;
        bool capped = false;
        for (int i = lane.begin; i < lane.end; ++i) {
            if (lane.drawn >= laneCap) {
                capped = true;
                break;
            }
            const uint32_t i0 = scene.indexData[3 * i + 0];
            const uint32_t i1 = scene.indexData[3 * i + 1];
            const uint32_t i2 = scene.indexData[3 * i + 2];
            if (i0 >= static_cast<uint32_t>(scene.vertices)
                || i1 >= static_cast<uint32_t>(scene.vertices)
                || i2 >= static_cast<uint32_t>(scene.vertices))
                continue;  // cannot happen for a mesh we wrote; skipped, not drawn

            const Vector3 corners[3] = {
                Vector3(scene.vertexData[3 * i0 + 0], scene.vertexData[3 * i0 + 1], scene.vertexData[3 * i0 + 2]),
                Vector3(scene.vertexData[3 * i1 + 0], scene.vertexData[3 * i1 + 1], scene.vertexData[3 * i1 + 2]),
                Vector3(scene.vertexData[3 * i2 + 0], scene.vertexData[3 * i2 + 1], scene.vertexData[3 * i2 + 2]),
            };

            const Vector3 centroid((corners[0].x + corners[1].x + corners[2].x) / 3.0,
                (corners[0].y + corners[1].y + corners[2].y) / 3.0,
                (corners[0].z + corners[1].z + corners[2].z) / 3.0);
            const double dx = centroid.x - eye.x;
            const double dy = centroid.y - eye.y;
            const double dz = centroid.z - eye.z;
            const double distCm = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(distCm) || distCm > radiusMaxCm) {
                ++g_frame.sceneCulled;
                continue;
            }

            ImVec2 screen[3];
            if (!Project(engine, cam, corners[0], screen[0])
                || !Project(engine, cam, corners[1], screen[1])
                || !Project(engine, cam, corners[2], screen[2])) {
                ++g_frame.projectFailed;
                continue;
            }
            for (int e = 0; e < 3; ++e) {
                drawList->AddLine(screen[e], screen[(e + 1) % 3], lane.color, kSceneTriThickness);
                ++g_frame.segments;
            }
            ++lane.drawn;
            ++drawn;
        }
        if (capped)
            g_frame.sceneTruncated = true;
    }

    g_frame.sceneDecoded += lanes[0].drawn;
    g_frame.sceneBoxes += lanes[1].drawn;
    return drawn;
}

/** Magenta ring-and-X: a visibility ray that was dropped instead of drawn. */
constexpr ImU32 kDropColor = IM_COL32(255, 0, 255, 255);

/**
 * Screen spot for a dropped ray's marker. A target that projects (even off-screen)
 * is pinned to the screen edge. A target behind the camera is anchored at the point
 * 150 cm along its ray from the eye. If even that has no screen position, the marker
 * goes to the bottom centre, so a drop is never invisible.
 */
ImVec2 DropAnchor(Engine& engine, const Engine::CameraCache& cam, const Vector3& eye,
    const Vector3& target)
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    constexpr float kInset = 12.f;
    ImVec2 p{};
    bool found = Project(engine, cam, target, p);
    if (!found) {
        constexpr double kAnchorCm = 150.0;
        const double dx = target.x - eye.x;
        const double dy = target.y - eye.y;
        const double dz = target.z - eye.z;
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(len) && len > kAnchorCm) {
            const double t = kAnchorCm / len;
            found = Project(engine, cam,
                Vector3(eye.x + dx * t, eye.y + dy * t, eye.z + dz * t), p);
        }
    }
    if (!found)
        return ImVec2(size.x * 0.5f, (std::max)(kInset, size.y - kInset));
    return ImVec2(std::clamp(p.x, kInset, (std::max)(kInset, size.x - kInset)),
        std::clamp(p.y, kInset, (std::max)(kInset, size.y - kInset)));
}

/**
 * The crosshair in screen space: the projection of a point straight ahead of the
 * camera. The camera origin itself never projects (its view depth is 0), so a
 * world-space segment from the eye depended on a near-plane pull that dropped the
 * entire ray whenever the pull failed. A visibility ray is meant to run crosshair
 * to target, and both of those are screen positions, so the line is built in
 * screen space where neither endpoint can be behind the camera.
 */
bool CrosshairScreen(Engine& engine, const Engine::CameraCache& cam, ImVec2& out)
{
    Vector3 axisX{};
    Vector3 axisY{};
    Vector3 axisZ{};
    EngineProjection::RotationGetAxes(cam.Rotation, axisX, axisY, axisZ);
    const Vector3 ahead(cam.Location.x + axisX.x * 1000.0,
        cam.Location.y + axisX.y * 1000.0,
        cam.Location.z + axisX.z * 1000.0);
    return Project(engine, cam, ahead, out);
}

/** Draw the ray crosshair -> target, coloured by the query. */
void DrawCrosshairRay(ImDrawList* dl, Engine& engine, const Engine::CameraCache& cam,
    const Vector3& target, ImU32 color, float thickness)
{
    ImVec2 from;
    if (!CrosshairScreen(engine, cam, from)) {
        ++g_frame.projectFailed;
        return;
    }
    ImVec2 to;
    if (!Project(engine, cam, target, to)) {
        ++g_frame.projectFailed;
        return;
    }
    dl->AddLine(from, to, color, thickness);
    ++g_frame.segments;
}

/** Draw the drop marker for a ray that was not drawn. */
void DropMarker(ImDrawList* dl, Engine& engine, const Engine::CameraCache& cam,
    const Vector3& eye, const Vector3& target)
{
    const ImVec2 at = DropAnchor(engine, cam, eye, target);
    constexpr float kR = 6.f;
    dl->AddCircle(at, kR, kDropColor, 12, 2.f);
    dl->AddLine(ImVec2(at.x - kR, at.y - kR), ImVec2(at.x + kR, at.y + kR), kDropColor, 2.f);
    dl->AddLine(ImVec2(at.x + kR, at.y - kR), ImVec2(at.x - kR, at.y + kR), kDropColor, 2.f);
}

bool DrawRay(ImDrawList* drawList, Engine& engine, const Engine::CameraCache& cam,
    const Vector3& eye, const Vector3& target, const char* label, bool entity)
{
    if (!drawList)
        return false;

    const Options opt = CurrentOptions();
    // Two lanes so a loot-heavy frame cannot spend every ray before a player or
    // bot is traced. Only the total is capped by maxRays per lane.
    int& laneRays = entity ? g_frame.entityRays : g_frame.worldRays;
    bool& laneBudgetHit = entity ? g_entityRayBudgetHit : g_worldRayBudgetHit;
    // Player and bot rays ignore the distance cap and get their own budget floor,
    // so a distant or crowded target is not silently dropped.
    const int laneCap = entity ? (opt.maxRays > kEntityRayFloor ? opt.maxRays : kEntityRayFloor)
                               : opt.maxRays;
    if (laneRays >= laneCap) {
        laneBudgetHit = true;
        DropMarker(drawList, engine, cam, eye, target);
        return false;
    }

    const double dx = target.x - eye.x;
    const double dy = target.y - eye.y;
    const double dz = target.z - eye.z;
    const double distanceCm = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(distanceCm))
        return false;
    if (!entity && distanceCm > static_cast<double>(opt.radiusM) * 100.0) {
        DropMarker(drawList, engine, cam, eye, target);
        return false;
    }

    ++laneRays;

    // The module's own counters are the honest way to tell "not judged" from
    // "judged and clear": both return false, and only the counters separate them.
    const CollisionBvh::QueryStats before = CollisionBvh::Stats();
    const bool blocked = CollisionBvh::Occluded(eye, target, 5.0,
        static_cast<double>(UpgradeOptions::visCheckClearanceCm));
    const CollisionBvh::QueryStats after = CollisionBvh::Stats();
    const RayVerdict verdict = ClassifyRay(before, after, blocked);

    ImU32 color = kTooCloseRay;
    switch (verdict) {
    case RayVerdict::NoScene:
        color = kNoSceneRay;
        ++g_frame.noScene;
        break;
    case RayVerdict::TooClose:
        color = kTooCloseRay;
        ++g_frame.tooClose;
        break;
    case RayVerdict::Blocked:
        color = kBlockedRay;
        ++g_frame.occluded;
        break;
    case RayVerdict::Clear:
        color = kClearRay;
        ++g_frame.clear;
        break;
    }

    const int segmentsBefore = g_frame.segments;
    DrawCrosshairRay(drawList, engine, cam, target, color, 1.5f);
    if (g_frame.segments == segmentsBefore)
        DropMarker(drawList, engine, cam, eye, target);

    // With no scene there is nothing to hit, so the marker query is skipped: it
    // could only add nothing, and it would count a second no-scene query per ray
    // (which is how one unanswered ray came to read as two in the counters).
    if (opt.drawHitMarkers && verdict != RayVerdict::NoScene) {
        const CollisionBvh::Hit hit = CollisionBvh::ClosestHit(eye, target);
        if (hit.hit && hit.fraction > 0.0 && hit.fraction < 1.0) {
            const Vector3 point(eye.x + dx * hit.fraction, eye.y + dy * hit.fraction,
                eye.z + dz * hit.fraction);
            ImVec2 marker;
            if (Project(engine, cam, point, marker)) {
                drawList->AddCircleFilled(marker, 3.f, kHitMarker);
                drawList->AddCircle(marker, 5.f, color, 0, 1.f);
            }
        }
    }

    if (label && label[0]) {
        ImVec2 at;
        if (Project(engine, cam, target, at))
            drawList->AddText(ImVec2(at.x + 6.f, at.y - 6.f), color, label);
    }
    return blocked;
}

bool DrawBoneRay(ImDrawList* drawList, Engine& engine, const Engine::CameraCache& cam,
    const Vector3& target, uint32_t visibleColor, uint32_t hiddenColor)
{
    if (!drawList)
        return false;
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z))
        return false;

    // Same family of query the vischeck consumer uses, so a bone that reads clear
    // here is a bone the aim will treat as reachable.
    const bool blocked = CollisionBvh::BlockedForConsumer(cam.Location, target,
        static_cast<double>(UpgradeOptions::visCheckClearanceCm));
    const int segmentsBefore = g_frame.segments;
    DrawCrosshairRay(drawList, engine, cam, target,
        static_cast<ImU32>(blocked ? hiddenColor : visibleColor), 1.5f);
    if (g_frame.segments == segmentsBefore)
        ++g_frame.projectFailed;
    else if (blocked)
        ++g_frame.occluded;
    else
        ++g_frame.clear;
    return blocked;
}

} // namespace CollisionDebug
