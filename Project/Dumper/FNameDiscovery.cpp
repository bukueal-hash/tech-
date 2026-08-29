#include "FNameDiscovery.h"

#include "../Core/Offsets.h"
#include "../Core/SteamDecrypt.hpp"
#include "../DMA/Memory.h"

#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <set>
#include <sstream>

namespace Dumper {

bool ApplyFNameGlobals(uint64_t moduleBase, const FNameGlobals& g)
{
    // v20260818 pipeline: GNamePool RVA + keystream only — the Aug 18 build has
    // no SIMD block mask, so the blockMask* fields are ignored.
    if (!g.poolRva || !g.keyTableRva)
        return false;

    steam_decrypt::ClearNameCache();
    steam_decrypt::FNameState& s = steam_decrypt::fname_state();

    // Inject the runtime-discovered pool RVA + keystream into the GNames
    // (v20260818) pipeline state so CI resolution uses them.
    GNames::gRuntimePoolRva = g.poolRva;
    s.gnamePoolRva = g.poolRva;
    s.keystreamBase = GNames::KeystreamBase;

    uint8_t ksBuf[GNames::KeystreamCount * 2]{};
    if (!PCIMemory::ReadVirtualMemory(
            static_cast<uintptr_t>(moduleBase + g.keyTableRva), ksBuf, sizeof(ksBuf)))
        return false;

    int nz = 0;
    for (int i = 0; i < GNames::KeystreamCount; ++i) {
        std::memcpy(&GNames::keyTable[i], ksBuf + i * 2, 2);
        nz += (GNames::keyTable[i] != 0);
    }
    if (nz < 8)
        return false;

    GNames::ready = true;
    s.ksLoaded = true;
    s.initialised = true;
    return true;
}

namespace {

constexpr int32_t kFUObjectItemSize = 0x18;
constexpr int32_t kMaxChunksProbe = 8;
constexpr size_t kMaxMaskTrials = 80;

struct MaskTrial {
    uint64_t rva = 0;
    uint64_t direct = 0;
};

bool ValidPtr(uint64_t p)
{
    return p >= 0x10000ULL && p <= 0x7FFFFFFFFFFFULL;
}

bool PlausibleMaskValue(uint64_t mask)
{
    if (!mask || mask == 0xFFFFFFFFFFFFFFFFULL)
        return false;
    if (mask >= 0x10000000000ULL && mask <= 0x7FFFFFFFFFFFULL)
        return false;
    int asciiPairs = 0;
    for (int i = 0; i < 4; ++i) {
        const unsigned char lo = static_cast<unsigned char>((mask >> (i * 16)) & 0xFF);
        const unsigned char hi = static_cast<unsigned char>((mask >> (i * 16 + 8)) & 0xFF);
        if (hi == 0 && lo >= 0x20 && lo < 0x7F)
            ++asciiPairs;
    }
    if (asciiPairs >= 3)
        return false;
    const int pop = static_cast<int>(__popcnt64(mask));
    return pop >= 8 && pop <= 56;
}

void AddUniqueDirect(std::vector<uint64_t>& list, uint64_t value)
{
    if (!PlausibleMaskValue(value))
        return;
    for (uint64_t v : list) {
        if (v == value)
            return;
    }
    list.push_back(value);
}

void AddMaskTrial(std::vector<MaskTrial>& trials, uint64_t rva, uint64_t direct)
{
    if (!direct && !rva)
        return;
    for (const MaskTrial& t : trials) {
        if (t.rva == rva && t.direct == direct)
            return;
    }
    trials.push_back({ rva, direct });
}

void CollectSampleObjectPtrs(uint64_t moduleBase, uint64_t gObjectRva, std::vector<uint64_t>& out, size_t maxObjs)
{
    out.clear();
    if (!gObjectRva)
        return;

    const uint64_t arr = moduleBase + gObjectRva;
    const uint64_t objects = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(arr + 0x00));
    if (!ValidPtr(objects))
        return;

