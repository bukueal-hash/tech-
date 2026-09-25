#pragma once
// ── Adopted from SDK/SDK.txt (repo root drop, game build offsets + decode) ────
// This file is the in-project copy of the SDK dump. MSVC adaptations vs the
// original .txt: functions are `inline` instead of `static` (so unused ones
// don't trip C4505 under /W4 /WX) and decode_ffproperty_offset uses a
// portable byte-swap instead of __builtin_bswap32.

#include <cstdint>   // u8..u64 aliases
#include <cstdlib>   // _byteswap_ulong (MSVC)
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

namespace game
{
    // GNAMES / the layout offsets agree with the 20260922 dump: its own header
    // (sdk/CppSDK/SDK/Basic.hpp, namespace Offsets) carries GWORLD 0x10839A98,
    // GNAMES 0x10987D40, GObjects 0x10C56FE0, ProcessEvent 0x5AF560 (index 0x4C)
    // and nothing else. GWORLD is a double-dereferenced global in this drop:
    // [base + 0x10839A98] -> intermediary -> [+0] UWorld. KEYTABLE and
    // FString_Verification_Offset are
    // NOT part of that dump — this build's FNamePool is vanilla and it has no
    // verification RVA — so they are carried over from the earlier drop and only
    // feed the fallback slot/keystream paths (gated on the key table loading).
    namespace offsets
    {
        constexpr u32 GWORLD = 0x10839A98;   // sdk/sdk.txt Global::UWORLD_BASE_RVA; double-deref
        constexpr u32 GNAMES = 0x10987D40;
        constexpr u32 KEYTABLE = 0x1082B26C;
        constexpr u32 UStruct_SuperStruct = 0xB0;
        constexpr u32 FString_Verification_Offset = 0xD3C29A0;
        constexpr u32 UStruct_PropertyLink = 0x118;
        constexpr u32 FProperty_PropertyLinkNext = 0xD8;
        constexpr u32 FField_ClassPrivate = 0x60;
        constexpr u32 FObjectPropertyBase_PropertyClass = 0x118;
        constexpr u32 FFieldClass_SuperClass = 0x40;
    }
    namespace detail
    {
        inline u32 rol32(u32 v, unsigned n) { return (v << n) | (v >> (32 - n)); }
        inline u64 rol64(u64 v, unsigned n) { return (v << n) | (v >> (64 - n)); }
        inline u64 clmul64_low(u64 a, u64 b) {
            u64 out = 0;
            while (b) { if (b & 1) out ^= a; b >>= 1; a <<= 1; }
            return out;
        }
        inline u64 shuffle64(u64 value) {
            constexpr unsigned indices[8] = {6, 5, 1, 2, 7, 3, 4, 0};
            u64 out = 0;
            const u8* src = reinterpret_cast<const u8*>(&value);
            u8* dst = reinterpret_cast<u8*>(&out);
            for (unsigned i = 0; i < 8; ++i) dst[i] = src[indices[i]];
            return out;
        }
        inline u32 uobject_selector(u64 game_address) {
            const u64 address = game_address + 0x10;
            u32 v = rol32(u32(address), 28) * 0x01000193u + 0x2306CC41u;
            v = rol32(v, 21) * 0x01000193u + u32(address >> 32) + 0x2306CC41u;
            v = rol32(v, 28) * 0x01000193u + 0x2306CC41u;
            v = (v >> 11) * 0x01000193u + 0x2306CC41u;
            return ((v >> 16) ^ v) & 3u;
        }
        inline u64 decode_uobject_slot(const u8* slot) {
            constexpr u64 first = 0x1ADA36649C975181ull;
            constexpr u64 second = 0x8E9484400ADF26C1ull;
            const u64 low = *reinterpret_cast<const u64*>(slot);
            const u64 high = *reinterpret_cast<const u64*>(slot + 8);
            return low ^ clmul64_low(second, clmul64_low(first, low) ^ high);
        }
        inline u32 fname_pool_selector(u64 address) {
            u32 v = rol32(u32(address), 17) * 0x01000193u + 0xD69AD929u;
            v = rol32(v, 27) * 0x01000193u + u32(address >> 32) + 0xD69AD929u;
            v = rol32(v, 17) * 0x01000193u + 0xD69AD929u;
            v = (v >> 5) * 0x01000193u + 0xD69AD929u;
            return v ^ (v >> 16);
        }
        inline u64 decode_fname_pool_first(const u8* slot) {
            static const u8 an[8] = {0x0A,0x9B,0x74,0x95,0x58,0xDD,0x72,0x8D};
            static const u8 aa[8] = {0xF5,0x64,0x8B,0x6A,0xA7,0x22,0x8D,0x72};
            static const u8 xx[8] = {0x90,0xA6,0x69,0x03,0xE6,0x1D,0x73,0x79};
            const u64 v = *reinterpret_cast<const u64*>(slot);
            const u64 selected = ((~v & *reinterpret_cast<const u64*>(an)) |
                                  (v & *reinterpret_cast<const u64*>(aa))) ^
                                 *reinterpret_cast<const u64*>(xx);
            return shuffle64(rol64(selected, 19));
        }
        inline u64 decode_fname_pool_second(const u8* slot) {
            static const u8 xx[8] = {0x9A,0x3D,0x1D,0x96,0xBE,0xC0,0x01,0xF4};
            return shuffle64(rol64(*reinterpret_cast<const u64*>(slot) ^
                                   *reinterpret_cast<const u64*>(xx), 19));
        }
        inline u32 bswap32(u32 v) {
#if defined(_MSC_VER)
            return _byteswap_ulong(v);
#else
            return __builtin_bswap32(v);
#endif
        }
    }
    namespace gasm
    {
        struct decrypt_ffieldclass_name_context { u64 _rax; u64 _rbx; };
        inline u64 decode_uobject_nameprivate(u64 game_address, const u8* object_buf) {
            const u32 selector = detail::uobject_selector(game_address) ^ 2u;
            return detail::rol64(detail::decode_uobject_slot(object_buf + 0x20 + selector * 0x20), 32);
        }
        inline u64 decode_uobject_classprivate(u64 game_address, const u8* object_buf) {
            const u32 selector = detail::uobject_selector(game_address);
            return detail::decode_uobject_slot(object_buf + 0x20 + selector * 0x20);
        }
        inline u64 decode_fname_pool_address(u64 gnames_address, const u8* fname_pools_buf, u32 chunk_offset) {
            const u32 selector = detail::fname_pool_selector(gnames_address + chunk_offset + 0x40);
            const u8* slots = fname_pools_buf + chunk_offset + 0x50;
            const u64 first = detail::decode_fname_pool_first(slots + (selector & 7u) * 0x20);
            const u64 second = detail::decode_fname_pool_second(slots + ((selector + 1u) & 7u) * 0x20);
            u64 mixed = detail::rol64(first, 40) * 0x00000100000001B3ull + 0x6C5FD4827126D389ull;
            mixed = detail::rol64(mixed, 50) * 0x00000100000001B3ull + 0x6C5FD4827126D389ull;
            return (mixed ^ second) + first;
        }
        inline u32 decode_fname_header_len(u16 hdr) {
            return ((u32(hdr) >> 3) & 0x3F8u) + (u32(hdr) >> 13);
        }
        inline u32 decode_fname(u16 hdr, u8* buf, const u16* key_table_base) {
            const u32 length = decode_fname_header_len(hdr);
            const u16* table = reinterpret_cast<const u16*>(reinterpret_cast<const u8*>(key_table_base) + 0x18);
            u32 state = length + 0xADFu, index = 0;
            for (; index + 1 < length; index += 2) {
                buf[index] ^= u8(table[state & 63u] >> 3);
                buf[index + 1] ^= u8(table[(state * 33u) & 63u] >> 3);
                state = state * 0x00762E41u + 0xFE5AE580u;
            }
            if (index < length) buf[index] ^= u8(table[state & 63u] >> 3);
            return length;
        }

