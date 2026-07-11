#pragma once
 
// =============================================================================
// ARC Raiders – FName decryption (CL-1315578 / 2026-07-09)
//
// Public API:
//     FName::GetActorFNameString(actor, game_base, reader)
//     FName::GetActorFNameId    (actor, game_base, reader)
//     FName::GetActorFNameNumber(actor, game_base, reader)
//     FName::ToString           (comp_index, game_base, reader)
//     FName::GetClassPtr        (obj_base, reader)
//     FName::GetObjectClassName (obj_base, game_base, reader)
//
// Pipeline (CL-1315578):
//   Slot hash:  ROL32(21/13/21/13), ADD=0xF3D8DA36
//   Slot decrypt: ROL64(29) → PSHUFLW(0x39) → CI = ROL32(hi32, 5)
//   Seed: scalar Pshuflw chain (no runtime SIMD masks)
//   Chunk: seed → PSHUFLW(0x8C) → >>30 → nameOff + chunkAddr
//   Block hash: ROL32(17/13/17/13), ADD=0xD5AF8E52
//   Block decrypt: ROL64(13) → PSHUFLW(0x93) → XOR(blockMask@RVA)
//   FNV: ROL64(37/40), ADD=0x10F3A73711CE0312
//   Name ptr: (Fv2 ^ V15) + V13 + 2*nameOff
//   Header: length=h>>6, isWide=(h & 0x20)
//   String: MUL/ADD PRNG, keyTable 64×u16, separate even/odd index
// =============================================================================
 
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <immintrin.h>
 
#include "arc_decrypt.h"
 
