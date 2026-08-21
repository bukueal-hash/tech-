#include "DumperPaths.h"

#include <Windows.h>

namespace Dumper {

namespace {

std::filesystem::path GetExecutableDir()
{
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return {};
    std::filesystem::path p(exePath);
    return p.parent_path();
}

} // namespace

std::filesystem::path GetHelpOutputDir()
{
    const std::filesystem::path exeDir = GetExecutableDir();
    if (!exeDir.empty()) {
        const std::filesystem::path candidate = exeDir.parent_path() / "help";
        if (std::filesystem::exists(candidate) || std::filesystem::exists(exeDir.parent_path()))
            return candidate;
    }
    return std::filesystem::path("help");
}

std::filesystem::path GetGlobalsJsonPath()
{
    return GetHelpOutputDir() / "globals.json";
}

std::filesystem::path GetDumpTxtPath()
{
    return GetHelpOutputDir() / "dump.txt";
}

std::filesystem::path GetDumpReportPath()
{
    return GetHelpOutputDir() / "dump_report.txt";
}

std::filesystem::path GetDumperLogPath()
{
    return GetHelpOutputDir() / "dumper_log.txt";
}

} // namespace Dumper