    for (int32_t chunk = 0; chunk < kMaxChunksProbe && out.size() < maxObjs; ++chunk) {
        const uint64_t chunkPtr = PCIMemory::Read<uint64_t>(
            static_cast<uintptr_t>(objects + static_cast<uint64_t>(chunk) * 8ULL));
        if (!ValidPtr(chunkPtr))
            break;

        for (int32_t idx = 0; idx < 128 && out.size() < maxObjs; ++idx) {
            const uint64_t item = chunkPtr + static_cast<uint64_t>(idx) * kFUObjectItemSize;
            const uint64_t obj = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(item));
            if (ValidPtr(obj))
                out.push_back(obj);
        }
    }
}

int ScoreNamesOnObjectsQuick(uint64_t moduleBase, const std::vector<uint64_t>& objects)
{
    if (!steam_decrypt::fname_state().initialised)
        return 0;

    int score = 0;
    for (uint64_t obj : objects) {
        const std::string name = steam_decrypt::GetActorFNameString(static_cast<uintptr_t>(obj));
        if (name.empty() || !steam_decrypt::IsPlausibleFNameText(name))
            continue;
        ++score;
        if (score >= 3)
            break;
    }
    return score;
}

int ScoreFNameIndicesQuick(uint64_t moduleBase)
{
    if (!steam_decrypt::fname_state().initialised)
        return 0;

    static const char* kCore[] = { "None", "Object", "Class", "Package", "Engine" };

    for (int32_t ci = 1; ci <= 4096; ++ci) {
        const std::string name = steam_decrypt::CachedNameString(ci, moduleBase);
        if (name.empty() || !steam_decrypt::IsPlausibleFNameText(name))
            continue;
        for (const char* known : kCore) {
            if (name == known)
                return (name == std::string("None")) ? 50 : 20;
        }
        if (ci <= 256)
            return 5;
    }
    return 0;
}

void AddUniqueRva(std::vector<uint64_t>& list, uint64_t rva)
{
    if (!rva)
        return;
    for (uint64_t v : list) {
        if (v == rva)
            return;
    }
    list.push_back(rva);
}

void AddLimitedNeighborhood(std::vector<uint64_t>& candidates, uint64_t anchor)
{
    for (int64_t delta = -0x1000; delta <= 0x1000; delta += 0x200) {
        AddUniqueRva(candidates, static_cast<uint64_t>(static_cast<int64_t>(anchor) + delta));
        if (candidates.size() >= kMaxMaskTrials)
            break;
    }
}

} // namespace

int ScoreFNameQuick(uint64_t moduleBase, const FNameGlobals& globals, uint64_t gObjectChunksRva)
{
    if (!ApplyFNameGlobals(moduleBase, globals))
        return 0;

    int score = ScoreFNameIndicesQuick(moduleBase);
    if (score >= 50)
        return score;

    std::vector<uint64_t> objects;
    CollectSampleObjectPtrs(moduleBase, gObjectChunksRva, objects, 16);
    score += ScoreNamesOnObjectsQuick(moduleBase, objects);
    return score;
}

int ScoreFNamePipeline(uint64_t moduleBase, const FNameGlobals& globals, uint64_t gObjectChunksRva)
{
    return ScoreFNameQuick(moduleBase, globals, gObjectChunksRva);
}

