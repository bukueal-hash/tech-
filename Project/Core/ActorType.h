#pragma once

#include "Offsets.h"
#include "ReadBudget.hpp"
#include "../DMA/Memory.h"

#include <atomic>
#include <chrono>
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

// ── Per-actor type memo ─────────────────────────────────────────────────────
// ReadActorTypeId is a single DMA pull read, and it is issued far more often
// than the value changes: each ring walk reads it once per actor, then every
// predicate that follows (LooksLikePlayerPawn, IsAnyBotActor, ShouldExclude…)
// reads the same field again, and the five scanner passes each repeat the whole
// set every turn. In a 1330-actor raid that is tens of thousands of identical
// reads of one field, on a bus that can only serve one scanner at a time — the
// measured cost behind a ~2.4 s scan cycle (perf_spike: EntityList p50 665 ms,
// RobotList 681 ms, Update 498 ms, on a single shared scan gate).
//
// The memo is deliberately narrow:
//   * only KNOWN type masks are cached. An unknown read is the interesting case
//     (a spawn still settling, a pointer not yet valid) and always goes to the
//     bus, so nothing that needs to change can be held back by a cache.
//   * 1 s TTL, so an address recycled by a later actor is re-read long before
//     it can be trusted for the rest of a raid.
//   * per-thread table: the five scanners never contend on it and nothing is
//     allocated on the scan path.
//   * direct-mapped (one slot per hash): a collision costs a re-read, never a
//     probe loop, so the failure mode is "slower", never "wrong".
//   * cleared whenever the runtime type offset changes, since every cached
//     value was read at the old offset.
//
// Its effect is reported as typeMemo.hits/misses in the scan_cycle record, so
// the win is measured rather than assumed.
inline constexpr int64_t kTypeMemoTtlMs = 1000;
inline constexpr size_t kTypeMemoSlots = 4096; // power of two

// No default member initializers on purpose: a plain aggregate is constant-
// initialized, so the thread_local table needs no first-use guard check on the
// hot read path (this runs tens of thousands of times per scan cycle).
struct TypeMemoEntry {
    uintptr_t actor;
    int64_t stampMs;
    uint32_t typeId;
};

inline TypeMemoEntry* TypeMemoTable()
{
    static thread_local TypeMemoEntry s_table[kTypeMemoSlots];
    return s_table;
}

inline std::atomic<uint64_t> g_typeMemoHits{ 0 };
inline std::atomic<uint64_t> g_typeMemoMisses{ 0 };

inline int64_t TypeMemoNowMs()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

/** Drop every cached entry. Callers own the calling thread's table only. */
inline void ClearActorTypeMemo()
{
    TypeMemoEntry* table = TypeMemoTable();
    for (size_t i = 0; i < kTypeMemoSlots; ++i)
        table[i].actor = 0;
}

inline size_t TypeMemoSlot(uintptr_t actor)
{
    const uintptr_t mixed = (actor >> 4) ^ (actor >> 20);
    return static_cast<size_t>(mixed) & (kTypeMemoSlots - 1);
}

/** Cumulative memo hits/misses, for the scan-cycle record. */
inline void TypeMemoStats(uint64_t& hits, uint64_t& misses)
{
    hits = g_typeMemoHits.load(std::memory_order_relaxed);
    misses = g_typeMemoMisses.load(std::memory_order_relaxed);
}

inline uint32_t ReadActorTypeId(uintptr_t actor)
{
    // Ledger: actor type id is an identity read; the memo means only misses reach
    // the bus, so this is a default (a caller's more specific purpose wins).
    ReadBudget::DefaultPurposeScope purpose(ReadBudget::Purpose::Name);
    if (!actor)
        return 0;

    TypeMemoEntry* table = TypeMemoTable();
    const size_t slot = TypeMemoSlot(actor);
    TypeMemoEntry& entry = table[slot];
    const int64_t nowMs = TypeMemoNowMs();
    if (entry.actor == actor && entry.stampMs != 0
        && nowMs - entry.stampMs < kTypeMemoTtlMs) {
        g_typeMemoHits.fetch_add(1, std::memory_order_relaxed);
        return entry.typeId;
    }
    g_typeMemoMisses.fetch_add(1, std::memory_order_relaxed);

    std::ptrdiff_t off = RuntimeActorTypeOffset();
    if (off < 0) {
        off = ScanActorForTypeOffset(actor);
        if (off >= 0) {
            // Every cached value was read at a different offset.
            ClearActorTypeMemo();
            RuntimeActorTypeOffset() = off;
        }
    }
    if (off < 0)
        off = Offsets::ActorTypeId;

    const uint32_t typeId = Memory::read<uint32_t>(actor + off);
    // Cache only decisive values: an unknown mask means "ask again", exactly
    // as it did before the memo existed.
    if (IsKnownActorTypeMasked(MaskActorTypeId(typeId))) {
        entry.actor = actor;
        entry.stampMs = nowMs;
        entry.typeId = typeId;
    } else {
        entry.actor = 0;
    }
    return typeId;
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
