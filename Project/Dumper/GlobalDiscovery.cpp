#include "GlobalDiscovery.h"

#include "FNameDiscovery.h"
#include "SigScanner.h"
#include "WorldResolveDump.h"
#include "../Core/Engine.h"
#include "../Core/Offsets.h"
#include "../DMA/Memory.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <intrin.h>
#include <iterator>
#include <sstream>
#include <vector>

namespace Dumper {

namespace {

bool ValidPtr(uint64_t p)
{
    return p >= 0x10000ULL && p <= 0x7FFFFFFFFFFFULL;
}

struct SigTarget {
    const char* name;
    const char* pattern;
    int dispOffset;
    int insnSize;
};

GlobalHit MakeHit(const char* name, uint64_t rva, const char* confidence, const char* sig, const char* notes = "")
{
    GlobalHit h;
    h.name = name;
    h.rva = rva;
    h.confidence = confidence;
    if (sig)
        h.sig = sig;
    if (notes)
        h.notes = notes;
    return h;
}

std::string HexU64(uint64_t v)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << v;
    return oss.str();
}

uint64_t PickBestUWorld(
    const ModuleImageCache& cache,
    const SigTarget* sigs,
    size_t sigCount,
    uint64_t moduleBase,
    DumperState& state)
{
    std::vector<uint64_t> candidateRvas;
    size_t totalSigHits = 0;

    uint64_t bestRva = 0;
    int bestScore = 0;
    const char* bestSig = nullptr;

    for (size_t s = 0; s < sigCount; ++s) {
        const auto& sig = sigs[s];
        const auto pattern = ParsePattern(sig.pattern);
        const auto hits = FindAllMatches(cache.Data(), cache.Size(), cache.BaseVa(), pattern);
        totalSigHits += hits.size();

        for (uint64_t hit : hits) {
            const uint64_t globalVa = ResolveRipRelativeVa(
                cache.Data(), cache.Size(), cache.BaseVa(), hit, sig.dispOffset, sig.insnSize);
            if (!globalVa || globalVa < cache.BaseVa())
                continue;

            uint64_t rva = 0;
            if (globalVa >= moduleBase)
                rva = globalVa - moduleBase;
            else if (globalVa >= cache.BaseVa())
                rva = globalVa - cache.BaseVa();

            if (!rva)
                continue;

            candidateRvas.push_back(rva);

            UWorldScoreDetail detail;
            const int score = ScoreUWorldSlotRva(moduleBase, rva, &detail);
            {
                std::ostringstream oss;
                oss << "UWorld cand rva=" << HexU64(rva)
                    << " slot=" << HexU64(detail.slot)
                    << " world=" << HexU64(detail.world)
                    << " score=" << score
                    << " (" << detail.notes << ")";
                state.AppendLog(oss.str());
            }

            if (score > bestScore) {
                bestScore = score;
                bestRva = rva;
                bestSig = sig.pattern;
            }
        }
    }

    state.AppendLog("UWorld: sig hits=" + std::to_string(totalSigHits)
                    + " scored candidates=" + std::to_string(candidateRvas.size()));

    const uintptr_t liveWorld = engine.ResolveBestGWorld(moduleBase);
    {
        std::ostringstream oss;
        oss << "UWorld: live ResolveBestGWorld=" << HexU64(liveWorld);
        state.AppendLog(oss.str());
    }

    if (!bestRva && liveWorld && !candidateRvas.empty()) {
        const uint64_t relaxed = FindSlotRvaForLiveWorld(
            moduleBase, liveWorld, candidateRvas.data(), candidateRvas.size());
        if (relaxed) {
            bestRva = relaxed;
            bestScore = ScoreUWorldSlotRva(moduleBase, relaxed, nullptr);
            state.AppendLog("UWorld: relaxed match to live world rva=" + HexU64(relaxed)
                            + " score=" + std::to_string(bestScore));
        }
    }

    if (!bestRva && Offsets::UWorld) {
        const uint64_t slotOnly = ReadWorldSlotNocache(moduleBase, static_cast<uint64_t>(Offsets::UWorld));
        if (slotOnly) {
            bestRva = static_cast<uint64_t>(Offsets::UWorld);
            bestScore = 3;
            state.AppendLog("UWorld: slot-only fallback rva=" + HexU64(bestRva)
                            + " slot=" + HexU64(slotOnly));
        }
    }

    if (bestRva) {
        std::ostringstream oss;
        const char* conf = bestScore >= 25 ? "validated" : "relaxed";
        oss << "UWorld: picked " << HexU64(bestRva) << " score=" << bestScore << " [" << conf << "]";
        state.AppendLog(oss.str());
        if (bestSig)
            state.AppendLog(std::string("UWorld sig: ") + bestSig);
        return bestRva;
    }

    if (Offsets::UWorld) {
        UWorldScoreDetail fb;
        const int fbScore = ScoreUWorldSlotRva(moduleBase, static_cast<uint64_t>(Offsets::UWorld), &fb);
        std::ostringstream oss;
        oss << "UWorld: Offsets.h fallback rva=" << HexU64(static_cast<uint64_t>(Offsets::UWorld))
            << " slot=" << HexU64(fb.slot) << " world=" << HexU64(fb.world)
            << " score=" << fbScore;
        state.AppendLog(oss.str());
        if (fbScore > 0) {
            state.AppendLog("UWorld: using Offsets.h RVA");
            return static_cast<uint64_t>(Offsets::UWorld);
        }
    }

    if (liveWorld) {
        const uint64_t gsWorld = TryWorldFromGameStateGlobalDump(moduleBase);
        if (gsWorld == liveWorld)
            state.AppendLog("UWorld: live world via GameStateGlobal but slot RVA unknown");
    }

    state.AppendLog("UWorld: no validated slot RVA");
    return 0;
}

bool ValidateGNamePool(uint64_t moduleBase, uint64_t rva, std::string& notes)
{
    uint8_t buf[64]{};
    if (!PCIMemory::ReadVirtualMemory(static_cast<uintptr_t>(moduleBase + rva), buf, sizeof(buf))) {
        notes = "pool read failed";
        return false;
    }
    int nz = 0;
    for (uint8_t b : buf)
        if (b)
            ++nz;
    if (nz < 4) {
        notes = "pool bytes mostly zero";
        return false;
    }
    notes = "pool region non-zero";
    return true;
}

bool ValidateKeyTable(uint64_t moduleBase, uint64_t rva, std::string& notes)
{
    uint16_t keys[64]{};
    if (!PCIMemory::ReadVirtualMemory(
            static_cast<uintptr_t>(moduleBase + rva), keys, sizeof(keys))) {
        notes = "key table read failed";
        return false;
    }
    int nz = 0;
    for (uint16_t k : keys)
        if (k)
            ++nz;
    if (nz < 8) {
        notes = "key table too sparse";
        return false;
    }
    notes = "key table populated";
    return true;
}

bool ValidateGUObject(uint64_t moduleBase, uint64_t rva, std::string& notes)
{
    const uint64_t arr = moduleBase + rva;
    const int32_t numElements = PCIMemory::Read<int32_t>(static_cast<uintptr_t>(arr + 0x14));
    const int32_t numChunks = PCIMemory::Read<int32_t>(static_cast<uintptr_t>(arr + 0x1C));
    if (numElements <= 0 || numElements > 5000000) {
        notes = "NumElements out of range";
        return false;
    }
    if (numChunks <= 0 || numChunks > 256) {
        notes = "NumChunks out of range";
        return false;
    }
    const uint64_t objects = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(arr + 0x00));
    if (!ValidPtr(objects)) {
        notes = "Objects chunk table invalid";
        return false;
    }
    const uint64_t chunk0 = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(objects));
    if (!ValidPtr(chunk0)) {
        notes = "chunk[0] invalid";
        return false;
    }
    notes = "GUObject layout ok";
    return true;
}

