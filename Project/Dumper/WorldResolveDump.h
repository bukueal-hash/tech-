#pragma once

#include <cstdint>
#include <string>

namespace Dumper {

struct UWorldScoreDetail {
    uint64_t rva = 0;
    uint64_t slot = 0;
    uint64_t world = 0;
    int score = 0;
    std::string notes;
};

uint64_t ReadWorldSlotNocache(uint64_t moduleBase, uint64_t slotRva);
uint64_t ResolvePersistentLevelDump(uint64_t world);
uint64_t PickValidWorldDump(uint64_t slot);
uint64_t TryWorldFromGameStateGlobalDump(uint64_t moduleBase);

/** Score a GWorld slot RVA using the same chain as live ESP. */
int ScoreUWorldSlotRva(uint64_t moduleBase, uint64_t slotRva, UWorldScoreDetail* outDetail = nullptr);

/** All sig-hit RVAs whose slot resolves to liveWorld (for relaxed discovery). */
uint64_t FindSlotRvaForLiveWorld(
    uint64_t moduleBase,
    uint64_t liveWorld,
    const uint64_t* candidateRvas,
    size_t candidateCount);

} // namespace Dumper
