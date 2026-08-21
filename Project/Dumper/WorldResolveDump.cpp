#include "WorldResolveDump.h"

#include "../Core/Engine.h"
#include "../Core/Offsets.h"
#include "../DMA/Memory.h"
#include "../Functions/WorldScanCommon.h"

#include <sstream>

namespace Dumper {

namespace {

uint64_t ReadU64Nocache(uint64_t address)
{
    uint64_t value = 0;
    if (!PCIMemory::ReadVirtualMemoryNoCache(static_cast<uintptr_t>(address), &value, sizeof(value)))
        return 0;
    return value;
}

bool LevelHasActors(uint64_t level)
{
    if (!Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(level)))
        return false;
    uintptr_t data = 0;
    int32_t count = 0;
    if (!WorldScan::ReadLevelActors(static_cast<uintptr_t>(level), data, count))
        return false;
    return Engine::IsPlausibleObjPtr(data) && count > 0 && count <= 10000;
}

bool LevelLooksOwnedByWorld(uint64_t level, uint64_t world)
{
    if (!Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(level)) || !world)
        return false;

    const uint64_t owning = PCIMemory::Read<uint64_t>(
        static_cast<uintptr_t>(level + Offsets::Level_OwningWorld));
    if (owning == world)
        return true;

    return LevelHasActors(level);
}

std::string DiagnoseWorldPtr(uint64_t world)
{
    std::ostringstream oss;
    if (!world) {
        oss << "null";
        return oss.str();
    }

    if (!Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(world)))
        oss << "ptr-weak";

    const uint64_t directPl = PCIMemory::Read<uint64_t>(
        static_cast<uintptr_t>(world + Offsets::PersistentLevel));
    oss << " PL@0x120=" << std::hex << directPl;
    if (LevelLooksOwnedByWorld(directPl, world))
        oss << " owned";
    else if (LevelHasActors(directPl))
        oss << " actors";
    else
        oss << " pl-fail";

    const uint64_t inner = ReadU64Nocache(world);
    oss << " deref=" << std::hex << inner;
    return oss.str();
}

} // namespace

uint64_t ReadWorldSlotNocache(uint64_t moduleBase, uint64_t slotRva)
{
    return ReadU64Nocache(moduleBase + slotRva);
}

uint64_t ResolvePersistentLevelDump(uint64_t world)
{
    if (!Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(world)))
        return 0;

    const uint64_t directPl = PCIMemory::Read<uint64_t>(
        static_cast<uintptr_t>(world + Offsets::PersistentLevel));
    if (LevelLooksOwnedByWorld(directPl, world))
        return directPl;
    if (LevelHasActors(directPl))
        return directPl;

    const uint64_t collectionsData = PCIMemory::Read<uint64_t>(
        static_cast<uintptr_t>(world + Offsets::LevelCollections));
    const int32_t collectionsNum = PCIMemory::Read<int32_t>(
        static_cast<uintptr_t>(world + Offsets::LevelCollections + 8));
    if (Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(collectionsData))
        && collectionsNum > 0 && collectionsNum <= 16) {
        const int limit = (collectionsNum > 4) ? 4 : collectionsNum;
        for (int i = 0; i < limit; ++i) {
            const uint64_t collection = collectionsData
                + static_cast<uint64_t>(i) * static_cast<uint64_t>(Offsets::LevelCollection_Stride);
            const uint64_t level = PCIMemory::Read<uint64_t>(
                static_cast<uintptr_t>(collection + Offsets::LevelCollection_PersistentLevel));
            if (LevelLooksOwnedByWorld(level, world) || LevelHasActors(level))
                return level;
        }
    }

    const uint64_t levelsData = PCIMemory::Read<uint64_t>(
        static_cast<uintptr_t>(world + Offsets::Levels));
    const int32_t levelsNum = PCIMemory::Read<int32_t>(
        static_cast<uintptr_t>(world + Offsets::Levels + 8));
    if (Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(levelsData))
        && levelsNum > 0 && levelsNum < 512) {
        const int limit = (levelsNum > 8) ? 8 : levelsNum;
        for (int i = 0; i < limit; ++i) {
            const uint64_t level = PCIMemory::Read<uint64_t>(
                static_cast<uintptr_t>(levelsData + static_cast<uint64_t>(i) * sizeof(uint64_t)));
            if (LevelLooksOwnedByWorld(level, world) || LevelHasActors(level))
                return level;
        }
    }

    return 0;
}

