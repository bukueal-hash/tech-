// Memory-read injection + FName decode pipeline suite — Pillar 1
// (docs/aplus-plan.md). steam_decrypt::g_memReadOverride routes every read
// through a fake backing store, so the REAL GNames / PlayerName decode math
// runs on synthetic buffers — no DMA hardware, no game.
//
// The pipeline under test is the SDK drop (Core/SDK.hpp): pool @ GNAMES,
// u16[64] key table @ KEYTABLE, chunk window (selector seed @ +0x40, eight
// 0x20-byte blocks @ +0x50), entry string via game::gasm::decode_fname. The
// legacy v818 shard/keystream path is still present as the fallback and is
// inert here (the fake store carries no legacy keystream), so a pass means the
// SDK path produced the answer.

#include "tests_main.hpp"

// SteamDecrypt.hpp pulls in vmmdll.h/leechcore.h (via Memory.h) which use
// nameless struct/union under /W4 — ThirdParty must never fail the build.
#pragma warning(push)
#pragma warning(disable : 4201)
#include "Core/SteamDecrypt.hpp"
#pragma warning(pop)

#include "fake_mem.hpp"   // FakeMem / ScopedFakeMem / ScopedFailReader

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

#include <cstring>
#include <string>
#include <vector>

namespace {

// ── SDK-scheme helpers ──────────────────────────────────────────────────────

constexpr uint16_t kKeyTableValue = 0x1234;

// The FName key table exactly as production builds it: u16[64] at +0x18 of the
// window read from base + KEYTABLE.
void writeKeyTable(FakeMem& fm, uint64_t base)
{
    for (int i = 0; i < 64; ++i)
        fm.writeU16(base + GNames::KeyTableRva + 0x18 + 2u * static_cast<uint64_t>(i),
            kKeyTableValue);
}

// Test-side copy of that window, for building ciphertext and references.
struct KeyTableWindow {
    std::vector<uint8_t> raw = std::vector<uint8_t>(GNames::KeyTableWindow, 0);
    KeyTableWindow()
    {
        for (int i = 0; i < 64; ++i)
            std::memcpy(raw.data() + 0x18 + i * 2, &kKeyTableValue, sizeof(kKeyTableValue));
    }
    const uint16_t* base() const { return reinterpret_cast<const uint16_t*>(raw.data()); }
};

// Inverse of game::gasm::decode_fname_header_len:
//   len = ((hdr >> 3) & 0x3F8) + (hdr >> 13)   (bit 0 = bIsWide, unused by length)
uint16_t headerFor(uint32_t len, bool wide)
{
    const uint16_t hdr = static_cast<uint16_t>((((len >> 3) & 0x7Fu) << 6) |
                                               ((len & 7u) << 13));
    return static_cast<uint16_t>(hdr | (wide ? 0x1u : 0u));
}

// Ciphertext for `plain`: decode_fname over a zero buffer yields the keystream.
std::vector<uint8_t> encodeFName(uint16_t header, const std::string& plain,
    const KeyTableWindow& kt)
{
    std::vector<uint8_t> buf(plain.size(), 0);
    game::gasm::decode_fname(header, buf.data(), kt.base());
    for (size_t i = 0; i < plain.size(); ++i)
        buf[i] = static_cast<uint8_t>(buf[i] ^ static_cast<uint8_t>(plain[i]));
    return buf;
}

// A key table with distinct entries. The uniform table above cannot tell the
// two seed variants apart: a constant table yields a constant keystream, so
// every seed would "decode" identically.
struct KeyTableWindowVaried {
    std::vector<uint8_t> raw = std::vector<uint8_t>(GNames::KeyTableWindow, 0);
    KeyTableWindowVaried()
    {
        for (int i = 0; i < 64; ++i) {
            const uint16_t v = static_cast<uint16_t>(0x0100u + 0x0101u * static_cast<uint32_t>(i));
            std::memcpy(raw.data() + 0x18 + i * 2, &v, sizeof(v));
        }
    }
    const uint16_t* base() const { return reinterpret_cast<const uint16_t*>(raw.data()); }
};

// ── v20260922 (CL-1389382) keystream-LCG name decode ────────────────────────
// The two drops write the narrow seed differently — the older "length + 0xADF"
// and the current "length - 33" — and it is tempting to "fix" one to the
// other. They are the same key: only the low byte feeds the six-bit index, and
// -33 mod 256 == 0xDF == 0xADF mod 256. Narrow entries therefore never needed a
// change; only wide entries did (keystream word XORed whole, instead of the
// FString pipeline run over them). Lock that down so nobody churns it again.
TEST_CASE("FName narrow seed is one key written two ways")
{
    for (uint32_t len : { 1u, 7u, 8u, 33u, 64u, 200u, 1000u, 2046u }) {
        CHECK(((len - 33u) & 0xFFu) == ((len + 0xADFu) & 0xFFu));
    }
}

TEST_CASE("FName v20260922 wide keystream decode round-trips")
{
    const KeyTableWindowVaried kt;
    const std::string plain = "PlayerOne";
    const uint16_t header = headerFor(
        static_cast<uint32_t>(plain.size()) * 2u, true);

    std::vector<uint16_t> cipher(plain.size(), 0);
    game::gasm::decode_fname_wide_v922(header, cipher.data(), kt.base());
    for (size_t i = 0; i < plain.size(); ++i)
        cipher[i] = static_cast<uint16_t>(
            cipher[i] ^ static_cast<uint16_t>(plain[i]));

    std::vector<uint16_t> buf = cipher;
    game::gasm::decode_fname_wide_v922(header, buf.data(), kt.base());
    std::string out;
    for (uint16_t w : buf)
        out.push_back(static_cast<char>(w & 0xFF));
    CHECK(out == plain);
}

// SDK chunk mix constants (game::gasm::decode_fname_pool_address).
constexpr uint64_t kChunkMixPrime = 0x00000100000001B3ULL;
constexpr uint64_t kChunkMixAdd = 0x6C5FD4827126D389ULL;

uint64_t mixFirstSlot(uint64_t first)
{
    uint64_t mixed = game::detail::rol64(first, 40) * kChunkMixPrime + kChunkMixAdd;
    return game::detail::rol64(mixed, 50) * kChunkMixPrime + kChunkMixAdd;
}

struct SdkChunk {
    uint64_t chunkAddr = 0;
    uint64_t entry = 0;
};

// Installs a chunk window whose two selected blocks decode to an entry pointer
// production accepts. The first block's raw bytes are searched (the decoded
// value behaves like a random 64-bit pointer, so a valid usermode hit is
// ~1/131072 — the cap makes failure essentially impossible).
SdkChunk installSdkChunk(FakeMem& fm, uint64_t base, int32_t compIndex)
{
    const uint32_t ci = static_cast<uint32_t>(compIndex);
    const uint64_t chunkAddr = base + GNames::NamesOffset +
        static_cast<uint64_t>((ci >> 8) & 0xFFFF00u);
    const uint32_t selector = game::detail::fname_pool_selector(chunkAddr + 0x40);
    const uint64_t nameOff = ci & 0xFFFFu;

    uint8_t secondRaw[8]{};
    const uint64_t second = game::detail::decode_fname_pool_second(secondRaw);

    for (uint64_t cand = 0; cand < (1ull << 24); ++cand) {
        uint8_t firstRaw[8];
        std::memcpy(firstRaw, &cand, sizeof(firstRaw));
        const uint64_t first = game::detail::decode_fname_pool_first(firstRaw);
        const uint64_t poolAddr = (mixFirstSlot(first) ^ second) + first;
        const uint64_t entry = poolAddr + 2ULL * nameOff;
        if (!steam_decrypt::ValidPtr(entry))
            continue;

        fm.write(chunkAddr + 0x50 + static_cast<uint64_t>(selector & 7u) * 0x20,
            firstRaw, sizeof(firstRaw));
        fm.write(chunkAddr + 0x50 + static_cast<uint64_t>((selector + 1u) & 7u) * 0x20,
            secondRaw, sizeof(secondRaw));
        return { chunkAddr, entry };
    }
    return {};
}

// ── Reference implementation of the SDK UTF-16 string decoder ────────────────
uint8_t refDecodeFStringByte(uint8_t encrypted, uint32_t& state)
{
    uint32_t value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(encrypted)));
    state = (game::detail::rol32(state * 0x01000193u + 0xD351FEECu, 14) + state) * 0x01000193u;
    value ^= state & 31u;
    uint32_t a = value + (value - 0x21u < 0x2Fu ? 0x2Fu : (value - 0x50u < 0x2Fu ? 0xFFFFFFD1u : 0));
    uint32_t middle = a + (a - 0x30u < 5u ? 5u : (a - 0x35u < 5u ? 0xFFFFFFFBu : 0));
    middle += middle - 0x61u < 0xDu ? 0xDu : (middle - 0x6Eu < 0xDu ? 0xFFFFFFF3u : 0);
    middle += middle - 0x41u < 0xDu ? 0xDu : (middle - 0x4Eu < 0xDu ? 0xFFFFFFF3u : 0);
    return static_cast<uint8_t>(middle + (middle - 0x21u < 0x2Fu ? 0x2Fu :
        (middle - 0x50u < 0x2Fu ? 0xFFFFFFD1u : 0)));
}

