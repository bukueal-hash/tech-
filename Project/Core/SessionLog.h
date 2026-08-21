#pragma once

#include <filesystem>

namespace SessionLog {

/** Redirect std::cout to help/session_log.txt (truncate each launch). */
void Init();

std::filesystem::path GetPath();

} // namespace SessionLog