namespace FName {
 
inline std::shared_mutex& g_name_cache_mtx() {
    static std::shared_mutex m;
    return m;
}
inline std::unordered_map<int32_t, std::string>& g_name_cache() {
    static std::unordered_map<int32_t, std::string> c;
    return c;
}
 
// === Common constants ===
static constexpr uint32_t HASH_PRIME       = 0x01000193u;
static constexpr int      SLOT_BASE_OFF    = 0x20;
static constexpr int      SLOT_STRIDE      = 0x20;
static constexpr uint64_t FNV_PRIME_COMMON = 0x100000001B3ULL;
 
// === Pool RVAs (CL-1315578) ===
static constexpr uint64_t RVA_GNAMEPOOL    = 0xE4F2A00ULL;
static constexpr uint64_t RVA_KEYTABLE     = 0xE4318DCULL;
static constexpr uint64_t RVA_BLOCK_MASK   = 0xB523C50ULL;
 
// === Slot hash constants ===
static constexpr uint32_t SLOT_HASH_ADD    = 0xF3D8DA36u;
static constexpr int      HASH_ROL1        = 21;
static constexpr int      HASH_ROL2        = 13;
static constexpr int      HASH_ROL3        = 21;
static constexpr int      HASH_ROL4        = 13;
 
// === Chunk layout ===
static constexpr uint64_t CHUNK_HASH_SEED  = 0x2F90ULL;
static constexpr uint64_t CHUNK_BLOCK_BASE = 0x2FA0ULL;
 
// === Block hash ===
static constexpr uint32_t BLOCK_HASH_ADD   = 0xD5AF8E52u;
 
// === FNV constants ===
static constexpr uint64_t FNV_ADD  = 0x10F3A73711CE0312ULL;
 
// === String decrypt constants ===
static constexpr uint32_t STRING_BIAS_NARROW = 0xFFFFA7B4u;
static constexpr uint32_t STRING_BIAS_WIDE   = 0x0000A7B4u;
static constexpr uint32_t STRING_MUL         = 0x6DDC5690u;
static constexpr uint32_t STRING_ADD         = 0x5EBF2255u;
static constexpr uint32_t STRING_ODD_MUL     = 0xFFFF584Cu;
static constexpr uint32_t STRING_ODD_ADD     = 0xF629u;
 
// === GUObjectArray RVAs (kept for arc_scanner) ===
static constexpr uint64_t RVA_GUOBJECTARRAY_CHUNKS = 0xE3B61C0ULL;
static constexpr uint64_t RVA_GOBJECT_ARRAY_BASE   = RVA_GUOBJECTARRAY_CHUNKS;
static constexpr uint64_t RVA_GOBJ_PSHUFB_MASK     = 0xAD97CC0ULL;
 
// === Per-base runtime state ===
struct FNameState {
    uint64_t gnamePoolRva       = 0;
    uint16_t keyTable[64]       = {};
    uint64_t blockMask          = 0;
    bool     ksLoaded           = false;
    bool     initialised        = false;
};
 
static FNameState& fname_state() {
    static FNameState s;
    return s;
}
 
// === Pshuflw on a 64-bit value (shuffles all 4 words) ===
static inline uint64_t Pshuflw64(uint64_t X, int Imm) {
    uint16_t W[4];
    W[0] = static_cast<uint16_t>(X);
    W[1] = static_cast<uint16_t>(X >> 16);
    W[2] = static_cast<uint16_t>(X >> 32);
    W[3] = static_cast<uint16_t>(X >> 48);
    return static_cast<uint64_t>(W[(Imm >> 0) & 3])
         | (static_cast<uint64_t>(W[(Imm >> 2) & 3]) << 16)
         | (static_cast<uint64_t>(W[(Imm >> 4) & 3]) << 32)
         | (static_cast<uint64_t>(W[(Imm >> 6) & 3]) << 48);
}
 
template<typename TReader>
static bool InitFNameState(uint64_t game_base, TReader& reader) {
    FNameState& s = fname_state();
    if (s.initialised) return true;
 
    s.gnamePoolRva = RVA_GNAMEPOOL;
 
    if (!s.ksLoaded) {
        uint8_t KtBuf[128] = {};
        if (reader.Read(game_base + RVA_KEYTABLE, KtBuf, sizeof(KtBuf))) {
            for (int I = 0; I < 64; ++I)
                std::memcpy(&s.keyTable[I], KtBuf + I * 2, 2);
            int Nz = 0;
            for (int I = 0; I < 64; ++I) Nz += (s.keyTable[I] != 0);
            if (Nz >= 8) {
                s.ksLoaded = true;
                std::printf("[fname] keyTable loaded (%d non-zero)\n", Nz);
            } else {
                std::printf("[-] keyTable mostly zeros (%d nz) — RVA may be stale\n", Nz);
            }
        } else {
            std::printf("[-] keyTable read failed @ RVA 0x%lX\n", (unsigned long)RVA_KEYTABLE);
        }
    }
 
    if (!reader.Read(game_base + RVA_BLOCK_MASK, &s.blockMask, sizeof(uint64_t))) {
        std::printf("[-] blockMask read failed @ RVA 0x%lX\n", (unsigned long)RVA_BLOCK_MASK);
        return false;
    }
 
    if (!s.ksLoaded) {
        std::printf("[-] FName: no keyTable — string decrypt will fail\n");
        return false;
    }
 
    std::printf("[fname] Pipeline ready (GNamePool @ 0x%lX, blockMask=0x%lX)\n",
                (unsigned long)(game_base + s.gnamePoolRva),
                (unsigned long)s.blockMask);
    s.initialised = true;
    return true;
}
 
 
// === GUObjectArray helpers (stale RVAs — kept for arc_scanner weak-ptr resolution) ===
 
template<typename TReader>
static inline uint64_t DecryptChunksManager(uint64_t game_base, TReader& reader) {
    uint64_t GuObjAbs = game_base + RVA_GOBJECT_ARRAY_BASE;
    alignas(16) uint8_t EncBytes[16] = {};
    alignas(16) uint8_t MaskBytes[16] = {};
    if (!reader.Read(GuObjAbs + 0xC0, EncBytes, 16)) return 0;
    if (!reader.Read(game_base + RVA_GOBJ_PSHUFB_MASK, MaskBytes, 16)) return 0;
    __m128i X    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(EncBytes));
    __m128i Mask = _mm_loadu_si128(reinterpret_cast<const __m128i*>(MaskBytes));
    __m128i X1 = _mm_shufflelo_epi16(X, 0x1B);
    __m128i X2 = _mm_or_si128(_mm_slli_epi16(X1, 3), _mm_srli_epi16(X1, 13));
    __m128i X3 = _mm_shuffle_epi8(X2, Mask);
    uint64_t Cm = 0;
    std::memcpy(&Cm, &X3, 8);
    return Cm;
}
 
template<typename TReader>
static inline uint32_t ReadGUObjectArrayNumElements(uint64_t game_base, TReader& reader) {
    uint64_t GuObjAbs = game_base + RVA_GOBJECT_ARRAY_BASE;
    static constexpr uint32_t kCountOffs[] = { 0xFC, 0x30 };
    for (uint32_t Off : kCountOffs) {
        uint32_t Cand = 0;
        if (!reader.Read(GuObjAbs + Off, &Cand, 4)) continue;
        if (Cand >= 1000 && Cand <= 2'000'000) return Cand;
    }
    return 0;
}
 
