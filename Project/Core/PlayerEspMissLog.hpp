#pragma once

// Player-ESP miss ledger (diagnostics only — never affects admission, Drawing,
// or frame selection).
//
// "Player ran up, no ESP" reports were un-debuggable because every pipeline
// stage (admission → retain → frame collect → range cull) drops players at
// different gates and only aggregate counters survive. This ledger records,
// per actor key, WHICH gate dropped the player and how often, then emits
// throttled NDJSON lines to the verification log (kArcVerifyPath) so a single
// session log answers "where did player X go".
//
// Threading: all Note/emit calls happen on worker threads (EntityList scan +
// CollectEspRenderFrame). The paint thread may only take a try-lock snapshot
// (SummaryTryLock) — never a blocking lock (see the G1/H9 Present-stall notes
// in Render.cpp).
//
// Pure helpers (ReasonName, FormatMissJson, throttle/snapshot logic via the
// injectable clock) are unit-tested in Tests/player_esp_miss_tests.cpp.

#include "AgentLog.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PlayerEspMiss {

// One entry per pipeline gate that can drop (or de-flag) a player. Order is
// grouped by pipeline stage; names are the strings that appear in the log.
enum class Reason : uint8_t {
    None = 0,
    // Admission (EntityList): candidate never made it into the player cache.
    AdmitBotClassified,
    AdmitPawnMissing,
    AdmitPawnNotInLevels,
    AdmitRootMissing,
    AdmitMeshMissing,
    AdmitPosInvalid,
    // Retention (EntityList): was cached, then evicted or de-flagged.
    EvictListGone,
    EvictRootStale,
    EvictGhost,
    EvictAllyHidden,
    EvictPsLost,
    EvictBotReclassified,
    EvictPosInvalid,
    DrawClearedDistance,
    // Frame collect (Esp.cpp): cached but not selected into the render frame.
    FrameNotDrawing,
    FrameNotInitialized,
    FrameAllyHidden,
    FrameDistance,
    FramePosition,
    FrameGateReject,
    FrameRangeCull,
    Count
};

inline constexpr size_t kReasonCount = static_cast<size_t>(Reason::Count);

inline const char* ReasonName(Reason r)
{
    switch (r) {
    case Reason::AdmitBotClassified: return "admit_bot_classified";
    case Reason::AdmitPawnMissing: return "admit_pawn_missing";
    case Reason::AdmitPawnNotInLevels: return "admit_pawn_not_in_levels";
    case Reason::AdmitRootMissing: return "admit_root_missing";
    case Reason::AdmitMeshMissing: return "admit_mesh_missing";
    case Reason::AdmitPosInvalid: return "admit_pos_invalid";
    case Reason::EvictListGone: return "evict_list_gone";
    case Reason::EvictRootStale: return "evict_root_stale";
    case Reason::EvictGhost: return "evict_ghost";
    case Reason::EvictAllyHidden: return "evict_ally_hidden";
    case Reason::EvictPsLost: return "evict_ps_lost";
    case Reason::EvictBotReclassified: return "evict_bot_reclassified";
    case Reason::EvictPosInvalid: return "evict_pos_invalid";
    case Reason::DrawClearedDistance: return "draw_cleared_distance";
    case Reason::FrameNotDrawing: return "frame_not_drawing";
    case Reason::FrameNotInitialized: return "frame_not_initialized";
    case Reason::FrameAllyHidden: return "frame_ally_hidden";
    case Reason::FrameDistance: return "frame_distance";
    case Reason::FramePosition: return "frame_position";
    case Reason::FrameGateReject: return "frame_gate_reject";
    case Reason::FrameRangeCull: return "frame_range_cull";
    case Reason::None:
    case Reason::Count:
    default: return "none";
    }
}

inline uint64_t NowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One tracked actor. A row is OPEN (a live "missing player") while its last
// action was a miss and it is recent enough to matter; once the actor makes a
// frame again (selectedMs) the row stops counting as open.
struct ActorMiss {
    uintptr_t actorKey = 0;
    std::string name;
    float distance = -1.f;
    uint64_t firstMs = 0;
    uint64_t lastMs = 0;      // last miss
    uint64_t selectedMs = 0;  // last time it was selected into a frame
    uint32_t total = 0;
    std::array<uint32_t, kReasonCount> byReason{};
    std::array<uint64_t, kReasonCount> lastNoteMs{};  // per-reason throttle
    uint64_t nameAskedMs = 0;

