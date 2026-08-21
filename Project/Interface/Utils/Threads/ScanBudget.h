#pragma once

// ScanBudget -- cooperative elapsed-time budget for gated scanner chunking.
//
// Problem: a single gated scan can hold the DMA bus for hundreds of ms
// (e.g. EntityList 781ms), starving the ungated latency passes (camera,
// position, aim).  The idle gap only bounds *turn frequency*, not *turn
// duration*.  A 781ms hold monopolizes the bus for its entire run.
//
// Solution: each scanner checks budget.expired() at phase boundaries and
// returns early when the budget is exhausted.  The gate releases, the 12ms
// idle gap fires, and ungated passes get their turn.  On the next
// SyncedThread tick, the scanner resumes from persistent state.
//
// Usage:
//   void Engine::EntityListScan() {
//       static int s_batchIdx = 0;
//       ScanBudget budget(50);  // 50ms hard cap
//
//       // Phase 1: always runs (fast)
//       if (s_batchIdx == 0) { /* scatter actor pointers */ }
//       if (budget.expired()) return;   // yield gate
//
//       // Phase 2: batched work
//       for (; s_batchIdx < count; ++s_batchIdx) {
//           /* process one player */
//           if (budget.expired()) return;  // yield gate, resume next turn
//       }
//       s_batchIdx = 0;  // done, reset for next turn
//   }

#include <chrono>

struct ScanBudget {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start;
    int maxMs;

    explicit ScanBudget(int maxMs)
        : start(Clock::now()), maxMs(maxMs) {}

    // Returns true if >= maxMs have elapsed since construction.
    bool expired() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count() >= maxMs;
    }

    // Elapsed milliseconds since construction.
    int elapsedMs() const {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start).count());
    }

    // Remaining milliseconds (can go negative).
    int remainingMs() const {
        return maxMs - elapsedMs();
    }
};
