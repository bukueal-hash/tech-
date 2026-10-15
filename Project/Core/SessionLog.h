#pragma once

#include <filesystem>

namespace SessionLog {

/** Redirect std::cout to help/session_log.txt (truncate each launch). */
void Init();

/** Flush/close and restore std::cout after all logging workers have joined. */
void Shutdown();

std::filesystem::path GetPath();

} // namespace SessionLog
