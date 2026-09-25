// ScanGatePolicy suite — pins the fairness fix for "RobotList waits seconds
// behind Update" on the shared DMA scan gate.
//
// The regression this locks down: with a raw mutex, the 16ms Update thread is
// always due and barges ahead of RobotList's 200ms admission pass, so bot
// discovery cadence collapsed. A starved waiter must preempt due-ordering, and
// an already-waiting scanner must not lose its turn to a fresh arrival.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/ScanGatePolicy.hpp"

#include <vector>

namespace {

using ScanGatePolicy::Candidate;
using ScanGatePolicy::PickNext;
using ScanGatePolicy::StarvationBoundMs;

int64_t Ms(int64_t v) { return v; }

} // namespace

TEST_CASE("ScanGatePolicy starvation bounds clamp per cadence")
{
    CHECK(StarvationBoundMs(16) == 200);   // Update: tight
    CHECK(StarvationBoundMs(200) == 500);  // RobotList
    CHECK(StarvationBoundMs(220) == 540);  // EntityList
    CHECK(StarvationBoundMs(250) == 600);  // ContainerList/ItemList
    CHECK(StarvationBoundMs(10000) == 800); // capped
}

TEST_CASE("a starved RobotList preempts a freshly-arrived Update (the bug)")
{
    // Update enqueued recently and is due already (due at 666); RobotList has
    // waited 650ms > its 500ms bound. The old mutex let Update barge here.
    std::vector<Candidate> q = {
        { 16,  Ms(650), Ms(650) },  // Update: last start 650, enqueued 650
        { 200, Ms(0),   Ms(50)  },  // RobotList: waiting since 50
    };
    CHECK(PickNext(q, Ms(700)) == 1);
}

TEST_CASE("nobody starved: the most-due waiter goes")
{
    // RobotList last ran at 400 (due 600) beats Update due at 696 even though
    // Update enqueued first — due order, not barging order.
    std::vector<Candidate> q = {
        { 16,  Ms(680), Ms(690) },
        { 200, Ms(400), Ms(695) },
    };
    CHECK(PickNext(q, Ms(700)) == 1);
}

TEST_CASE("a scanner that never ran is immediately due")
{
    std::vector<Candidate> q = {
        { 16,  Ms(690), Ms(695) },
        { 200, Candidate::kNeverRan(), Ms(695) },
    };
    CHECK(PickNext(q, Ms(700)) == 1);
}

TEST_CASE("when several are starved, the longest-waiting goes first")
{
    std::vector<Candidate> q = {
        { 16,  Ms(0), Ms(500) }, // Update bound 200, waited 200 -> starved
        { 200, Ms(0), Ms(100) }, // RobotList bound 500, waited 600 -> starved
    };
    // At now=700: Update waited 200 (not > 200? 200 > 200 is false) — make it
    // unambiguous: Update waited 300 (enqueued 400), RobotList waited 600.
    q[0].enqueueMs = Ms(400);
    CHECK(PickNext(q, Ms(700)) == 1);
}

TEST_CASE("exact ties keep queue order (FIFO)")
{
    std::vector<Candidate> q = {
        { 200, Ms(0), Ms(600) },
        { 200, Ms(0), Ms(600) },
    };
    CHECK(PickNext(q, Ms(700)) == 0);
}

TEST_CASE("empty queue has no pick")
{
    CHECK(PickNext({}, Ms(0)) == -1);
}
