#pragma once

#include "ModuleImageCache.h"
#include "DumperState.h"
#include "SigScanner.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Dumper {

struct FNameGlobals {
    uint64_t poolRva = 0;
    uint64_t keyTableRva = 0;
    uint64_t blockMaskRva = 0;
    /** When non-zero, use as block mask value instead of reading blockMaskRva. */
    uint64_t blockMaskDirect = 0;
};

/** Apply pool/key/mask RVAs into live FName state. */
bool ApplyFNameGlobals(uint64_t moduleBase, const FNameGlobals& g);

/** Fast FName probe (None + a few core names). */
int ScoreFNameQuick(uint64_t moduleBase, const FNameGlobals& globals, uint64_t gObjectChunksRva);

/** Legacy full pipeline score. */
int ScoreFNamePipeline(uint64_t moduleBase, const FNameGlobals& globals, uint64_t gObjectChunksRva);

/** Extract block-mask candidate RVAs from code patterns. */
std::vector<uint64_t> ExtractBlockMaskCandidatesFromCode(const ModuleImageCache& cache, uint64_t moduleBase);

/** Find block mask RVA via code patterns + fast FName scoring. */
uint64_t DiscoverBlockMaskRva(
    uint64_t moduleBase,
    const ModuleImageCache& cache,
    FNameGlobals& partial,
    uint64_t gObjectChunksRva,
    DumperState& state);

/** Validate GName pool by resolving names from GObject samples. */
bool ValidateGNamePoolWithObjects(
    uint64_t moduleBase,
    const FNameGlobals& globals,
    uint64_t gObjectChunksRva,
    std::string& notes);

} // namespace Dumper