bool ValidateGUObjectRelaxed(uint64_t moduleBase, uint64_t rva, std::string& notes)
{
    const uint64_t arr = moduleBase + rva;
    const uint64_t objects = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(arr + 0x00));
    if (!ValidPtr(objects)) {
        notes = "Objects ptr invalid";
        return false;
    }
    const uint64_t chunk0 = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(objects));
    if (!ValidPtr(chunk0)) {
        notes = "chunk[0] invalid";
        return false;
    }

    int validObjs = 0;
    for (int i = 0; i < 16; ++i) {
        const uint64_t obj = PCIMemory::Read<uint64_t>(
            static_cast<uintptr_t>(chunk0 + static_cast<uint64_t>(i) * 0x18ULL));
        if (ValidPtr(obj))
            ++validObjs;
    }
    if (validObjs < 2) {
        notes = "too few object slots in chunk0";
        return false;
    }
    notes = "relaxed chunk object scan ok";
    return true;
}

bool ValidateBlockMask(uint64_t moduleBase, uint64_t rva, std::string& notes)
{
    const uint64_t mask = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(moduleBase + rva));
    if (!mask) {
        notes = "mask zero";
        return false;
    }
    if (mask == 0xFFFFFFFFFFFFFFFFULL) {
        notes = "mask all-ones";
        return false;
    }
    const int pop = static_cast<int>(__popcnt64(mask));
    if (pop < 8 || pop > 56) {
        notes = "mask entropy weak";
        return false;
    }
    notes = "mask entropy ok";
    return true;
}

