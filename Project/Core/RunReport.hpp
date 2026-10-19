#pragma once

// Phase 1 — run report (advantage 1: "one anonymous object per frame").
//
// Purpose: every frame/ESP result is checked against ONE machine-readable object
// instead of a screenshot of the debug HUD. The HUD stays the human view; this
// is the checkable one, and the two are fed from the same counters.
//
// Shape, after EntityDiagnostics.hpp / AgentLog.h precedent:
//   * FrameSample is a fixed-size POD built on a WORKER thread at the existing
//     Esp render-frame publish point (Functions/Esp.cpp, CollectEspRenderFrame).
//   * Publish() swaps one immutable shared_ptr — never a torn read, always a
//     complete sample or none at all. It is cheap enough for the frame path
//     (the frame itself is published the same way) and does no file I/O.
//   * Writer owns a dedicated thread. It serializes at most once per
//     kWriteIntervalMs into help/run_report.json via a temp file + replace, so
//     a reader never observes a half-written document. Never called from paint.
//   * Every failure is counted AND surfaced: into the report's own "writer"
//     section, into OutputDebugString, and into EntityDiagnostics' error log.
//     A denied write or a full disk shows up as a failure, not as silence.
//
// What the report is NOT: a runtime memory-write counter. memoryWritesPerformed
// is a declaration backed by the Phase 0 write inventory (no write call site
// exists under Project/ outside Tests/), not a measurement. See
// .planning/2026-10-13-external-adoption/phase0-write-inventory.md.

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "EspFramePolicy.hpp"
#include "PlayerEspMissLog.hpp"

namespace RunReport {

inline constexpr size_t kReasonCount = PlayerEspMiss::kReasonCount;
inline constexpr uintmax_t kMaxReportBytes = 64ull * 1024ull;
inline constexpr uint64_t kWriteIntervalMs = 1000;
// The HUD's player-miss summary uses the same window, so the report's
// byReason totals describe the same open rows the summarised line does.
inline constexpr uint64_t kMissWindowMs = 30000;

inline uint64_t SteadyNowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline int64_t SystemNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ── The snapshot ──────────────────────────────────────────────────────────

// One frame, as observed by the worker that built it. Plain data: no strings,
// no pointers into engine state, nothing that can dangle after publication.
struct FrameSample {
    // Identity / freshness (steady-clock ms, same domain as SteadyNowMs).
    uint64_t frameSeq = 0;
    uint64_t worldGeneration = 0;
    uint64_t collectStampMs = 0;   // frame collection finished (out.collectStampMs)
    uint64_t sampleStampMs = 0;    // this sample was built

    // Timings measured in the frame build, milliseconds.
    //   collect  — the whole CollectEspRenderFrame pass
    //   read     — the DMA read batch (scatter prepare + execute)
    //   discovery— cache walk that turns entity caches into frame entries on
    //              this frame. Our entity *scanning* runs on its own worker, so
    //              this is the frame-build discovery, not the scanner pass.
    double collectMs = 0.0;
    double readMs = 0.0;
    double discoveryMs = 0.0;
    //   bone      — the skeleton refresh for the nearest players
    //               (kMaxEspFrameBoneReads); the only part of the collect that
    //               scales with how many players are close
    //   cacheWait — time blocked acquiring a cache mutex a gated scanner holds.
    //               Together with collectMs this says whether a slow frame was
    //               our own work or a wait for someone else's pass.
    double boneMs = 0.0;
    double cacheWaitMs = 0.0;

    // Camera (CameraOkForEsp: usable FOV + plausible location).
    bool cameraValid = false;
    float camFov = 0.f;

    // Per-kind frame entry counts — the same numbers the HUD's EspDraw row sums.
    uint32_t playerCount = 0;
    uint32_t robotCount = 0;
    uint32_t worldCount = 0;
    uint32_t drawCount = 0;

    // engine.m_playerDiag as of this sample (the HUD's PInit/PDraw/PFrame/PDrop
    // rows). valid=false when the diag lock was contended, so the report can say
    // "absent" rather than publish a stale or invented zero.
    bool playerDiagValid = false;
    int32_t playerInitialized = 0;
    int32_t playerUninitialized = 0;
    int32_t playerDrawing = 0;
    int32_t frameSelected = 0;
    int32_t frameNotDrawing = 0;
    int32_t frameNotInitialized = 0;
    int32_t frameAlly = 0;
    int32_t frameDistance = 0;
    int32_t framePosition = 0;

    // engine.ActorsCount at sample time; -1 when not observable (lock contended).
    int32_t actorCount = -1;

