#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include "EntityDiagnostics.hpp"

// Agent NDJSON instrumentation is disabled — stubs only. The one exception is the
// throttled verification taps (raid heartbeat, bone_resolve, player_name), which
// still write to kArcVerifyPath below.

// The verification taps write next to the process working directory, so no
// absolute machine path is compiled in.






inline constexpr const char* kArcDebugLogPath = "NUL";
// The trace file used to be named debug-<agent-session>.log (debug-5681af.log,
// debug-c190fb.log): a per-session filename left over from the instrumenting
// runs, so every user's trace landed in a file named after somebody else's
// session, and those two names are what put ~200 MB of logs into git history.
// One neutral name, and the wide literal is the same file for the
// allocation-free crash path (see CrashHandler.cpp).
// Real file used ONLY by the throttled verification taps (raid heartbeat,
// bone_resolve, player_name). Everything else stays on NUL.
inline constexpr const char* kArcVerifyPath = "arc_trace.log";
inline constexpr const wchar_t* kArcVerifyPathW = L"arc_trace.log";

// ── Paint-thread stall diagnostics (measure-only) ──────────────────────────
// Measures the Present-loop iteration time on the paint thread into a
// nonblocking-producer synchronized in-memory ring. File I/O happens ONLY in PaintStallFlushToLog,
// which must be called from a worker thread — never from paint — so the
// diagnostic never perturbs the stall it measures. A stall > one vsync
// (~16.6ms at 60Hz) re-shows the previous backbuffer = the "ghost copy"
// flash; kPaintStallMs is the threshold for what counts as a stall.
struct PaintSpikeRec {
    float ms = 0.f;
    int64_t tsMs = 0;  // system_clock, correlates with other log taps
    char phase[8] = "-";  // where the stall time went: New/Rend/Draw/Pres
};

inline constexpr int kPaintStallSlots = 16;
inline constexpr float kPaintStallMs = 25.f;

inline std::atomic<uint64_t>& PaintStallFrameCount()
{
    static std::atomic<uint64_t> v{0};
    return v;
}
inline std::atomic<float>& PaintStallMaxMs()
{
    static std::atomic<float> v{0.f};
    return v;
}
inline std::atomic<float>& PaintStallSumMs()
{
    static std::atomic<float> v{0.f};
    return v;
}
inline std::atomic<int>& PaintStallRingHead()
{
    static std::atomic<int> v{0};
    return v;
}
inline std::mutex& PaintStallRingMutex()
{
    static std::mutex mutex;
    return mutex;
}
inline PaintSpikeRec* PaintStallRing()
{
    static PaintSpikeRec ring[kPaintStallSlots]{};
    return ring;
}

// ── RenderEsp sub-phase probe (measure-only, memory) ──────────────────────
// Split of where [Rend] time goes. Called per pass per frame from Esp.cpp;
// max ms per phase per window is appended to the paint_stall line by the
// worker flush. Phase 4 (tracer) is only nonzero when the debug overlay
// (GhostTracePaint) is running, which also tells us if it's enabled.
inline constexpr int kPaintSubPhases = 7;  // cam world player bot tracer radar other
inline const char* PaintSubPhaseName(int i)
{
    static const char* n[kPaintSubPhases] =
        {"cam", "world", "player", "bot", "tracer", "radar", "other"};
    return (i >= 0 && i < kPaintSubPhases) ? n[i] : "?";
}
inline std::atomic<float>& PaintSubMaxMs(int i)
{
    static std::atomic<float> v[kPaintSubPhases]{};
    return v[i];
}
inline void PaintSubPhaseNote(int phase, float ms)
{
    if (phase < 0 || phase >= kPaintSubPhases)
        return;
    float m = PaintSubMaxMs(phase).load(std::memory_order_relaxed);
    while (ms > m && !PaintSubMaxMs(phase).compare_exchange_weak(m, ms)) {
    }
}

// Called by the paint (Present) loop once per iteration.
// worstPhase = which paint phase consumed the stall ("New" NewFrame,
// "Rend" app Render, "Draw" ImGui RenderDrawData, "Pres" Present).
inline void PaintStallNote(float iterMs, const char* worstPhase,
                           [[maybe_unused]] float worstPhaseMs)
{
    PaintStallFrameCount().fetch_add(1, std::memory_order_relaxed);
    PaintStallSumMs().fetch_add(iterMs, std::memory_order_relaxed);
    float m = PaintStallMaxMs().load(std::memory_order_relaxed);
    while (iterMs > m && !PaintStallMaxMs().compare_exchange_weak(m, iterMs)) {
    }
    if (iterMs >= kPaintStallMs) {
        // Never wait on paint for a worker snapshot. A contended sample is
        // omitted; the aggregate frame/stall counters still record the frame.
        std::unique_lock<std::mutex> lock(PaintStallRingMutex(), std::try_to_lock);
        if (!lock.owns_lock())
            return;
        const int head = PaintStallRingHead().load(std::memory_order_relaxed);
        PaintStallRingHead().store((head + 1) % kPaintStallSlots,
            std::memory_order_relaxed);
        PaintSpikeRec& rec = PaintStallRing()[head];
        rec.ms = iterMs;
        rec.tsMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        std::snprintf(rec.phase, sizeof(rec.phase), "%s",
            worstPhase ? worstPhase : "-");
    }
}