template<typename TReader>
static inline uint64_t SimdDecryptBlob(uint64_t blob_addr, uint64_t game_base, TReader& reader) {
    alignas(16) uint8_t EncBytes[16] = {};
    alignas(16) uint8_t MaskBytes[16] = {};
    if (!reader.Read(blob_addr, EncBytes, 16)) return 0;
    if (!reader.Read(game_base + RVA_GOBJ_PSHUFB_MASK, MaskBytes, 16)) return 0;
    __m128i X    = _mm_loadu_si128(reinterpret_cast<const __m128i*>(EncBytes));
    __m128i Mask = _mm_loadu_si128(reinterpret_cast<const __m128i*>(MaskBytes));
    __m128i X1 = _mm_shufflelo_epi16(X, 0x1B);
    __m128i X2 = _mm_or_si128(_mm_slli_epi16(X1, 3), _mm_srli_epi16(X1, 13));
    __m128i X3 = _mm_shuffle_epi8(X2, Mask);
    uint64_t Out = 0;
    std::memcpy(&Out, &X3, 8);
    return Out;
}
 
template<typename TReader>
static inline uint64_t FindChunksArray(uint64_t chunks_manager, uint64_t game_base,
                                        uint64_t module_size, TReader& reader,
                                        bool verbose = false) {
    auto InModule    = [&](uint64_t P) { return P >= game_base && P < game_base + module_size; };
    auto HeapNonMod  = [&](uint64_t P) {
        return P > 0x10000ULL && P < 0x800000000000ULL && !InModule(P);
    };
    if (!HeapNonMod(chunks_manager)) return 0;
 
    auto Validate = [&](uint64_t Cand, int& FailAt, uint64_t& FailAddr, uint64_t& FailVal) -> int {
        int V = 0;
        for (int I = 0; I < 2; ++I) {
            uint64_t ChunkPtr = 0;
            if (!reader.Read(Cand + (uint64_t)I * 8, &ChunkPtr, 8))  { FailAt = 0; FailAddr = Cand + I*8; return V; }
            if (!HeapNonMod(ChunkPtr))                                { FailAt = 1; FailVal = ChunkPtr; return V; }
            uint64_t Obj = 0;
            if (!reader.Read(ChunkPtr, &Obj, 8))                      { FailAt = 2; FailAddr = ChunkPtr; return V; }
            if (!HeapNonMod(Obj))                                     { FailAt = 3; FailVal = Obj; return V; }
            uint64_t Vt = 0;
            if (!reader.Read(Obj, &Vt, 8))                            { FailAt = 4; FailAddr = Obj; return V; }
            if (!InModule(Vt))                                        { FailAt = 5; FailVal = Vt; return V; }
            ++V;
        }
        return V;
    };
 
    {
        static constexpr uint32_t kInnerOffs[] = { 0x90, 0xA0, 0xB0, 0x70, 0x80 };
        for (uint32_t BlobOff : kInnerOffs) {
            uint64_t Cand = SimdDecryptBlob(chunks_manager + BlobOff, game_base, reader);
            if (!HeapNonMod(Cand)) {
                if (verbose) std::printf("[mapcal-fca]   SIMD@+0x%03x → 0x%lx (not heap)\n",
                                          BlobOff, (unsigned long)Cand);
                continue;
            }
            int FailAt = -1;
            uint64_t FailAddr = 0, FailVal = 0;
            int Valid = Validate(Cand, FailAt, FailAddr, FailVal);
            if (verbose) std::printf("[mapcal-fca]   SIMD@+0x%03x → 0x%lx valid=%d/2 fa=%d faddr=0x%lx fval=0x%lx\n",
                                      BlobOff, (unsigned long)Cand, Valid, FailAt,
                                      (unsigned long)FailAddr, (unsigned long)FailVal);
            if (Valid >= 2) return Cand;
        }
    }
 
    int CandCount = 0;
    for (uint32_t Off = 0; Off <= 0x400; Off += 8) {
        uint64_t Cand = 0;
        if (!reader.Read(chunks_manager + Off, &Cand, 8)) continue;
        if (!HeapNonMod(Cand)) continue;
        CandCount++;
        int FailAt = -1;
        uint64_t FailAddr = 0, FailVal = 0;
        int Valid = Validate(Cand, FailAt, FailAddr, FailVal);
        if (verbose && CandCount <= 12)
            std::printf("[mapcal-fca]   raw@+0x%03x cand=0x%lx valid=%d/2 fa=%d faddr=0x%lx fval=0x%lx\n",
                        Off, (unsigned long)Cand, Valid, FailAt,
                        (unsigned long)FailAddr, (unsigned long)FailVal);
        if (Valid >= 2) return Cand;
    }
    if (verbose) std::printf("[mapcal-fca] no candidate validated (raw heap-cand count=%d)\n", CandCount);
    return 0;
}
 