    // Open misses by reason (-1 open rows means "not observable").
    int32_t missOpen = -1;
    std::array<uint32_t, kReasonCount> missByReason{};

    // Last Esp frame result and failure streak (engine.m_espFrameDiagnostics).
    int32_t lastResult = 0;
    int32_t consecutiveFailures = 0;
    uint64_t lastFailureMs = 0;

    // Cumulative DMA scatter telemetry (Core/Memory.h).
    uint64_t dmaScatterPrepares = 0;
    uint64_t dmaScatterExecutes = 0;
    uint64_t dmaScatterLastBatch = 0;

};

// ── Publication (worker thread) ───────────────────────────────────────────

inline std::atomic<std::shared_ptr<const FrameSample>>& LatestSample()
{
    static std::atomic<std::shared_ptr<const FrameSample>> sample{ nullptr };
    return sample;
}

inline std::atomic<uint64_t>& PublishedCount()
{
    static std::atomic<uint64_t> count{ 0 };
    return count;
}

inline std::atomic<bool>& WriterReady()
{
    static std::atomic<bool> ready{ false };
    return ready;
}

// Called from the frame-collect worker. Cheap: one allocation for a ~200 byte
// POD plus an atomic swap — the same cost class as publishing the render frame
// itself, and it never touches a file or waits on the writer.
inline void Publish(FrameSample sample)
{
    if (!WriterReady().load(std::memory_order_acquire))
        return;
    sample.sampleStampMs = SteadyNowMs();
    std::shared_ptr<const FrameSample> boxed =
        std::make_shared<const FrameSample>(sample);
    LatestSample().store(std::move(boxed), std::memory_order_release);
    PublishedCount().fetch_add(1, std::memory_order_relaxed);
}

inline std::shared_ptr<const FrameSample> Latest()
{
    return LatestSample().load(std::memory_order_acquire);
}

// ── Input accounting (device-level truth) ─────────────────────────────────
//
// "Submitted" means the command was handed to the device layer (MAKCU serial or
// KmBoxNet UDP). Counted at the device calls in Hardware/KmBox.cpp so the number
// is the real one, not an intent. "Rejected" belongs to the Phase 3 single input
// gate: the counter exists and is wired now, and stays 0 until that gate lands.
// pendingRelease is true while a press has been sent and no release has followed.

inline std::atomic<uint64_t>& InputSubmitted()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}
inline std::atomic<uint64_t>& InputMovesSubmitted()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}
inline std::atomic<uint64_t>& InputPressesSubmitted()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}
inline std::atomic<uint64_t>& InputRejected()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}
inline std::atomic<bool>& PressOutstanding()
{
    static std::atomic<bool> v{ false };
    return v;
}

inline void NoteInputMoveSubmitted()
{
    InputSubmitted().fetch_add(1, std::memory_order_relaxed);
    InputMovesSubmitted().fetch_add(1, std::memory_order_relaxed);
}
inline void NoteInputPressSubmitted()
{
    InputSubmitted().fetch_add(1, std::memory_order_relaxed);
    InputPressesSubmitted().fetch_add(1, std::memory_order_relaxed);
}
inline void NoteInputReleaseSubmitted()
{
    // A release is a device command but not a press; it is counted in the total
    // so "submitted" means everything handed to the device.
    InputSubmitted().fetch_add(1, std::memory_order_relaxed);
}
inline void NoteInputRejected()
{
    InputRejected().fetch_add(1, std::memory_order_relaxed);
}

// ── Serialization (pure — no state, worker thread only) ───────────────────

inline std::string Escape(std::string_view value, size_t maxLength = 96)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char ch : value.substr(0, maxLength)) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[8]{};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                out += escaped;
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    return out;
}