std::vector<uint64_t> ExtractBlockMaskCandidatesFromCode(const ModuleImageCache& cache, uint64_t moduleBase)
{
    std::vector<uint64_t> candidates;

    auto vaToRva = [&](uint64_t va) -> uint64_t {
        if (va >= moduleBase)
            return va - moduleBase;
        if (va >= cache.BaseVa())
            return va - cache.BaseVa();
        return 0;
    };

    const char* patterns[] = {
        "48 8B 05 ? ? ? ? 48 31 C1 48 C1 C1",
        "48 8B 0D ? ? ? ? 48 31 C9",
        "48 8B 05 ? ? ? ? 48 31 C8",
        "48 33 05 ? ? ? ? 48 C1 C0 0D",
        "48 33 0D ? ? ? ? 48 C1 C0 0D",
        "66 0F 70 ? 93 48 33",
        "48 C1 C0 0D 66 0F 70 ? 93 48 33",
        "48 8B 05 ? ? ? ? 48 35",
    };

    for (const char* patStr : patterns) {
        const auto pat = ParsePattern(patStr);
        const auto hits = FindAllMatches(cache.Data(), cache.Size(), cache.BaseVa(), pat);
        for (uint64_t hit : hits) {
            const uint64_t va7 = ResolveRipRelativeVa(
                cache.Data(), cache.Size(), cache.BaseVa(), hit, 3, 7);
            const uint64_t rva = vaToRva(va7);
            if (rva)
                AddUniqueRva(candidates, rva);

            const size_t off = static_cast<size_t>(hit - cache.BaseVa());
            if (off + 10 < cache.Size()) {
                const uint8_t* ip = cache.Data() + off;
                if (ip[0] == 0x48 && ip[1] == 0x35 && off + 10 <= cache.Size()) {
                    uint64_t imm = 0;
                    std::memcpy(&imm, ip + 2, sizeof(uint64_t));
                    if (imm && imm != 0xFFFFFFFFFFFFFFFFULL)
                        AddUniqueRva(candidates, Offsets::FNameBlockMaskRva);
                }
            }
        }
    }

    return candidates;
}

std::vector<uint64_t> ExtractBlockMaskDirectFromLive(const ModuleImageCache& cache, uint64_t moduleBase)
{
    std::vector<uint64_t> masks;
    if (!moduleBase || !cache.Data() || cache.Size() < 32)
        return masks;

    const uint8_t* buf = cache.Data();
    const size_t toRead = cache.Size();
    const char* patterns[] = {
        "48 8B 05 ? ? ? ? 48 31 C1 48 C1 C1",
        "48 8B 05 ? ? ? ? 48 35",
        "48 33 05 ? ? ? ? 48 C1 C0 0D",
    };
    for (const char* patStr : patterns) {
        const auto pat = ParsePattern(patStr);
        const auto hits = FindAllMatches(buf, toRead, cache.BaseVa(), pat);
        for (uint64_t hit : hits) {
            const uint64_t va7 = ResolveRipRelativeVa(
                buf, toRead, cache.BaseVa(), hit, 3, 7);
            if (!va7 || va7 < moduleBase)
                continue;
            const uint64_t maskRva = va7 - moduleBase;
            uint64_t maskVal = 0;
            if (PCIMemory::ReadVirtualMemory(
                    static_cast<uintptr_t>(moduleBase + maskRva), &maskVal, sizeof(maskVal)))
                AddUniqueDirect(masks, maskVal);
        }
    }

    return masks;
}

bool ValidateGNamePoolWithObjects(
    uint64_t moduleBase,
    const FNameGlobals& globals,
    uint64_t gObjectChunksRva,
    std::string& notes)
{
    const int score = ScoreFNameQuick(moduleBase, globals, gObjectChunksRva);
    if (score < 1) {
        notes = "FName score too low (" + std::to_string(score) + ")";
        return false;
    }
    notes = "FName score=" + std::to_string(score);
    return true;
}