// === UObject slot mixing byte ===
 
static inline uint8_t obj_slot_mix_byte(uint64_t ObjPtr) {
    uint64_t P  = ObjPtr + 0x10;
    uint32_t Lo = static_cast<uint32_t>(P);
    uint32_t Hi = static_cast<uint32_t>(P >> 32);
 
    uint32_t A = HASH_PRIME * rotl32(Lo, HASH_ROL1) + SLOT_HASH_ADD;
    uint32_t B = HASH_PRIME * rotl32(A, HASH_ROL2) + Hi + SLOT_HASH_ADD;
    uint32_t C = HASH_PRIME * rotl32(B, HASH_ROL3) + SLOT_HASH_ADD;
    uint32_t V = HASH_PRIME * rotl32(C, HASH_ROL4) + SLOT_HASH_ADD;
 
    return static_cast<uint8_t>(V) ^ static_cast<uint8_t>(V >> 16);
}
 
static inline uint32_t obj_name_slot(uint64_t ObjPtr) {
    return (static_cast<uint32_t>(obj_slot_mix_byte(ObjPtr)) & 3u) ^ 2u;
}
 
static inline uint32_t obj_class_slot(uint64_t ObjPtr) {
    return static_cast<uint32_t>(obj_slot_mix_byte(ObjPtr)) & 3u;
}
 
// === Slot decrypt (CL-1315578) ===
 
static inline uint64_t DecryptUObjSlotName(uint64_t Raw) {
    uint64_t V = rotl64(Raw, 29);
    V = Pshuflw64(V, 0x39);
    uint32_t Ci = rotl32(static_cast<uint32_t>(V >> 32), 5);
    uint32_t Number = static_cast<uint32_t>(V);
    return static_cast<uint64_t>(Ci) | (static_cast<uint64_t>(Number) << 32);
}
 
static inline uint64_t DecryptUObjSlotClass(uint64_t Raw) {
    uint64_t V = rotl64(Raw, 29);
    V = Pshuflw64(V, 0x39);
    return V;
}
 
template<typename TReader>
static inline uint64_t ReadSlotRaw(uint64_t ObjBase, uint32_t Slot, TReader& reader) {
    uint64_t Addr = ObjBase + SLOT_BASE_OFF + static_cast<uint64_t>(Slot) * SLOT_STRIDE;
    uint64_t Raw = 0;
    if (!reader.Read(Addr, &Raw, sizeof(uint64_t))) return 0;
    return Raw;
}
 
template<typename TReader>
static inline uint64_t FindFNameSlot(uint64_t ObjBase, TReader& reader) {
    uint32_t Ns = obj_name_slot(ObjBase);
    uint64_t Raw = ReadSlotRaw(ObjBase, Ns, reader);
    if (!Raw) return 0;
    return DecryptUObjSlotName(Raw);
}
 
// === Seed computation (CL-1315578 — pure scalar, no runtime masks) ===
 
static inline uint64_t ComputeNameSeed(int32_t CompIndex) {
    uint64_t X = static_cast<uint64_t>(static_cast<uint32_t>(CompIndex));
    uint64_t T = (X << 16) >> 50;
    X = Pshuflw64((X << 30) | T, 0x72);
    X = Pshuflw64(X, 0x8C);
    X = (X >> 14) << 14;
    X = Pshuflw64(X, 0x72);
    return X;
}
 
// === CI → FNameEntry* (CL-1315578) ===
 