    uint32_t ReasonCount(Reason r) const
    {
        const size_t i = static_cast<size_t>(r);
        return i < kReasonCount ? byReason[i] : 0;
    }

    Reason TopReason() const
    {
        Reason best = Reason::None;
        uint32_t bestN = 0;
        for (size_t i = 1; i < kReasonCount; ++i) {
            if (byReason[i] > bestN) {
                bestN = byReason[i];
                best = static_cast<Reason>(i);
            }
        }
        return best;
    }

    bool Open(uint64_t nowMs, uint64_t maxAgeMs) const
    {
        return total > 0
            && (selectedMs == 0 || selectedMs < lastMs)
            && nowMs >= lastMs
            && nowMs - lastMs <= maxAgeMs;
    }
};

inline std::string JsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                    static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

inline std::string ActorKeyHex(uintptr_t key)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX",
        static_cast<unsigned long long>(key));
    return buf;
}

// ── Pure formatters (unit-tested) ─────────────────────────────────────────

// Full NDJSON line for one miss event, matching the kArcVerifyPath tap format
// used by player_admit_stats / player_name in EntityList.cpp.
inline std::string FormatMissJson(const ActorMiss& row, Reason reason,
                                  uint64_t nowMs, int64_t timestampMs)
{
    const uint64_t ageMs = nowMs >= row.lastMs ? nowMs - row.lastMs : 0;
    std::string out;
    out.reserve(256);
    out += "{\"sessionId\":\"c190fb\",\"runId\":\"player-esp-miss\","
           "\"hypothesisId\":\"PM1\",\"location\":\"PlayerEspMissLog\","
           "\"message\":\"player_miss\",\"data\":{\"actor\":\"";
    out += ActorKeyHex(row.actorKey);
    out += "\",\"name\":\"";
    out += JsonEscape(row.name);
    out += "\",\"reason\":\"";
    out += ReasonName(reason);
    out += "\",\"dist\":";
    char num[48];
    std::snprintf(num, sizeof(num), "%.1f", row.distance);
    out += num;
    out += ",\"count\":";
    out += std::to_string(row.ReasonCount(reason));
    out += ",\"total\":";
    out += std::to_string(row.total);
    out += ",\"ageMs\":";
    out += std::to_string(ageMs);
    out += "},\"timestamp\":";
    out += std::to_string(timestampMs);
    out += "}";
    return out;
}

// Compact "name reason xN (age)" fragment used by the console/overlay summary.
inline std::string FormatRowShort(const ActorMiss& row, uint64_t nowMs)
{
    const uint64_t ageMs = nowMs >= row.lastMs ? nowMs - row.lastMs : 0;
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s %s x%u (%llus)",
        row.name.empty() ? ActorKeyHex(row.actorKey).c_str() : row.name.c_str(),
        ReasonName(row.TopReason()),
        static_cast<unsigned>(row.total),
        static_cast<unsigned long long>(ageMs / 1000));
    return buf;
}

// ── Ledger ────────────────────────────────────────────────────────────────

class MissLedger {
public:
    // A miss is recorded at most once per (actor, reason) per kNoteThrottleMs
    // for LOGGING; counts always accumulate so the summary shows real rates.
    static constexpr uint64_t kNoteThrottleMs = 2000;
    // Lazy name resolution (DMA-backed) is retried at most this often.
    static constexpr uint64_t kNameAskThrottleMs = 5000;
    static constexpr size_t kMaxTrackedActors = 512;

    using NameFn = std::function<std::string()>;
    using Sink = std::function<void(const std::string&)>;

