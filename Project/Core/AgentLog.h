#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

// Agent NDJSON instrumentation disabled — stubs only (no file I/O, no worker thread).

inline int64_t ArcAgentLogNowMs() { return 0; }

inline std::string ArcAgentLogJsonEscape(const std::string& value) { return value; }

inline void ArcAsyncLog_Start() {}
inline void ArcAsyncLog_Stop() {}
inline void ArcAsyncLog_Push(std::string, std::string) {}

inline constexpr const char* kArcDebugLogPath = "F:/Test/ARCs/debug-c190fb.log";
inline constexpr const char* kArcDebugLogPath5681af = "F:/Test/ARCs/debug-5681af.log";

/**
 * Buffered debug log append — single persistent file handle + mutex.
 * Replaces 18+ direct std::ofstream open/close calls that each hit the
 * 280+MB log file and stalled the paint thread for 1.5s per call.
 * Callers write one JSON line; this function handles the file I/O.
 */
inline void DebugLogAppend(const std::string& line)
{
    static std::mutex s_mu;
    static std::ofstream s_stream;
    std::lock_guard<std::mutex> lock(s_mu);
    if (!s_stream.is_open())
        s_stream.open(kArcDebugLogPath, std::ios::app);
    if (s_stream)
        s_stream << line;
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