uint64_t PickBestRvaAllHits(
    const ModuleImageCache& cache,
    uint64_t moduleBase,
    const SigTarget& target,
    const std::function<bool(uint64_t, std::string&)>& validate,
    DumperState& state)
{
    const auto pattern = ParsePattern(target.pattern);
    const auto hits = FindAllMatches(cache.Data(), cache.Size(), cache.BaseVa(), pattern);
    if (hits.empty())
        return 0;

    std::ostringstream oss;
    oss << target.name << ": " << hits.size() << " sig hit(s)";
    state.AppendLog(oss.str());

    auto globalToRva = [&](uint64_t globalVa) -> uint64_t {
        if (!globalVa)
            return 0;
        if (globalVa >= moduleBase)
            return globalVa - moduleBase;
        if (globalVa >= cache.BaseVa())
            return globalVa - cache.BaseVa();
        return 0;
    };

    for (uint64_t hit : hits) {
        const uint64_t globalVa = ResolveRipRelativeVa(
            cache.Data(), cache.Size(), cache.BaseVa(), hit, target.dispOffset, target.insnSize);
        const uint64_t rva = globalToRva(globalVa);
        if (!rva)
            continue;
        std::string notes;
        if (validate && validate(rva, notes)) {
            state.AppendLog(std::string(target.name) + ": validated " + HexU64(rva) + " (" + notes + ")");
            return rva;
        }
    }
    return 0;
}

