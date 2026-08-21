#pragma once

#include "Offsets.h"
#include "../DMA/Memory.h"

#include <cstdint>
#include <cstddef>
#include <string>

namespace ArcActorType {

enum class EActorType : uint32_t {
    EACTOR_SPAWN    = 0x011F0000u,
    EACTOR_PLAYER   = 0x010E0000u,
    EACTOR_CHEST    = 0x00080000u,
    EACTOR_ARC_WASP = 0x000A0000u,
    EACTOR_TARGET   = 0x010A0000u,
    EACTOR_LOOT     = 0x000C0000u,
};

inline uint32_t MaskActorTypeId(uint32_t id)
{
    return id & 0xFFFF0000u;
}

inline bool IsPlayerClassId(uint32_t maskedId)
{
    return maskedId == static_cast<uint32_t>(EActorType::EACTOR_PLAYER);
}

inline bool IsBotClassId(uint32_t maskedId)
{
    return maskedId == static_cast<uint32_t>(EActorType::EACTOR_ARC_WASP);
}

inline bool IsGroundLootClassId(uint32_t maskedId)
{
    return maskedId == static_cast<uint32_t>(EActorType::EACTOR_LOOT);
}

inline bool IsChestClassId(uint32_t maskedId)
{
    return maskedId == static_cast<uint32_t>(EActorType::EACTOR_CHEST);
}

inline bool IsWorldItemClassIdMasked(uint32_t maskedId)
{
    return IsGroundLootClassId(maskedId) || IsChestClassId(maskedId);
}

inline bool IsKnownActorTypeMasked(uint32_t maskedId)
{
    return IsPlayerClassId(maskedId)
        || IsBotClassId(maskedId)
        || maskedId == static_cast<uint32_t>(EActorType::EACTOR_TARGET)
        || IsWorldItemClassIdMasked(maskedId)
        || maskedId == static_cast<uint32_t>(EActorType::EACTOR_SPAWN);
}

inline std::ptrdiff_t& RuntimeActorTypeOffset()
{
    static std::ptrdiff_t s_off = -1;
    return s_off;
}

inline std::ptrdiff_t ScanActorForTypeOffset(uintptr_t actor)
{
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return -1;

    static const std::ptrdiff_t kPriority[] = {
        Offsets::ActorTypeId,
        0xB0,
        Offsets::ClassDefaultObjectAlt,
        Offsets::ClassDefaultObject,
        Offsets::ActorID,
    };

    for (std::ptrdiff_t off : kPriority) {
        if (off < 0)
            continue;
        const uint32_t raw = Memory::read<uint32_t>(actor + off);
        if (IsKnownActorTypeMasked(MaskActorTypeId(raw)))
            return off;
    }

    for (std::ptrdiff_t off = 0x50; off <= 0x140; off += 4) {
        const uint32_t raw = Memory::read<uint32_t>(actor + off);
        if (IsKnownActorTypeMasked(MaskActorTypeId(raw)))
            return off;
    }
    return -1;
}

inline uint32_t ReadActorTypeId(uintptr_t actor)
{
    if (!actor)
        return 0;

    std::ptrdiff_t off = RuntimeActorTypeOffset();
    if (off < 0) {
        off = ScanActorForTypeOffset(actor);
        if (off >= 0)
            RuntimeActorTypeOffset() = off;
    }
    if (off < 0)
        off = Offsets::ActorTypeId;

    return Memory::read<uint32_t>(actor + off);
}

inline bool IsTargetBotActor(uintptr_t actor)
{
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return false;

    const uint32_t masked = MaskActorTypeId(ReadActorTypeId(actor));
    if (masked != static_cast<uint32_t>(EActorType::EACTOR_TARGET))
        return false;

    const uint64_t flag = Memory::read<uint64_t>(actor + 0x1D0);
    return flag == 1;
}

inline bool IsAnyBotActor(uintptr_t actor)
{
    if (!actor)
        return false;

    const uint32_t masked = MaskActorTypeId(ReadActorTypeId(actor));
    return IsBotClassId(masked) || IsTargetBotActor(actor);
}

} // namespace ArcActorType