template<typename TReader>
static inline uint64_t ResolveNamePtr(int32_t CompIndex, uint64_t game_base, TReader& reader) {
    if (CompIndex <= 0) return 0;
 
    const FNameState& S = fname_state();
 
    uint64_t Seed = ComputeNameSeed(CompIndex);
 
    uint32_t Eax = static_cast<uint32_t>(Pshuflw64(Seed, 0x8C) >> 30);
    uint16_t NameOff = static_cast<uint16_t>(Eax);
    uint64_t ChunkAddr = game_base + S.gnamePoolRva +
                         (static_cast<uint64_t>(Eax >> 8) & 0xFFFF00ULL);
 
    uint64_t HashAddr = ChunkAddr + CHUNK_HASH_SEED;
    uint32_t HLo = rotl32(static_cast<uint32_t>(HashAddr), 17);
    uint32_t HHi = static_cast<uint32_t>(HashAddr >> 32);
    uint32_t A = HASH_PRIME * HLo + BLOCK_HASH_ADD;
    uint32_t B = HASH_PRIME * rotl32(A, 13) + HHi + BLOCK_HASH_ADD;
    uint32_t C = HASH_PRIME * rotl32(B, 17) + BLOCK_HASH_ADD;
    uint32_t D = HASH_PRIME * rotl32(C, 13) + BLOCK_HASH_ADD;
    uint32_t Mix = D ^ (D >> 16);
    uint64_t BIdx1 = static_cast<uint64_t>(Mix & 7u);
    uint64_t BIdx2 = static_cast<uint64_t>((Mix + 1u) & 7u);
 
    uint64_t B1Addr = ChunkAddr + CHUNK_BLOCK_BASE + 32ULL * BIdx1;
    uint64_t B2Addr = ChunkAddr + CHUNK_BLOCK_BASE + 32ULL * BIdx2;
    if (!is_valid_ptr(B1Addr) || !is_valid_ptr(B2Addr)) return 0;
 
    uint64_t B1Raw = 0, B2Raw = 0;
    if (!reader.Read(B1Addr, &B1Raw, 8)) return 0;
    if (!reader.Read(B2Addr, &B2Raw, 8)) return 0;
 
    auto DecryptBlock = [&](uint64_t Raw) -> uint64_t {
        uint64_t B = rotl64(Raw, 13);
        B = Pshuflw64(B, 0x93);
        return B ^ S.blockMask;
    };
 
    uint64_t V13 = DecryptBlock(B1Raw);
    uint64_t V15 = DecryptBlock(B2Raw);
 
    uint64_t Fv1 = FNV_PRIME_COMMON * rotl64(V13, 37) + FNV_ADD;
    uint64_t Fv2 = FNV_PRIME_COMMON * rotl64(Fv1, 40) + FNV_ADD;
 
    uint64_t NamePtr = (Fv2 ^ V15) + V13 + 2ULL * NameOff;
    if (!is_valid_ptr(NamePtr)) return 0;
    return NamePtr;
}
 
// === FNameEntry → string (CL-1315578) ===
 
template<typename TReader>
static inline std::string DecryptNameString(uint64_t NameEntryPtr, TReader& reader) {
    if (!NameEntryPtr) return {};
 
    const FNameState& S = fname_state();
    if (!S.ksLoaded) return {};
 
    uint16_t Header = 0;
    if (!reader.Read(NameEntryPtr, &Header, sizeof(uint16_t)) || !Header) return {};
 
    int  Length = static_cast<int>(Header >> 6);
    bool IsWide = (Header & 0x20u) != 0;
    if (Length <= 0 || Length > 256) return {};
 
    if (!IsWide) {
        uint8_t Bytes[256] = {};
        if (!reader.Read(NameEntryPtr + 2, Bytes, Length)) return {};
 
        uint32_t Eax = static_cast<uint32_t>(Length) + STRING_BIAS_NARROW;
        int N = Length & ~1;
        for (int K = 0; 2 * K < N; K++) {
            Bytes[2 * K]     ^= static_cast<uint8_t>(S.keyTable[Eax & 0x3Fu] >> 3);
            Bytes[2 * K + 1] ^= static_cast<uint8_t>(S.keyTable[(Eax * STRING_ODD_MUL + STRING_ODD_ADD) & 0x3Du] >> 3);
            Eax = Eax * STRING_MUL + STRING_ADD;
        }
        if ((Length & 1) != 0)
            Bytes[Length - 1] ^= static_cast<uint8_t>(S.keyTable[Eax & 0x3Fu] >> 3);
 
        std::string Out;
        Out.reserve(Length);
        for (int J = 0; J < Length; ++J) {
            uint8_t Ch = Bytes[J];
            if (!Ch) break;
            Out.push_back((Ch >= 32 && Ch <= 126) ? static_cast<char>(Ch) : '?');
        }
        return Out;
    } else {
        uint16_t Wides[256] = {};
        if (!reader.Read(NameEntryPtr + 2, Wides,
                         static_cast<size_t>(Length) * sizeof(uint16_t))) return {};
 
        uint32_t Eax = static_cast<uint32_t>(Length) + STRING_BIAS_WIDE;
        int N = Length & ~1;
        for (int K = 0; 2 * K < N; K++) {
            Wides[2 * K]     ^= S.keyTable[Eax & 0x3Fu];
            Wides[2 * K + 1] ^= S.keyTable[(Eax * STRING_ODD_MUL + STRING_ODD_ADD) & 0x3Du];
            Eax = Eax * STRING_MUL + STRING_ADD;
        }
        if ((Length & 1) != 0)
            Wides[Length - 1] ^= S.keyTable[Eax & 0x3Fu];
 
        std::string Out;
        Out.reserve(Length);
        for (int J = 0; J < Length; ++J) {
            uint16_t Ch = Wides[J];
            if (!Ch) break;
            Out.push_back((Ch >= 32 && Ch <= 126) ? static_cast<char>(Ch) : '?');
        }
        return Out;
    }
}
 
