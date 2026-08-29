// Memory-read injection + FName decode pipeline suite — Pillar 1
// (docs/aplus-plan.md). steam_decrypt::g_memReadOverride routes every read
// through a fake backing store, so the REAL GNames / PlayerName decode math
// runs on synthetic buffers — no DMA hardware, no game.

#include "tests_main.hpp"

// SteamDecrypt.hpp pulls in vmmdll.h/leechcore.h (via Memory.h) which use
// nameless struct/union under /W4 — ThirdParty must never fail the build.
#pragma warning(push)
#pragma warning(disable : 4201)
#include "Core/SteamDecrypt.hpp"
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

#include <cstring>
#include <map>
#include <string>

namespace {

// ── Fake backing store ──────────────────────────────────────────────────────
class FakeMem {
public:
    std::map<uint64_t, uint8_t> buf;

    void write(uint64_t addr, const void* data, size_t n)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i)
            buf[addr + i] = p[i];
    }
    void writeU16(uint64_t addr, uint16_t v) { write(addr, &v, sizeof(v)); }
    void writeU64(uint64_t addr, uint64_t v) { write(addr, &v, sizeof(v)); }

    // Reads succeed with 0-fill for gaps (mirrors DMA page-cache behavior).
    bool read(uint64_t addr, void* out, size_t n) const
    {
        auto* p = static_cast<uint8_t*>(out);
        for (size_t i = 0; i < n; ++i) {
            const auto it = buf.find(addr + i);
            p[i] = (it != buf.end()) ? it->second : 0;
        }
        return true;
    }

    static bool readFn(uint64_t addr, void* out, size_t n)
    {
        return s_instance && s_instance->read(addr, out, n);
    }
    static FakeMem* s_instance;
};
FakeMem* FakeMem::s_instance = nullptr;

struct ScopedFakeMem {
    FakeMem mem;
    ScopedFakeMem()
    {
        FakeMem::s_instance = &mem;
        steam_decrypt::g_memReadOverride = &FakeMem::readFn;
    }
    ~ScopedFakeMem()
    {
        steam_decrypt::g_memReadOverride = nullptr;
        FakeMem::s_instance = nullptr;
    }
};

struct ScopedFailReader {
    static bool failFn(uint64_t, void*, size_t) { return false; }
    ScopedFailReader() { steam_decrypt::g_memReadOverride = &failFn; }
    ~ScopedFailReader() { steam_decrypt::g_memReadOverride = nullptr; }
};

// ── helpers over the GNames constants ───────────────────────────────────────
uint64_t rol64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }
uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// Inverse of GNames::DecodeBlock (bijective: ROL/XOR steps).
uint64_t inverseDecodeBlock(uint64_t v)
{
    const uint32_t d0 = static_cast<uint32_t>(v);
    const uint32_t d1 = static_cast<uint32_t>(v >> 32);
    const uint64_t x =
        static_cast<uint64_t>(rol32(d0, 30)) |
        (static_cast<uint64_t>(rol32(d1, 30)) << 32);
    return rol64(x ^ GNames::BlockXor, 60); // ROR64 by 4 == ROL64 by 60
}

struct PipelineImage {
    uint64_t base = 0x1000000;
    uint64_t entryPtr = 0;
};

// Builds a consistent fake memory image: keystream (144 x 0x1234), one shard
// pair, and returns the FNameEntry address for a chosen V13 + name offset.
PipelineImage installPipeline(FakeMem& fm, uint64_t v13, int nameOff)
{
    const uint64_t base = 0x1000000;

    // Keystream: 144 x 0x1234 (≥8 nonzero, so GNames::Init accepts it).
    for (int i = 0; i < 144; ++i)
        fm.writeU16(base + GNames::KeystreamRva + 2u * static_cast<uint64_t>(i), 0x1234);

    const uint64_t chunkAddr = base + GNames::NamesOffset; // ChunkOff = 0
    uint32_t b1 = 0, b2 = 0;
    GNames::ShardHash(chunkAddr + GNames::ShardSeedOff, b1, b2);
    const uint64_t blockBase = chunkAddr + GNames::ShardBlockBaseOff;
    fm.writeU64(blockBase + GNames::ShardBlockStride * b1, inverseDecodeBlock(v13));
    fm.writeU64(blockBase + GNames::ShardBlockStride * b2, 0);

    const uint64_t v15 = GNames::DecodeBlock(0);
    uint64_t fv = GNames::FnvPrime * rol64(v13, GNames::FnvRol1) + GNames::FnvAdd;
    fv = GNames::FnvPrime * rol64(fv, GNames::FnvRol2) + GNames::FnvAdd;
    const uint64_t entry = v13 + (v15 ^ fv) + 2ULL * static_cast<uint64_t>(nameOff);
    return { base, entry };
}

