#pragma once

#include "DumperState.h"
#include "GlobalDiscovery.h"

#include <filesystem>
#include <string>

namespace Dumper {

struct SdkDumpResult {
    size_t classesWritten = 0;
    std::string summary;
};

SdkDumpResult RunSdkDump(
    uint64_t moduleBase,
    const DiscoveredGlobals& globals,
    const std::filesystem::path& outPath,
    DumperState& state);

} // namespace Dumper