uint64_t PickValidWorldDump(uint64_t slot)
{
    if (!slot)
        return 0;

    const auto tryWorld = [&](uint64_t w) -> uint64_t {
        if (!w)
            return 0;
        if (ResolvePersistentLevelDump(w))
            return w;
        if (Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(w)))
            return w;
        return 0;
    };

    if (Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(slot))) {
        if (const uint64_t w = tryWorld(slot))
            return w;
    }

    const uint64_t inner = ReadU64Nocache(slot);
    if (const uint64_t w = tryWorld(inner))
        return w;

    if (slot >= 0x10000ULL && slot <= 0x7FFFFFFFFFFFULL) {
        if (ResolvePersistentLevelDump(slot))
            return slot;
        if (ResolvePersistentLevelDump(inner))
            return inner;
    }

    return 0;
}

uint64_t TryWorldFromGameStateGlobalDump(uint64_t moduleBase)
{
    if (!moduleBase)
        return 0;

    const uint64_t gs = PCIMemory::Read<uint64_t>(
        static_cast<uintptr_t>(moduleBase + Offsets::GameStateGlobalRva));
    if (!Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(gs)))
        return 0;

    const uint64_t arrData = PCIMemory::Read<uint64_t>(
        static_cast<uintptr_t>(gs + Offsets::GameState_PlayerArray));
    const int32_t arrNum = PCIMemory::Read<int32_t>(
        static_cast<uintptr_t>(gs + Offsets::GameState_PlayerArray + 8));
    if (!Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(arrData)) || arrNum <= 0 || arrNum > 128)
        return 0;

    static const std::ptrdiff_t kOuterCands[] = { 0x20, 0x28, 0x18, 0x30, 0x10, 0x40 };
    for (std::ptrdiff_t off : kOuterCands) {
        const uint64_t cand = PCIMemory::Read<uint64_t>(static_cast<uintptr_t>(gs + off));
        if (ResolvePersistentLevelDump(cand))
            return cand;
    }
    return 0;
}

int ScoreUWorldSlotRva(uint64_t moduleBase, uint64_t slotRva, UWorldScoreDetail* outDetail)
{
    UWorldScoreDetail local;
    local.rva = slotRva;
    local.slot = ReadWorldSlotNocache(moduleBase, slotRva);
    local.world = PickValidWorldDump(local.slot);

    if (!local.world) {
        local.notes = "help chain failed";
        if (local.slot)
            local.notes += " {" + DiagnoseWorldPtr(local.slot) + "}";
        if (local.slot && !local.world) {
            local.score = 3;
            local.notes += " [slot-only]";
        }
        if (outDetail)
            *outDetail = local;
        return local.score;
    }

    local.score = 5;
    const bool hasPl = ResolvePersistentLevelDump(local.world) != 0;
    if (hasPl)
        local.score += 20;
    else
        local.score += 5;

    if (Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(PCIMemory::Read<uint64_t>(
            static_cast<uintptr_t>(local.world + Offsets::OwningGameInstance)))))
        local.score += 25;

    const uint64_t levelsData = PCIMemory::Read<uint64_t>(
        static_cast<uintptr_t>(local.world + Offsets::Levels));
    const int32_t levelsNum = PCIMemory::Read<int32_t>(
        static_cast<uintptr_t>(local.world + Offsets::Levels + 8));
    if (Engine::IsPlausibleObjPtr(static_cast<uintptr_t>(levelsData))
        && levelsNum > 0 && levelsNum <= 64)
        local.score += 10;

    if (local.world != local.slot && local.slot)
        local.score += 5;

    local.notes = hasPl ? "ok" : "relaxed-inner";
    if (outDetail)
        *outDetail = local;
    return local.score;
}

uint64_t FindSlotRvaForLiveWorld(
    uint64_t moduleBase,
    uint64_t liveWorld,
    const uint64_t* candidateRvas,
    size_t candidateCount)
{
    if (!moduleBase || !liveWorld || !candidateRvas)
        return 0;

    for (size_t i = 0; i < candidateCount; ++i) {
        const uint64_t rva = candidateRvas[i];
        const uint64_t slot = ReadWorldSlotNocache(moduleBase, rva);
        const uint64_t world = PickValidWorldDump(slot);
        if (world == liveWorld)
            return rva;
    }
    return 0;
}

} // namespace Dumper