uint64_t DiscoverBlockMaskRva(
    uint64_t moduleBase,
    const ModuleImageCache& cache,
    FNameGlobals& partial,
    uint64_t gObjectChunksRva,
    DumperState& state)
{
    if (!partial.poolRva || !partial.keyTableRva || !gObjectChunksRva)
        return 0;

    std::vector<MaskTrial> trials;

    uint64_t staleDirect = 0;
    if (PCIMemory::ReadVirtualMemory(
            static_cast<uintptr_t>(moduleBase + Offsets::FNameBlockMaskRva),
            &staleDirect, sizeof(staleDirect))
        && PlausibleMaskValue(staleDirect)) {
        AddMaskTrial(trials, Offsets::FNameBlockMaskRva, staleDirect);
        state.AppendLog("BlockMask: stale RVA direct=" + ([&]() {
            std::ostringstream o;
            o << std::hex << staleDirect << std::dec;
            return o.str();
        })());
    }

    const uint64_t moduleLo = moduleBase > 0x200000 ? moduleBase - 0x200000 : moduleBase;
    const uint64_t moduleHi = moduleBase + 0x20000000ULL;
    for (uint64_t direct : ExtractBlockMaskDirectFromLive(cache, moduleBase)) {
        if (direct >= moduleLo && direct <= moduleHi)
            continue;
        AddMaskTrial(trials, 0, direct);
    }

    std::vector<uint64_t> rvaCandidates;
    AddUniqueRva(rvaCandidates, Offsets::FNameBlockMaskRva);
    for (uint64_t rva : ExtractBlockMaskCandidatesFromCode(cache, moduleBase))
        AddUniqueRva(rvaCandidates, rva);
    AddLimitedNeighborhood(rvaCandidates, Offsets::FNameBlockMaskRva);

    for (uint64_t rva : rvaCandidates)
        AddMaskTrial(trials, rva, 0);

    if (trials.size() > kMaxMaskTrials)
        trials.resize(kMaxMaskTrials);

    state.AppendLog("BlockMask: testing " + std::to_string(trials.size()) + " trials (capped)");

    uint64_t bestRva = 0;
    uint64_t bestDirect = 0;
    int bestScore = 0;
    size_t tested = 0;
    const size_t totalTrials = trials.size();

    FNameGlobals trial = partial;
    for (const MaskTrial& mt : trials) {
        if (state.IsCancelRequested())
            break;

        trial.blockMaskRva = mt.rva;
        trial.blockMaskDirect = mt.direct;
        if (!trial.blockMaskDirect && trial.blockMaskRva) {
            uint64_t readMask = 0;
            if (PCIMemory::ReadVirtualMemory(
                    static_cast<uintptr_t>(moduleBase + trial.blockMaskRva),
                    &readMask, sizeof(readMask))
                && PlausibleMaskValue(readMask))
                trial.blockMaskDirect = readMask;
        }

        const int score = ScoreFNameQuick(moduleBase, trial, gObjectChunksRva);
        ++tested;

        if (tested % 10 == 0 || tested == totalTrials) {
            std::ostringstream oss;
            oss << "BlockMask: trial " << tested << "/" << totalTrials;
            if (mt.direct)
                oss << " direct=" << std::hex << mt.direct << std::dec;
            else
                oss << " rva=" << std::hex << mt.rva << std::dec;
            oss << " score=" << score;
            state.AppendLog(oss.str());
        }

        if (score > bestScore) {
            bestScore = score;
            bestRva = mt.rva ? mt.rva : Offsets::FNameBlockMaskRva;
            bestDirect = trial.blockMaskDirect;
        }
        if (bestScore >= 50)
            break;
    }

    state.AppendLog("BlockMask: done tested=" + std::to_string(tested)
                    + " best score=" + std::to_string(bestScore));

    if (bestScore >= 1 && (bestRva || bestDirect)) {
        partial.blockMaskDirect = bestDirect;
        partial.blockMaskRva = bestRva;
        std::ostringstream oss;
        oss << "BlockMask: best";
        if (bestDirect)
            oss << " direct=" << std::hex << bestDirect << std::dec;
        if (bestRva)
            oss << " rva=" << std::hex << bestRva << std::dec;
        oss << " score=" << bestScore;
        state.AppendLog(oss.str());
        return bestRva ? bestRva : Offsets::FNameBlockMaskRva;
    }

    state.AppendLog("BlockMask: threshold miss");
    return 0;
}

} // namespace Dumper