uint64_t PickBestRva(
    const ModuleImageCache& cache,
    uint64_t moduleBase,
    const SigTarget& target,
    const std::function<bool(uint64_t, std::string&)>& validate,
    DumperState& state)
{
    const auto pattern = ParsePattern(target.pattern);
    const auto hits = FindAllMatches(cache.Data(), cache.Size(), cache.BaseVa(), pattern);
    if (hits.empty()) {
        state.AppendLog(std::string(target.name) + ": no sig hits");
        return 0;
    }

    auto globalToRva = [&](uint64_t globalVa) -> uint64_t {
        if (!globalVa)
            return 0;
        if (globalVa >= moduleBase)
            return globalVa - moduleBase;
        if (globalVa >= cache.BaseVa())
            return globalVa - cache.BaseVa();
        return 0;
    };

    for (uint64_t hit : hits) {
        const uint64_t globalVa = ResolveRipRelativeVa(
            cache.Data(), cache.Size(), cache.BaseVa(), hit, target.dispOffset, target.insnSize);
        const uint64_t rva = globalToRva(globalVa);
        if (!rva)
            continue;
        std::string notes;
        if (validate && validate(rva, notes)) {
            state.AppendLog(std::string(target.name) + ": validated " + HexU64(rva) + " (" + notes + ")");
            return rva;
        }
    }

    const uint64_t fallbackVa = ResolveRipRelativeVa(
        cache.Data(), cache.Size(), cache.BaseVa(), hits[0], target.dispOffset, target.insnSize);
    const uint64_t fallbackRva = globalToRva(fallbackVa);
    if (fallbackRva) {
        state.AppendLog(std::string(target.name) + ": using unvalidated candidate " + HexU64(fallbackRva));
        return fallbackRva;
    }
    return 0;
}

uint64_t ScanFFieldKeys(const ModuleImageCache& cache, uint64_t moduleBase, int keyIndex, DumperState& state)
{
    const char* pattern = "F3 0F 7E 05 ? ? ? ? F3 0F 7E 0D ? ? ? ?";
    const auto pat = ParsePattern(pattern);
    const auto hits = FindAllMatches(cache.Data(), cache.Size(), cache.BaseVa(), pat);
    for (uint64_t hit : hits) {
        const uint64_t key0Va = ResolveRipRelativeVa(cache.Data(), cache.Size(), cache.BaseVa(), hit, 4, 8);
        const uint64_t key1Va = ResolveRipRelativeVa(cache.Data(), cache.Size(), cache.BaseVa(), hit + 8, 4, 8);
        if (!key0Va || !key1Va)
            continue;
        const uint64_t rva0 = key0Va >= moduleBase ? key0Va - moduleBase : key0Va - cache.BaseVa();
        const uint64_t rva1 = key1Va >= moduleBase ? key1Va - moduleBase : key1Va - cache.BaseVa();
        state.AppendLog("FField name keys: " + HexU64(rva0) + " / " + HexU64(rva1));
        return (keyIndex == 0) ? rva0 : rva1;
    }
    state.AppendLog("FField name keys: sig not found");
    return 0;
}

} // namespace

