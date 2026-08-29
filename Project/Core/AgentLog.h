#pragma once

#include <cstdint>
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