    // Returns true when this call was NOT throttled (i.e. a log line was
    // emitted). NowMs is explicit so tests drive the clock.
    bool NoteAt(uint64_t nowMs, uintptr_t key, const std::string& name,
                float distance, Reason reason)
    {
        if (key == 0 || reason == Reason::None || reason >= Reason::Count)
            return false;
        std::string line;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            ActorMiss& row = RowFor(key, nowMs);
            if (!name.empty() && row.name != name)
                row.name = name;
            if (distance >= 0.f)
                row.distance = distance;
            row.lastMs = nowMs;
            if (row.firstMs == 0)
                row.firstMs = nowMs;
            const size_t idx = static_cast<size_t>(reason);
            ++row.byReason[idx];
            ++row.total;
            // Throttle against the last EMITTED note, not the last attempt:
            // stamping on every attempt would starve the log of a player who
            // misses continuously (attempts arrive faster than the window).
            const bool throttled =
                row.lastNoteMs[idx] != 0
                && nowMs >= row.lastNoteMs[idx]
                && nowMs - row.lastNoteMs[idx] < kNoteThrottleMs;
            if (throttled)
                return false;
            row.lastNoteMs[idx] = nowMs;
            line = FormatMissJson(row, reason, nowMs,
                static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count()));
        }
        Emit(line);
        return true;
    }

    // Same, but resolves the actor name lazily (and only when the row has no
    // name yet, at most once per kNameAskThrottleMs) so admission drop paths
    // do not pay a DMA name read per pass.
    bool NoteLazyAt(uint64_t nowMs, uintptr_t key, float distance,
                    Reason reason, NameFn nameFn)
    {
        if (key == 0 || reason == Reason::None || reason >= Reason::Count)
            return false;
        std::string resolved;
        bool needName = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            // RowFor also creates the row on first sight, so the ask stamp
            // below survives even when the name resolves to empty — otherwise
            // undecryptable names would re-trigger a DMA read on every pass.
            ActorMiss& row = RowFor(key, nowMs);
            needName = row.name.empty()
                && (row.nameAskedMs == 0
                    || nowMs < row.nameAskedMs
                    || nowMs - row.nameAskedMs >= kNameAskThrottleMs);
            if (needName)
                row.nameAskedMs = nowMs;
        }
        if (needName && nameFn)
            resolved = nameFn();
        return NoteAt(nowMs, key, resolved, distance, reason);
    }

    // Positive path: the actor was selected into an ESP render frame. Clears
    // its open-miss state so the summary only lists live misses.
    void NoteSelectedAt(uint64_t nowMs, uintptr_t key, const std::string& name)
    {
        if (key == 0)
            return;
        std::lock_guard<std::mutex> lk(mutex_);
        ActorMiss& row = RowFor(key, nowMs);
        if (!name.empty() && row.name != name)
            row.name = name;
        row.selectedMs = nowMs;
    }

    bool Note(uintptr_t key, const std::string& name, float distance,
              Reason reason)
    {
        return NoteAt(NowMs(), key, name, distance, reason);
    }

    bool NoteLazy(uintptr_t key, float distance, Reason reason, NameFn nameFn)
    {
        return NoteLazyAt(NowMs(), key, distance, reason, std::move(nameFn));
    }

    void NoteSelected(uintptr_t key, const std::string& name)
    {
        NoteSelectedAt(NowMs(), key, name);
    }

    // Open misses, most-frequent first (bounded). Worker-thread use.
    std::vector<ActorMiss> OpenMisses(uint64_t nowMs, uint64_t maxAgeMs,
                                      size_t maxRows) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<ActorMiss> rows;
        rows.reserve(actors_.size());
        for (const auto& [key, row] : actors_) {
            (void)key;
            if (row.Open(nowMs, maxAgeMs))
                rows.push_back(row);
        }
        std::sort(rows.begin(), rows.end(),
            [](const ActorMiss& a, const ActorMiss& b) {
                if (a.total != b.total)
                    return a.total > b.total;
                return a.lastMs > b.lastMs;
            });
        if (rows.size() > maxRows)
            rows.resize(maxRows);
        return rows;
    }

    // Paint-thread safe: try_lock only; on contention returns false and the
    // caller keeps its previous summary.
    bool SummaryTryLock(std::string& out, uint64_t nowMs, size_t maxRows,
                        uint64_t maxAgeMs = 30000) const
    {
        std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
        if (!lk.owns_lock())
            return false;
        std::vector<ActorMiss> rows;
        rows.reserve(actors_.size());
        for (const auto& [key, row] : actors_) {
            (void)key;
            if (row.Open(nowMs, maxAgeMs))
                rows.push_back(row);
        }
        std::sort(rows.begin(), rows.end(),
            [](const ActorMiss& a, const ActorMiss& b) {
                return a.total > b.total;
            });
        if (rows.size() > maxRows)
            rows.resize(maxRows);
        out = FormatSummaryShort(rows, nowMs);
        return true;
    }

    static std::string FormatSummaryShort(const std::vector<ActorMiss>& rows,
                                          uint64_t nowMs)
    {
        if (rows.empty())
            return std::string();
        std::string out = "open=" + std::to_string(rows.size());
        for (const ActorMiss& row : rows) {
            out += " | ";
            out += FormatRowShort(row, nowMs);
        }
        return out;
    }

    // Full NDJSON summary line (worker thread; periodic file trace).
    std::string FormatSummaryLine(uint64_t nowMs, size_t maxRows,
                                  uint64_t maxAgeMs = 30000) const
    {
        const std::vector<ActorMiss> rows = OpenMisses(nowMs, maxAgeMs, maxRows);
        std::string out;
        out.reserve(256 + rows.size() * 128);
        out += "{\"sessionId\":\"c190fb\",\"runId\":\"player-esp-miss\","
               "\"hypothesisId\":\"PM1\",\"location\":\"PlayerEspMissLog\","
               "\"message\":\"player_miss_summary\",\"data\":{\"open\":";
        out += std::to_string(rows.size());
        out += ",\"rows\":[";
        bool first = true;
        for (const ActorMiss& row : rows) {
            if (!first)
                out += ",";
            first = false;
            out += "{\"actor\":\"";
            out += ActorKeyHex(row.actorKey);
            out += "\",\"name\":\"";
            out += JsonEscape(row.name);
            out += "\",\"top\":\"";
            out += ReasonName(row.TopReason());
            out += "\",\"count\":";
            out += std::to_string(row.total);
            out += ",\"dist\":";
            char num[48];
            std::snprintf(num, sizeof(num), "%.1f", row.distance);
            out += num;
            out += ",\"ageMs\":";
            out += std::to_string(nowMs >= row.lastMs ? nowMs - row.lastMs : 0);
            out += "}";
        }
        out += "]},\"timestamp\":";
        out += std::to_string(static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        out += "}";
        return out;
    }

    std::string FormatConsoleSummary(uint64_t nowMs, size_t maxRows) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<ActorMiss> rows;
        rows.reserve(actors_.size());
        for (const auto& [key, row] : actors_) {
            (void)key;
            if (row.Open(nowMs, 30000))
                rows.push_back(row);
        }
        std::sort(rows.begin(), rows.end(),
            [](const ActorMiss& a, const ActorMiss& b) {
                return a.total > b.total;
            });
        if (rows.size() > maxRows)
            rows.resize(maxRows);
        return FormatSummaryShort(rows, nowMs);
    }

    // Sink defaults to appending to kArcVerifyPath; tests swap in a collector
    // (or nullptr to silence). Called OUTSIDE the ledger lock.
    void SetSink(Sink sink)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        sink_ = std::move(sink);
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        actors_.clear();
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return actors_.size();
    }