// Called by a WORKER thread (~1s cadence, see Esp.cpp CollectEspRenderFrame).
// Writes one NDJSON line when stalls were recorded since the last flush.
inline void PaintStallFlushToLog()
{
    static auto s_last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last < std::chrono::seconds(1))
        return;
    s_last = now;

    const uint64_t frames = PaintStallFrameCount().exchange(0, std::memory_order_relaxed);
    if (frames == 0)
        return;
    const float sum = PaintStallSumMs().exchange(0.f, std::memory_order_relaxed);
    const float maxMs = PaintStallMaxMs().exchange(0.f, std::memory_order_relaxed);

    char spikes[512]{};
    int n = 0;
    PaintSpikeRec snapshot[kPaintStallSlots]{};
    int head = 0;
    {
        std::lock_guard<std::mutex> lock(PaintStallRingMutex());
        head = PaintStallRingHead().load(std::memory_order_relaxed);
        for (int i = 0; i < kPaintStallSlots; ++i) {
            snapshot[i] = PaintStallRing()[i];
            PaintStallRing()[i] = PaintSpikeRec{};
        }
    }
    for (int i = 0; i < kPaintStallSlots; ++i) {
        const PaintSpikeRec& rec =
            snapshot[(head - 1 - i + kPaintStallSlots * 2) % kPaintStallSlots];
        if (rec.ms <= 0.f)
            continue;
        const int need = std::snprintf(nullptr, 0, "%s%.0fms@%lld[%s]",
            n ? "," : "", rec.ms, static_cast<long long>(rec.tsMs), rec.phase);
        if (n + need >= static_cast<int>(sizeof(spikes)) - 1)
            break;
        n += std::snprintf(spikes + n, sizeof(spikes) - static_cast<size_t>(n),
            "%s%.0fms@%lld[%s]", n ? "," : "", rec.ms,
            static_cast<long long>(rec.tsMs), rec.phase);
    }
    if (n == 0)
        return;

    const float avg = frames ? sum / static_cast<float>(frames) : 0.f;

    // Drain sub-phase maxes (memory-only; reset after read).
    char sub[128]{};
    int sn = 0;
    for (int i = 0; i < kPaintSubPhases; ++i) {
        const float m = PaintSubMaxMs(i).exchange(0.f, std::memory_order_relaxed);
        if (m <= 0.f)
            continue;
        const int need = std::snprintf(nullptr, 0, "%s%s:%d",
            sn ? "," : "", PaintSubPhaseName(i), static_cast<int>(m));
        if (sn + need >= static_cast<int>(sizeof(sub)) - 1)
            break;
        sn += std::snprintf(sub + sn, sizeof(sub) - static_cast<size_t>(sn),
            "%s%s:%d", sn ? "," : "", PaintSubPhaseName(i), static_cast<int>(m));
    }

    std::ostringstream f;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"location\":\"AgentLog.h\",\"message\":\"paint_stall\","
      << "\"data\":{\"frames\":" << frames
      << ",\"maxMs\":" << static_cast<int>(maxMs)
      << ",\"avgMs\":" << static_cast<int>(avg)
      << ",\"sub\":\"" << sub << "\""
      << ",\"spikes\":\"" << spikes << "\"}"
      << ",\"ts\":" << ts << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}