DiscoveredGlobals DiscoverGlobals(
    uint64_t moduleBase,
    const ModuleImageCache& cache,
    DumperState& state)
{
    DiscoveredGlobals out;
    if (!cache.IsLoaded())
        return out;

    state.SetProgress(25);

    const SigTarget uWorldSigs[] = {
        { "UWorld", "48 8B 05 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9 74", 3, 7 },
        { "UWorld", "48 8B 1D ? ? ? ? 48 85 DB 74 ? 48 8B CB", 3, 7 },
        { "UWorld", "48 8B 05 ? ? ? ? 4D 85 C0", 3, 7 },
        { "UWorld", "48 8B 05 ? ? ? ? 48 8B 80 ? ? ? ? 48 85 C0", 3, 7 },
        { "UWorld", "48 8B 05 ? ? ? ? 48 85 C0 74 ? 48 8B 88", 3, 7 },
    };
    out.uWorldRva = PickBestUWorld(cache, uWorldSigs, std::size(uWorldSigs), moduleBase, state);
    if (out.uWorldRva) {
        const int score = ScoreUWorldSlotRva(moduleBase, out.uWorldRva, nullptr);
        const char* conf = score >= 25 ? "validated" : "relaxed";
        out.report.push_back(MakeHit("UWorld", out.uWorldRva, conf, "scored"));
    } else
        out.report.push_back(MakeHit("UWorld", 0, "failed", nullptr, "no validated hit"));

    state.SetProgress(35);

    const SigTarget gNameSigs[] = {
        { "GNamePoolRva", "48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8B D0 C1 EA 10", 3, 7 },
        { "GNamePoolRva", "48 8D 35 ? ? ? ? EB ? 48 8D 0D", 3, 7 },
    };
    for (const auto& sig : gNameSigs) {
        if (out.gNamePoolRva)
            break;
        out.gNamePoolRva = PickBestRva(cache, moduleBase, sig, [&](uint64_t rva, std::string& n) {
            return ValidateGNamePool(moduleBase, rva, n);
        }, state);
        if (out.gNamePoolRva)
            out.report.push_back(MakeHit("GNamePoolRva", out.gNamePoolRva, "validated", sig.pattern));
    }
    if (!out.gNamePoolRva)
        out.report.push_back(MakeHit("GNamePoolRva", 0, "failed", nullptr));

    state.SetProgress(45);

    const SigTarget gObjSigs[] = {
        { "GUObjectArrayChunksRva", "48 8B 05 ? ? ? ? 48 8B 0C C8 48 8D 04 D1", 3, 7 },
        { "GUObjectArrayChunksRva", "48 8B 05 ? ? ? ? 48 63 C8 48 C1 E1", 3, 7 },
        { "GUObjectArrayChunksRva", "48 8B 05 ? ? ? ? 48 8B 14 C8", 3, 7 },
        { "GUObjectArrayChunksRva", "4C 8B 05 ? ? ? ? 4D 8B 0C C0", 3, 7 },
        { "GUObjectArrayChunksRva", "48 8B 0D ? ? ? ? 48 8B 04 C1", 3, 7 },
        { "GUObjectArrayChunksRva", "48 8B 05 ? ? ? ? 48 85 C0 74 ? 8B 48 14", 3, 7 },
        { "GUObjectArrayChunksRva", "4C 8D 05 ? ? ? ? 45 33 C0 48 8B 14 C8", 3, 7 },
        { "GUObjectArrayChunksRva", "48 8B 05 ? ? ? ? 48 8B 0C C8 48 85 C9", 3, 7 },
    };
    for (const auto& sig : gObjSigs) {
        if (out.gUObjectChunksRva)
            break;
        out.gUObjectChunksRva = PickBestRvaAllHits(cache, moduleBase, sig, [&](uint64_t rva, std::string& n) {
            return ValidateGUObject(moduleBase, rva, n);
        }, state);
        if (out.gUObjectChunksRva)
            out.report.push_back(MakeHit("GUObjectArrayChunksRva", out.gUObjectChunksRva, "validated", sig.pattern));
    }
    if (!out.gUObjectChunksRva) {
        for (const auto& sig : gObjSigs) {
            if (out.gUObjectChunksRva)
                break;
            out.gUObjectChunksRva = PickBestRvaAllHits(cache, moduleBase, sig, [&](uint64_t rva, std::string& n) {
                return ValidateGUObjectRelaxed(moduleBase, rva, n);
            }, state);
            if (out.gUObjectChunksRva) {
                out.report.push_back(MakeHit(
                    "GUObjectArrayChunksRva", out.gUObjectChunksRva, "relaxed", sig.pattern, "chunk object scan"));
            }
        }
    }
    if (!out.gUObjectChunksRva)
        out.report.push_back(MakeHit("GUObjectArrayChunksRva", 0, "failed", nullptr));

    state.SetProgress(55);

    const SigTarget keyTableSigs[] = {
        { "FNameKeyTableRva", "48 8D 0D ? ? ? ? 48 8B D0 E8 ? ? ? ? 48 8B F8", 3, 7 },
        { "FNameKeyTableRva", "4C 8D 05 ? ? ? ? 48 8D 15", 3, 7 },
    };
    for (const auto& sig : keyTableSigs) {
        if (out.fNameKeyTableRva)
            break;
        out.fNameKeyTableRva = PickBestRva(cache, moduleBase, sig, [&](uint64_t rva, std::string& n) {
            return ValidateKeyTable(moduleBase, rva, n);
        }, state);
        if (out.fNameKeyTableRva)
            out.report.push_back(MakeHit("FNameKeyTableRva", out.fNameKeyTableRva, "validated", sig.pattern));
    }
    if (!out.fNameKeyTableRva)
        out.report.push_back(MakeHit("FNameKeyTableRva", 0, "failed", nullptr));

    const SigTarget blockMaskSigs[] = {
        { "FNameBlockMaskRva", "48 8B 05 ? ? ? ? 48 31 C1 48 C1 C1", 3, 7 },
        { "FNameBlockMaskRva", "48 8B 0D ? ? ? ? 48 33 C8", 3, 7 },
        { "FNameBlockMaskRva", "48 8B 05 ? ? ? ? 48 33 C1", 3, 7 },
        { "FNameBlockMaskRva", "48 8B 0D ? ? ? ? 48 31 C8", 3, 7 },
        { "FNameBlockMaskRva", "4C 8B 05 ? ? ? ? 49 31 C1", 3, 7 },
        { "FNameBlockMaskRva", "48 8B 05 ? ? ? ? 48 31 C8 48 C1 C0", 3, 7 },
        { "FNameBlockMaskRva", "48 8B 1D ? ? ? ? 48 31 C3", 3, 7 },
        { "FNameBlockMaskRva", "48 8B 05 ? ? ? ? 48 35", 3, 7 },
    };
    for (const auto& sig : blockMaskSigs) {
        if (out.fNameBlockMaskRva)
            break;
        out.fNameBlockMaskRva = PickBestRvaAllHits(cache, moduleBase, sig, [&](uint64_t rva, std::string& n) {
            return ValidateBlockMask(moduleBase, rva, n);
        }, state);
        if (out.fNameBlockMaskRva)
            out.report.push_back(MakeHit("FNameBlockMaskRva", out.fNameBlockMaskRva, "validated", sig.pattern));
    }
    if (!out.fNameBlockMaskRva)
        out.report.push_back(MakeHit("FNameBlockMaskRva", 0, "failed", nullptr, "sig scan miss"));

    if (!out.fNameBlockMaskRva && out.gNamePoolRva && out.fNameKeyTableRva && out.gUObjectChunksRva) {
        FNameGlobals partial{ out.gNamePoolRva, out.fNameKeyTableRva, 0 };
        out.fNameBlockMaskRva = DiscoverBlockMaskRva(
            moduleBase, cache, partial, out.gUObjectChunksRva, state);
        if (partial.blockMaskDirect)
            out.fNameBlockMaskDirect = partial.blockMaskDirect;
        if (partial.poolRva && partial.poolRva != out.gNamePoolRva) {
            out.gNamePoolRva = partial.poolRva;
            state.AppendLog("GNamePoolRva updated to " + HexU64(out.gNamePoolRva));
        }
        if (out.fNameBlockMaskRva) {
            out.report.push_back(MakeHit(
                "FNameBlockMaskRva", out.fNameBlockMaskRva, "fname-scored", "FName pipeline"));
        }
    }

    if (out.gNamePoolRva && out.fNameKeyTableRva && out.gUObjectChunksRva) {
        FNameGlobals full{
            out.gNamePoolRva,
            out.fNameKeyTableRva,
            out.fNameBlockMaskRva,
            out.fNameBlockMaskDirect,
        };
        std::string notes;
        if (ValidateGNamePoolWithObjects(moduleBase, full, out.gUObjectChunksRva, notes))
            state.AppendLog("GNamePool FName verify: " + notes);
        else
            state.AppendLog("GNamePool FName verify FAILED: " + notes);
    }

    out.fFieldNameKey0Rva = ScanFFieldKeys(cache, moduleBase, 0, state);
    out.fFieldNameKey1Rva = ScanFFieldKeys(cache, moduleBase, 1, state);
    if (out.fFieldNameKey0Rva)
        out.report.push_back(MakeHit("FFieldNameKey0Rva", out.fFieldNameKey0Rva, "candidate", "F3 0F 7E 05 ..."));
    if (out.fFieldNameKey1Rva)
        out.report.push_back(MakeHit("FFieldNameKey1Rva", out.fFieldNameKey1Rva, "candidate", "F3 0F 7E 0D ..."));

    state.SetGlobals(out.report);
    state.SetProgress(60);
    return out;
}