inline std::string JsonFloat(double value)
{
    if (!std::isfinite(value))
        return "null";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

struct WriterStats {
    uint64_t writesAttempted = 0;
    uint64_t writesFailed = 0;
    uint64_t lastWriteUtcMs = 0;
    std::string lastError;
};

inline std::string FormatReportJson(const FrameSample* s, const WriterStats& stats)
{
    const uint64_t nowSteady = SteadyNowMs();
    std::ostringstream out;
    out.imbue(std::locale::classic());

    out << "{\"schema\":1"
        << ",\"generatedUtcMs\":" << SystemNowMs()
        << ",\"pid\":" << static_cast<unsigned long long>(GetCurrentProcessId())
        << ",\"publishedSamples\":"
        << static_cast<unsigned long long>(
            PublishedCount().load(std::memory_order_relaxed));

    out << ",\"frame\":";
    if (!s) {
        // No frame published yet (pre-attach, pre-raid, or every collect failed).
        // Absent, never a placeholder frame of zeros.
        out << "null";
    } else {
        const bool haveCollect = s->collectStampMs != 0 && nowSteady >= s->collectStampMs;
        const int64_t collectAge = haveCollect
            ? static_cast<int64_t>(nowSteady - s->collectStampMs) : -1;
        const bool haveFailure = s->lastFailureMs != 0 && nowSteady >= s->lastFailureMs;
        const int64_t failureAge = haveFailure
            ? static_cast<int64_t>(nowSteady - s->lastFailureMs) : -1;
        const bool fresh = collectAge >= 0
            && static_cast<uint64_t>(collectAge) <= EspFramePolicy::kMaxFrameAgeMs;

        out << "{\"frameSeq\":" << s->frameSeq
            << ",\"worldGeneration\":" << s->worldGeneration
            << ",\"frameAgeMs\":" << collectAge
            << ",\"dataFresh\":" << (fresh ? "true" : "false")
            << ",\"sampleAgeMs\":" << (nowSteady >= s->sampleStampMs
                ? static_cast<int64_t>(nowSteady - s->sampleStampMs) : -1)
            << ",\"collectMilliseconds\":" << JsonFloat(s->collectMs)
            << ",\"readMilliseconds\":" << JsonFloat(s->readMs)
            << ",\"discoveryMilliseconds\":" << JsonFloat(s->discoveryMs)
            << ",\"boneMilliseconds\":" << JsonFloat(s->boneMs)
            << ",\"cacheWaitMilliseconds\":" << JsonFloat(s->cacheWaitMs)
            << ",\"cameraValid\":" << (s->cameraValid ? "true" : "false")
            << ",\"cameraFov\":" << JsonFloat(s->camFov)
            << ",\"lastResult\":" << s->lastResult
            << ",\"consecutiveFailures\":" << s->consecutiveFailures
            << ",\"failureAgeMs\":" << failureAge
            << "}";

        out << ",\"counts\":{\"draw\":" << s->drawCount
            << ",\"players\":" << s->playerCount
            << ",\"robots\":" << s->robotCount
            << ",\"world\":" << s->worldCount
            << ",\"actors\":" << s->actorCount << "}";

        out << ",\"playerPipeline\":";
        if (!s->playerDiagValid) {
            out << "null";
        } else {
            out << "{\"initialized\":" << s->playerInitialized
                << ",\"uninitialized\":" << s->playerUninitialized
                << ",\"drawing\":" << s->playerDrawing
                << ",\"frameSelected\":" << s->frameSelected
                << ",\"frameNotDrawing\":" << s->frameNotDrawing
                << ",\"frameNotInitialized\":" << s->frameNotInitialized
                << ",\"frameAllyHidden\":" << s->frameAlly
                << ",\"frameDistance\":" << s->frameDistance
                << ",\"framePosition\":" << s->framePosition << "}";
        }

        out << ",\"misses\":";
        if (s->missOpen < 0) {
            out << "null";
        } else {
            out << "{\"open\":" << s->missOpen << ",\"byReason\":{";
            bool first = true;
            for (size_t i = 1; i < kReasonCount; ++i) {
                const uint32_t count = s->missByReason[i];
                if (count == 0)
                    continue;
                if (!first)
                    out << ",";
                first = false;
                out << "\"" << PlayerEspMiss::ReasonName(
                    static_cast<PlayerEspMiss::Reason>(i)) << "\":" << count;
            }
            out << "}}";
        }

        out << ",\"dmaScatter\":{\"prepares\":" << s->dmaScatterPrepares
            << ",\"executes\":" << s->dmaScatterExecutes
            << ",\"lastBatch\":" << s->dmaScatterLastBatch << "}";

    }

    out << ",\"input\":{\"submitted\":"
        << static_cast<unsigned long long>(
            InputSubmitted().load(std::memory_order_relaxed))
        << ",\"moves\":"
        << static_cast<unsigned long long>(
            InputMovesSubmitted().load(std::memory_order_relaxed))
        << ",\"presses\":"
        << static_cast<unsigned long long>(
            InputPressesSubmitted().load(std::memory_order_relaxed))
        << ",\"rejected\":"
        << static_cast<unsigned long long>(
            InputRejected().load(std::memory_order_relaxed))
        << ",\"pendingRelease\":"
        << (PressOutstanding().load(std::memory_order_relaxed) ? "true" : "false")
        << "}";

    out << ",\"writer\":{\"writesAttempted\":" << stats.writesAttempted
        << ",\"writesFailed\":" << stats.writesFailed
        << ",\"lastWriteUtcMs\":" << stats.lastWriteUtcMs
        << ",\"lastError\":\"" << Escape(stats.lastError) << "\"}";

    // The read-only declaration. Phase 0's inventory found no write call site
    // under Project/ outside Tests/; this field records that claim where the
    // other evidence is, so a reader does not have to trust the changelog.
    out << ",\"readOnly\":{\"memoryWritesPerformed\":false,"
           "\"basis\":\"phase0-write-inventory: no write call site in Project/ outside Tests/\"}";

    out << "}\n";
    return out.str();
}

// ── Writer (own thread; never the paint path) ─────────────────────────────

class Writer {
public:
    static Writer& Instance()
    {
        static Writer writer;
        return writer;
    }

    void Init()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running.load(std::memory_order_acquire))
            return;
        m_stopRequested.store(false, std::memory_order_release);
        m_path = ResolvePath();
        m_tempPath = m_path;
        m_tempPath += L".tmp";
        m_worker = std::thread([this] { Run(); });
        m_running.store(true, std::memory_order_release);
        WriterReady().store(true, std::memory_order_release);
    }

    void Shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running.load(std::memory_order_acquire))
                return;
            // Stop accepting samples first, then let the writer land its final
            // report (a stopped writer must not publish a frame after close).
            WriterReady().store(false, std::memory_order_release);
            m_stopRequested.store(true, std::memory_order_release);
        }
        m_wake.notify_all();
        if (m_worker.joinable())
            m_worker.join();
        m_running.store(false, std::memory_order_release);
    }

    std::filesystem::path GetPath() const { return m_path; }

