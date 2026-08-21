#pragma once

#include "DumperState.h"
#include "ModuleImageCache.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Dumper {

struct DiscoveredGlobals {
    uint64_t uWorldRva = 0;
    uint64_t gNamePoolRva = 0;
    uint64_t gUObjectChunksRva = 0;
    uint64_t fNameKeyTableRva = 0;
    uint64_t fNameBlockMaskRva = 0;
    /** When set, mask value was found in code / trial — not read from fNameBlockMaskRva. */
    uint64_t fNameBlockMaskDirect = 0;
    uint64_t fFieldNameKey0Rva = 0;
    uint64_t fFieldNameKey1Rva = 0;
    std::vector<GlobalHit> report;
};

DiscoveredGlobals DiscoverGlobals(
    uint64_t moduleBase,
    const ModuleImageCache& cache,
    DumperState& state);

bool WriteGlobalsJson(
    const std::filesystem::path& path,
    uint64_t moduleBase,
    uint32_t imageSize,
    const DiscoveredGlobals& globals);

bool AppendDumpReport(
    const std::filesystem::path& path,
    const DiscoveredGlobals& globals,
    const std::string& extra);

} // namespace Dumper
