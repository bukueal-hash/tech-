#include "SdkDumper.h"

#include "FNameDiscovery.h"
#include "GlobalDiscovery.h"
#include "../Core/Offsets.h"
#include "../Core/SteamDecrypt.hpp"
#include "../DMA/Memory.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <immintrin.h>
#include <sstream>
#include <unordered_set>

namespace Dumper {

namespace {

constexpr uint32_t kSuperStruct = 0xA8;
constexpr uint32_t kChildProperties = 0x108;
constexpr uint32_t kFFieldNext = 0x80;
constexpr uint32_t kFFieldOffsetEnc = 0xB4;
constexpr uint32_t kFFieldNameEnc = 0x60;
constexpr uint32_t kFFieldOffsetXor = 0xA217C376u;
constexpr uint64_t kFFieldNameXor = 0x890EF320D7E2DC4CULL;
constexpr int32_t kMaxClasses = 5000;
constexpr int32_t kElementsPerChunk = 0x10000;

bool ValidPtr(uint64_t p)
{
    return p >= 0x10000ULL && p <= 0x7FFFFFFFFFFFULL;
}

bool ClassPrefixAllowed(const std::string& name, bool strict)
{
    if (!strict)
        return steam_decrypt::IsPlausibleFNameText(name);

    static const char* kPrefixes[] = {
        "Pioneer", "Embark", "ARC", "Angelscript", "Constructable", "Inventory",
        "Loot", "Container", "Player", "World", "Game", "Character", "Actor",
        "Engine", "Blueprint", "Mesh", "Component", "Material", "Widget", "Anim",
        "Niagara", "Sound", "Texture", "Static", "Skeletal", "Landscape",
        "ControlRig", "Geometry", "Chaos", "MetaSound", "Default__", "BP_", "ABP_",
        "Field", "Struct", "Function", "Enum", "Property", "Package", "Object"
    };
    for (const char* p : kPrefixes) {
        if (name.rfind(p, 0) == 0)
            return true;
    }
    return false;
}

bool InitFNameForDump(uint64_t moduleBase, const DiscoveredGlobals& globals, DumperState& state)
{
    FNameGlobals cfg{
        globals.gNamePoolRva ? globals.gNamePoolRva : Offsets::GNamePoolRva,
        globals.fNameKeyTableRva ? globals.fNameKeyTableRva : Offsets::FNameKeyTableRva,
        globals.fNameBlockMaskRva ? globals.fNameBlockMaskRva : Offsets::FNameBlockMaskRva,
        globals.fNameBlockMaskDirect,
    };

    if (!cfg.blockMaskDirect && !cfg.blockMaskRva && Offsets::FNameBlockMaskRva) {
        cfg.blockMaskRva = Offsets::FNameBlockMaskRva;
        state.AppendLog("FName init: using stale Offsets.h block mask RVA (discovery miss)");
    }

    if (!ApplyFNameGlobals(moduleBase, cfg)) {
        state.AppendLog("FName init: pool/key/mask apply failed");
        return false;
    }

    const int score = ScoreFNameQuick(
        moduleBase, cfg, globals.gUObjectChunksRva ? globals.gUObjectChunksRva : Offsets::GUObjectArrayChunksRva);
    state.AppendLog("FName init score=" + std::to_string(score));
    if (score >= 1) {
        if (score < 3)
            state.AppendLog("FName init: low score — continuing dump anyway");
        return true;
    }
    return false;
}

uint32_t DecryptFFieldOffset(uint64_t fieldPtr)
{
    const uint32_t enc = PCIMemory::Read<uint32_t>(static_cast<uintptr_t>(fieldPtr + kFFieldOffsetEnc));
    const uint32_t decrypted = enc ^ kFFieldOffsetXor;
    const uint32_t page = (decrypted >> 16) & 0xFFu;
    const uint32_t offsetInPage = (decrypted >> 24) & 0xFFu;
    return (page * 0x100u) + offsetInPage;
}

std::string DecryptFFieldName(uint64_t moduleBase, uint64_t fieldPtr, uint64_t key0Rva, uint64_t key1Rva)
{
    if (!key0Rva || !key1Rva)
        return {};

    __m128i v7 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(moduleBase + key0Rva));
    __m128i v8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(moduleBase + key1Rva));
    __m128i si128{};
    if (!PCIMemory::ReadVirtualMemory(
            static_cast<uintptr_t>(fieldPtr + kFFieldNameEnc), &si128, sizeof(si128)))
        return {};

    __m128i v9 = _mm_xor_si128(_mm_shuffle_epi8(si128, v7), v8);
    uint64_t v10 = static_cast<uint64_t>(_mm_cvtsi128_si64(v9)) ^ kFFieldNameXor;

