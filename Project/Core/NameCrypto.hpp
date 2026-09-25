#pragma once

// =============================================================================
// NameCrypto — the v20260922 (CL-1389382) crypto pipeline, ported 1:1 from
// sdk/sdk.txt (FName pipeline + UObject slots + FField names + TEB/TLS/
// game-instance decrypts). Function names follow the drop (PascalCase of the
// drop name) so tools/sdk_gap_report.py can trace every pipeline function to
// its port.
//
// Pure math + reader-templated walks (R::Read(addr, dst, size) -> bool), so
// Tests/name_crypto_tests.cpp drives every decode against a synthetic memory
// image. The DMA binding lives in Core/Reflection.hpp.
//
// Pipeline summary (drop header, v20260922):
//   UObject slot hash: obj+0x10 seed, SHR4 + 3 imul rounds -> 2-bit slot
//     Name = idx^2, Class = idx, Outer = (idx+1)&3
//   Slot decode: T = Hi ^ ClmulLo(K2, Lo); V = ClmulLo(K1, T) ^ Lo; ROL64(V,32)
//   Shard hash: 3 imul rounds with ROL32(17/27/17) + final SHR(5)
//   Block decode: XOR(BLOCK_XOR) -> ROL64(19) -> PSHUFB({6,5,1,2,7,3,4,0})
//   FNV64 mix: P*ROL64(v,40) + A ; P*ROL64(fv,50) + A
//   Entry = V14 + (V15 ^ Fv) + 2*NameOff
//   Header: IsWide = (h & 1), length = (h >> 13) | ((h >> 3) & 0x3F8)
//   String decrypt (paired LCG): K = length-33, base idx 12, step K = 65*K+0x80
//     narrow: byte ^= (ks >> 3) & 0xFF, paired index (33*K) & 0x3F
//     wide:   u16  ^= ks (full)
//   FField name: V = ROL64(Src + ADD1, 17) ^ Second; V = ROL64(V + ADD2, 47)
//     FName = ROL64(Src ^ V, 32)
//   FProperty offset: Bswap32(raw) ^ OFFSET_XOR_KEY (== bswap32(raw ^ 0xDE28E18D),
//     SDK.hpp's folded form — the two keys are byte-reverses of each other)
//   TEB decrypt: XOR key -> per-word ROR 1 -> PSHUFB({6,3,1,7,0,2,4,5}) -> ROR64 3
// =============================================================================

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace namecrypto {

// ---------------------------------------------------------------------------
// Constants (sdk drop namespaces: FName, FField, FFieldCrypto, TebDecrypt,
// WineTeb, PlayerDecrypt, OuterDecrypt, GameInstanceTlsDecrypt,
// GameInstanceStaticDecrypt, GameInstanceDecrypt, Global, ObjectXorKey,
// GetObjectIdCrypto, Crypto, MeshBoneInfo)
// ---------------------------------------------------------------------------

// -- primitives ------------------------------------------------------------
constexpr uint32_t kHashPrime       = 0x01000193u;
constexpr uint64_t kFnvPrimeCommon  = 0x100000001B3ULL;

// -- UObject slots ---------------------------------------------------------
constexpr int      kSlotBaseOff     = 0x20;
constexpr int      kSlotStride      = 0x20;
constexpr uint32_t kUobjSlotHashAdd = 0x2306CC41u;
constexpr uint32_t kUobjSlotNameXor = 0x2u;
constexpr uint32_t kUobjSlotClassAdj = 0x0u;
constexpr uint32_t kUobjSlotOuterAdj = 0x1u;
constexpr int      kUobjNameRol64   = 0x20;
constexpr uint64_t kSlotClmulK1     = 0x8E9484400ADF26C1ULL;
constexpr uint64_t kSlotClmulK2     = 0x1ADA36649C975181ULL;

// -- FName pool ------------------------------------------------------------
constexpr uint64_t kRvaGnamePool        = 0x10987D40ULL;
constexpr uint64_t kRvaKeystream        = 0x1082B26CULL;
constexpr int      kKeystreamBaseIdx    = 0xC;
constexpr int      kKeystreamCount      = 0x100;
constexpr uint32_t kShardHashAdd        = 0xD69AD929u;
constexpr uint64_t kShardHashSeedOff    = 0x40ULL;
constexpr uint64_t kShardBlockBaseOff   = 0x50ULL;
constexpr uint64_t kShardBlockStride    = 0x20ULL;
constexpr int      kShardRolA           = 0x11;
constexpr int      kShardRolB           = 0x1B;
constexpr int      kShardFinalShr       = 0x5;
constexpr uint64_t kBlockXor            = 0xF401C0BE961D3D9AULL;
constexpr int      kBlockRol64          = 0x13;
constexpr uint8_t  kBlockPshufb[8]      = { 6, 5, 1, 2, 7, 3, 4, 0 };
constexpr uint64_t kFnvAdd              = 0x6C5FD4827126D389ULL;
constexpr int      kFnvRol1             = 0x28;
constexpr int      kFnvRol2             = 0x32;
constexpr uint16_t kHdrIsWideBit        = 0x0001u;
constexpr int      kNarrowKeyShift      = 0x3;
constexpr uint32_t kFuObjectItemStride  = 24;
constexpr uint32_t kFuObjectItemObjOff  = 8;
constexpr uint32_t kItemsPerChunk       = 0x10000;
constexpr int      kChunkIndexShift     = 0x10;
constexpr uint64_t kChunkIndexMask      = 0xFFFFULL;
constexpr uint64_t kUObjectInternalIdx  = 0x90ULL;

// -- FField / UStruct layout + crypto ---------------------------------------
constexpr uint64_t kFfieldOwner         = 0x58;
constexpr uint64_t kFfieldClassPrivate  = 0x60;
constexpr uint64_t kFfieldNext          = 0x68;
constexpr uint64_t kFfieldFnameEnc      = 0x90;
constexpr uint64_t kFfieldArrayDim      = 0xA8;
constexpr uint64_t kFfieldElementSize   = 0xA4;
constexpr uint64_t kFfieldPropertyFlags = 0xB0;
constexpr uint64_t kFfieldOffsetEnc     = 0xBC;
constexpr uint64_t kFfieldBoolFieldSize = 0x118;
constexpr uint64_t kFfieldBoolByteOffset = 0x119;
constexpr uint64_t kFfieldBoolByteMask  = 0x11A;
constexpr uint64_t kFfieldBoolFieldMask = 0x11B;
constexpr uint64_t kFfieldObjPropClass  = 0x118;
constexpr uint64_t kFfieldMapKeyProp    = 0x118;
constexpr uint64_t kFfieldMapValueProp  = 0x120;
constexpr uint64_t kUStructSuperStruct  = 0xB0;
constexpr uint64_t kUStructChildren     = 0xB8;
constexpr uint64_t kUStructChildProperties = 0xE0;
constexpr uint64_t kUStructPropertiesSize = 0xD8;
constexpr uint64_t kUStructMinAlignment = 0x94;
constexpr uint64_t kFfieldAddConst      = 0x44CB31F912F95FFFULL;
constexpr uint64_t kFfieldAdd2Const     = 0xBB34B0C01E5AA000ULL;
constexpr uint32_t kOffsetXorKey        = 0x8DE128DEu;

// -- TEB (Wine/Proton) ------------------------------------------------------
constexpr uint32_t kTebKeyOff           = 0x1F8;
constexpr uint32_t kTebSelfPtrOff       = 0x30;
constexpr uint8_t  kTebShufMask[8]      = { 6, 3, 1, 7, 0, 2, 4, 5 };
constexpr uint32_t kWordRor             = 0x1;
constexpr uint32_t kQwordRor            = 0x3;
constexpr uint32_t kWineTebSelfPtr      = 0x30;
constexpr uint32_t kWineTebThreadTls    = 0x58;

// -- PlayerDecrypt (static SIMD LocalPlayer slot) ----------------------------
constexpr uint32_t kPlayerOff           = 0x4B0;
constexpr uint64_t kBlendXorMask        = 0x9A492C85DDF6F193ULL;
constexpr uint32_t kLaneRot             = 0xD;
constexpr uint32_t kFinalRot            = 0x27;

// -- OuterDecrypt -----------------------------------------------------------
constexpr uint32_t kOuterHashAdd        = 0x20193F54u;
constexpr uint32_t kOuterHashRot1       = 0x19;
constexpr uint32_t kOuterHashRot2       = 0xF;

// -- GameInstanceTlsDecrypt -------------------------------------------------
constexpr uint32_t kTlsIndexRva         = 0x0EAF5D78u;
constexpr uint32_t kTlsHashAdd          = 0x957E395Cu;
constexpr uint64_t kKeyFnvPrime         = 0x100000001B3ULL;
constexpr uint64_t kKeyFnvAdd           = 0xB92AB41238A38FDCULL;
constexpr uint32_t kKeyFnvRot1          = 0x26;
constexpr uint32_t kKeyFnvRot2          = 0x2B;
constexpr uint32_t kTibTlsSlotsOff      = 0x58;
constexpr uint32_t kTlsInitFlagOff      = 0xED0;
constexpr uint32_t kTlsKeyTableOff      = 0xEE0;
constexpr uint32_t kTlsTableStride      = 0x90;
constexpr uint32_t kTlsVtFnIndexOff     = 0x48;
constexpr uint32_t kTlsXmmInputOff      = 0x80;
constexpr uint32_t kTlsMaxSlots         = 0x440;

// -- GameInstanceStaticDecrypt ----------------------------------------------
constexpr uint32_t kStageArrayRva       = 0x10F35650u;
constexpr uint32_t kPshufbMaskRva       = 0x0DDFC1D0u;
constexpr uint32_t kGiK1                = 0x742217C8u;
constexpr uint32_t kGiK2                = 0x0005E838u;
constexpr uint32_t kNeg109              = 0xFFFFFF93u;
constexpr uint64_t kGiXorMask           = 0x291AED004FAE1FACULL;
constexpr uint64_t kGiFnv64             = 0x100000001B3ULL;
constexpr uint64_t kGiAdd64             = 0x2A79E93E092D2538ULL;
constexpr uint32_t kRot64_1             = 0x38;
constexpr uint32_t kRot64_2             = 0x21;
constexpr uint32_t kResultDeref         = 0x18;

// -- GameInstanceDecrypt / Global / misc ------------------------------------
constexpr uint64_t kMinValidPointer     = 0x1000ULL;
constexpr uint64_t kMaxValidPointer     = 0x7FFFFFFFFFFFULL;
constexpr uint32_t kGiShuffleMaskRva    = 0xB09C350u;
constexpr uint32_t kGiXorKeyRva         = 0xB06C380u;
constexpr uint32_t kGiXorKeyRva2        = 0xB06C390u;
constexpr uint32_t kWorldOffset         = 0x2F0;
constexpr uint64_t kUWorldBaseRva       = 0x10839A98ULL;
constexpr uint32_t kObjectXorKeyRva     = 0xD91F885u;
constexpr uint32_t kSimdMaskRva         = 0xAD2FC50u;
constexpr uint64_t kXmmXorVal           = 0xA738DD8241D227C2ULL;
constexpr uint32_t kMeshBoneInfoName    = 0x0;
constexpr uint32_t kMeshBoneInfoParentIndex = 0x8;
constexpr uint32_t kMeshBoneInfoStride  = 0x18;

// ---------------------------------------------------------------------------
// Pipeline state + TEB key
// ---------------------------------------------------------------------------

struct PipelineState {
    uint64_t gnamePoolRva = kRvaGnamePool;
    uint16_t keystream[kKeystreamCount] = {};
    bool     ksLoaded = false;
};

inline std::atomic<uint64_t>& CachedTebKey()
{
    static std::atomic<uint64_t> s{ 0 };
    return s;
}

inline std::atomic<bool>& TebKeyValid()
{
    static std::atomic<bool> s{ false };
    return s;
}

// get_teb_key / set_teb_key (drop names)
inline uint64_t GetTebKey()
{
    if (TebKeyValid().load(std::memory_order_relaxed))
        return CachedTebKey().load(std::memory_order_relaxed);
    return 0;
}

inline void SetTebKey(uint64_t key)
{
    CachedTebKey().store(key, std::memory_order_relaxed);
    TebKeyValid().store(true, std::memory_order_relaxed);
}

inline void ClearTebKey()
{
    TebKeyValid().store(false, std::memory_order_relaxed);
    CachedTebKey().store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Primitive helpers (drop game::detail)
// ---------------------------------------------------------------------------

inline uint32_t Rol32(uint32_t v, int s)
{
    s &= 31;
    return (v << s) | (v >> ((32 - s) & 31));
}

inline uint64_t Rol64(uint64_t v, int s)
{
    s &= 63;
    return (v << s) | (v >> ((64 - s) & 63));
}

// Portable 64x64 GF(2) carry-less multiply, low half (clmul64_low).
inline uint64_t ClmulLo(uint64_t x, uint64_t y)
{
    uint64_t r = 0;
    for (int b = 0; b < 64; ++b)
        if (y & (1ULL << b))
            r ^= x << b;
    return r;
}

// PSHUFB-style byte shuffle of a u64 (shuffle64).
inline uint64_t Pshufb64(uint64_t x, const uint8_t mask[8])
{
    uint8_t in[8], out[8] = {};
    std::memcpy(in, &x, 8);
    for (int i = 0; i < 8; ++i) {
        const uint8_t sel = static_cast<uint8_t>(mask[i] & 0x0F);
        out[i] = (sel < 8) ? in[sel] : 0;
    }
    uint64_t r = 0;
    std::memcpy(&r, out, 8);
    return r;
}

// 16-byte shuffle whose low 8 result bytes come from a 16-byte source
// (GameInstanceStaticDecrypt::pshufb_lo8).
inline uint64_t PshufbLo8(uint64_t dataLo, uint64_t dataHi, uint64_t mask)
{
    uint8_t src[16];
    std::memcpy(src, &dataLo, 8);
    std::memcpy(src + 8, &dataHi, 8);
    uint8_t m[8];
    std::memcpy(m, &mask, 8);
    uint8_t out[8];
    for (int i = 0; i < 8; ++i) {
        const uint8_t sel = m[i];
        out[i] = (sel & 0x80) ? 0 : src[sel & 0xF];
    }
    uint64_t r = 0;
    std::memcpy(&r, out, 8);
    return r;
}

inline uint32_t Bswap32(uint32_t v)
{
    return (v << 24) | ((v << 8) & 0x00FF0000u) |
           ((v >> 8) & 0x0000FF00u) | (v >> 24);
}

inline bool IsPlausibleNamePtr(uint64_t p)
{
    return p >= 0x10000ULL && p < 0x800000000000ULL;
}

template <typename T, typename R>
inline T ReadVal(const R& r, uint64_t addr)
{
    T v{};
    r.Read(addr, &v, sizeof(T));
    return v;
}

// ---------------------------------------------------------------------------
// Decode primitives (drop game::gasm / FName)
// ---------------------------------------------------------------------------

// Name-pool block decode (DecodeBlock): XOR -> ROL64(19) -> PSHUFB.
inline uint64_t DecodeBlock(uint64_t raw)
{
    uint64_t x = raw ^ kBlockXor;
    x = Rol64(x, kBlockRol64);
    return Pshufb64(x, kBlockPshufb);
}

// UObject slot CLMUL decode (DecodeSlotLo).
inline uint64_t DecodeSlotLo(uint64_t lo, uint64_t hi)
{
    const uint64_t t = hi ^ ClmulLo(kSlotClmulK2, lo);
    const uint64_t v = ClmulLo(kSlotClmulK1, t) ^ lo;
    return Rol64(v, kUobjNameRol64);
}

// FField name ADD/ROL decode (DecodeFFieldName / decode_ffield_nameprivate).
inline uint64_t DecodeFFieldName(uint64_t source, uint64_t second)
{
    uint64_t v = Rol64(source + kFfieldAddConst, 17);
    v ^= second;
    v = Rol64(v + kFfieldAdd2Const, 47);
    return Rol64(source ^ v, 32);
}

// FProperty offset decode (drop: decode_ffproperty_offset). The drop XORs
// after the byte-swap (OFFSET_XOR_KEY = 0x8DE128DE); SDK.hpp's folded form
// bswap32(raw ^ 0xDE28E18D) is algebraically identical because the two keys
// are byte-reverses of each other. Getting the swap side wrong silently swaps
// the keys, so Tests/name_crypto_tests.cpp pins both forms.
inline uint32_t DecodeFfPropertyOffset(uint32_t raw)
{
    return Bswap32(raw) ^ kOffsetXorKey;
}

// Name-pool shard hash (ShardHash): 3 imul rounds + SHR5.
inline void ShardHash(uint64_t seedAddr, uint32_t& bidx1, uint32_t& bidx2)
{
    const uint32_t lo = static_cast<uint32_t>(seedAddr);
    const uint32_t hi = static_cast<uint32_t>(seedAddr >> 32);
    const uint32_t p = kHashPrime;
    const uint32_t a = kShardHashAdd;
    uint32_t r = p * Rol32(lo, kShardRolA) + a;
    r = p * Rol32(r, kShardRolB) + hi + a;
    r = p * Rol32(r, kShardRolA) + a;
    const uint32_t v9 = p * (r >> kShardFinalShr) + a;
    const uint8_t b = static_cast<uint8_t>(v9) ^ static_cast<uint8_t>(v9 >> 16);
    bidx1 = static_cast<uint32_t>(b) & 7u;
    bidx2 = (bidx1 + 1u) & 7u;
}

// UObject slot hash (obj_slot_hash): obj+0x10 seed, SHR4 + 3 imul rounds.
inline uint32_t ObjSlotHash(uint64_t objPtr)
{
    const uint64_t seed = objPtr + 0x10;
    const uint32_t lo = static_cast<uint32_t>(seed);
    const uint32_t hi = static_cast<uint32_t>(seed >> 32);
    const uint32_t p = kHashPrime;
    const uint32_t a = kUobjSlotHashAdd;
    uint32_t h = p * (lo >> 4) + a;
    h = p * Rol32(h, 21) + hi + a;
    h = p * Rol32(h, 28) + a;
    return p * (h >> 11) + a;
}

inline uint32_t ObjSlotIndexBase(uint64_t objPtr)
{
    const uint32_t v3 = ObjSlotHash(objPtr);
    return (static_cast<uint32_t>(static_cast<uint8_t>(v3)) ^
            static_cast<uint32_t>(static_cast<uint8_t>(v3 >> 16))) & 3u;
}

inline uint32_t ObjNameSlot(uint64_t objPtr)
{
    return ObjSlotIndexBase(objPtr) ^ kUobjSlotNameXor;
}

inline uint32_t ObjClassSlot(uint64_t objPtr)
{
    return (ObjSlotIndexBase(objPtr) + kUobjSlotClassAdj) & 3u;
}

inline uint32_t ObjOuterSlot(uint64_t objPtr)
{
    return (ObjSlotIndexBase(objPtr) + kUobjSlotOuterAdj) & 3u;
}

// FNV64 double mix used by the name-pool pointer chain.
inline uint64_t FnvMix2(uint64_t v14)
{
    uint64_t fv = kFnvPrimeCommon * Rol64(v14, kFnvRol1) + kFnvAdd;
    fv = kFnvPrimeCommon * Rol64(fv, kFnvRol2) + kFnvAdd;
    return fv;
}

// FNameEntry header decode (decode_fname_header_len).
inline void FNameHeaderDecode(uint16_t header, bool& isWide, int& rawLen)
{
    isWide = (header & kHdrIsWideBit) != 0;
    rawLen = static_cast<int>((header >> 13) | ((header >> 3) & 0x3F8u));
}

// ---------------------------------------------------------------------------
// String decrypt (pure, over an already-read payload)
// ---------------------------------------------------------------------------

// Paired-LCG keystream decrypt (decode_fname / decode_fname_wide_v922).
inline std::string DecryptNameBytes(const PipelineState& s,
                                    const uint8_t* payload, int rawLen,
                                    bool isWide)
{
    if (!s.ksLoaded || !payload || rawLen <= 0)
        return {};

    auto ksWord = [&s](uint8_t k, int mul) -> uint16_t {
        const int idx = ((mul * static_cast<int>(k)) & 0x3F) + kKeystreamBaseIdx;
        if (idx < 0 || idx >= kKeystreamCount)
            return 0;
        return s.keystream[idx];
    };

    if (!isWide) {
        const int length = rawLen;
        std::string out;
        out.reserve(static_cast<size_t>(length));
        std::vector<uint8_t> buf(payload, payload + length);
        uint32_t key = static_cast<uint32_t>(length) - 33u;
        for (int i = 0; i < length; i += 2) {
            const uint8_t k = static_cast<uint8_t>(key);
            buf[static_cast<size_t>(i)] ^=
                static_cast<uint8_t>(ksWord(k, 1) >> kNarrowKeyShift);
            if (i + 1 < length)
                buf[static_cast<size_t>(i) + 1] ^=
                    static_cast<uint8_t>(ksWord(k, 33) >> kNarrowKeyShift);
            key = (key & ~0xFFu) | static_cast<uint8_t>(65u * k + 0x80u);
        }
        for (int j = 0; j < length; ++j) {
            if (!buf[static_cast<size_t>(j)])
                break;
            out.push_back(static_cast<char>(buf[static_cast<size_t>(j)]));
        }
        return out;
    }

    const int length = rawLen / 2;
    if (length <= 0 || length > 128)
        return {};
    std::vector<uint16_t> wbuf(static_cast<size_t>(length), 0);
    std::memcpy(wbuf.data(), payload,
                static_cast<size_t>(length) * sizeof(uint16_t));
    uint32_t key = static_cast<uint32_t>(length) + 2783u;
    int nonAscii = 0;
    std::string out;
    out.reserve(static_cast<size_t>(length));
    for (int i = 0; i < length; i += 2) {
        const uint8_t k = static_cast<uint8_t>(key);
        if (i < length) {
            const uint16_t w =
                static_cast<uint16_t>(wbuf[static_cast<size_t>(i)] ^ ksWord(k, 1));
            const char ch = static_cast<char>(w & 0xFF);
            if ((w & 0xFF00u) != 0 || static_cast<uint8_t>(ch) >= 0x80u)
                ++nonAscii;
            out.push_back(ch);
        }
        if (i + 1 < length) {
            const uint16_t w2 = static_cast<uint16_t>(
                wbuf[static_cast<size_t>(i) + 1] ^ ksWord(k, 33));
            const char ch2 = static_cast<char>(w2 & 0xFF);
            if ((w2 & 0xFF00u) != 0 || static_cast<uint8_t>(ch2) >= 0x80u)
                ++nonAscii;
            out.push_back(ch2);
        }
        key = (key & ~0xFFu) | static_cast<uint8_t>(65u * k + 0x80u);
    }
    if (!out.empty() && nonAscii * 5 > static_cast<int>(out.size()))
        return {};
    return out;
}

// Printability gate for decoded names (is_sane_name).
inline bool IsSaneName(const std::string& s)
{
    if (s.empty() || s.size() > 128)
        return false;
    int printable = 0;
    for (unsigned char c : s)
        if (c >= 32 && c <= 126)
            ++printable;
    return printable * 5 >= static_cast<int>(s.size()) * 4;
}

// utf16_to_utf8_portable — wide-name conversion in the FName string decode.
inline std::string Utf16ToUtf8Portable(const uint16_t* data, size_t len)
{
    if (!data || len == 0)
        return {};
    std::string out;
    out.reserve(len * 2);

    auto emit = [&out](uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };

    for (size_t i = 0; i < len; ++i) {
        const uint16_t w1 = data[i];
        if (w1 >= 0xD800 && w1 <= 0xDBFF) {
            if (i + 1 < len) {
                const uint16_t w2 = data[i + 1];
                if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                    const uint32_t cp = 0x10000u +
                        ((static_cast<uint32_t>(w1 - 0xD800) << 10) |
                         static_cast<uint32_t>(w2 - 0xDC00));
                    emit(cp);
                    ++i;
                    continue;
                }
            }
            emit(0xFFFD);
            continue;
        }
        if (w1 >= 0xDC00 && w1 <= 0xDFFF) {
            emit(0xFFFD);
            continue;
        }
        emit(static_cast<uint32_t>(w1));
    }
    return out;
}

// ---------------------------------------------------------------------------
// TEB decrypt (TebDecrypt / decrypt_teb)
// ---------------------------------------------------------------------------

inline uint64_t DecryptTeb(uint64_t enc, uint64_t tebKey)
{
    if (enc == 0)
        return 0;
    uint64_t x = enc ^ tebKey;

    uint16_t w[4];
    std::memcpy(w, &x, 8);
    for (int i = 0; i < 4; ++i)
        w[i] = static_cast<uint16_t>((w[i] >> kWordRor) | (w[i] << (16 - kWordRor)));
    uint8_t bytes[8];
    std::memcpy(bytes, w, 8);

    uint8_t shuffled[8];
    for (int i = 0; i < 8; ++i)
        shuffled[i] = bytes[kTebShufMask[i]];

    uint64_t s;
    std::memcpy(&s, shuffled, 8);
    const uint64_t result = (s >> kQwordRor) | (s << (64 - kQwordRor));
    return IsPlausibleNamePtr(result) ? result : 0;
}

// ---------------------------------------------------------------------------
// Reader-templated walks (R::Read(addr, dst, size) -> bool)
// ---------------------------------------------------------------------------

template <typename R>
inline uint64_t ReadSlotDecoded(uint64_t objBase, uint32_t slot, const R& reader)
{
    const uint64_t addr =
        objBase + kSlotBaseOff + static_cast<uint64_t>(slot) * kSlotStride;
    uint64_t lo = 0, hi = 0;
    if (!reader.Read(addr, &lo, 8))
        return 0;
    if (!reader.Read(addr + 8, &hi, 8))
        return 0;
    if (!lo && !hi)
        return 0;
    return DecodeSlotLo(lo, hi);
}

template <typename R>
inline uint64_t ReadSlotDecodedAsPtr(uint64_t objBase, uint32_t slot, const R& reader)
{
    return Rol64(ReadSlotDecoded(objBase, slot, reader), kUobjNameRol64);
}

template <typename R>
inline uint64_t ReadSlotRaw(uint64_t objBase, uint32_t slot, const R& reader)
{
    const uint64_t addr =
        objBase + kSlotBaseOff + static_cast<uint64_t>(slot) * kSlotStride;
    return ReadVal<uint64_t>(reader, addr);
}

// FindFNameSlot: the actor's NamePrivate slot (index+number packed u64).
template <typename R>
inline uint64_t FindFNameSlot(uint64_t objBase, const R& reader)
{
    return ReadSlotDecoded(objBase, ObjNameSlot(objBase), reader);
}

// ResolveNamePtr: CI -> FNameEntry* via chunk/shard/blocks + FNV.
template <typename R>
inline uint64_t ResolveNamePtr(int32_t compIndex, uint64_t gameBase,
                               const PipelineState& s, const R& reader)
{
    if (compIndex < 0 || !s.ksLoaded)
        return 0;

    const uint32_t ci = static_cast<uint32_t>(compIndex);
    const uint32_t nameOff = ci & 0xFFFFu;
    const uint32_t chunkOff = (ci >> 8) & 0xFFFF00u;
    // Drop: ChunkAddr = GameBase + S.gnamePoolRva + ChunkOff.
    const uint64_t chunkAddr = gameBase + s.gnamePoolRva + chunkOff;

    uint32_t bidx1 = 0, bidx2 = 0;
    ShardHash(chunkAddr + kShardHashSeedOff, bidx1, bidx2);

    const uint64_t blockBase = chunkAddr + kShardBlockBaseOff;
    const uint64_t raw1 = ReadVal<uint64_t>(reader, blockBase + kShardBlockStride * bidx1);
    const uint64_t raw2 = ReadVal<uint64_t>(reader, blockBase + kShardBlockStride * bidx2);
    if (!raw1 && !raw2)
        return 0;

    const uint64_t v14 = DecodeBlock(raw1);
    const uint64_t v15 = DecodeBlock(raw2);
    const uint64_t fv = FnvMix2(v14);
    const uint64_t entryPtr = v14 + (v15 ^ fv) + 2ULL * nameOff;

    return IsPlausibleNamePtr(entryPtr) ? entryPtr : 0;
}

// DecryptNameString: FNameEntry* -> text (header + paired-LCG payload).
template <typename R>
inline std::string DecryptNameString(uint64_t nameEntryPtr, const PipelineState& s,
                                     const R& reader)
{
    if (!nameEntryPtr || !s.ksLoaded)
        return {};
    const uint16_t header = ReadVal<uint16_t>(reader, nameEntryPtr);
    if (!header)
        return {};
    bool isWide = false;
    int rawLen = 0;
    FNameHeaderDecode(header, isWide, rawLen);
    if (rawLen <= 0 || rawLen > 2046)
        return {};

    int byteCount = rawLen;
    if (isWide) {
        const int length = rawLen / 2;
        if (length <= 0 || length > 128)
            return {};
        byteCount = length * 2;
    }
    std::vector<uint8_t> payload(static_cast<size_t>(byteCount), 0);
    if (!reader.Read(nameEntryPtr + 2, payload.data(),
                     static_cast<size_t>(byteCount)))
        return {};
    return DecryptNameBytes(s, payload.data(), rawLen, isWide);
}

// ---------------------------------------------------------------------------
// TLS / game-instance / player decrypts
// ---------------------------------------------------------------------------

// tls_hash: slot hash over the TLS block address.
inline uint32_t TlsHash(uint64_t addr)
{
    const uint32_t lo32 = static_cast<uint32_t>(addr);
    const uint32_t hi32 = static_cast<uint32_t>(addr >> 32);
    uint32_t h = Rol32(lo32, 15) * kHashPrime + kTlsHashAdd;
    h = Rol32(h, 18) * kHashPrime + hi32 + kTlsHashAdd;
    h = Rol32(h, 15) * kHashPrime + kTlsHashAdd;
    h = Rol32(h, 18) * kHashPrime + kTlsHashAdd;
    return h;
}

// read_tls_index: TLS index from the module image.
template <typename R>
inline uint32_t ReadTlsIndex(uint64_t gameBase, const R& reader)
{
    return ReadVal<uint32_t>(reader, gameBase + kTlsIndexRva);
}

// find_tls_base: first TLS block with the init flag set.
template <typename R>
inline uint64_t FindTlsBase(uint64_t teb, const R& reader)
{
    if (!IsPlausibleNamePtr(teb))
        return 0;
    const uint64_t tlsSlots = ReadVal<uint64_t>(reader, teb + kTibTlsSlotsOff);
    if (!IsPlausibleNamePtr(tlsSlots))
        return 0;
    for (uint32_t idx = 0; idx < kTlsMaxSlots; ++idx) {
        const uint64_t entry = ReadVal<uint64_t>(reader, tlsSlots + static_cast<uint64_t>(idx) * 8);
        if (!IsPlausibleNamePtr(entry))
            continue;
        if (ReadVal<uint8_t>(reader, entry + kTlsInitFlagOff) != 1)
            continue;
        return entry;
    }
    return 0;
}

// tls_slot_value: block slot -> vtable+0x48 fn -> value.
template <typename R>
inline uint64_t TlsSlotValue(uint64_t tlsBase, uint32_t slot, const R& reader)
{
    const uint64_t vtablePtrAddr =
        tlsBase + kTlsKeyTableOff + static_cast<uint64_t>(slot) * kTlsTableStride;
    const uint64_t vtable = ReadVal<uint64_t>(reader, vtablePtrAddr);
    if (!IsPlausibleNamePtr(vtable))
        return 0;
    const uint64_t fn = ReadVal<uint64_t>(reader, vtable + kTlsVtFnIndexOff);
    if (!IsPlausibleNamePtr(fn))
        return 0;
    return ReadVal<uint64_t>(reader, fn);
}

// decrypt_game_instance_tls: TLS block pair -> FNV chain -> GameInstance*.
template <typename R>
inline uint64_t DecryptGameInstanceTls(uint64_t teb, const R& reader)
{
    const uint64_t tlsBase = FindTlsBase(teb, reader);
    if (!tlsBase)
        return 0;
    const uint32_t h = TlsHash(tlsBase + kTlsInitFlagOff);
    const uint32_t slotA = (h ^ (h >> 16)) & 7u;
    const uint32_t slotB = (slotA + 1u) & 7u;

    const uint64_t a = TlsSlotValue(tlsBase, slotA, reader);
    const uint64_t b = TlsSlotValue(tlsBase, slotB, reader);
    if (a == 0 && b == 0)
        return 0;

    const uint64_t inner = kKeyFnvPrime * Rol64(a, kKeyFnvRot1) + kKeyFnvAdd;
    const uint64_t outer = kKeyFnvPrime * Rol64(inner, kKeyFnvRot2) + kKeyFnvAdd;
    const uint64_t result = a + (b ^ outer);
    return IsPlausibleNamePtr(result) ? result : 0;
}

// decrypt_player_from_controller: APlayerController -> ULocalPlayer (static
// SIMD mask; TEB-key fast path via DecryptTeb when a key is cached).
template <typename R>
inline uint64_t DecryptPlayerFromController(uint64_t controller, const R& reader)
{
    if (!IsPlausibleNamePtr(controller))
        return 0;
    const uint64_t enc = ReadVal<uint64_t>(reader, controller + kPlayerOff);
    if (enc == 0)
        return 0;

    const uint64_t tebKey = GetTebKey();
    if (tebKey != 0)
        return DecryptTeb(enc, tebKey);

    const uint32_t lo = static_cast<uint32_t>(enc);
    const uint32_t hi = static_cast<uint32_t>(enc >> 32);
    const uint64_t rot = static_cast<uint64_t>(Rol32(lo, kLaneRot)) |
                         (static_cast<uint64_t>(Rol32(hi, kLaneRot)) << 32);
    const uint64_t blended = rot ^ kBlendXorMask;
    if (blended == 0)
        return 0;
    const uint64_t result = Rol64(blended, kFinalRot);
    return IsPlausibleNamePtr(result) ? result : 0;
}

// decrypt_game_state: UWorld -> AGameStateBase via LevelCollections[0] (the
// SIMD slot was retired; the chain is plaintext on this build).
template <typename R>
inline uint64_t DecryptGameState(uint64_t uworldAddr, uint64_t levelCollectionsOff,
                                 uint64_t gameStateOff, const R& reader)
{
    if (!IsPlausibleNamePtr(uworldAddr))
        return 0;
    const uint64_t lcData = ReadVal<uint64_t>(reader, uworldAddr + levelCollectionsOff);
    if (lcData < kMinValidPointer || lcData > kMaxValidPointer)
        return 0;
    const uint64_t result = ReadVal<uint64_t>(reader, lcData + gameStateOff);
    if (result < kMinValidPointer || result > kMaxValidPointer)
        return 0;
    return result;
}

} // namespace namecrypto
