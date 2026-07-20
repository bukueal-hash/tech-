#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace PlayerTrack {

inline std::mutex g_mu;
inline std::unordered_set<uintptr_t> g_tracked;

inline void SetTracked(uintptr_t pawn, bool on)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (on)
        g_tracked.insert(pawn);
    else
        g_tracked.erase(pawn);
}

inline bool IsTracked(uintptr_t pawn)
{
    if (!pawn)
        return false;
    std::lock_guard<std::mutex> lock(g_mu);
    return g_tracked.contains(pawn);
}

inline size_t TrackedCount()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_tracked.size();
}

inline void ClearAll()
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_tracked.clear();
}

inline void PruneNotIn(const std::vector<uintptr_t>& livePawns)
{
    std::unordered_set<uintptr_t> live(livePawns.begin(), livePawns.end());
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto it = g_tracked.begin(); it != g_tracked.end(); ) {
        if (!live.contains(*it))
            it = g_tracked.erase(it);
        else
            ++it;
    }
}

} // namespace PlayerTrack