// Finds a V13 whose resulting EntryPtr is a valid usermode pointer.
// EntryPtr ≈ V13 + (V15 ^ Fv(V13)) behaves like a random 64-bit value, so a
// valid (<2^47) hit is ~1/131072 — search enough space to make failure
// essentially impossible (2^22 attempts).
PipelineImage installPipelineWithValidEntry(FakeMem& fm, int nameOff)
{
    uint64_t v13 = 0x12345678;
    for (int attempt = 0; attempt < (1 << 22); ++attempt) {
        PipelineImage img = installPipeline(fm, v13, nameOff);
        if (steam_decrypt::ValidPtr(img.entryPtr))
            return img;
        v13 += 0x1000;
    }
    return { 0, 0 }; // unreachable: p(miss) ≈ e^-32
}

} // namespace

TEST_CASE("g_memReadOverride routes MemRead") {
    ScopedFakeMem fm;
    fm.mem.writeU64(0x7777, 0xDEADBEEFCAFEBABEULL);
    uint64_t v = 0;
    CHECK(steam_decrypt::MemRead(0x7777, &v, sizeof(v)));
    CHECK(v == 0xDEADBEEFCAFEBABEULL);

    ScopedFailReader fr;
    CHECK_FALSE(steam_decrypt::MemRead(0x7777, &v, sizeof(v)));
}

TEST_CASE("GNames::ClmulLo known vectors")
{
    CHECK(GNames::ClmulLo(0, 0x1234) == 0);
    CHECK(GNames::ClmulLo(0x1234, 0) == 0);
    CHECK(GNames::ClmulLo(1, 0x5) == 0x5);
    CHECK(GNames::ClmulLo(0x3, 0x3) == 0x5);   // 11 x 11 carryless = 101
    CHECK(GNames::ClmulLo(0xA, 0x6) == 0x3C);  // 1010 x 110 carryless = 111100
}

TEST_CASE("GNames::DecodeSlot16")
{
    CHECK(GNames::DecodeSlot16(0, 0) == 0);

    const uint64_t a = GNames::DecodeSlot16(0x1122334455667788ULL, 0xAABBCCDDEEFF0011ULL);
    const uint64_t b = GNames::DecodeSlot16(0x1122334455667788ULL, 0xAABBCCDDEEFF0011ULL);
    CHECK(a == b);  // deterministic

    CHECK(GNames::DecodeSlot16(1, 0) != 0);
}

TEST_CASE("GNames slot selection relations")
{
    for (uint64_t ptr : { 0x1000ULL, 0x12345678ULL, 0x7FFFFFFFFFFFULL, 0xABCDEFULL }) {
        const uint32_t h = GNames::SlotHash(ptr);
        CHECK(GNames::NameSlot(ptr) == ((h & 3u) ^ 2u));
        CHECK(GNames::ClassSlot(ptr) == ((h & 3u) ^ 0u));
        CHECK(GNames::OuterSlot(ptr) == ((h & 3u) ^ 1u));
        CHECK(GNames::NameSlot(ptr) <= 3);
    }
}

TEST_CASE("GNames FName pipeline end-to-end on synthetic memory") {
    ScopedFakeMem fm;
    // ResetTables (not just GNames::Reset) so fname_state().initialised is
    // cleared too — InitFNameState would otherwise short-circuit on state
    // carried over from earlier test cases.
    steam_decrypt::ResetTables();

    // narrow name "test_item" (9 chars), plaintext
    const PipelineImage img = installPipelineWithValidEntry(fm.mem, 9);
    REQUIRE(steam_decrypt::ValidPtr(img.entryPtr));

    const char name[] = "test_item";
    fm.mem.writeU16(img.entryPtr, static_cast<uint16_t>(9)); // header: len 9, narrow
    const uint8_t key = static_cast<uint8_t>(0x1234 >> 3); // 0x246 & 0xFF
    for (int i = 0; i < 9; ++i) {
        const uint8_t raw = static_cast<uint8_t>(name[i]);
        const uint8_t enc = static_cast<uint8_t>(raw ^ key);
        fm.mem.write(img.entryPtr + 2 + i, &enc, 1);
    }

    REQUIRE(steam_decrypt::InitFNameState(img.base));
    const uint64_t ptr = GNames::ResolveNamePointer(img.base, 9);
    CHECK(ptr == img.entryPtr);
    CHECK(GNames::DecodeString(ptr) == "test_item");
    CHECK(steam_decrypt::CachedNameString(9, img.base) == "test_item");
}