static inline bool is_sane_name(const std::string& S) {
    if (S.empty() || S.size() > 128) return false;
    int Printable = 0;
    for (unsigned char C : S)
        if (C >= 32 && C <= 126) ++Printable;
    return Printable * 5 >= static_cast<int>(S.size()) * 4;
}
 
// === Public API ===
 
template<typename TReader>
static inline int32_t GetActorFNameId(uint64_t actor_base, uint64_t game_base, TReader& reader) {
    if (!actor_base) return 0;
    if (!InitFNameState(game_base, reader)) return 0;
    uint64_t Raw = FindFNameSlot(actor_base, reader);
    return static_cast<int32_t>(Raw & 0xFFFFFFFFu);
}
 
template<typename TReader>
static inline uint32_t GetActorFNameNumber(uint64_t actor_base, uint64_t game_base, TReader& reader) {
    if (!actor_base) return 0;
    if (!InitFNameState(game_base, reader)) return 0;
    uint64_t Raw = FindFNameSlot(actor_base, reader);
    return static_cast<uint32_t>(Raw >> 32);
}
 
template<typename TReader>
static inline std::string CachedNameString(int32_t comp_index,
                                           uint64_t game_base,
                                           TReader& reader) {
    if (comp_index <= 0) return {};
    {
        std::shared_lock<std::shared_mutex> Lk(g_name_cache_mtx());
        auto It = g_name_cache().find(comp_index);
        if (It != g_name_cache().end()) return It->second;
    }
    if (!InitFNameState(game_base, reader)) return {};
    uint64_t Ptr = ResolveNamePtr(comp_index, game_base, reader);
    if (!Ptr) return {};
    std::string Str = DecryptNameString(Ptr, reader);
    if (Str.empty() || !is_sane_name(Str)) return {};
    {
        std::unique_lock<std::shared_mutex> Lk(g_name_cache_mtx());
        g_name_cache().emplace(comp_index, Str);
    }
    return Str;
}
 
template<typename TReader>
static inline std::string ToString(int32_t comp_index, uint64_t game_base, TReader& reader) {
    return CachedNameString(comp_index, game_base, reader);
}
 
template<typename TReader>
static inline std::string GetActorFNameString(uint64_t actor_base, uint64_t game_base, TReader& reader) {
    if (!actor_base) return {};
    if (!InitFNameState(game_base, reader)) return {};
    uint64_t Raw = FindFNameSlot(actor_base, reader);
    int32_t  CI  = static_cast<int32_t>(Raw & 0xFFFFFFFFu);
    if (CI > 0) {
        std::string Str = CachedNameString(CI, game_base, reader);
        if (!Str.empty()) return Str;
    }
    int32_t RawCI = reader.template ReadVal<int32_t>(actor_base + 0x18);
    if (RawCI > 1 && RawCI < 0x2000000) {
        std::string Str = CachedNameString(RawCI, game_base, reader);
        if (!Str.empty()) return Str;
    }
    return {};
}
 
template<typename TReader>
static inline uint64_t GetClassPtr(uint64_t ObjBase, TReader& reader) {
    if (!ObjBase) return 0;
    uint32_t Slot = obj_class_slot(ObjBase);
    uint64_t Raw = ReadSlotRaw(ObjBase, Slot, reader);
    if (!Raw) return 0;
    uint64_t Decoded = DecryptUObjSlotClass(Raw);
    if (Decoded < 0x10000ULL) return 0;
    if (Decoded >= 0x800000000000ULL) return 0;
    return Decoded;
}
 