private:
    ActorMiss& RowFor(uintptr_t key, uint64_t nowMs)
    {
        auto it = actors_.find(key);
        if (it == actors_.end()) {
            if (actors_.size() >= kMaxTrackedActors) {
                // Bounded: drop the least-recently-touched row.
                auto oldest = actors_.begin();
                for (auto i = actors_.begin(); i != actors_.end(); ++i) {
                    if (i->second.lastMs < oldest->second.lastMs)
                        oldest = i;
                }
                actors_.erase(oldest);
            }
            ActorMiss row;
            row.actorKey = key;
            row.firstMs = nowMs;
            it = actors_.emplace(key, std::move(row)).first;
        }
        return it->second;
    }

    void Emit(const std::string& line)
    {
        Sink sink;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            sink = sink_;
            if (!sink) {
                // Default tap: same verification log as player_admit_stats.
                sink = [](const std::string& s) {
                    std::ofstream f(kArcVerifyPath, std::ios::app);
                    if (f)
                        f << s << "\n";
                };
                sink_ = sink;
            }
        }
        sink(line);
    }

    mutable std::mutex mutex_;
    std::unordered_map<uintptr_t, ActorMiss> actors_;
    Sink sink_;
};

inline MissLedger& Global()
{
    static MissLedger ledger;
    return ledger;
}

} // namespace PlayerEspMiss
