// CollisionMirror KD-tree raycast suite.
// Pure math: no DMA, no ImGui, no process. Exercises the tree build and
// segment raycast that back the LRTS+collision combined vischeck.
//
// Cases:
//   - wall blocks a ray through it
//   - wall-clearing ray passes
//   - boundary / glancing behavior
//   - fail-open on null tree
//   - KD-tree split correctness on a larger mesh

#pragma warning(push)
#pragma warning(disable : 4459)
#include "tests_main.hpp"
#include "Core/Vector.hpp"
#include "Functions/CollisionMirror.h"
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

#include <cmath>
#include <vector>

using namespace CollisionMirror;

namespace {

Vector3 V(double x, double y, double z)
{
    return Vector3(x, y, z);
}

// A vertical wall in the XZ plane at y=0, spanning |x|<=10, z in [0,10].
std::vector<Tri> MakeWall()
{
    std::vector<Tri> tris;
    // Two triangles make a 20x10 quad at y=0.
    tris.push_back({ V(-10, 0, 0), V(10, 0, 0), V(10, 0, 10) });
    tris.push_back({ V(-10, 0, 0), V(10, 0, 10), V(-10, 0, 10) });
    return tris;
}

} // namespace

TEST_CASE("CollisionMirror: wall blocks the segment through it")
{
    auto tris = MakeWall();
    KDNode* tree = BuildTree(tris);
    REQUIRE(tree != nullptr);

    // Camera at (-5, -20, 5), target at (-5, 20, 5): segment crosses y=0.
    const bool vis = IsVisible(tree, V(-5, -20, 5), V(-5, 20, 5));
    CHECK_FALSE(vis);

    FreeTree(tree);
}

TEST_CASE("CollisionMirror: wall-clearing ray passes")
{
    auto tris = MakeWall();
    KDNode* tree = BuildTree(tris);
    REQUIRE(tree != nullptr);

    // Camera above the wall, target above: ray at z=50 clears it.
    const bool vis = IsVisible(tree, V(0, -20, 50), V(0, 20, 50));
    CHECK(vis);

    FreeTree(tree);
}

TEST_CASE("CollisionMirror: ray to the side of the wall passes")
{
    auto tris = MakeWall();
    KDNode* tree = BuildTree(tris);
    REQUIRE(tree != nullptr);

    // Camera and target both at x=30: outside the wall's x-span.
    const bool vis = IsVisible(tree, V(30, -20, 5), V(30, 20, 5));
    CHECK(vis);

    FreeTree(tree);
}

TEST_CASE("CollisionMirror: hit triangle is reported")
{
    auto tris = MakeWall();
    KDNode* tree = BuildTree(tris);
    REQUIRE(tree != nullptr);

    const Tri* hit = nullptr;
    const bool vis = IsVisible(tree, V(-5, -20, 5), V(-5, 20, 5), &hit);
    CHECK_FALSE(vis);
    REQUIRE(hit != nullptr);
    // The hit triangle lives on the y=0 plane.
    CHECK(std::fabs(hit->p0.y) < 1e-6);
    CHECK(std::fabs(hit->p1.y) < 1e-6);
    CHECK(std::fabs(hit->p2.y) < 1e-6);

    FreeTree(tree);
}

TEST_CASE("CollisionMirror: null tree fails open")
{
    // No tree (warmup) -> visible, matching LRTS warmup behavior.
    const bool vis = IsVisible(nullptr, V(0, 0, 0), V(0, 0, 100));
    CHECK(vis);
}

TEST_CASE("CollisionMirror: degenerate segment (same point) is visible")
{
    auto tris = MakeWall();
    KDNode* tree = BuildTree(tris);
    REQUIRE(tree != nullptr);

    const bool vis = IsVisible(tree, V(0, 0, 5), V(0, 0, 5));
    CHECK(vis);

    FreeTree(tree);
}

TEST_CASE("CollisionMirror: boundary ray starting on the wall is not falsely blocked")
{
    // A ray starting exactly on the wall surface must not hit it (t > eps).
    auto tris = MakeWall();
    KDNode* tree = BuildTree(tris);
    REQUIRE(tree != nullptr);

    // Camera ON the wall plane, looking away — no blocking triangle inside
    // the segment (t in (eps,1) strictly), so visible.
    const bool vis = IsVisible(tree, V(0, 0, 5), V(0, -20, 5));
    CHECK(vis);

    FreeTree(tree);
}

TEST_CASE("CollisionMirror: larger mesh builds and queries consistently")
{
    // A grid of 10x10 unit walls in a plane — exercises median splits.
    std::vector<Tri> tris;
    for (int i = -5; i < 5; ++i) {
        for (int j = -5; j < 5; ++j) {
            const double x0 = static_cast<double>(i) * 2.0;
            const double z0 = static_cast<double>(j) * 2.0;
            tris.push_back({ V(x0, 0, z0), V(x0 + 2, 0, z0), V(x0 + 2, 0, z0 + 2) });
            tris.push_back({ V(x0, 0, z0), V(x0 + 2, 0, z0 + 2), V(x0, 0, z0 + 2) });
        }
    }
    KDNode* tree = BuildTree(tris);
    REQUIRE(tree != nullptr);

    // Through the center: blocked.
    CHECK_FALSE(IsVisible(tree, V(0, -20, 0), V(0, 20, 0)));
    // Above everything: clear.
    CHECK(IsVisible(tree, V(0, -20, 500), V(0, 20, 500)));
    // Far to the side: clear.
    CHECK(IsVisible(tree, V(500, -20, 0), V(500, 20, 0)));

    FreeTree(tree);
}

TEST_CASE("CollisionMirror: query ray hit copy (QueryRay math via IsVisible)")
{
    auto tris = MakeWall();
    KDNode* tree = BuildTree(tris);
    REQUIRE(tree != nullptr);

    // The segment from behind the wall into it must report a hit.
    const Tri* hit = nullptr;
    const bool vis = IsVisible(tree, V(0, -20, 5), V(0, 5, 5), &hit);
    CHECK_FALSE(vis);
    REQUIRE(hit != nullptr);

    FreeTree(tree);
}