    char buf[128]{};
    size_t len = 0;
    for (size_t i = 0; i < sizeof(buf) - 1; ++i) {
        const char c = reinterpret_cast<char*>(&v10)[i % 8];
        if (!c)
            break;
        if (c < 32 || c > 126)
            return {};
        buf[len++] = c;
    }
    return std::string(buf, len);
}

std::string GetObjectClassName(uint64_t moduleBase, uint64_t objPtr)
{
    const uintptr_t cls = steam_decrypt::GetActorClassPtr(static_cast<uintptr_t>(objPtr));
    if (!cls)
        return {};
    return steam_decrypt::GetActorFNameString(cls);
}

constexpr int32_t kFUObjectItemSize = 0x18;
constexpr int32_t kMaxChunksProbe = 256;
constexpr int32_t kRelaxedSlotsPerChunk = 0x10000;

struct GObjectWalkPlan {
    uint64_t objectsTable = 0;
    int32_t numChunks = 0;
    int32_t numElements = 0;
    bool relaxed = false;
};

bool ResolveGObjectWalk(uint64_t arr, GObjectWalkPlan& plan, DumperState& state)
{
    plan = {};
    plan.objectsTable = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(arr + 0x00));
    if (!ValidPtr(plan.objectsTable)) {
        state.AppendLog("GUObject: Objects table ptr invalid");
        return false;
    }

    const int32_t numElements = PCIMemory::Read<int32_t>(static_cast<uintptr_t>(arr + 0x14));
    const int32_t numChunks = PCIMemory::Read<int32_t>(static_cast<uintptr_t>(arr + 0x1C));
    if (numElements > 0 && numElements <= 5000000 && numChunks > 0 && numChunks <= kMaxChunksProbe) {
        const uint64_t chunk0 = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(plan.objectsTable));
        if (ValidPtr(chunk0)) {
            plan.numElements = numElements;
            plan.numChunks = numChunks;
            plan.relaxed = false;
            state.AppendLog("GUObject: strict layout ok");
            return true;
        }
    }

    int32_t chunksFound = 0;
    for (int32_t i = 0; i < kMaxChunksProbe; ++i) {
        const uint64_t chunkPtr = PCIMemory::Read<uint64_t>(
            static_cast<uintptr_t>(plan.objectsTable + static_cast<uint64_t>(i) * 8ULL));
        if (!ValidPtr(chunkPtr))
            break;

        int validObjs = 0;
        for (int j = 0; j < 16; ++j) {
            const uint64_t obj = PCIMemory::Read<uint64_t>(
                static_cast<uintptr_t>(chunkPtr + static_cast<uint64_t>(j) * kFUObjectItemSize));
            if (ValidPtr(obj))
                ++validObjs;
        }
        if (validObjs < 1)
            break;
        ++chunksFound;
    }

    if (chunksFound <= 0) {
        state.AppendLog("GUObject: relaxed chunk probe found nothing");
        return false;
    }

    plan.numChunks = chunksFound;
    plan.numElements = chunksFound * kElementsPerChunk;
    plan.relaxed = true;
    state.AppendLog("GUObject: using relaxed walk, chunks=" + std::to_string(chunksFound));
    return true;
}

void DumpStructFields(
    std::ofstream& out,
    uint64_t moduleBase,
    uint64_t structPtr,
    const DiscoveredGlobals& globals,
    std::unordered_set<uint64_t>& visited)
{
    if (!ValidPtr(structPtr) || !visited.insert(structPtr).second)
        return;

    uint64_t field = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(structPtr + kChildProperties));
    const uint64_t key0 = globals.fFieldNameKey0Rva ? globals.fFieldNameKey0Rva : Offsets::FFieldNameKey0Rva;
    const uint64_t key1 = globals.fFieldNameKey1Rva ? globals.fFieldNameKey1Rva : Offsets::FFieldNameKey1Rva;
    int guard = 0;
    while (ValidPtr(field) && guard++ < 512) {
        const std::string fname = DecryptFFieldName(moduleBase, field, key0, key1);
        const uint32_t off = DecryptFFieldOffset(field);
        if (!fname.empty() && off < 0x10000u)
            out << "\t" << fname << " = 0x" << std::hex << off << std::dec << "\n";

        field = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(field + kFFieldNext));
    }

    const uint64_t superStruct = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(structPtr + kSuperStruct));
    if (ValidPtr(superStruct))
        DumpStructFields(out, moduleBase, superStruct, globals, visited);
}

struct DumpPassStats {
    int objsSeen = 0;
    int namesEmpty = 0;
    int prefixSkip = 0;
    int classFail = 0;
};