// ── ESP paint frame-time / lock-wait probe (measure-only, memory) ──────────
// Baseline for "why is paint slow": per-pass wall time plus the time spent
// inside each paint-side lock acquisition, try_lock misses included. Memory-only
// (relaxed atomics), drained by a WORKER thread in EspPaintProfileFlushToLog();
// the caller gates that on var::debug_esp_paint, so a normal session pays only
// the atomics. Emits one esp_paint_profile NDJSON line per window.
inline constexpr int kEspPaintLocks = 6;
inline const char* EspPaintLockName(int i)
{
    static const char* n[kEspPaintLocks] =
        {"frame", "state", "player", "robot", "world", "cam"};
    return (i >= 0 && i < kEspPaintLocks) ? n[i] : "?";
}
inline std::atomic<uint64_t>& EspPaintFrameCount()
{
    static std::atomic<uint64_t> v{0};
    return v;
}
inline std::atomic<float>& EspPaintSumMs()
{
    static std::atomic<float> v{0.f};
    return v;
}
inline std::atomic<float>& EspPaintMaxMs()
{
    static std::atomic<float> v{0.f};
    return v;
}
inline std::atomic<uint64_t>& EspPaintLockTries(int i)
{
    static std::atomic<uint64_t> v[kEspPaintLocks]{};
    return v[i];
}
inline std::atomic<uint64_t>& EspPaintLockMisses(int i)
{
    static std::atomic<uint64_t> v[kEspPaintLocks]{};
    return v[i];
}
inline std::atomic<float>& EspPaintLockSumMs(int i)
{
    static std::atomic<float> v[kEspPaintLocks]{};
    return v[i];
}
inline std::atomic<float>& EspPaintLockMaxMs(int i)
{
    static std::atomic<float> v[kEspPaintLocks]{};
    return v[i];
}
inline void EspPaintNoteMax(std::atomic<float>& slot, float ms)
{
    float m = slot.load(std::memory_order_relaxed);
    while (ms > m && !slot.compare_exchange_weak(m, ms)) {
    }
}

// Called by paint once per ESP pass. ms = wall time inside the pass.
inline void EspPaintFrameNote(float ms)
{
    EspPaintFrameCount().fetch_add(1, std::memory_order_relaxed);
    EspPaintSumMs().fetch_add(ms, std::memory_order_relaxed);
    EspPaintNoteMax(EspPaintMaxMs(), ms);
}

// Called by paint at each lock acquisition point. slot = EspPaintLockName
// index; acquired is false when a try_lock lost the race (paint never blocks
// on these, so a miss is the wait — it reuses the retained snapshot instead).
inline void EspPaintLockNote(int slot, bool acquired, float waitMs)
{
    if (slot < 0 || slot >= kEspPaintLocks)
        return;
    EspPaintLockTries(slot).fetch_add(1, std::memory_order_relaxed);
    if (!acquired)
        EspPaintLockMisses(slot).fetch_add(1, std::memory_order_relaxed);
    EspPaintLockSumMs(slot).fetch_add(waitMs, std::memory_order_relaxed);
    EspPaintNoteMax(EspPaintLockMaxMs(slot), waitMs);
}

// Worker thread only (~1s cadence), gated by var::debug_esp_paint at the call
// site. Never call from paint: this builds strings and writes the log.
inline void EspPaintProfileFlushToLog()
{
    static auto s_last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last < std::chrono::seconds(1))
        return;
    s_last = now;

    const uint64_t frames = EspPaintFrameCount().exchange(0, std::memory_order_relaxed);
    if (frames == 0)
        return;
    const float sum = EspPaintSumMs().exchange(0.f, std::memory_order_relaxed);
    const float maxMs = EspPaintMaxMs().exchange(0.f, std::memory_order_relaxed);

    const auto r2 = [](float v) {
        return static_cast<float>(static_cast<int>(v * 100.f + 0.5f)) / 100.f;
    };
    std::ostringstream locks;
    locks << "{";
    bool firstLock = true;
    for (int i = 0; i < kEspPaintLocks; ++i) {
        const uint64_t tries =
            EspPaintLockTries(i).exchange(0, std::memory_order_relaxed);
        const uint64_t miss =
            EspPaintLockMisses(i).exchange(0, std::memory_order_relaxed);
        const float lsum =
            EspPaintLockSumMs(i).exchange(0.f, std::memory_order_relaxed);
        const float lmax =
            EspPaintLockMaxMs(i).exchange(0.f, std::memory_order_relaxed);
        if (!tries && !miss)
            continue;
        if (!firstLock)
            locks << ",";
        firstLock = false;
        locks << "\"" << EspPaintLockName(i) << "\":{\"try\":" << tries
              << ",\"miss\":" << miss
              << ",\"avgMs\":" << r2(tries ? lsum / static_cast<float>(tries) : 0.f)
              << ",\"maxMs\":" << r2(lmax) << "}";
    }
    locks << "}";

    std::ostringstream f;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"location\":\"AgentLog.h\",\"message\":\"esp_paint_profile\","
      << "\"data\":{\"frames\":" << frames
      << ",\"avgMs\":" << r2(frames ? sum / static_cast<float>(frames) : 0.f)
      << ",\"maxMs\":" << r2(maxMs)
      << ",\"locks\":" << locks.str() << "}"
      << ",\"ts\":" << ts << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}