private:
    ~Writer() { Shutdown(); }
    Writer() = default;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_worker;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopRequested{ false };
    std::filesystem::path m_path;
    std::filesystem::path m_tempPath;

    // Writer-thread-owned: no other thread reads these.
    WriterStats m_stats{};

    static std::filesystem::path ResolvePath()
    {
        wchar_t exePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
            return std::filesystem::path(L"help") / L"run_report.json";
        const std::filesystem::path exeDir =
            std::filesystem::path(exePath).parent_path();
        return exeDir.parent_path() / L"help" / L"run_report.json";
    }

    void FailWrite(const std::string& message)
    {
        ++m_stats.writesFailed;
        m_stats.lastError = message;
        std::string line = "[runreport] " + message + "\n";
        OutputDebugStringA(line.c_str());
        EntityDiagnostics::LogError(
            "RunReport", message.empty() ? "write failed" : message);
    }

    void WriteOnce()
    {
        const std::shared_ptr<const FrameSample> sample = Latest();
        const std::string json = FormatReportJson(sample.get(), m_stats);
        ++m_stats.writesAttempted;
        if (json.size() > kMaxReportBytes) {
            FailWrite("report exceeds size bound");
            return;
        }

        std::ofstream file(m_tempPath,
            std::ios::out | std::ios::trunc | std::ios::binary);
        if (!file) {
            FailWrite("could not open " + m_tempPath.string());
            return;
        }
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!file) {
            FailWrite("write failed");
            return;
        }
        file.flush();
        if (!file) {
            FailWrite("flush failed");
            return;
        }
        file.close();
        if (file.fail()) {
            FailWrite("close failed");
            return;
        }

        // Replace, so a reader between writes never sees a partial document.
        std::error_code ec;
        std::filesystem::rename(m_tempPath, m_path, ec);
        if (ec) {
            FailWrite("replace failed: " + ec.message());
            std::error_code cleanup;
            std::filesystem::remove(m_tempPath, cleanup);
            return;
        }
        m_stats.lastWriteUtcMs = static_cast<uint64_t>(SystemNowMs());
        m_stats.lastError.clear();
    }

    void Run()
    {
        std::error_code ec;
        const std::filesystem::path dir = m_path.parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir, ec);
            if (ec)
                FailWrite("could not create " + dir.string() + ": " + ec.message());
        }

        while (true) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait_for(lock, std::chrono::milliseconds(kWriteIntervalMs),
                    [this] {
                        return m_stopRequested.load(std::memory_order_acquire);
                    });
                if (m_stopRequested.load(std::memory_order_acquire))
                    break;
            }
            WriteOnce();
        }

        // Final report: the write is intentional after stop (last observed
        // state), while Publish() has already stopped accepting samples.
        WriteOnce();
    }
};

inline void Init() { Writer::Instance().Init(); }
inline void Shutdown() { Writer::Instance().Shutdown(); }
inline std::filesystem::path GetPath() { return Writer::Instance().GetPath(); }

} // namespace RunReport