TEST_CASE("GNames wide FName decode") {
    ScopedFakeMem fm;
    steam_decrypt::ResetTables();

    const PipelineImage img = installPipelineWithValidEntry(fm.mem, 3);
    REQUIRE(steam_decrypt::ValidPtr(img.entryPtr));

    fm.mem.writeU16(img.entryPtr, static_cast<uint16_t>(0x8000 | 3)); // wide, len 3
    for (int i = 0; i < 3; ++i) {
        const uint16_t raw = static_cast<uint16_t>('A' + i);
        const uint16_t enc = static_cast<uint16_t>(raw ^ 0x1234);
        fm.mem.writeU16(img.entryPtr + 2 + 2 * i, enc);
    }

    REQUIRE(steam_decrypt::InitFNameState(img.base));
    const uint64_t ptr = GNames::ResolveNamePointer(img.base, 3);
    CHECK(ptr == img.entryPtr);
    CHECK(GNames::DecodeString(ptr) == "ABC");
}

TEST_CASE("GNames DecodeString rejects garbage") {
    ScopedFakeMem fm;

    // Not ready yet -> empty.
    steam_decrypt::ResetTables();
    CHECK(GNames::DecodeString(0).empty());
    CHECK(GNames::DecodeString(0x1000).empty());

    // Install a keystream (144 x 0x1234) so GNames::Init accepts it, then init.
    for (int i = 0; i < 144; ++i)
        fm.mem.writeU16(0x1000000ULL + GNames::KeystreamRva + 2u * static_cast<uint64_t>(i), 0x1234);
    REQUIRE(steam_decrypt::InitFNameState(0x1000000));

    CHECK(GNames::DecodeString(0).empty());     // null ptr
    fm.mem.writeU16(0x1000, static_cast<uint16_t>(0));   // header 0
    CHECK(GNames::DecodeString(0x1000).empty());
    fm.mem.writeU16(0x1000, static_cast<uint16_t>(0x8000)); // wide, len 0
    CHECK(GNames::DecodeString(0x1000).empty());
    fm.mem.writeU16(0x1000, static_cast<uint16_t>(1024)); // len 1024 > 1023
    CHECK(GNames::DecodeString(0x1000).empty());
}

TEST_CASE("PlayerName scramble helpers")
{
    CHECK(PlayerName::RotateCharacter('a', 'a', 'z', 13) == 'n');
    CHECK(PlayerName::RotateCharacter('n', 'a', 'z', 13) == 'a'); // wrap
    CHECK(PlayerName::RotateCharacter('0', '0', '9', 5) == '5');
    CHECK(PlayerName::RotateCharacter('!', 33, 126, 47) == 'P'); // 33+47=80
    CHECK(PlayerName::RotateCharacter(0x00, 33, 126, 47) == 0x00); // out of range

    CHECK(steam_decrypt::rotl32(0x80000000u, 1) == 1u);
    CHECK(steam_decrypt::rotl32(0x1u, 31) == 0x80000000u);
}

TEST_CASE("Plausibility gates")
{
    CHECK(steam_decrypt::IsPlausibleFNameText("Item_HealthKit"));
    CHECK_FALSE(steam_decrypt::IsPlausibleFNameText(""));
    CHECK_FALSE(steam_decrypt::IsPlausibleFNameText(std::string(200, 'a')));
    CHECK_FALSE(steam_decrypt::IsPlausibleFNameText(std::string("\x01\x02\x03", 3)));

    CHECK(steam_decrypt::IsPlausibleArcPlayerName("Execoper"));
    CHECK(steam_decrypt::IsPlausibleArcPlayerName("Asero_2017"));
    // NOTE: "xX_Slayer_Xx" is REJECTED by the algorithm (10 letters, 2 vowels
    // -> vowel ratio 8 < 10) — pinned here so that behavior stays visible.
    CHECK_FALSE(steam_decrypt::IsPlausibleArcPlayerName("xX_Slayer_Xx"));
    CHECK_FALSE(steam_decrypt::IsPlausibleArcPlayerName(""));
    CHECK_FALSE(steam_decrypt::IsPlausibleArcPlayerName("a"));
    CHECK_FALSE(steam_decrypt::IsPlausibleArcPlayerName("2!}lvfc")); // scrambled
    CHECK_FALSE(steam_decrypt::IsPlausibleArcPlayerName(std::string("q\x17", 2)));
    CHECK_FALSE(steam_decrypt::IsPlausibleArcPlayerName(std::string(40, 'a')));
    CHECK_FALSE(steam_decrypt::IsPlausibleArcPlayerName("!startspunct"));
}