template<typename TReader>
static inline std::string GetObjectClassName(uint64_t obj_base, uint64_t game_base, TReader& reader) {
    uint64_t ClassPtr = GetClassPtr(obj_base, reader);
    if (!ClassPtr) return {};
    return GetActorFNameString(ClassPtr, game_base, reader);
}
 
} // namespace FName
Code:
inline void DecryptPlayerName(std::vector<uint16_t>& NameBuffer, int MaxLength)
{
    if (NameBuffer.empty() || MaxLength <= 0)
        return;
    int len = (std::min)(MaxLength, (int)NameBuffer.size());
    if (len < 1 || NameBuffer[0] == 0)
        return;
 
    uint32_t v8 = 0;
    int i = 0;
    for (; i < len && NameBuffer[i] != 0; ++i)
    {
        uint32_t v9 = v8 + rotl32(16777619u * v8 + 0x20003155u, 23);
        v8 = 16777619u * v9;
 
        uint8_t mask = static_cast<uint8_t>((static_cast<uint32_t>(-109) * v9)) & 0x1Fu;
        int v10 = static_cast<int>(NameBuffer[i] ^ mask);
 
        int v11 = 0;
        if (static_cast<uint32_t>(v10 - 80) < 0x2Fu) v11 = -47;
        if (static_cast<uint32_t>(v10 - 33) < 0x2Fu) v11 = 47;
        int v14 = v10 + v11;
 
        int v15 = 0;
        if (static_cast<uint32_t>(v14 - 53) < 5u) v15 = -5;
        if (static_cast<uint32_t>(v14 - 48) < 5u) v15 = 5;
        int v16 = v14 + v15;
 
        int v17 = 0;
        if (static_cast<uint32_t>(v16 - 110) < 0xDu) v17 = -13;
        if (static_cast<uint32_t>(v16 - 97)  < 0xDu) v17 = 13;
        int v18 = v16 + v17;
 
        int v19 = 0;
        if (static_cast<uint32_t>(v18 - 78) < 0xDu) v19 = -13;
        if (static_cast<uint32_t>(v18 - 65) < 0xDu) v19 = 13;
        int v22 = v18 + v19;
 
        int v23 = 0;
        if (static_cast<uint32_t>(v22 - 80) < 0x2Fu) v23 = -47;
        if (static_cast<uint32_t>(v22 - 33) < 0x2Fu) v23 = 47;
 
        NameBuffer[i] = static_cast<uint16_t>(v22 + v23);
    }
    if (i < static_cast<int>(NameBuffer.size()))
        NameBuffer[i] = 0;
}
Code:
static inline uint64_t DecryptBoneArray(uint64_t MeshAddr, IMemoryReader& Reader, uint64_t /*game_base*/ = 0) {
    alignas(16) __m128i Enc{};
    if (!Reader.Read(MeshAddr + 0x7A0, &Enc, sizeof(__m128i))) return 0;
    if (!_mm_cvtsi128_si64(Enc)) return 0;
 
    alignas(16) const __m128i XorKey = _mm_set_epi64x(0LL, static_cast<int64_t>(0x878588013124D57FULL));
    __m128i V = _mm_xor_si128(Enc, XorKey);
    V = _mm_or_si128(_mm_slli_epi32(V, 7), _mm_srli_epi32(V, 25));
    V = _mm_shufflelo_epi16(V, 0x1B);
 
    uint64_t Base = static_cast<uint64_t>(_mm_cvtsi128_si64(V));
    if (!is_valid_ptr(Base)) return 0;
 
    uint32_t LodDword = Reader.ReadVal<uint32_t>(MeshAddr + 0x830);
    uint32_t LodIndex = (LodDword >> 11) & 0x10;
 
    uint64_t BoneAddr = Base + LodIndex + 0xB8;
    if (!is_valid_ptr(BoneAddr)) return 0;
 
    uint64_t BoneArray = Reader.ReadVal<uint64_t>(BoneAddr);
    if (!is_valid_ptr(BoneArray)) return 0;
    return BoneArray;
}
ARC Raiders Offsets — CL-1315578 (2026-07-09)
==================================================

[Global]
UWORLD_BASE_RVA 0xE91A288

[UWorld]
PERSISTENT_LEVEL 0x158 // was 0x110 (CL-1299607)
STREAMING_LEVELS 0x178 // was 0x120
LEVELS 0x4A0 // was 0x368, count=3 in PR
INSTANCE_TIME 0x1E0 // CL-1233465 live verified
GAME_INSTANCE 0x4D8 // was 0x3B0 (encrypted)
LEVEL_COLLECTIONS 0x370 // was 0x210
AUTHORITY_GAME_MODE 0x3F0 // was 0x350
PHYSICS_FIELD 0x560 // was 0x428

[LevelCollection (size=0x78)]
GAME_STATE 0x08
NET_DRIVER 0x10
DEMO_NET_DRIVER 0x18
PERSISTENT_LEVEL 0x20
LEVELS 0x28

