#pragma once

// =============================================================================
// Reflection — the SDK reflection walker (Core/NameCrypto.hpp over DMA).
//
// The API is the contract pinned by Tests/reflection_tests.cpp and consumed by
// Engine::Update (the ReflClass / ReflProps / ReflSample / FStrCheck rows of
// the debug overlay):
//
//   PropertyOffset / PropertyNameIndex  FProperty's encrypted offset + name
//   SuperStruct / PropertyLink /       UStruct link reads and the inheritance
//   NextProperty / WalkClassChain      chain
//   WalkProperties                     the PropertyLink chain -> PropertyInfo
//   BuildReport                        chain + properties in one pass
//   CheckFStringPipeline               the verification FString self-check
//                                      that proves which player-name scramble
//                                      this build uses (Update feeds the
//                                      winner to SetPreferredNameKey)
//   ClassOf                            the object's UClass (encrypted slots)
//   ResolveNameString                  CI -> text through the v20260922 FName
//                                      pipeline — the resolver BoneRoster
//                                      calibration was starving on (resolved:0
//                                      with the legacy plain walk)
//
// Layout comes from the generated Offsets.h (via Core/SteamDecrypt.hpp — the
// dump is the source of truth); the crypto comes from Core/NameCrypto.hpp
// (ported 1:1 from sdk/sdk.txt). Every read goes through steam_decrypt::MemRead
// so Tests drive the whole pipeline through the fake-memory seam
// (Tests/fake_mem.hpp).
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/NameCrypto.hpp"

// Memory interface — routed through the steam_decrypt::MemRead test seam
// (Pillar 1 / docs/aplus-plan.md). vmmdll warnings are ThirdParty's.
#pragma warning(push)
#pragma warning(disable : 4201)
#include "Core/SteamDecrypt.hpp"
#pragma warning(pop)