bool WriteGlobalsJson(
    const std::filesystem::path& path,
    uint64_t moduleBase,
    uint32_t imageSize,
    const DiscoveredGlobals& globals)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;

    f << "{\n";
    f << "  \"build\": \"auto\",\n";
    f << "  \"moduleBase\": \"" << HexU64(moduleBase) << "\",\n";
    f << "  \"imageSize\": " << imageSize << ",\n";
    f << "  \"globals\": {\n";

    auto writeEntry = [&](const char* key, uint64_t rva, const char* confidence, const char* sig) {
        f << "    \"" << key << "\": { \"rva\": \"" << HexU64(rva) << "\", \"confidence\": \""
          << confidence << "\"";
        if (sig && *sig)
            f << ", \"sig\": \"" << sig << "\"";
        f << " }";
    };

    const char* uConf = globals.uWorldRva ? "validated" : "failed";
    const char* nConf = globals.gNamePoolRva
        ? (globals.fNameBlockMaskRva ? "fname-validated" : "validated")
        : "failed";
    const char* oConf = globals.gUObjectChunksRva ? "validated" : "failed";
    const char* ktConf = globals.fNameKeyTableRva
        ? (globals.fNameBlockMaskRva ? "fname-validated" : "validated")
        : "failed";
    const char* bmConf = globals.fNameBlockMaskRva ? "fname-scored" : "failed";

    writeEntry("UWorld", globals.uWorldRva, uConf, "48 8B 05 ? ? ? ?");
    f << ",\n";
    writeEntry("GNamePoolRva", globals.gNamePoolRva, nConf, "48 8D 0D ? ? ? ?");
    f << ",\n";
    writeEntry("GUObjectArrayChunksRva", globals.gUObjectChunksRva, oConf, "48 8B 05 ? ? ? ?");
    f << "\n  },\n";
    f << "  \"steamDecrypt\": {\n";
    writeEntry("FNameKeyTableRva", globals.fNameKeyTableRva, ktConf, nullptr);
    f << ",\n";
    writeEntry("FNameBlockMaskRva", globals.fNameBlockMaskRva, bmConf, nullptr);
    if (globals.fNameBlockMaskDirect) {
        f << ",\n    \"FNameBlockMaskDirect\": { \"value\": \"" << HexU64(globals.fNameBlockMaskDirect)
          << "\", \"confidence\": \"fname-scored\" }";
    }
    f << ",\n";
    writeEntry("FFieldNameKey0Rva", globals.fFieldNameKey0Rva, globals.fFieldNameKey0Rva ? "candidate" : "failed", nullptr);
    f << ",\n";
    writeEntry("FFieldNameKey1Rva", globals.fFieldNameKey1Rva, globals.fFieldNameKey1Rva ? "candidate" : "failed", nullptr);
    f << "\n  }\n";
    f << "}\n";
    return true;
}

bool AppendDumpReport(
    const std::filesystem::path& path,
    const DiscoveredGlobals& globals,
    const std::string& extra)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;

    f << "=== Arc Raiders Dumper Report ===\n\n";
    for (const auto& hit : globals.report) {
        f << hit.name << ": " << (hit.rva ? HexU64(hit.rva) : "NOT FOUND");
        f << " [" << hit.confidence << "]";
        if (!hit.notes.empty())
            f << " — " << hit.notes;
        f << "\n";
    }
    if (!extra.empty()) {
        f << "\n--- SDK dump ---\n";
        f << extra << "\n";
    }
    return true;
}

} // namespace Dumper
