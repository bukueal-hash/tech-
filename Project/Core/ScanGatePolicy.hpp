#pragma once

// =============================================================================
// ScanGatePolicy — who gets the DMA scan gate next.
//
// The heavy scanners (Update, EntityList, RobotList, ContainerList, ItemList)
// share one DMA bus and take turns (EngineThreads.cpp). The old gate was a raw
// std::mutex: because Update runs every 16ms it was essentially ALWAYS due, so
// it barged ahead of RobotList's 200ms admission pass indefinitely — bot
// discovery waited seconds behind Update ("2.4s behind Update").
//
// This policy bounds every scanner's wait instead of leaving it to mutex luck:
//   1) A waiter past its starvation bound (~2x its cadence, clamped to
//      [200, 800]ms of waiting) preempts everyone — the most starved first.
//   2) Otherwise the most DUE waiter goes (last turn start + cadence).
//   3) Ties keep queue order (FIFO). A scanner that has never run is due now.
//
// Pure decision logic — no threading — so Tests/scan_gate_policy_tests.cpp can
// pin the starvation bounds under a simulated Update barrage.
// =============================================================================

#include <cstdint>
#include <vector>

namespace ScanGatePolicy {

struct Candidate {
    int64_t cadenceMs = 0;
    // Steady-ms of this scanner's last turn start; kNeverRan = never ran.
    int64_t lastStartMs = kNeverRan();
    int64_t enqueueMs = 0;

    static constexpr int64_t kNeverRan()
    {
        return INT64_MIN / 4; // far past any steady clock — "never ran" = due now
    }
};

// How long a scanner may sit in the queue before it preempts due-ordering.
// Fast scanners get tight bounds (their staleness is expensive), slow scanners
// get generous ones, and the clamp keeps the extremes sane.
inline int64_t StarvationBoundMs(int64_t cadenceMs)
{
    int64_t bound = 2 * cadenceMs + 100;
    if (bound < 200)
        bound = 200;
    if (bound > 800)
        bound = 800;
    return bound;
}

// Index of the next waiter, or -1 for an empty queue.
inline int PickNext(const std::vector<Candidate>& queue, int64_t nowMs)
{
    int best = -1;
    bool bestStarved = false;
    int64_t bestWaited = 0;
    int64_t bestDue = 0;

    for (int i = 0; i < static_cast<int>(queue.size()); ++i) {
        const Candidate& c = queue[static_cast<size_t>(i)];
        const int64_t waited = nowMs - c.enqueueMs;
        const bool starved = waited > StarvationBoundMs(c.cadenceMs);
        const int64_t due = c.lastStartMs + c.cadenceMs;

        if (best < 0) {
            best = i;
            bestStarved = starved;
            bestWaited = waited;
            bestDue = due;
            continue;
        }
        if (starved != bestStarved) {
            // Any starved waiter beats any non-starved one.
            if (starved) {
                best = i;
                bestStarved = true;
                bestWaited = waited;
                bestDue = due;
            }
            continue;
        }
        if (bestStarved) {
            if (waited > bestWaited) { // the MOST starved goes first
                best = i;
                bestWaited = waited;
                bestDue = due;
            }
        } else if (due < bestDue) { // strict: ties keep queue order (FIFO)
            best = i;
            bestWaited = waited;
            bestDue = due;
        }
    }
    return best;
}

} // namespace ScanGatePolicy