namespace Reflection {

// Adapter so every namecrypto walk reads through the project's DMA layer
// (and its test seam) via steam_decrypt::MemRead.
struct MemoryReader {
    bool Read(uint64_t addr, void* dst, size_t n) const
    {
        return steam_decrypt::MemRead(addr, dst, n);
    }
};

inline MemoryReader Dma()
{
    return MemoryReader{};
}

// ---------------------------------------------------------------------------
// v20260922 FName pipeline state + name cache
// ---------------------------------------------------------------------------

inline namecrypto::PipelineState& NameState()
{
    static namecrypto::PipelineState s;
    return s;
}

inline std::mutex& NameInitMutex()
{
    static std::mutex m;
    return m;
}

inline std::shared_mutex& NameCacheMutex()
{
    static std::shared_mutex m;
    return m;
}

inline std::unordered_map<int32_t, std::string>& NameCache()
{
    static std::unordered_map<int32_t, std::string> c;
    return c;
}

// Drops every memoized name and forces a keystream reload. Called on detach /
// re-attach (a new build carries a new keystream and GName pool) and by tests
// that swap synthetic memory images.
inline void ResetNames()
{
    {
        std::unique_lock<std::shared_mutex> lock(NameCacheMutex());
        NameCache().clear();
    }
    std::lock_guard<std::mutex> lock(NameInitMutex());
    NameState().ksLoaded = false;
}

// Loads the v20260922 keystream (drop: InitFNameState). Idempotent. A
// mostly-zero table means the RVA is stale for this build and fails loudly.
inline bool EnsureNames(uint64_t gameBase)
{
    namecrypto::PipelineState& s = NameState();
    if (s.ksLoaded)
        return true;
    std::lock_guard<std::mutex> lock(NameInitMutex());
    if (s.ksLoaded)
        return true;

    s.gnamePoolRva = namecrypto::kRvaGnamePool;

    uint8_t ksBuf[namecrypto::kKeystreamCount * 2] = {};
    const MemoryReader reader = Dma();
    if (!reader.Read(gameBase + namecrypto::kRvaKeystream, ksBuf, sizeof(ksBuf)))
        return false;
    int nonZero = 0;
    for (int i = 0; i < namecrypto::kKeystreamCount; ++i) {
        uint16_t w = 0;
        std::memcpy(&w, ksBuf + i * 2, 2);
        s.keystream[i] = w;
        nonZero += (w != 0) ? 1 : 0;
    }
    if (nonZero < 32)
        return false;
    s.ksLoaded = true;
    return true;
}

// CI -> text through the v20260922 pipeline. Empty when the index does not
// resolve or the decode is implausible (never garbage). Only resolved names
// are memoized, so a keystream that loads late still recovers.
inline std::string ResolveNameString(int32_t compIndex, uint64_t gameBase)
{
    if (compIndex < 0)
        return {};
    {
        std::shared_lock<std::shared_mutex> lock(NameCacheMutex());
        auto it = NameCache().find(compIndex);
        if (it != NameCache().end())
            return it->second;
    }
    if (!EnsureNames(gameBase))
        return {};

    const MemoryReader reader = Dma();
    std::string out;
    if (const uint64_t entry =
            namecrypto::ResolveNamePtr(compIndex, gameBase, NameState(), reader)) {
        out = namecrypto::DecryptNameString(entry, NameState(), reader);
        if (!namecrypto::IsSaneName(out))
            out.clear();
    }
    if (!out.empty()) {
        std::unique_lock<std::shared_mutex> lock(NameCacheMutex());
        NameCache()[compIndex] = out;
    }
    return out;
}

// ---------------------------------------------------------------------------
// FProperty primitives (the decoders Tests/reflection_tests.cpp pins)
// ---------------------------------------------------------------------------

// Property offsets live well inside the first 64 KiB of an instance; decoded
// values past that are misreads and collapse to 0.
constexpr uint32_t kMaxPropertyOffset = 0x10000u;

// Encrypted property offset (drop: decode_ffproperty_offset).
inline uint32_t PropertyOffset(uint64_t prop)
{
    if (!namecrypto::IsPlausibleNamePtr(prop))
        return 0;
    const uint32_t raw = namecrypto::ReadVal<uint32_t>(
        Dma(), prop + namecrypto::kFfieldOffsetEnc);
    const uint32_t off = namecrypto::DecodeFfPropertyOffset(raw);
    return (off < kMaxPropertyOffset) ? off : 0;
}

// Encrypted FField name -> comp index (low 32 of the decoded packed FName).
// Negative when the property cannot be read.
inline int32_t PropertyNameIndex(uint64_t prop)
{
    if (!namecrypto::IsPlausibleNamePtr(prop))
        return -1;
    const MemoryReader reader = Dma();
    const uint64_t src = namecrypto::ReadVal<uint64_t>(
        reader, prop + namecrypto::kFfieldFnameEnc);
    const uint64_t second = namecrypto::ReadVal<uint64_t>(
        reader, prop + namecrypto::kFfieldFnameEnc + 8);
    return static_cast<int32_t>(
        namecrypto::DecodeFFieldName(src, second) & 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// UStruct links (Offsets.h — the generated dump values)
// ---------------------------------------------------------------------------

inline uint64_t SuperStruct(uint64_t cls)
{
    if (!namecrypto::IsPlausibleNamePtr(cls))
        return 0;
    return namecrypto::ReadVal<uint64_t>(
        Dma(), cls + Offsets::UStruct_SuperStruct);
}

// The linked-properties head (the dump's UStruct.Children).
inline uint64_t PropertyLink(uint64_t cls)
{
    if (!namecrypto::IsPlausibleNamePtr(cls))
        return 0;
    return namecrypto::ReadVal<uint64_t>(
        Dma(), cls + Offsets::UStruct_PropertyLink);
}

inline uint64_t NextProperty(uint64_t prop)
{
    if (!namecrypto::IsPlausibleNamePtr(prop))
        return 0;
    return namecrypto::ReadVal<uint64_t>(
        Dma(), prop + Offsets::FProperty_PropertyLinkNext);
}

// The inheritance chain: class, super, super's super, ... (cycle-guarded).
inline std::vector<uint64_t> WalkClassChain(uint64_t classPtr, uint64_t gameBase)
{
    (void)gameBase; // reserved for name-tagged chains
    std::vector<uint64_t> chain;
    uint64_t cls = classPtr;
    while (namecrypto::IsPlausibleNamePtr(cls) && chain.size() < 32) {
        if (std::find(chain.begin(), chain.end(), cls) != chain.end())
            break;
        chain.push_back(cls);
        cls = SuperStruct(cls);
    }
    return chain;
}

// ---------------------------------------------------------------------------
// Property walk + class report
// ---------------------------------------------------------------------------

struct PropertyInfo {
    std::string name;       // resolved through the pipeline ("" when the name
                            // does not resolve — never garbage)
    uint32_t offset = 0;    // decoded FProperty offset (0 when implausible)
    uint64_t typeClass = 0; // FObjectPropertyBase::PropertyClass (wantTypes)
    std::string typeName;   // resolved class name (wantTypes)
};

// Walks the PropertyLink chain (linked-properties head + FField::Next links)
// into at most maxProps entries. Inherited properties are part of the same
// linked list, so one walk covers the whole class.
inline std::vector<PropertyInfo> WalkProperties(uint64_t classPtr,
                                               uint64_t gameBase,
                                               int maxProps, bool wantTypes)
{
    std::vector<PropertyInfo> out;
    if (!namecrypto::IsPlausibleNamePtr(classPtr) || maxProps <= 0)
        return out;

    const MemoryReader reader = Dma();
    uint64_t prop = PropertyLink(classPtr);
    // The chain repeats inherited links; the node cap guards a corrupted Next.
    const uint64_t nodeCap = static_cast<uint64_t>(maxProps) * 8 + 64;
    for (uint64_t nodes = 0;
         namecrypto::IsPlausibleNamePtr(prop) &&
             out.size() < static_cast<size_t>(maxProps) && nodes < nodeCap;
         ++nodes) {
        PropertyInfo info;
        const int32_t ci = PropertyNameIndex(prop);
        if (ci >= 0)
            info.name = ResolveNameString(ci, gameBase);
        info.offset = PropertyOffset(prop);
        if (wantTypes) {
            info.typeClass = namecrypto::ReadVal<uint64_t>(
                reader, prop + Offsets::FObjectPropertyBase_PropertyClass);
            if (namecrypto::IsPlausibleNamePtr(info.typeClass)) {
                const uint64_t packed =
                    namecrypto::FindFNameSlot(info.typeClass, reader);
                const int32_t tci = static_cast<int32_t>(packed & 0xFFFFFFFFu);
                if (tci >= 0)
                    info.typeName = ResolveNameString(tci, gameBase);
            }
        }
        out.push_back(std::move(info));
        prop = NextProperty(prop);
    }
    return out;
}

struct ClassReport {
    uint64_t uclass = 0;
    std::vector<uint64_t> chain;
    std::vector<PropertyInfo> properties;
};

inline ClassReport BuildReport(uint64_t classPtr, uint64_t gameBase,
                               int maxProps, bool wantTypes)
{
    ClassReport report;
    if (!namecrypto::IsPlausibleNamePtr(classPtr))
        return report; // invalid class -> empty report
    report.uclass = classPtr;
    report.chain = WalkClassChain(classPtr, gameBase);
    report.properties = WalkProperties(classPtr, gameBase, maxProps, wantTypes);
    return report;
}

// ---------------------------------------------------------------------------
// Object introspection (encrypted UObject slots)
// ---------------------------------------------------------------------------

// SDK.hpp's older translation of the slot hash: its first round rotates the
// seed instead of shifting it (the drop revises these hashes across builds —
// like OFFSET_XOR_KEY "was 0xEE0CA1CB"). Kept as a fallback so ClassOf works
// whether the build runs the drop's form or SDK.hpp's.
inline uint32_t ObjSlotIndexBaseLegacy(uint64_t objPtr)
{
    const uint64_t seed = objPtr + 0x10;
    const uint32_t lo = static_cast<uint32_t>(seed);
    const uint32_t hi = static_cast<uint32_t>(seed >> 32);
    const uint32_t p = namecrypto::kHashPrime;
    const uint32_t a = namecrypto::kUobjSlotHashAdd;
    uint32_t h = p * namecrypto::Rol32(lo, 28) + a;
    h = p * namecrypto::Rol32(h, 21) + hi + a;
    h = p * namecrypto::Rol32(h, 28) + a;
    const uint32_t v3 = p * (h >> 11) + a;
    return (static_cast<uint32_t>(static_cast<uint8_t>(v3)) ^
            static_cast<uint32_t>(static_cast<uint8_t>(v3 >> 16))) & 3u;
}

// The object's UClass through the encrypted slots (ClassPrivate). The drop's
// v20260922 slot hash leads; the legacy hash and a bounded sweep of the four
// slots cover a patch that reshuffles them.
inline uint64_t ClassOf(uint64_t obj)
{
    if (!namecrypto::IsPlausibleNamePtr(obj))
        return 0;
    const MemoryReader reader = Dma();
    const uint32_t order[6] = {
        namecrypto::ObjSlotIndexBase(obj), // drop: class slot = index base
        ObjSlotIndexBaseLegacy(obj),       // SDK.hpp's older hash
        0u, 1u, 2u, 3u
    };
    bool tried[4] = { false, false, false, false };
    for (uint32_t slot : order) {
        slot &= 3u;
        if (tried[slot])
            continue;
        tried[slot] = true;
        const uint64_t cls = namecrypto::ReadSlotDecodedAsPtr(obj, slot, reader);
        if (namecrypto::IsPlausibleNamePtr(cls))
            return cls;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// FString verification self-check
// ---------------------------------------------------------------------------

struct FStringCheck {
    bool readOk = false; // the verification FString was read this call
    int candidate = -1;  // scramble index (0..4, see below), 5 = plain text,
                         // -1 = nothing decoded
    std::string text;    // "<label>:<decoded>" for the diagnostics row
};

inline bool IsPrintableAscii(const std::string& s)
{
    if (s.empty())
        return false;
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7E)
            return false;
    return true;
}

// Reads the SDK's verification FString (Offsets::FStringVerificationRva) and
// records which player-name scramble decodes it to printable text. The winner
// is the candidate index steam_decrypt::TryPreferredPlayerNameKey understands:
//   0=current  1=simd  2=legacy  3=forum  4=sdk  (5=plain: not scrambled)
// Update keeps 0..4 as the verified key; plain text is the safety net.
inline FStringCheck CheckFStringPipeline(uint64_t gameBase)
{
    FStringCheck out;
    if (!gameBase)
        return out;

    const MemoryReader reader = Dma();
    const uint64_t addr = gameBase + Offsets::FStringVerificationRva;
    uint64_t textData = 0;
    int32_t num = 0;
    if (!reader.Read(addr, &textData, sizeof(textData)) ||
        !reader.Read(addr + 8, &num, sizeof(num)))
        return out;
    // Num() counts the terminator too. Nonsense lengths are refused outright.
    if (num < 2 || num > 64)
        return out;

    // Heap payload pointer; small inline buffers read from the struct itself.
    std::vector<uint16_t> chars(static_cast<size_t>(num), 0);
    if (namecrypto::IsPlausibleNamePtr(textData)) {
        for (int32_t i = 0; i < num; ++i)
            reader.Read(textData + static_cast<uint64_t>(i) * 2,
                        &chars[static_cast<size_t>(i)], 2);
    } else if (num <= 8) {
        for (int32_t i = 0; i < num; ++i)
            reader.Read(addr + static_cast<uint64_t>(i) * 2,
                        &chars[static_cast<size_t>(i)], 2);
    } else {
        return out;
    }
    out.readOk = true;

    using Scramble = void (*)(std::vector<uint16_t>&, int);
    static const Scramble kScrambles[5] = {
        &steam_decrypt::DecryptPlayerName,
        &steam_decrypt::DecryptPlayerNameSimd,
        &steam_decrypt::DecryptPlayerNameLegacy,
        &steam_decrypt::DecryptPlayerNameForum,
        &steam_decrypt::DecryptPlayerNameSdk,
    };
    static const char* kLabels[6] = {
        "cur", "simd", "legacy", "forum", "sdk", "plain"
    };

    // Num includes the terminator: try the real length first, then the padded.
    const int lens[2] = { num - 1, num };
    for (int len : lens) {
        if (len < 1)
            continue;
        for (int k = 0; k < 5; ++k) {
            std::vector<uint16_t> copy(chars.begin(), chars.begin() + len);
            kScrambles[k](copy, len);
            const std::string text =
                steam_decrypt::WideCharsToPlayerName(copy, len);
            if (IsPrintableAscii(text)) {
                out.candidate = k;
                out.text = std::string(kLabels[k]) + ":" + text;
                return out;
            }
        }
        // Plain text only wins when nothing scrambled the payload.
        const std::string raw =
            steam_decrypt::WideCharsToPlayerName(chars, len);
        if (IsPrintableAscii(raw)) {
            out.candidate = 5;
            out.text = std::string(kLabels[5]) + ":" + raw;
            return out;
        }
    }
    return out; // read ok, nothing decoded: candidate stays -1
}

} // namespace Reflection