std::string refDecodeWide(const std::vector<uint16_t>& stored)
{
    std::vector<uint16_t> buf = stored;
    uint32_t state = 0;
    // decode_fstring is a NUL-terminated decoder: it stops at the first zero
    // element and leaves it untouched.
    for (size_t i = 0; i < buf.size() && buf[i]; ++i) {
        buf[i] = static_cast<uint16_t>((buf[i] & 0xFF00u) |
            refDecodeFStringByte(static_cast<uint8_t>(buf[i]), state));
    }
    std::string out;
    for (uint16_t w : buf) {
        if (!w)
            break;
        out.push_back(w < 0x80 ? static_cast<char>(w) : '?');
    }
    return out;
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

TEST_CASE("GNames::DecodeSlot16 (SDK name/class slot decode)")
{
    CHECK(GNames::DecodeSlot16(0, 0) == 0);

    const uint64_t a = GNames::DecodeSlot16(0x1122334455667788ULL, 0xAABBCCDDEEFF0011ULL);
    const uint64_t b = GNames::DecodeSlot16(0x1122334455667788ULL, 0xAABBCCDDEEFF0011ULL);
    CHECK(a == b);  // deterministic

    CHECK(GNames::DecodeSlot16(1, 0) != 0);

    // The name slot adds ROL64(32) on top of the class slot's plain decode.
    const uint64_t lo = 1, hi = 0;
    uint8_t slot[16]{};
    std::memcpy(slot, &lo, sizeof(lo));
    std::memcpy(slot + 8, &hi, sizeof(hi));

    const uint64_t classSlot = GNames::DecodeClassSlot16(lo, hi);
    CHECK(classSlot == game::detail::decode_uobject_slot(slot));
    CHECK(GNames::DecodeSlot16(lo, hi) == game::detail::rol64(classSlot, 32));
}

TEST_CASE("SDK FName header length decode")
{
    for (uint32_t len : { 0u, 1u, 7u, 8u, 9u, 63u, 1000u, 1023u })
        CHECK(game::gasm::decode_fname_header_len(headerFor(len, false)) == len);

    // bIsWide (bit 0) must not leak into the length.
    CHECK(game::gasm::decode_fname_header_len(headerFor(9, true)) == 9);
    CHECK(game::gasm::decode_fname_header_len(0) == 0);
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

TEST_CASE("GNames object slot decode honours the SDK selectors")
{
    ScopedFakeMem fm;
    const uint64_t obj = 0x2000000;

    uint8_t window[GNames::SlotWindowSize];
    for (size_t i = 0; i < sizeof(window); ++i)
        window[i] = static_cast<uint8_t>(0x11u * (i % 7) + 1);
    fm.mem.write(obj, window, sizeof(window));

    const uint32_t nameSlot = GNames::NameSlot(obj);
    const uint32_t classSlot = GNames::ClassSlot(obj);

    CHECK(GNames::ReadNameSlot16(obj) ==
        game::detail::rol64(
            game::detail::decode_uobject_slot(window + 0x20 + nameSlot * 0x20), 32));
    CHECK(GNames::ReadClassSlot16(obj) ==
        game::detail::decode_uobject_slot(window + 0x20 + classSlot * 0x20));
}

TEST_CASE("GNames FName pipeline end-to-end on synthetic memory (SDK scheme)")
{
    ScopedFakeMem fm;
    // ResetTables (not just GNames::Reset) so fname_state().initialised is
    // cleared too — InitFNameState would otherwise short-circuit on state
    // carried over from earlier test cases.
    steam_decrypt::ResetTables();

    const uint64_t base = 0x1000000;
    writeKeyTable(fm.mem, base);
    REQUIRE(steam_decrypt::InitFNameState(base));

    // narrow name "test_item" (9 chars) in the SDK header format
    const uint16_t header = headerFor(9, false);
    const KeyTableWindow kt;
    const std::vector<uint8_t> cipher = encodeFName(header, "test_item", kt);

    const SdkChunk chunk = installSdkChunk(fm.mem, base, 9);
    REQUIRE(steam_decrypt::ValidPtr(chunk.entry));

    fm.mem.writeU16(chunk.entry, header);
    fm.mem.write(chunk.entry + 2, cipher.data(), cipher.size());

    const uint64_t ptr = GNames::ResolveNamePointer(base, 9);
    CHECK(ptr == chunk.entry);
    CHECK(GNames::DecodeString(ptr) == "test_item");
    CHECK(steam_decrypt::CachedNameString(9, base) == "test_item");
}

TEST_CASE("GNames wide FName decode (SDK UTF-16 path)")
{
    ScopedFakeMem fm;
    steam_decrypt::ResetTables();

    const uint64_t base = 0x1000000;
    writeKeyTable(fm.mem, base);
    REQUIRE(steam_decrypt::InitFNameState(base));

    // bIsWide is bit 0; stored elements stay 16-bit.
    const uint16_t header = headerFor(4, true);
    CHECK((header & 0x1u) != 0);

    const uint64_t entry = 0x3000000;
    const std::vector<uint16_t> stored = { 0x0041, 0x2200, 0x7E01, 0x0000 };
    for (size_t i = 0; i < stored.size(); ++i)
        fm.mem.writeU16(entry + 2 + 2 * i, stored[i]);

    fm.mem.writeU16(entry, header);

    CHECK(GNames::DecodeStringSdk(entry) == refDecodeWide(stored));
}

TEST_CASE("GNames DecodeString rejects garbage") {
    ScopedFakeMem fm;

    // Not ready yet -> empty.
    steam_decrypt::ResetTables();
    CHECK(GNames::DecodeString(0).empty());
    CHECK(GNames::DecodeString(0x1000).empty());

    // Install the FName key table, then init.
    const uint64_t base = 0x1000000;
    writeKeyTable(fm.mem, base);
    REQUIRE(steam_decrypt::InitFNameState(base));

    CHECK(GNames::DecodeString(0).empty());      // null ptr
    CHECK(GNames::DecodeStringSdk(0).empty());

    fm.mem.writeU16(0x1000, static_cast<uint16_t>(0));   // header 0 -> len 0
    CHECK(GNames::DecodeStringSdk(0x1000).empty());
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

// ── SDK plain FNamePool (sdk/CppSDK/SDK/Basic.hpp) ───────────────────────────
// The dumped SDK is the authority for this build: Blocks[0x2000] at pool+0x40,
// 2-byte entry stride, block = index >> 16, u16 header (bit 0 wide, bits 6..15
// length), text at entry + 2. These cases pin that pipeline.

TEST_CASE("SDK plain FNamePool walk resolves and decodes")
{
    ScopedFakeMem fm;
    steam_decrypt::ResetTables();

    const uint64_t base = 0x1000000;
    const uint64_t block = 0x3000000;
    const int32_t index = 9;

    fm.mem.writeU64(base + GNames::NamesOffset + Offsets::FNamePool_Blocks, block);

    const uint64_t entry =
        block + static_cast<uint64_t>(index) * Offsets::FNamePool_EntryStride;
    fm.mem.writeU16(entry, static_cast<uint16_t>(9u << Offsets::FNameEntry_LenShift));
    fm.mem.write(entry + 2, "test_item", 9);

    REQUIRE(steam_decrypt::InitFNameState(base));   // the pool is the only gate

    CHECK(GNames::ResolveNamePointerPlain(base, index) == entry);
    CHECK(GNames::ResolveNamePointer(base, index) == entry);
    CHECK(GNames::DecodeStringPlain(entry) == "test_item");
    CHECK(GNames::DecodeString(entry) == "test_item");
    CHECK(steam_decrypt::CachedNameString(index, base) == "test_item");
}

TEST_CASE("SDK plain FNamePool spans blocks and rejects a wrong layout")
{
    ScopedFakeMem fm;
    steam_decrypt::ResetTables();

    const uint64_t base = 0x1000000;
    const uint64_t block0 = 0x3000000;
    const uint64_t block1 = 0x3100000;
    fm.mem.writeU64(base + GNames::NamesOffset + Offsets::FNamePool_Blocks, block0);
    fm.mem.writeU64(base + GNames::NamesOffset + Offsets::FNamePool_Blocks + 8, block1);

    // index 0x10005 -> block 1, in-chunk 5 (index >> 16 / index & 0xFFFF).
    const int32_t index = 0x10005;
    const uint64_t entry = block1 + 5ull * Offsets::FNamePool_EntryStride;
    fm.mem.writeU16(entry, static_cast<uint16_t>(9u << Offsets::FNameEntry_LenShift));
    fm.mem.write(entry + 2, "Some_Name", 9);

    REQUIRE(steam_decrypt::InitFNameState(base));
    CHECK(GNames::ResolveNamePointerPlain(base, index) == entry);
    CHECK(GNames::DecodeString(entry) == "Some_Name");

    // Text that is not name text is never published as a name.
    const uint64_t garbage = block0 + 0x40;
    fm.mem.writeU16(garbage, static_cast<uint16_t>(9u << Offsets::FNameEntry_LenShift));
    fm.mem.write(garbage + 2, "2#52?/2#+", 9);
    CHECK(GNames::DecodeStringPlain(garbage) == "2#52?/2#+");
    CHECK(GNames::DecodeString(garbage).empty());

    CHECK(GNames::LooksLikeNameEntry("Item_HealthKit"));
    CHECK(GNames::LooksLikeNameEntry("BP_PioneerCharacter_C"));
    CHECK_FALSE(GNames::LooksLikeNameEntry("2#52?/2#+"));
    CHECK_FALSE(GNames::LooksLikeNameEntry(""));
}

// sdk/sdk.txt drop (2026-09-22): APlayerController::GetLocalPlayer (sub_3680940)
// keeps the local player at controller+0x4B0 behind ROL32(13) per 32-bit lane,
// XOR 0x9A492C85DDF6F193, ROL64(39). Encrypt the expected pointer by inverting
// the pipeline, then check the decrypt returns exactly it — a wrong rotate, a
// wrong mask or a stale slot offset could not round-trip.
TEST_CASE("PlayerLink decrypts the controller's local player (PC+0x4B0)")
{
    CHECK(Offsets::PlayerDecrypt_LocalPlayerOffset == 0x4B0);
    CHECK(Offsets::PlayerDecrypt_LaneRot == 13);
    CHECK(Offsets::PlayerDecrypt_FinalRot == 39);
    CHECK(Offsets::PlayerDecrypt_BlendXorMask == 0x9A492C85DDF6F193ull);

    auto ror32 = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto ror64 = [](uint64_t x, int n) { return (x >> n) | (x << (64 - n)); };
    auto encrypt = [&](uint64_t plain) {
        const uint64_t blended = ror64(plain, Offsets::PlayerDecrypt_FinalRot);
        const uint64_t rot = blended ^ Offsets::PlayerDecrypt_BlendXorMask;
        return static_cast<uint64_t>(
                   ror32(static_cast<uint32_t>(rot), Offsets::PlayerDecrypt_LaneRot)) |
               (static_cast<uint64_t>(ror32(static_cast<uint32_t>(rot >> 32),
                    Offsets::PlayerDecrypt_LaneRot)) << 32);
    };

    const uint64_t plain = 0x2A1B3C4D5E00ull;
    CHECK(PlayerLink::DecryptStatic(encrypt(plain)) == plain);

    // A different lane pattern: the rotate is per 32-bit lane, so a flat ROL64
    // would get one of these wrong.
    const uint64_t plain2 = 0x1F0E0D0C0B0Aull;
    CHECK(PlayerLink::DecryptStatic(encrypt(plain2)) == plain2);
    CHECK(plain != plain2);

    // Never publish an empty slot or a result outside user space.
    CHECK(PlayerLink::DecryptStatic(0) == 0);
    CHECK(PlayerLink::DecryptStatic(1) == 0);

    // The TEB key is only honoured when something sets it.
    PlayerLink::SetTebKey(0);
    CHECK(PlayerLink::TebKey() == 0);
}

TEST_CASE("SDK plain wide FName entry (bIsWide is bit 0)")
{
    ScopedFakeMem fm;
    steam_decrypt::ResetTables();

    const uint64_t base = 0x1000000;
    const uint64_t block = 0x3000000;
    fm.mem.writeU64(base + GNames::NamesOffset + Offsets::FNamePool_Blocks, block);

    const int32_t index = 3;
    const uint64_t entry = block + 3ull * Offsets::FNamePool_EntryStride;
    fm.mem.writeU16(entry, static_cast<uint16_t>((4u << Offsets::FNameEntry_LenShift) |
                                                  Offsets::FNameEntry_WideBit));
    const wchar_t* text = L"Wide";
    for (int i = 0; i < 4; ++i)
        fm.mem.writeU16(entry + 2 + 2ull * i, static_cast<uint16_t>(text[i]));

    REQUIRE(steam_decrypt::InitFNameState(base));
    CHECK(GNames::DecodeStringPlain(entry) == "Wide");
    CHECK(GNames::DecodeString(entry) == "Wide");
    CHECK(GNames::ResolveNamePointer(base, index) == entry);
}

TEST_CASE("UObject fields come from the SDK layout (Class +0x20, Name +0x98)")
{
    ScopedFakeMem fm;
    steam_decrypt::ResetTables();

    const uint64_t base = 0x1000000;
    const uint64_t obj = 0x2000000;
    const uint64_t cls = 0x2100000;

    const uint64_t block = 0x3000000;
    fm.mem.writeU64(base + GNames::NamesOffset + Offsets::FNamePool_Blocks, block);

    // The class object's own FName, so the class name resolves too.
    const int32_t classIndex = 1;
    const uint64_t classEntry = block + static_cast<uint64_t>(classIndex) *
        Offsets::FNamePool_EntryStride;
    fm.mem.writeU16(classEntry, static_cast<uint16_t>(9u << Offsets::FNameEntry_LenShift));
    fm.mem.write(classEntry + 2, "PioneerCh", 9);

    fm.mem.writeU64(cls + Offsets::UObject_NamePrivate, classIndex);
    fm.mem.writeU32(obj + Offsets::UObject_NamePrivate, 9);   // comparison index
    fm.mem.writeU32(obj + Offsets::UObject_NamePrivate + 4, 0);
    fm.mem.writeU64(obj + Offsets::UObject_ClassPrivate, cls);

    REQUIRE(steam_decrypt::InitFNameState(base));
    CHECK(steam_decrypt::FindFNameSlot(obj) == 9u);
    CHECK(steam_decrypt::GetActorClassPtr(obj) == cls);
    CHECK(steam_decrypt::CachedNameString(classIndex, base) == "PioneerCh");
}

// Layout invariants taken from the dumped SDK's own headers. A regression here
// means the tool is reading the wrong field again (the c190fb log's actors:-1
// was exactly this: AActors read 8 bytes early, so the "count" was the data
// pointer's low dword).
TEST_CASE("SDK header layout invariants (Basic.hpp / CoreUObject_classes.hpp)")
{
    // sdk/CppSDK/SDK/Level_classes.hpp: TArray<class AActor*> Actors @ 0x110.
    CHECK(Offsets::AActors == 0x110);
    CHECK(Offsets::ActorsCount == Offsets::AActors + 0x8);
    CHECK(Offsets::LevelActorContainer_Actors + 0x8 == Offsets::LevelActorContainer_ActorCount);

    // sdk/CppSDK/SDK/CoreUObject_classes.hpp: UObject is 0xA0 bytes.
    CHECK(Offsets::UObject_ClassPrivate == 0x20);
    CHECK(Offsets::UObject_InternalIndex == 0x90);
    CHECK(Offsets::UObject_NamePrivate == 0x98);
    CHECK(Offsets::UObject_OuterPrivate == 0xA0);
    CHECK(Offsets::UObject_Size == 0xA0);

    // sdk/CppSDK/SDK/Basic.hpp: FNamePool layout.
    CHECK(Offsets::FNamePool_Blocks == 0x40);
    CHECK(Offsets::FNamePool_EntryStride == 2);
    CHECK(Offsets::FNamePool_BlockOffsetBits == 16);
    CHECK((1u << Offsets::FNamePool_BlockOffsetBits) == 0x10000u);

    // sdk/CppSDK/SDK/Basic.hpp: FNameEntryHeader (bit 0 wide, bits 6..15 len).
    CHECK(Offsets::FNameEntry_WideBit == 0x1);
    CHECK(Offsets::FNameEntry_LenShift == 6);
    CHECK(Offsets::FNameEntry_LenMask == 0x3FF);
    CHECK(Offsets::FNameEntry_TextOffset == 2);

    // sdk/SDK.txt (20260922): the local-player-controller chain. The engine's own
    // bIsLocalPlayerController flag is what identifies the local PC without reading
    // the live-pinned camera slot, so pin its offset and the chain around it.CHECK( Offsets::OwningGameInstance == 0x478 );
    CHECK(Offsets::LocalPlayers == 0x130);
    CHECK(Offsets::LocalPlayer_PlayerController == 0xA0);
    CHECK(Offsets::PlayerController_bIsLocalPlayerController == 0xD64);
    CHECK(Offsets::PlayerController_bIsLocalPlayerController_Mask == 0x1);
    CHECK(Offsets::AController_PlayerState == 0x3D0);
    CHECK(Offsets::AcknowledgedPawn == 0x418);
    // 0xD64 must land inside APlayerController's own range (0x460..0xDE0).
    CHECK(Offsets::PlayerController_bIsLocalPlayerController > Offsets::APlayerCameraManager);

    // v20260922: ComponentToWorld is 0x310 (ArcOffsets + our own probe — every
    // bot root read at the old 0x2D0 slot came back implausible); 0x2D0 stays
    // as the probe's alternate. Translation is ComponentToWorld + 0x20, i.e.
    // 0x330, which is the slot ArcOffsets::SceneComponent::WORLD_LOCATION_DOUBLE
    // names. A drift here moves every projected box at once.
    CHECK(Offsets::ComponentToWorld == 0x310);
    CHECK(Offsets::ComponentToWorld_Alt == 0x2D0);
    CHECK(Offsets::WorldLocation == 0x330);
    CHECK(Offsets::Transform_Rotation == 0x0);
    CHECK(Offsets::Transform_Translation == 0x20);
    CHECK(Offsets::Transform_Scale3D == 0x40);
    CHECK(Offsets::Transform_Size == 0x60);
    CHECK(Offsets::ComponentToWorld_Rotation == Offsets::ComponentToWorld);
    CHECK(Offsets::ComponentToWorld_Translation == Offsets::ComponentToWorld + 0x20);
    CHECK(Offsets::ComponentToWorld_Scale3D == Offsets::ComponentToWorld + 0x40);
    CHECK(Offsets::WorldLocation == Offsets::ComponentToWorld_Translation);
    CHECK(Offsets::WorldLocation == Offsets::ComponentToWorld + 0x20);
    // The block has to fit inside SceneComponent (dump size 0x370), and the two
    // candidates must stay distinct - they overlap (0x2D0+0x60 > 0x310), which is
    // exactly why the probe decides per mesh instead of reading one slot blindly.
    CHECK(Offsets::ComponentToWorld_Scale3D + 0x18 <= 0x370);
    CHECK(Offsets::ComponentToWorld_Alt != Offsets::ComponentToWorld);

    // sdk/sdk.txt drop: bone v922 selector + camera LockedFOV.
    CHECK(Offsets::BoneV922SeedOffset == 0x7B0);
    CHECK(Offsets::BoneV922SelectorOffset == 0x848);
    CHECK(Offsets::BoneV922DescriptorBase == 0x18);
    CHECK(Offsets::BoneV922DescriptorStride == 0x10);
    CHECK(Offsets::LockedFOV == 0x43C);

    // sdk/CppSDK/SDK/Basic.hpp: TUObjectArray / FUObjectItem.
    CHECK(Offsets::GObjects_ItemStride == 0x18);
    CHECK(Offsets::GObjects_Item_Object == 0x8);
    CHECK(Offsets::GObjects_ElementsPerChunk == 0x10000u);
}

// Slots the reconciliation moved off a hand-written literal because a named
// source disagreed: the dump property for the Pickup trio, the dump's State on
// SalvageExtractionPointBase (the CL drop's 0xBD2 is a whole block below it),
// and two fields only the CL drop carries. Pinning them keeps a literal from
// quietly reappearing where the source is known.
TEST_CASE("Offsets the offline sources own (see tools/reconcile_offsets.py)")
{
    // tools/sdk_index.json (2026-09-22): Angelscript.Pickup.*
    CHECK(Offsets::Pickup_RootCollider == 0x480);
    CHECK(Offsets::Pickup_Interaction == 0x498);
    CHECK(Offsets::Pickup_DefaultPickupDataAsset == 0x4A8);
    CHECK(Offsets::Pickup_RootCollider < Offsets::Pickup_Interaction);
    CHECK(Offsets::Pickup_Interaction < Offsets::Pickup_DefaultPickupDataAsset);

    // Angelscript.SalvageExtractionPointBase: State 0xC32 (the drop's 0xBD2 sits
    // 0x60 earlier - the pre-shift block), inside a 0xC50 class.
    CHECK(Offsets::ExtractionPoint_State == 0xC32);
    CHECK(Offsets::ExtractionPoint_State < 0xC50);

    // sdk/sdk.txt drop, live-verified fields with no dump counterpart.
    CHECK(Offsets::UActorComponent_WorldPrivate == 0x140);
    CHECK(Offsets::UWorld_TimeSeconds == 0x240);

    // Same-named property, two classes: UWorld's own AuthorityGameMode is the
    // dump's 0x310; GameState's is 0x430. The row must follow UWorld's.
    CHECK(Offsets::AuthorityGameMode == 0x310);
}

// The UObject outer chain (Core/SteamDecrypt.hpp, namespace OuterLink).
//
// Two schemes can hold an Outer on this build — the v20260922 slot the SDK's own
// name/class decoders use, and the CL-1299607 OuterDecrypt cipher — so the ladder
// is what is under test: rungs are tried in order and a candidate only counts
// when the caller's predicate accepts it. The suite drives the ladder with a
// map-backed reader because the live path reads uncached DMA memory that the
// harness deliberately does not intercept.
TEST_CASE("OuterLink: slot cipher, rung ladder and outer-chain walk")
{
    using Rung = OuterLink::Rung;
    using OuterLink::DropSlot;
    using OuterLink::EncryptSlot;
    using OuterLink::DecryptSlot;

    // -- the cipher inverts itself. A flat ROL64 instead of per-lane ROL32, or a
    //    swapped rotation order, fails at least one of these.
    const uint64_t ptrA = 0x1F3A4B5C6D0ull;
    const uint64_t ptrB = 0x204FFEEDD00ull;
    CHECK(DecryptSlot(EncryptSlot(ptrA)) == ptrA);
    CHECK(DecryptSlot(EncryptSlot(ptrB)) == ptrB);
    CHECK(EncryptSlot(ptrA) != EncryptSlot(ptrB));
    CHECK(DecryptSlot(0) == 0);
    // a decrypt that lands outside user space (or unaligned) is not a pointer
    CHECK(DecryptSlot(0xFFFFFFFFFFFFFFFFull) == 0);
    CHECK(OuterLink::Plausible(ptrA));
    CHECK_FALSE(OuterLink::Plausible(ptrA + 1));
    CHECK_FALSE(OuterLink::Plausible(0x10));

    // -- slot selection is the drop's own expression, recomputed here
    const uint64_t obj = 0x1A2B3C4D5E0ull;
    const uint32_t h = OuterLink::Hash(obj);
    CHECK(DropSlot(obj) == (uint32_t)(((h ^ (h >> 16)) & 3u) ^ 2u));
    CHECK(DropSlot(obj) < 4u);
    CHECK(DropSlot(obj + 0x40) < 4u);

    // -- the ladder, driven by a fake slot map
    std::map<uint64_t, uint64_t> mem;
    const auto read = [&mem](uint64_t addr) {
        const auto it = mem.find(addr);
        return it == mem.end() ? 0ull : it->second;
    };
    const auto slotAddr = [](uint64_t base) {
        return base + static_cast<uint64_t>(Offsets::OuterDecrypt_SlotBaseOff)
            + static_cast<uint64_t>(DropSlot(base))
            * static_cast<uint64_t>(Offsets::OuterDecrypt_SlotStride);
    };

    const uint64_t owner = 0x1C0FFEE000ull;
    mem.clear();
    mem[slotAddr(obj)] = EncryptSlot(owner);
    auto hop = OuterLink::FromObject(obj, read,
        [owner](uint64_t p) { return p == owner; });
    CHECK(hop.rung == Rung::DropCipher);
    CHECK(hop.ptr == owner);

    // the same slot, a different live pointer: the caller's predicate decides,
    // not the fact that a rung produced something valid-looking
    mem.clear();
    mem[slotAddr(obj)] = EncryptSlot(0x1D01234500ull);
    hop = OuterLink::FromObject(obj, read, [owner](uint64_t p) { return p == owner; });
    CHECK(hop.ptr == 0);
    CHECK(hop.rung == Rung::None);

    // -- the plain rungs come last, in order: 0xA0 (the dump's OuterPrivate), +0x20
    mem.clear();
    mem[obj + static_cast<uint64_t>(Offsets::UObject_OuterPrivate)] = owner;
    hop = OuterLink::FromObject(obj, read, [owner](uint64_t p) { return p == owner; });
    CHECK(hop.rung == Rung::PlainOuter);
    mem.clear();
    mem[obj + static_cast<uint64_t>(Offsets::OuterDecrypt_SlotBaseOff)] = owner;
    hop = OuterLink::FromObject(obj, read, [owner](uint64_t p) { return p == owner; });
    CHECK(hop.rung == Rung::PlainSlot);

    // -- the chain walk: component -> subobject -> actor
    const uint64_t comp = 0x190AAAA000ull;
    const uint64_t mid = 0x190BBBB000ull;
    const uint64_t act = 0x190CCCC000ull;
    mem.clear();
    mem[slotAddr(comp)] = EncryptSlot(mid);
    mem[slotAddr(mid)] = EncryptSlot(act);
    CHECK(OuterLink::ResolveOwner(comp, read, &OuterLink::Plausible,
        [act](uint64_t p) { return p == act; }) == act);
    // the walk never returns the object it started from
    CHECK(OuterLink::ResolveOwner(comp, read, &OuterLink::Plausible,
        [comp](uint64_t p) { return p == comp; }) == 0);
    // a budget smaller than the chain is a miss, not a wrong answer
    CHECK(OuterLink::ResolveOwner(comp, read, &OuterLink::Plausible,
        [act](uint64_t p) { return p == act; }, 1) == 0);
    // the hop predicate is what a hop may land on: refusing mid cuts the chain
    // even though the slot itself decrypts cleanly
    CHECK(OuterLink::ResolveOwner(comp, read,
        [mid](uint64_t p) { return p != mid; },
        [act](uint64_t p) { return p == act; }) == 0);

    // a self-outer cannot loop forever
    mem.clear();
    mem[slotAddr(mid)] = EncryptSlot(mid);
    CHECK(OuterLink::ResolveOwner(mid, read, &OuterLink::Plausible,
        [mid](uint64_t p) { return p == mid; }) == 0);

    // -- the actor rule: a RootComponent whose own Outer is the object
    const uint64_t rootOff = static_cast<uint64_t>(Offsets::RootComponent);
    mem.clear();
    mem[act + rootOff] = comp;
    mem[slotAddr(comp)] = EncryptSlot(act);
    CHECK(OuterLink::OwnerIsActor(read, act, rootOff));
    // the same shape pointed at somebody else is not an actor
    CHECK_FALSE(OuterLink::OwnerIsActor(read, mid, rootOff));
    mem[act + rootOff] = 0x10;   // not a pointer
    CHECK_FALSE(OuterLink::OwnerIsActor(read, act, rootOff));
}