        // ── v20260922 (CL-1389382) WIDE name decode ────────────────────────
        // Same keystream window and recurrence as decode_fname (the drop's base
        // index 12 is this +0x18; 0x762E41 & 0xFF == 65, 0xFE5AE580 & 0xFF ==
        // 0x80, and only the low byte feeds the 6-bit index), and the same seed
        // — the drop writes it as charCount + 2783, and 2783 % 256 == 223 ==
        // -33 % 256, so its narrow "length - 33" is the length + 0xADF already
        // used below. Narrow entries therefore needed no change at all.
        //
        // Wide entries DID change: the older drop ran the FString pipeline over
        // them, while the current one XORs the keystream word whole
        // (u16 ^= ks, not ks >> 3) into each u16. Both are tried by the name
        // reader, keystream first.
        //
        // inline, not static: used only by the name reader, and a header-only
        // static helper trips C4505 (warning-as-error) in every translation
        // unit that includes SDK.hpp without calling it.
        inline u8 keystream_name_key_step(u8 k) {
            return static_cast<u8>(65u * static_cast<u32>(k) + 0x80u);
        }

        inline u16 keystream_name_word(const u16* key_table_base, u8 k, u32 mul) {
            const u16* table = reinterpret_cast<const u16*>(
                reinterpret_cast<const u8*>(key_table_base) + 0x18);
            return table[(mul * static_cast<u32>(k)) & 0x3Fu];
        }

