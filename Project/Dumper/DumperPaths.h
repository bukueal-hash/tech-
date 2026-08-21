#pragma once

#include <filesystem>
#include <string>

namespace Dumper {

std::filesystem::path GetHelpOutputDir();
std::filesystem::path GetGlobalsJsonPath();
std::filesystem::path GetDumpTxtPath();
std::filesystem::path GetDumpReportPath();
std::filesystem::path GetDumperLogPath();

} // namespace Dumper
