#include "SigScanner.h"

#include <cstdlib>
#include <cstring>

namespace Dumper {

std::vector<PatternByte> ParsePattern(const char* signature)
{
    std::vector<PatternByte> out;
    if (!signature)
        return out;

    const char* p = signature;
    while (*p) {
        while (*p == ' ')
            ++p;
        if (!*p)
            break;

        PatternByte b{};
        if (p[0] == '?' && (p[1] == ' ' || p[1] == '\0' || p[1] == '?')) {
            b.wildcard = true;
            p += (p[1] == '?') ? 2 : 1;
        } else {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 16);
            if (end == p)
                break;
            b.value = static_cast<uint8_t>(v & 0xFFu);
            p = end;
        }
        out.push_back(b);
        if (*p == ' ')
            ++p;
    }
    return out;
}

std::vector<uint64_t> FindAllMatches(
    const uint8_t* data,
    size_t size,
    uint64_t baseVa,
    const std::vector<PatternByte>& pattern)
{
    std::vector<uint64_t> hits;
    if (!data || pattern.empty() || size < pattern.size())
        return hits;

    const size_t limit = size - pattern.size();
    for (size_t i = 0; i <= limit; ++i) {
        bool ok = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            if (!pattern[j].wildcard && data[i + j] != pattern[j].value) {
                ok = false;
                break;
            }
        }
        if (ok)
            hits.push_back(baseVa + i);
    }
    return hits;
}

uint64_t ResolveRipRelativeVa(
    const uint8_t* data,
    size_t size,
    uint64_t baseVa,
    uint64_t matchVa,
    int dispOffset,
    int insnSize)
{
    if (!data || matchVa < baseVa)
        return 0;
    const size_t off = static_cast<size_t>(matchVa - baseVa);
    if (off + static_cast<size_t>(dispOffset) + 4 > size)
        return 0;

    int32_t disp = 0;
    std::memcpy(&disp, data + off + static_cast<size_t>(dispOffset), sizeof(disp));
    return matchVa + static_cast<uint64_t>(insnSize) + static_cast<uint64_t>(disp);
}

} // namespace Dumper