        inline u32 decode_fname_wide_v922(u16 hdr, u16* buf, const u16* key_table_base) {
            const u32 byteCount = decode_fname_header_len(hdr);
            const u32 chars = byteCount / 2;
            if (chars == 0)
                return 0;
            u8 key = static_cast<u8>(chars + 2783u);
            for (u32 i = 0; i < chars; i += 2) {
                buf[i] ^= keystream_name_word(key_table_base, key, 1);
                if (i + 1 < chars)
                    buf[i + 1] ^= keystream_name_word(key_table_base, key, 33);
                key = keystream_name_key_step(key);
            }
            return chars;
        }
        inline u8 decode_fstring_byte(u8 encrypted, u32& state) {
            u32 value = u32(int32_t(int8_t(encrypted)));
            state = (detail::rol32(state * 0x01000193u + 0xD351FEECu, 14) + state) * 0x01000193u;
            value ^= state & 31u;
            u32 a = value + (value - 0x21u < 0x2Fu ? 0x2Fu : (value - 0x50u < 0x2Fu ? 0xFFFFFFD1u : 0));
            u32 middle = a + (a - 0x30u < 5u ? 5u : (a - 0x35u < 5u ? 0xFFFFFFFBu : 0));
            middle += middle - 0x61u < 0xDu ? 0xDu : (middle - 0x6Eu < 0xDu ? 0xFFFFFFF3u : 0);
            middle += middle - 0x41u < 0xDu ? 0xDu : (middle - 0x4Eu < 0xDu ? 0xFFFFFFF3u : 0);
            return u8(middle + (middle - 0x21u < 0x2Fu ? 0x2Fu :
                                (middle - 0x50u < 0x2Fu ? 0xFFFFFFD1u : 0)));
        }
        inline u64 decode_fstring(u8* fstring) {
            u32 state = 0;
            for (u32 i = 0; fstring[i]; ++i)
                fstring[i] = decode_fstring_byte(fstring[i], state);
            return reinterpret_cast<u64>(fstring);
        }
        inline u64 decode_fstring(u16* fstring) {
            u32 state = 0;
            for (u32 i = 0; fstring[i]; ++i)
                fstring[i] = u16((fstring[i] & 0xFF00u) | decode_fstring_byte(u8(fstring[i]), state));
            return reinterpret_cast<u64>(fstring);
        }
        inline u64 decode_ffield_nameprivate(const u8* field_buf) {
            const u64 source = *reinterpret_cast<const u64*>(field_buf + 0x90);
            u64 value = detail::rol64(source + 0x44CB31F912F95FFFull, 17);
            value ^= *reinterpret_cast<const u64*>(field_buf + 0x98);
            value = detail::rol64(value + 0xBB34B0C01E5AA000ull, 47);
            return detail::rol64(source ^ value, 32);
        }
        inline u64 decode_ffieldclass_name(const decrypt_ffieldclass_name_context& context,
                                           const u8* rax_buf) {
            (void)context._rax;
            (void)context._rbx;
            u16 rotated[4];
            for (unsigned i = 0; i < 4; ++i) {
                const u16 value = *reinterpret_cast<const u16*>(rax_buf + 0x20 + i * sizeof(u16));
                rotated[i] = u16((value << 13) | (value >> 3));
            }
            const unsigned shuffle[4] = {1, 3, 0, 2};
            u64 value = 0;
            for (unsigned i = 0; i < 4; ++i)
                value |= u64(rotated[shuffle[i]]) << (i * 16);
            const u32 low = detail::rol32(u32(value), 10);
            const u32 high = detail::rol32(u32(value >> 32), 10);
            return detail::rol64(u64(low) | (u64(high) << 32), 32);
        }
        inline u64 decode_ffproperty_offset(const u8* property_buf) {
            const u32 encrypted = *reinterpret_cast<const u32*>(property_buf + 0xBC);
            return detail::bswap32(encrypted ^ 0xDE28E18Du);
        }
        // Seed slot lives at mesh_component + 0x7B0; callers that already hold
        // the 8-byte window (Bones::DecryptBoneArray) use the _slot overload
        // instead of reading a full mesh-sized buffer.
        inline u64 decode_bonearray_table_address_slot(const u8* seed_slot) {
            const u32 low = detail::rol32(
                *reinterpret_cast<const u32*>(seed_slot) ^ 0x2B46E100u, 3) +
                0xD4B91F00u;
            const u32 high = detail::rol32(
                *reinterpret_cast<const u32*>(seed_slot + 4) ^ 0xC05F2101u, 3) +
                0x3FA0DEFFu;
            return u64(low) | (u64(high) << 32);
        }
        inline u64 decode_bonearray_table_address(const u8* mesh_component_buf) {
            return decode_bonearray_table_address_slot(mesh_component_buf + 0x7B0);
        }
    }
}