size_t RunDumpPass(
    std::ofstream& out,
    uint64_t moduleBase,
    const DiscoveredGlobals& globals,
    const GObjectWalkPlan& walk,
    bool strictPrefix,
    std::unordered_set<std::string>& writtenClasses,
    DumpPassStats& stats,
    DumperState& state)
{
    const size_t before = writtenClasses.size();

    for (int32_t chunk = 0; chunk < walk.numChunks && writtenClasses.size() < static_cast<size_t>(kMaxClasses); ++chunk) {
        if (state.IsCancelRequested())
            break;

        const uint64_t chunkPtr = PCIMemory::Read<uint64_t>(
            static_cast<uintptr_t>(walk.objectsTable + static_cast<uint64_t>(chunk) * 8ULL));
        if (!ValidPtr(chunkPtr))
            continue;

        const int32_t chunkStart = chunk * kElementsPerChunk;
        const int32_t slotCount = walk.relaxed
            ? kRelaxedSlotsPerChunk
            : (std::min)(walk.numElements - chunkStart, kElementsPerChunk);

        for (int32_t idx = 0; idx < slotCount; ++idx) {
            if (state.IsCancelRequested() || writtenClasses.size() >= static_cast<size_t>(kMaxClasses))
                break;

            const uint64_t item = chunkPtr + static_cast<uint64_t>(idx) * kFUObjectItemSize;
            const uint64_t obj = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(item));
            if (!ValidPtr(obj))
                continue;

            ++stats.objsSeen;

            const uint32_t flags = PCIMemory::Read<uint32_t>(static_cast<uintptr_t>(obj + Offsets::UObject_ObjectFlags));
            if (flags & Offsets::RF_BeginDestroyed)
                continue;

            const std::string className = GetObjectClassName(moduleBase, obj);
            if (className.empty()) {
                ++stats.namesEmpty;
                continue;
            }
            if (!ClassPrefixAllowed(className, strictPrefix)) {
                ++stats.prefixSkip;
                continue;
            }
            if (!writtenClasses.insert(className).second)
                continue;

            const uintptr_t cls = steam_decrypt::GetActorClassPtr(static_cast<uintptr_t>(obj));
            if (!cls) {
                ++stats.classFail;
                writtenClasses.erase(className);
                continue;
            }

            out << "Class: " << className << "\n";
            std::unordered_set<uint64_t> visited;
            DumpStructFields(out, moduleBase, cls, globals, visited);
            out << "\n";

            if (writtenClasses.size() % 200 == 0) {
                const int pct = 60 + static_cast<int>((writtenClasses.size() * 35) / kMaxClasses);
                state.SetProgress((std::min)(pct, 95));
            }
        }
    }

    return writtenClasses.size() - before;
}

} // namespace

SdkDumpResult RunSdkDump(
    uint64_t moduleBase,
    const DiscoveredGlobals& globals,
    const std::filesystem::path& outPath,
    DumperState& state)
{
    SdkDumpResult result;
    if (!InitFNameForDump(moduleBase, globals, state)) {
        result.summary = "FName init failed — check globals / steamDecrypt RVAs";
        state.AppendLog(result.summary);
        return result;
    }

    const uint64_t chunksRva = globals.gUObjectChunksRva ? globals.gUObjectChunksRva : Offsets::GUObjectArrayChunksRva;
    const uint64_t arr = moduleBase + chunksRva;

    GObjectWalkPlan walk{};
    if (!ResolveGObjectWalk(arr, walk, state)) {
        result.summary = "GUObjectArray unreadable";
        state.AppendLog(result.summary);
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    std::ofstream out(outPath, std::ios::trunc);
    if (!out) {
        result.summary = "failed to open dump.txt";
        state.AppendLog(result.summary);
        return result;
    }

    std::unordered_set<std::string> writtenClasses;
    DumpPassStats stats{};

    size_t added = RunDumpPass(out, moduleBase, globals, walk, true, writtenClasses, stats, state);
    if (added == 0) {
        state.AppendLog("SDK strict pass empty — retrying relaxed prefix filter");
        RunDumpPass(out, moduleBase, globals, walk, false, writtenClasses, stats, state);
    }

    result.classesWritten = writtenClasses.size();

    std::ostringstream oss;
    oss << "classes=" << result.classesWritten
        << " objs=" << stats.objsSeen
        << " namesEmpty=" << stats.namesEmpty
        << " prefixSkip=" << stats.prefixSkip
        << " classFail=" << stats.classFail;
    result.summary = oss.str();
    state.AppendLog("SDK dump: " + result.summary);
    if (stats.objsSeen > 0 && stats.namesEmpty >= stats.objsSeen * 9 / 10)
        state.AppendLog("SDK hint: namesEmpty≈objs — GObject item decrypt or FName mask may be wrong");
    state.SetProgress(98);
    return result;
}

} // namespace Dumper
