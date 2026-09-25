// NameCrypto suite — pins Core/NameCrypto.hpp (the v20260922 pipeline port)
// against known-answer vectors transcribed independently from sdk/sdk.txt:
// block/slot decodes, shard + object slot hashes, FField name + FProperty
// offset, FNV mix, TEB decrypt, and the paired-LCG name string decrypt. The
// last case drives the whole CI -> FNameEntry -> string walk (through
// Reflection::ResolveNameString) over the fake-memory seam.

#include "tests_main.hpp"

#include "fake_mem.hpp"

#pragma warning(push)
#pragma warning(disable : 4201)
#include "Core/NameCrypto.hpp"
#include "Core/Reflection.hpp"
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

#include <cstring>

namespace {

uint32_t bswap32v(uint32_t v) { return _byteswap_ulong(v); }

// Inverse of DecodeBlock (XOR -> ROL64(19) -> PSHUFB), for laying out
// synthetic pool block words in the fixtures below.
uint64_t EncodeBlock(uint64_t decoded)
{
    uint8_t inv[8];
    for (int i = 0; i < 8; ++i)
        inv[namecrypto::kBlockPshufb[i]] = static_cast<uint8_t>(i);
    uint8_t bytes[8], out[8] = {};
    std::memcpy(bytes, &decoded, 8);
    for (int i = 0; i < 8; ++i)
        out[i] = bytes[inv[i]];
    uint64_t unshuf = 0;
    std::memcpy(&unshuf, out, 8);
    return namecrypto::Rol64(unshuf, 64 - namecrypto::kBlockRol64) ^
           namecrypto::kBlockXor;
}

struct MemReader {
    bool Read(uint64_t addr, void* dst, size_t n) const
    {
        return FakeMem::readFn(addr, dst, n);
    }
};

// Keystream pattern behind the string-decrypt vectors (mirrors the Python
// transcription used to compute them: ks[i] = 0x0100 + 7*i).
namecrypto::PipelineState VectorState()
{
    namecrypto::PipelineState s;
    for (int i = 0; i < namecrypto::kKeystreamCount; ++i)
        s.keystream[i] = static_cast<uint16_t>(0x0100 + 7 * i);
    s.ksLoaded = true;
    return s;
}

} // namespace

TEST_CASE("NameCrypto decode primitives match the v20260922 drop")
{
    CHECK(namecrypto::DecodeBlock(0x0123456789ABCDEFULL) == 0x14B7832EAFA9FDC8ULL);
    CHECK(namecrypto::DecodeBlock(0xFEDCBA9876543210ULL) == 0xEB487CD150560237ULL);

    CHECK(namecrypto::DecodeSlotLo(0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL) ==
          0x1AC1650038F03CC4ULL);

    CHECK(namecrypto::DecodeFFieldName(0x0123456789ABCDEFULL,
                                       0xFEDCBA9876543210ULL) ==
          0x93E2E81EADC520CEULL);

    CHECK(namecrypto::FnvMix2(0x0123456789ABCDEFULL) == 0x3D14764043BFE75EULL);

    CHECK(namecrypto::TlsHash(0x00007FF612345678ULL) == 0x6FF39192u);
}

TEST_CASE("NameCrypto shard and object slot hashes match the drop")
{
    uint32_t b1 = 0, b2 = 0;
    namecrypto::ShardHash(0x123456789ULL, b1, b2);
    CHECK(b1 == 5u);
    CHECK(b2 == 6u);

    CHECK(namecrypto::ObjSlotHash(0x00007FF612345678ULL) == 0xF4ED5FDFu);
    CHECK(namecrypto::ObjSlotIndexBase(0x00007FF612345678ULL) == 2u);
    CHECK(namecrypto::ObjNameSlot(0x00007FF612345678ULL) == 0u);
    CHECK(namecrypto::ObjClassSlot(0x00007FF612345678ULL) == 2u);
    CHECK(namecrypto::ObjOuterSlot(0x00007FF612345678ULL) == 3u);

    CHECK(namecrypto::ObjSlotHash(0x500000ULL) == 0xD13C74A0u);
    CHECK(namecrypto::ObjSlotIndexBase(0x500000ULL) == 0u);
    CHECK(namecrypto::ObjNameSlot(0x500000ULL) == 2u);
    CHECK(namecrypto::ObjClassSlot(0x500000ULL) == 0u);
    CHECK(namecrypto::ObjOuterSlot(0x500000ULL) == 1u);
}

TEST_CASE("NameCrypto property offset is the drop's bswap32 form")
{
    // bswap32(raw) ^ 0x8DE128DE == bswap32(raw ^ 0xDE28E18D): the drop's
    // OFFSET_XOR_KEY and SDK.hpp's folded key are byte-reverses of each other,
    // so getting the swap side wrong silently swaps the two keys.
    const uint32_t raws[] = { 0xAABBCCDDu, 0x9A1BC39Cu, 0x00000000u, 0xFFFFFFFFu };
    const uint32_t expected[] = { 0x502D9374u, 0x11223344u, 0x8DE128DEu, 0x721ED721u };
    for (int i = 0; i < 4; ++i) {
        CHECK(namecrypto::DecodeFfPropertyOffset(raws[i]) == expected[i]);
        CHECK(namecrypto::DecodeFfPropertyOffset(raws[i]) ==
              bswap32v(raws[i] ^ 0xDE28E18Du));
    }
}