[Level]
ACTORS 0x108 // unreflected, live confirmed
ACTOR_COUNT 0x110
ACTOR_MAX 0x114
OWNING_WORLD 0x130
ACTOR_CLUSTER 0x150 // was 0x148

[LevelActorContainer]
ACTORS 0x98
ACTOR_COUNT 0xA8

[GameState]
GAME_STATE_GLOBAL_RVA 0xDCA7C88
GAME_MODE_CLASS 0x03C0
AUTHORITY_GAME_MODE 0x03C8
SPECTATOR_CLASS 0x03D0
PLAYER_ARRAY 0x0480 // count=5 in PracticeRange
B_REPLICATED_HAS_BEGUN_PLAY 0x04A8
REPLICATED_WORLD_TIME_SECONDS 0x04AC
REPLICATED_WORLD_TIME_SECONDS_DOUBLE 0x04B0
SERVER_WORLD_TIME_DELTA_SECONDS 0x04B8
SERVER_WORLD_TIME_SECONDS_UPDATE_FREQ 0x04BC
STAGE_INFO 0x08B8
PLAYER_STATES 0x0940
GAME_PHASE 0x0950
ENEMY_COUNT 0x0980
PICKUP_COUNT 0x0984

[GameInstance]
LOCAL_PLAYERS 0x0120 // was 0x0130

[LocalPlayer]
PLAYER_CONTROLLER 0x00A0
CONTROLLER_ID 0x0270

[GameViewportClient]
WORLD 0x0178 // SDK+0x08, runtime verified

[PlayerController]
PLAYER_STATE 0x3A8
ACKNOWLEDGED_PAWN 0x3E0
CHARACTER 0x3F0
PLAYER_CAMERA_MANAGER 0x4E0
CONTROL_ROTATION 0x418

[PlayerCameraManager]
VIEW_TARGET 0x04B0 // was 0x4A0
POV (FMinimalViewInfo) 0x04C0 // VIEW_TARGET+0x10
Location 0x04C0 // POV+0x00, FVector double
Rotation 0x04E8 // POV+0x28, FRotator double
FOV 0x0510 // POV+0x50, float
PC_OWNER 0x0420
DEFAULT_FOV 0x0430 // float 90.0

[Actor]
OWNER 0x1C0
ROOT_COMPONENT 0x218
PLAYER_STATE 0x3A8
MESH 0x428 // Character::Mesh
HEALTH_COMPONENT 0xDC8

[SceneComponent]
RELATIVE_LOCATION 0x218
RELATIVE_ROTATION 0x230
RELATIVE_SCALE3D 0x248
COMPONENT_VELOCITY 0x260
COMPONENT_TO_WORLD 0x330 // +0x10 from RELATIVE fields
WORLD_LOCATION_DOUBLE 0x350 // CTW+0x20 Translation
B_VISIBLE_BYTE / MASK 0x278 / 0x20
B_HIDDEN_IN_GAME_BYTE / MASK 0x279 / 0x10

[Pickup]
ROOT_COLLIDER 0x460
INTERACTION 0x478
DEFAULT_PICKUP_DATA_ASSET 0x488




[PhysX (NpScene / PxShape / PxGeom)]
NpScene::ACTOR_ARRAY_DATA 0x2618
NpScene::ACTOR_ARRAY_COUNT 0x2620
NpRigidActor::SHAPE_MANAGER 0x30
NpRigidActor::POSE 0xA0
NpShape::LOCAL_POSE 0x90
NpShape::GEOM_TYPE 0xB8
NpShape::GEOM_DATA 0xBC
GuConvexMesh::NB_HULL_VERTICES 0x46
GuConvexMesh::HULL_BLOB_PTR 0x48
GuTriangleMesh::VERTICES 0x28
GuTriangleMesh::TRIANGLES 0x30

[Lighting]
LightComponentBase::BRIGHTNESS 0x3A0 // was 0x3C0
LightComponent::TEMPERATURE 0x3C8 // was 0x3E8
DirectionalLight::SHADOW_CASCADE_BIAS 0x4D0 // was 0x4F0
SkyLight::B_REAL_TIME_CAPTURE 0x3C8 // was 0x3E8
SkyAtmosphere::TRANSFORM_MODE 0x390 // was 0x3B0
VolumetricCloud::LAYER_BOTTOM_ALTITUDE 0x390 // was 0x3B0
HeightFog::FOG_DENSITY 0x390 // was 0x3B0
PostProcess::SETTINGS 0x3A0 // was 0x3C0
PostProcessVolume::SETTINGS 0x3E0 // was 0x3D0