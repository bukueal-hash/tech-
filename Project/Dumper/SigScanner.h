#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Dumper {

struct PatternByte {
    uint8_t value = 0;
    bool wildcard = false;
};

std::vector<PatternByte> ParsePattern(const char* signature);

std::vector<uint64_t> FindAllMatches(
    const uint8_t* data,
    size_t size,
    uint64_t baseVa,
    const std::vector<PatternByte>& pattern);

uint64_t ResolveRipRelativeVa(
    const uint8_t* data,
    size_t size,
    uint64_t baseVa,
    uint64_t matchVa,
    int dispOffset,
    int insnSize);

} // namespace Dumper