TEST_CASE("NameCrypto FName header decode round-trips")
{
    // Narrow "Pelvis" (6 chars): len = ((len & 7) << 13) | ((len & 0x3F8) << 3)
    bool isWide = true;
    int rawLen = 0;
    const uint16_t narrowHdr = static_cast<uint16_t>(((6 & 7) << 13) | ((6 & 0x3F8) << 3));
    namecrypto::FNameHeaderDecode(narrowHdr, isWide, rawLen);
    CHECK_FALSE(isWide);
    CHECK(rawLen == 6);

    // Wide "Rifle" (5 chars -> 10 bytes) sets the wide bit.
    const uint16_t wideHdr = static_cast<uint16_t>(((10 & 7) << 13) | ((10 & 0x3F8) << 3) | 1);
    namecrypto::FNameHeaderDecode(wideHdr, isWide, rawLen);
    CHECK(isWide);
    CHECK(rawLen == 10);
}

TEST_CASE("NameCrypto paired-LCG string decrypt matches the drop")
{
    const namecrypto::PipelineState s = VectorState();

    // "Pelvis" (narrow, rawLen 6).
    const uint8_t narrow[] = { 0x1A, 0x4B, 0x26, 0x58, 0x23, 0x5D };
    CHECK(namecrypto::DecryptNameBytes(s, narrow, 6, false) == "Pelvis");

    // "Rifle" (wide, 5 chars).
    const uint8_t wide[] = {
        0x02, 0x02, 0x39, 0x02, 0x36, 0x02, 0x3C, 0x02, 0x35, 0x02
    };
    CHECK(namecrypto::DecryptNameBytes(s, wide, 10, true) == "Rifle");
}

TEST_CASE("NameCrypto TEB decrypt known answers")
{
    const uint64_t key = 0x0123456789ABCDEFULL;
    CHECK(namecrypto::DecryptTeb(0x3523456789AB9BFDULL, key) == 0x0000000123456000ULL);
    CHECK(namecrypto::DecryptTeb(0x01234567898BDDEFULL, key) == 0x0000020000010000ULL);
    // Decodes to an implausible pointer -> refused.
    CHECK(namecrypto::DecryptTeb(0x002345678DABCDEFULL, key) == 0);
    CHECK(namecrypto::DecryptTeb(0, key) == 0);
}

TEST_CASE("NameCrypto slot reads decode the CLMUL slot window")
{
    ScopedFakeMem fm;
    const MemReader reader;
    const uint64_t obj = 0x00007FF612345678ULL;
    const uint64_t lo = 0x1122334455667788ULL;
    const uint64_t hi = 0x99AABBCCDDEEFF00ULL;
    const uint32_t slot = namecrypto::ObjClassSlot(obj);
    const uint64_t at = obj + namecrypto::kSlotBaseOff +
                        static_cast<uint64_t>(slot) * namecrypto::kSlotStride;
    fm.mem.writeU64(at, lo);
    fm.mem.writeU64(at + 8, hi);

    CHECK(namecrypto::ReadSlotDecoded(obj, slot, reader) ==
          namecrypto::DecodeSlotLo(lo, hi));
    CHECK(namecrypto::ReadSlotDecodedAsPtr(obj, slot, reader) ==
          namecrypto::Rol64(namecrypto::DecodeSlotLo(lo, hi), 32));
}

TEST_CASE("Reflection resolves names through the v20260922 pipeline")
{
    ScopedFakeMem fm;
    Reflection::ResetNames();

    const uint64_t base = 0x1000000;
    // Install the keystream where InitFNameState loads it.
    for (int i = 0; i < namecrypto::kKeystreamCount; ++i)
        fm.mem.writeU16(base + namecrypto::kRvaKeystream +
                            static_cast<uint64_t>(i) * 2,
                        static_cast<uint16_t>(0x0100 + 7 * i));

    // CI 0x1234: nameOff 0x1234 in chunk 0 of the shard pool.
    const int32_t ci = 0x1234;
    const uint64_t chunkAddr = base + namecrypto::kRvaGnamePool;
    uint32_t b1 = 0, b2 = 0;
    namecrypto::ShardHash(chunkAddr + namecrypto::kShardHashSeedOff, b1, b2);

    // Choose the second block word so the entry lands at a fixed address:
    // entry = v14 + (v15 ^ FnvMix2(v14)) + 2*nameOff.
    const uint64_t v14 = 0x20000000ULL;
    const uint64_t entry = 0x30000000ULL;
    const uint64_t v15 =
        (entry - v14 - 2ULL * 0x1234) ^ namecrypto::FnvMix2(v14);
    fm.mem.writeU64(chunkAddr + namecrypto::kShardBlockBaseOff +
                        namecrypto::kShardBlockStride * b1,
                    EncodeBlock(v14));
    fm.mem.writeU64(chunkAddr + namecrypto::kShardBlockBaseOff +
                        namecrypto::kShardBlockStride * b2,
                    EncodeBlock(v15));

    // FNameEntry: narrow header for 6 chars + the encrypted "Pelvis" payload.
    fm.mem.writeU16(entry,
                    static_cast<uint16_t>(((6 & 7) << 13) | ((6 & 0x3F8) << 3)));
    const uint8_t payload[] = { 0x1A, 0x4B, 0x26, 0x58, 0x23, 0x5D };
    fm.mem.write(entry + 2, payload, sizeof(payload));

    CHECK(Reflection::ResolveNameString(ci, base) == "Pelvis");
    CHECK(Reflection::ResolveNameString(ci, base) == "Pelvis"); // memoized
    CHECK(Reflection::ResolveNameString(-1, base).empty());

    Reflection::ResetNames();
}
