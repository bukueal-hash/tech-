#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

// Agent NDJSON instrumentation disabled — stubs only (no file I/O, no worker thread).

inline int64_t ArcAgentLogNowMs() { return 0; }

inline std::string ArcAgentLogJsonEscape(const std::string& value) { return value; }

inline void ArcAsyncLog_Start() {}
inline void ArcAsyncLog_Stop() {}
inline void ArcAsyncLog_Push(std::string, std::string) {}

inline constexpr const char* kArcDebugLogPath = "NUL";
inline constexpr const char* kArcDebugLogPath5681af = "F:/Test/ARCs/debug-5681af.log";
// Real file used ONLY by the throttled verification taps (raid heartbeat,
// bone_resolve, player_name). Everything else stays on NUL.
inline constexpr const char* kArcVerifyPath = "F:/Test/ARCs/debug-c190fb.log";

// ── Paint-thread stall diagnostics (measure-only) ──────────────────────────
// Measures the Present-loop iteration time on the paint thread into a
// lock-free in-memory ring. File I/O happens ONLY in PaintStallFlushToLog,
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
inline void PaintStallNote(float iterMs, const char* worstPhase, float worstPhaseMs)
{
    PaintStallFrameCount().fetch_add(1, std::memory_order_relaxed);
    PaintStallSumMs().fetch_add(iterMs, std::memory_order_relaxed);
    float m = PaintStallMaxMs().load(std::memory_order_relaxed);
    while (iterMs > m && !PaintStallMaxMs().compare_exchange_weak(m, iterMs)) {
    }
    if (iterMs >= kPaintStallMs) {
        const int head = PaintStallRingHead().fetch_add(1, std::memory_order_relaxed);
        PaintSpikeRec& rec = PaintStallRing()[head % kPaintStallSlots];
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
    const int head = PaintStallRingHead().load(std::memory_order_relaxed);
    for (int i = 0; i < kPaintStallSlots; ++i) {
        const PaintSpikeRec& rec =
            PaintStallRing()[(head - 1 - i + kPaintStallSlots * 2) % kPaintStallSlots];
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

    std::ofstream f(kArcVerifyPath, std::ios::app);
    if (!f)
        return;
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"location\":\"AgentLog.h\",\"message\":\"paint_stall\","
      << "\"data\":{\"frames\":" << frames
      << ",\"maxMs\":" << static_cast<int>(maxMs)
      << ",\"avgMs\":" << static_cast<int>(avg)
      << ",\"sub\":\"" << sub << "\""
      << ",\"spikes\":\"" << spikes << "\"}"
      << ",\"ts\":" << ts << "}\n";
}

inline void ArcAgentLogTo(
    const char*,
    const char*,
    const char*,
    const char*,
    const char*,
    const char*,
    const std::string&)
{
}

inline void ArcAgentLog(
    const char*,
    const char*,
    const char*,
    const char*,
    const std::string&)
{
}

inline void ArcAgentLog5681af(
    const char*,
    const char*,
    const char*,
    const char*,
    const std::string&)
{
}
