#pragma once
// FName / player-name / bone decrypt — PlayerName/Bones/GNames namespaces.
// FName: the dumped SDK (sdk/CppSDK/SDK/Basic.hpp) defines this build's pool as
// the vanilla FNamePool — Blocks[0x2000] @ pool+0x40, 2-byte stride, block =
// index >> 16, u16 header (bit 0 wide, bits 6..15 len). That plain walk is the
// primary pipeline; the PCLMULQDQ slot / keystream schemes from the earlier drop
// remain compiled as fallbacks and are gated on the entry-text plausibility check.
// Player-name scramble: CL-1341255 key 0xD351FEEC rol 28, legacy 0xA7A3FF6B rol 19 fallback.

#include "Memory.h"
#include "Offsets.h"
#include "Cache.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <immintrin.h>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <Windows.h>

namespace steam_decrypt {

// ── Helpers ──────────────────────────────────────────────────────────────────

inline bool ValidPtr(uint64_t p)
{
	return p >= 0x10000ULL && p <= 0x7FFFFFFFFFFFULL;
}

inline bool IsUsermodePtr(uint64_t p)
{
	return ValidPtr(p);
}

inline uint32_t rotl32(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

inline uint64_t rotl64(uint64_t x, int n)
{
	return (x << n) | (x >> (64 - n));
}

// Test seam (Pillar 1 / docs/aplus-plan.md): when set, MemRead routes through
// this instead of the DMA backend, so the decode math can run on synthetic
// buffers in Project.Tests. Production never sets it (nullptr = DMA).
inline bool (*g_memReadOverride)(uint64_t addr, void* buf, size_t size) = nullptr;

inline bool MemRead(uint64_t addr, void* buf, size_t size)
{
	if (!addr || !buf || !size)
		return false;
	if (g_memReadOverride)
		return g_memReadOverride(addr, buf, size);
	return Memory::ReadRaw(static_cast<uintptr_t>(addr), buf, size);
}

template <typename T>
inline T MemReadVal(uint64_t addr)
{
	T v{};
	MemRead(addr, &v, sizeof(T));
	return v;
}

} // namespace steam_decrypt

// ── PlayerName namespace ─────────────────────────────────────────────────────
// CL-1341255 (2026-08-18) scramble key: 0xD351FEEC, rol 28.
// Pre-CL-1341255 builds used the legacy key 0xA7A3FF6B, rol 19 — kept as an
// automatic fallback in ReadPlayerNameFromFString for older builds.

namespace PlayerName {

// sdk/sdk.txt PRNG patch history:
//   2026-08-18 → ADD=0xD351FEEC, ROL=28  (CL-1341255, RVA 0x3731030)
//   2026-09-22 → ADD=0xD351FEEC, ROL=14   <-- current build
// The key did not move with today's patch, the rotation did; rol 28 is the
// previous build's scramble and decodes this build's names to ciphertext.
constexpr uint32_t kKeyCurrent = 0xD351FEECu;
constexpr int      kRotCurrent = 14;
constexpr uint32_t kKeyLegacy  = 0xA7A3FF6Bu;
constexpr int      kRotLegacy  = 19;

inline uint32_t RotateLeft32(uint32_t value, int count) {
	return (value << count) | (value >> (32 - count));
}

inline uint8_t RotateCharacter(int character, int low, int high, int amount) {
	return character >= low && character <= high
		? static_cast<uint8_t>(
			(character - low + amount) % (high - low + 1) + low)
		: static_cast<uint8_t>(character);
}

inline void BuildSubstitution(uint8_t (&substitution)[256]) {
	for (int character = 0; character < 256; ++character) {
		uint8_t value = RotateCharacter(character, 33, 126, 47);
		value = RotateCharacter(value, 48, 57, 5);
		value = RotateCharacter(value, 97, 122, 13);
		value = RotateCharacter(value, 65, 90, 13);
		substitution[character] =
			RotateCharacter(value, 33, 126, 47);
	}
}

// Decrypt an FString buffer using an explicit scramble key + rotation amount.
inline void DecryptWithKey(std::vector<uint16_t>& buffer, int maxLength,
	uint32_t key, int rotAmount) {
	if (buffer.empty() || maxLength <= 0)
		return;

	const int length = (std::min)(
		maxLength, static_cast<int>(buffer.size()));
	if (length < 1 || buffer[0] == 0)
		return;

	static uint8_t substitution[256]{};
	static bool initialized = false;
	if (!initialized) {
		BuildSubstitution(substitution);
		initialized = true;
	}

	uint32_t state = 0;
	int i = 0;
	for (; i < length && buffer[i] != 0; ++i) {
		state = 16777619u *
			(state + RotateLeft32(
				16777619u * state + key, rotAmount));
		buffer[i] = substitution[
			(buffer[i] ^ static_cast<int>(state & 0x1Fu)) & 0xFF];
	}

	if (i < static_cast<int>(buffer.size()))
		buffer[i] = 0;
}

// Current build (CL-1341255) key — the caller retries with the legacy key
// via DecryptWithKey(kKeyLegacy, kRotLegacy) when the result isn't plausible.
inline void Decrypt(std::vector<uint16_t>& buffer, int maxLength) {
	DecryptWithKey(buffer, maxLength, kKeyCurrent, kRotCurrent);
}

// ── SDK drop (sdk/sdk.txt, "v922") FString pipeline ─────────────────────────
// The hand-rolled rol-28 variants above are CL-1341255 era. The drop documents
// the current scramble as
//
//   state = (rol32(state * 0x01000193 + 0xD351FEEC, 14) + state) * 0x01000193
//   byte ^= state & 31            (low byte of the wchar only)
//
// followed by a printable-range correction chain, all of which already lives in
// Core/SDK.hpp as game::gasm::decode_fstring_byte. The state advance happens
// BEFORE the XOR there, and the high byte of each wchar is preserved. Every
// in-repo variant rotates by 28 and substitutes instead, which is why names
// came back as ciphertext ("NbIe)xj[,U" in debug-c190fb.log).
inline void DecryptPlayerNameSdk(std::vector<uint16_t>& buffer, int maxLength)
{
	if (buffer.empty() || maxLength <= 0)
		return;
	const int length = (std::min)(maxLength, static_cast<int>(buffer.size()));
	if (length < 1 || buffer[0] == 0)
		return;

	uint32_t state = 0;
	int i = 0;
	for (; i < length && buffer[i] != 0; ++i) {
		const uint8_t decoded = game::gasm::decode_fstring_byte(
			static_cast<uint8_t>(buffer[static_cast<size_t>(i)] & 0xFF), state);
		buffer[static_cast<size_t>(i)] = static_cast<uint16_t>(
			(buffer[static_cast<size_t>(i)] & 0xFF00) | decoded);
	}
	if (i < static_cast<int>(buffer.size()))
		buffer[static_cast<size_t>(i)] = 0;
}

// ── CL-1341255 SIMD name decrypt ────────────────────────────────────────────
// Scramble key 0xD351FEEC/rol28 is stale on this build. New pipeline:
// 16-byte PSHUFB mask read from game base @ RVA 0xAD2FC50 (never hardcode —
// it moves per build), then XOR each qword lane with 0xA738DD8241D227C2.
constexpr uint64_t kSimdXorVal = 0xA738DD8241D227C2ULL;

inline bool GetSimdMask(/*out*/ uint8_t (&mask)[16]) {
	static alignas(16) uint8_t cached[16]{};
	static bool cachedOk = false;
	static std::mutex mtx;
	std::lock_guard<std::mutex> lock(mtx);
	if (!cachedOk) {
		const uint64_t base = Memory::getBaseAddress();
		if (!base)
			return false;
		cachedOk = steam_decrypt::MemRead(
			base + Offsets::PlayerNameSimdMaskRva, cached, sizeof(cached));
		if (cachedOk) {
			bool allZero = true;
			for (int i = 0; i < 16; ++i)
				if (cached[i]) { allZero = false; break; }
			if (allZero)
				cachedOk = false;
		}
	}
	if (!cachedOk)
		return false;
	memcpy(mask, cached, sizeof(mask));
	return true;
}

// SIMD pipeline attempt — caller gates on name plausibility and falls back
// to the legacy scramble keys when this doesn't produce a real name.
inline void DecryptSimd(std::vector<uint16_t>& buffer, int maxLength) {
	if (buffer.empty() || maxLength <= 0)
		return;

	alignas(16) uint8_t mask[16];
	if (!GetSimdMask(mask))
		return;

	const int length = (std::min)(
		maxLength, static_cast<int>(buffer.size()));

	const __m128i shuffleMask = _mm_loadu_si128(reinterpret_cast<const __m128i*>(mask));
	const __m128i xorVal = _mm_set1_epi64x(static_cast<long long>(kSimdXorVal));

	int i = 0;
	for (; i + 8 <= length; i += 8) {
		__m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&buffer[static_cast<size_t>(i)]));
		v = _mm_shuffle_epi8(v, shuffleMask);
		v = _mm_xor_si128(v, xorVal);
		_mm_storeu_si128(reinterpret_cast<__m128i*>(&buffer[static_cast<size_t>(i)]), v);
	}
	for (; i < length && buffer[static_cast<size_t>(i)] != 0; ++i) {
		// tail (< 8 wchars): byte-wise shuffle equivalent is identity for the
		// low lanes the mask keeps in place — apply XOR only.
		buffer[static_cast<size_t>(i)] = static_cast<uint16_t>(
			buffer[static_cast<size_t>(i)] ^ static_cast<uint16_t>(kSimdXorVal & 0xFFFFu));
	}
}

} // namespace PlayerName

// ── PlayerLink: APlayerController → ULocalPlayer (2026-09-22 drop) ──────────
// RE source: APlayerController::GetLocalPlayer (sub_3680940). This build keeps
// the local player at controller+0x4B0 *encrypted*, which is exactly why the
// back-pointer scan (look for a slot whose +0xA0 points at the PC) has never
// resolved a LocalPlayer on it — the slot is not a plain ULocalPlayer*.
//
// Static SIMD path (no TLS, works on any controller pointer):
//   enc     = read64(controller + 0x4B0)
//   rot     = ROL32 each 32-bit lane by 13
//   blended = rot ^ 0x9A492C85DDF6F193   (the xmm AND / ANDNOT pair is a
//                                         bit-complement blend, so one XOR)
//   ULocalPlayer* = ROL64(blended, 39)
//
// The drop also carries a TEB variant (XOR with TEB+0x1F8, ROR16 x1, PSHUFB
// {6,3,1,7,0,2,4,5}, ROR64 x3) used whenever a TEB key is available. An
// external DMA reader usually does not have one, so the static path is primary
// here and the TEB key is honoured only when something calls SetTebKey.
namespace PlayerLink {

inline std::atomic<uint64_t> g_tebKey{0};

inline void SetTebKey(uint64_t key) { g_tebKey.store(key, std::memory_order_relaxed); }
inline uint64_t TebKey() { return g_tebKey.load(std::memory_order_relaxed); }
inline void ClearTebKey() { SetTebKey(0); }

inline uint64_t DecryptTeb(uint64_t enc, uint64_t tebKey) {
	if (enc == 0 || tebKey == 0)
		return 0;

	uint64_t x = enc ^ tebKey;
	uint16_t words[4];
	std::memcpy(words, &x, 8);
	for (int i = 0; i < 4; ++i)
		words[i] = static_cast<uint16_t>(
			(words[i] >> Offsets::TebWordRor) |
			(words[i] << (16 - Offsets::TebWordRor)));

	const uint8_t mask[8] = { 6, 3, 1, 7, 0, 2, 4, 5 };
	uint8_t bytes[8];
	std::memcpy(bytes, words, 8);
	uint8_t shuffled[8];
	for (int i = 0; i < 8; ++i)
		shuffled[i] = bytes[mask[i]];

	uint64_t s;
	std::memcpy(&s, shuffled, 8);
	const uint64_t result =
		(s >> Offsets::TebQwordRor) | (s << (64 - Offsets::TebQwordRor));
	return steam_decrypt::ValidPtr(result) ? result : 0;
}

/**
 * Decrypt controller+0x4B0 into the controller's ULocalPlayer.
 * Returns 0 when the slot is empty or the decrypt lands outside a usable range.
 * Callers still validate identity (LocalPlayer::PlayerController @ 0xA0 back-ref).
 */
/**
 * Pure maths half of the drop's static pipeline: encrypted qword -> pointer.
 * Split out from the memory read so the transform is unit-testable on synthetic
 * values (the read itself goes through the uncached DMA path, which the test
 * harness deliberately does not intercept).
 */
inline uint64_t DecryptStatic(uint64_t enc) {
	if (enc == 0)
		return 0;

	const uint32_t lo = static_cast<uint32_t>(enc);
	const uint32_t hi = static_cast<uint32_t>(enc >> 32);
	const uint64_t rot =
		static_cast<uint64_t>(steam_decrypt::rotl32(lo, Offsets::PlayerDecrypt_LaneRot)) |
		(static_cast<uint64_t>(steam_decrypt::rotl32(hi, Offsets::PlayerDecrypt_LaneRot)) << 32);
	const uint64_t blended = rot ^ Offsets::PlayerDecrypt_BlendXorMask;
	if (blended == 0)
		return 0;

	const uint64_t result = steam_decrypt::rotl64(blended, Offsets::PlayerDecrypt_FinalRot);
	return steam_decrypt::ValidPtr(result) ? result : 0;
}

inline uint64_t FromController(uint64_t controller) {
	if (!steam_decrypt::ValidPtr(controller))
		return 0;

	// Uncached on purpose: a cached read froze stale pointers elsewhere in the
	// chain, and this slot changes on every controller swap.
	const uint64_t enc = Memory::read_nocache<uint64_t>(
		controller + Offsets::PlayerDecrypt_LocalPlayerOffset);
	if (enc == 0)
		return 0;

	if (const uint64_t tebKey = TebKey(); tebKey != 0)
		return DecryptTeb(enc, tebKey);
	return DecryptStatic(enc);
}

} // namespace PlayerLink

// ── GameInstanceStaticDecrypt: exact SDK drop path ───────────────────────────
// sdk/sdk.txt: decrypt_game_instance_static (sub_145046B00). This is distinct
// from the retired world+slot XOR path: it derives two stage blocks from the
// module-wide stage array and dereferences result+0x18 for UGameInstance.
namespace GameInstanceLink {

inline uint64_t PshufbLo8(uint64_t lo, uint64_t hi, uint64_t mask)
{
	uint8_t src[16]{};
	uint8_t m[8]{};
	uint8_t out[8]{};
	std::memcpy(src, &lo, 8);
	std::memcpy(src + 8, &hi, 8);
	std::memcpy(m, &mask, 8);
	for (int i = 0; i < 8; ++i)
		out[i] = (m[i] & 0x80u) ? 0 : src[m[i] & 0x0Fu];
	uint64_t result = 0;
	std::memcpy(&result, out, sizeof(result));
	return result;
}

inline uint64_t DecryptStatic(uint64_t moduleBase)
{
	if (!moduleBase)
		return 0;
	const uint64_t stage = moduleBase + Offsets::GameInstanceStaticStageArrayRva;
	const uint64_t maskAddr = moduleBase + Offsets::GameInstanceStaticPshufbMaskRva;
	const uint64_t seedLo = steam_decrypt::MemReadVal<uint64_t>(stage);
	const uint64_t mask = steam_decrypt::MemReadVal<uint64_t>(maskAddr);
	if (!seedLo || !mask)
		return 0;

	constexpr uint32_t P = 0x01000193u;
	constexpr uint32_t K1 = 0x742217C8u;
	constexpr uint32_t K2 = 0x0005E838u;
	constexpr uint32_t NEG109 = 0xFFFFFF93u;
	uint32_t h1 = P * steam_decrypt::rotl32(static_cast<uint32_t>(seedLo), 21) - K1;
	uint32_t h2 = P * steam_decrypt::rotl32(h1, 17) +
		static_cast<uint32_t>(seedLo >> 32) - K1;
	uint32_t h3 = P * steam_decrypt::rotl32(h2, 21) - K1;
	const uint32_t v0 = steam_decrypt::rotl32(h3, 17);
	const uint32_t v1 = (NEG109 * v0) ^ ((P * v0 + K2) >> 16);
	const uint32_t slotA = v1 & 7u;
	const uint32_t slotB = (v1 + 1u) & 7u;
	const uint64_t blockA = stage + (2ull * slotA + 1ull) * 16ull;
	const uint64_t blockB = stage + (2ull * slotB + 1ull) * 16ull;
	const uint64_t aLo = steam_decrypt::MemReadVal<uint64_t>(blockA);
	const uint64_t aHi = steam_decrypt::MemReadVal<uint64_t>(blockA + 8);
	const uint64_t bLo = steam_decrypt::MemReadVal<uint64_t>(blockB);
	const uint64_t bHi = steam_decrypt::MemReadVal<uint64_t>(blockB + 8);
	const uint64_t v3 = PshufbLo8(aLo, aHi, mask) ^ Offsets::GameInstanceStaticXorMask;
	const uint64_t vb = PshufbLo8(bLo, bHi, mask) ^ Offsets::GameInstanceStaticXorMask;
	constexpr uint64_t FNV64 = 0x100000001B3ull;
	uint64_t inner = FNV64 * steam_decrypt::rotl64(v3, 56) + Offsets::GameInstanceStaticAdd;
	const uint64_t outer = FNV64 * steam_decrypt::rotl64(inner, 33) + Offsets::GameInstanceStaticAdd;
	const uint64_t seedPtr = (outer ^ vb) + v3;
	const uint64_t result = steam_decrypt::MemReadVal<uint64_t>(
		seedPtr + Offsets::GameInstanceStaticResultDeref);
	return steam_decrypt::ValidPtr(result) ? result : 0;
}

} // namespace GameInstanceLink

// ── Bones namespace (CL-1341255 v818 pipeline) ─────────────────────────────
// Theia-style SIMD pointer decrypt, bones are plaintext component-space transforms.
// CL-1341255 v818: seed @ mesh+0x7B0, LOD @ mesh+0x7F8, descriptor @ bone+0x48,
// stride 0x60, ROL64=50, ROL32=22, XOR key removed (0), PSHUFB mask below.
// Same pipeline on all maps — the CTW block (Offsets::ComponentToWorld) is what
// makes it work off-Stella.

namespace Bones {

constexpr uint64_t SeedOffset       = 0x7B0;   // CL-1341255 (was 0x790)
constexpr uint64_t SeedOffsetLegacy = 0x790;
constexpr uint64_t LodOffset        = 0x7D0;   // CL-1341255 (esasiolan + texaftertex confirm)
constexpr uint64_t LodOffsetLegacy  = 0x7F8;   // alternate slot
constexpr uint64_t DescriptorOffset = 0x48;    // CL-1341255 (was 0xB0)
constexpr uint64_t BoneStride       = 0x60;
constexpr uint32_t LodShiftRight    = 27;
constexpr uint32_t LodBitMask       = 0xFFFFFFF0;
constexpr uint64_t XorKey           = 0;       // CL-1341255: XOR key removed
constexpr int      Rol32Amount      = 22;      // CL-1341255 (was 5)
constexpr int      Rol64Amount      = 50;      // CL-1341255 (new step)
constexpr int      MaxBoneCount     = 500;

inline uint64_t DecryptBoneArrayPointer(const uint8_t seed[16]) {
	// Pure function lives in BoneMath.hpp (XorKey is 0 on this build, so the
	// legacy xor step is a no-op and was dropped there).
	return BoneMath::DecryptBoneArrayPointer(seed);
}

struct BoneArrayResult {
	uint64_t Array = 0;
	int32_t  Count = 0;
};

/**
 * Decrypt bone array pointer from SkeletalMeshComponent.
 * Returns plaintext FTransform bones (no per-bone decrypt).
 *   +0x00 Rotation quat  (4x double: X, Y, Z, W)
 *   +0x20 Translation    (3x double: X, Y, Z)
 *   +0x38 padding        (1x double)
 *   +0x40 Scale3D        (3x double: X, Y, Z)
 */
inline BoneArrayResult DecryptBoneArray(
	uint64_t mesh)
{
	BoneArrayResult result{};
	if (!steam_decrypt::ValidPtr(mesh))
		return result;

	// SDK drop: decode_bonearray_table_address reads the dword pair at
	// mesh+0x7B0/+0x7B4. The v818 PSHUFB pipeline stays as the fallback.
	alignas(16) uint8_t seed[0x20]{};
	if (!steam_decrypt::MemRead(mesh + SeedOffset, seed, sizeof(seed)))
		return result;

	uint64_t base = game::gasm::decode_bonearray_table_address_slot(seed);
	if (!steam_decrypt::ValidPtr(base))
		base = DecryptBoneArrayPointer(seed);   // legacy PSHUFB pipeline
	if (!steam_decrypt::ValidPtr(base))
		return result;

	// The {array,count} descriptor has moved between CL revisions while the seed
	// slot stayed at mesh+0x7B0:
	//   * CL-1341255: index = (lod32 @ mesh+0x7D0 >> 27) & 0xFFFFFFF0,
	//     descriptor = base + index + 0x48;
	//   * 2026-09-22 drop (sdk/sdk.txt, CL-1389382 "v922"): selector =
	//     (dword @ mesh+0x848 >> 15) & 1, descriptor = base + 0x18 + 0x10 * sel.
	// Try both and keep whichever yields a plausible array + count, so a bone
	// layout change can't silently zero the skeleton.
	const uint32_t lodDword =
		steam_decrypt::MemReadVal<uint32_t>(mesh + LodOffset);
	const uint32_t lodIndex =
		(lodDword >> LodShiftRight) & LodBitMask;
	const uint32_t selDword =
		steam_decrypt::MemReadVal<uint32_t>(mesh + Offsets::BoneV922SelectorOffset);
	const uint32_t selector =
		(selDword >> Offsets::BoneV922SelectorShift) & Offsets::BoneV922SelectorMask;

	const uint64_t descriptors[] = {
		base + lodIndex + DescriptorOffset,
		base + Offsets::BoneV922DescriptorBase +
			Offsets::BoneV922DescriptorStride * selector,
	};
	for (const uint64_t descriptor : descriptors) {
		if (!steam_decrypt::ValidPtr(descriptor))
			continue;

		const uint64_t boneArray =
			steam_decrypt::MemReadVal<uint64_t>(descriptor);
		if (!steam_decrypt::ValidPtr(boneArray))
			continue;

		const int32_t count =
			steam_decrypt::MemReadVal<int32_t>(descriptor + 8);
		if (count <= 0 || count > MaxBoneCount)
			continue;

		result.Array = boneArray;
		result.Count = count;
		return result;
	}

	return result;
}

// Forum-verified decrypt (UC post, tested in-game on BP_PioneerCharacter_C:
// count=97, sensible translations). Key 0xA738DD8241D227C2 (both lanes),
// ROL64 by 0x26, pshuflw 0x39, LOD at mesh+lodOff, idx = (lod>>11)&0x10,
// count @ base+idx+0x98, array @ base+idx+0x90. Seed/LOD slots differ per
// build (+0x10/+0x20 shifts observed) — the caller tries every combo and the
// score gate keeps whichever yields a real skeleton.
inline uint64_t DecryptBoneArrayForum(
	uint64_t mesh, uint64_t seedOff, uint64_t lodOff)
{
	if (!steam_decrypt::ValidPtr(mesh))
		return 0;

	alignas(16) uint8_t seed[16]{};
	if (!steam_decrypt::MemRead(mesh + seedOff, seed, sizeof(seed)))
		return 0;
	const uint64_t encLo = steam_decrypt::MemReadVal<uint64_t>(mesh + seedOff);
	const uint64_t encHi = steam_decrypt::MemReadVal<uint64_t>(mesh + seedOff + 8);
	if (!encLo && !encHi)
		return 0;

	constexpr uint64_t kKey = 0xA738DD8241D227C2ULL;
	const __m128i v = _mm_or_si128(
		_mm_slli_epi64(
			_mm_xor_si128(
				_mm_loadu_si128(reinterpret_cast<const __m128i*>(seed)),
				_mm_set1_epi64x(static_cast<long long>(kKey))),
			0x26),
		_mm_srli_epi64(
			_mm_xor_si128(
				_mm_loadu_si128(reinterpret_cast<const __m128i*>(seed)),
				_mm_set1_epi64x(static_cast<long long>(kKey))),
			0x1A));
	const __m128i shuffled = _mm_shufflelo_epi16(v, 0x39);
	const uint64_t base = static_cast<uint64_t>(
		_mm_cvtsi128_si64(shuffled));
	if (!steam_decrypt::ValidPtr(base))
		return 0;

	const uint32_t lod = steam_decrypt::MemReadVal<uint32_t>(mesh + lodOff);
	const uint32_t idx = (lod >> 11) & 0x10u;

	const uint32_t count = steam_decrypt::MemReadVal<uint32_t>(base + idx + 0x98);
	if (count <= 0 || count > MaxBoneCount)
		return 0;

	const uint64_t arr = steam_decrypt::MemReadVal<uint64_t>(base + idx + 0x90);
	if (!steam_decrypt::ValidPtr(arr))
		return 0;
	return arr;
}

} // namespace Bones

// ── GNames namespace ─────────────────────────────────────────────────────────
// PRIMARY: the plain SDK pool of THIS build (sdk/CppSDK/SDK/Basic.hpp) — see
// ResolveNamePointerPlain / DecodeStringPlain below; no key material involved.
// FALLBACKS (earlier drops), tried only when the plain walk yields nothing:
// The retired SDK drop (Core/SDK.hpp):
//   pool @ game::offsets::GNAMES, u16[64] key table @ game::offsets::KEYTABLE
//   chunk window: selector seed @ +0x40, eight 0x20-byte blocks @ +0x50
//   chunk base   -> game::gasm::decode_fname_pool_address
//   entry string -> game::gasm::decode_fname
//   header: len = ((hdr >> 3) & 0x3F8) + (hdr >> 13), bIsWide = hdr & 1
// The v20260818 PCLMULQDQ/keystream/shard scheme is retained as the fallback
// (*Legacy functions below) so builds that never picked up the SDK change keep
// resolving names:
//   Slot hash:   ROL32(0x19/0x0E/0x19/0x0E)*P + ADD 0xD4C2DB3A, Hi folded step 2
//   Slot decrypt: 16B (Lo,Hi); T=Hi^clmul(K1,Lo); V=clmul(K2,T)^Lo; ROL64(V,32)
//   Shard hash (hashes ADDRESS): ROL32(0x17/0x15/0x17)*P+ADD 0x30091BB7
//   Block decode: ROL64(4)^0xF31D220392B6800B, per-dword ROL32(2)
//   FNV:         FNV64_PRIME*ROL64(V,48/46)+ADD 0x6463CD794F959557; ptr chain NOP
//   Header:      len=hdr&0x03FF, wide=hdr&0x8000
//   String:      keystream[144] base=80, KEY_INIT=0xD917+len, +1/element, narrow ^= key>>3

namespace GNames {

// ── SDK pipeline constants ───────────────────────────────────────────────────
constexpr uint64_t NamesOffset = Offsets::GNamePoolRva;      // FName pool (SDK GNAMES)

// SDK container layout (sdk/CppSDK/SDK/Basic.hpp). THIS build ships the vanilla
// FNamePool: Blocks[0x2000] at +0x40 (CurrentBlock 0x38, CurrentByteCursor
// 0x3C), a 2-byte entry stride, and block = index >> 16. The dump's own header
// defines no key table, no keystream and no shard slots, so the plain walk
// below decodes names with no key material at all.
constexpr uint64_t PoolBlocksOff = static_cast<uint64_t>(Offsets::FNamePool_Blocks);
constexpr uint64_t EntryStride   = static_cast<uint64_t>(Offsets::FNamePool_EntryStride);
constexpr int      PoolBlockBits = static_cast<int>(Offsets::FNamePool_BlockOffsetBits);
constexpr uint64_t EntryTextOff  = static_cast<uint64_t>(Offsets::FNameEntry_TextOffset);
constexpr uint64_t KeyTableRva = Offsets::FNameKeyTableRva;  // u16[64] key table (SDK KEYTABLE)
constexpr uint64_t KeyTableWindow = 0x18 + 64 * 2;         // decode_fname reads u16[64] at +0x18
constexpr uint64_t ChunkWindowSize = 0x50 + 8 * 0x20;      // seed @ +0x40, slots @ +0x50 (stride 0x20)

// ── Legacy (v20260818) fallback constants ───────────────────────────────────
constexpr uint64_t KeystreamRvaLegacy = Offsets::FNameKeystreamLegacyRva;
constexpr int      KeystreamBase = 80;
constexpr int      KeystreamCount = 144;

constexpr uint32_t HashPrime32 = 0x01000193;
constexpr uint32_t SlotHashAdd = 0xD4C2DB3A;  // UOBJ_SLOT_HASH_ADD (was 0x21B21773)
constexpr int      SlotRolA = 0x19;
constexpr int      SlotRolB = 0x0E;
constexpr int      SlotRolC = 0x19;
constexpr int      SlotRolD = 0x0E;
constexpr uint32_t NameSlotXor = 2u;
constexpr uint32_t ClassSlotAdj = 0u;
constexpr uint32_t OuterSlotAdj = 1u;
constexpr uint64_t SlotClmulK1 = 0x0B6641A64F1B214DULL;
constexpr uint64_t SlotClmulK2 = 0x8FA21A13D9179A47ULL;
constexpr int      SlotRol64Final = 32;

constexpr uint32_t ShardHashAdd = 0x30091BB7u;
constexpr uint64_t ShardSeedOff = 0x6FD0ULL;
constexpr uint64_t ShardBlockBaseOff = 0x6FE0ULL;
constexpr uint64_t ShardBlockStride = 32ULL;
constexpr int      ShardRolA = 0x17;
constexpr int      ShardRolB = 0x15;
constexpr int      ShardRolC = 0x17;
constexpr int      ShardShrD = 0x0B;

constexpr int      BlockRol64 = 4;
constexpr int      BlockRol32 = 2;
constexpr uint64_t BlockXor = 0xF31D220392B6800BULL;

constexpr uint64_t FnvPrime = 0x100000001B3ULL;
constexpr uint64_t FnvAdd = 0x6463CD794F959557ULL;
constexpr int      FnvRol1 = 0x30;   // 48
constexpr int      FnvRol2 = 0x2E;   // 46

constexpr uint16_t HdrWideBit = 0x8000u;
constexpr uint16_t HdrLengthMask = 0x03FFu;
constexpr uint32_t KeyInitAdd = 0xD917u;
constexpr uint32_t KeyAdvance = 1u;
constexpr uint32_t KeyIndexMask = 0x3Fu;
constexpr int      NarrowKeyShift = 3;

// SDK key table window — raw bytes handed straight to game::gasm::decode_fname.
inline uint8_t keyTableRaw[KeyTableWindow]{};
// Legacy keystream table.
inline uint16_t keyTable[256]{};
inline bool ready = false;        // FName pool reachable (SDK plain layout)
inline bool keyTableReady = false; // SDK-drop key table loaded (slot path only)
inline bool legacyReady = false;  // legacy keystream loaded
// Runtime pool override; defaults to the compile-time name-pool RVA.
inline uint64_t gRuntimePoolRva = NamesOffset;

inline uint32_t RotateLeft32(uint32_t value, int count) {
	return (value << count) | (value >> (32 - count));
}

inline uint64_t RotateLeft64(uint64_t value, int count) {
	return (value << count) | (value >> (64 - count));
}

// Carry-less multiply (low 64 bits) — delegates to the SDK's primitive.
inline uint64_t ClmulLo(uint64_t X, uint64_t Y) {
	return game::detail::clmul64_low(X, Y);
}

// Key table view decode_fname expects: u16[64] starting at +0x18.
inline const u16* KeyTableBase() {
	return reinterpret_cast<const u16*>(keyTableRaw);
}

inline void Reset() {
	std::memset(keyTableRaw, 0, sizeof(keyTableRaw));
	std::memset(keyTable, 0, sizeof(keyTable));
	gRuntimePoolRva = NamesOffset;
	ready = false;
	keyTableReady = false;
	legacyReady = false;
}

// SDK init: load the FName key table (game::offsets::KEYTABLE). The legacy
// keystream is loaded best-effort — it only feeds the fallback path.
inline bool Init(uint64_t moduleBase)
{
	if (ready)
		return true;

	if (!moduleBase)
		return false;

	// Primary gate: the FName pool itself. This build ships the vanilla pool
	// (sdk/CppSDK/SDK/Basic.hpp), so names decode with no key material; the key
	// table below only feeds the retired slot / keystream paths.
	const uint64_t PoolBlock0 = steam_decrypt::MemReadVal<uint64_t>(
		moduleBase + gRuntimePoolRva + PoolBlocksOff);
	ready = steam_decrypt::ValidPtr(PoolBlock0);

	keyTableReady = steam_decrypt::MemRead(moduleBase + KeyTableRva, keyTableRaw, sizeof(keyTableRaw));

	int nz = 0;
	for (int I = 0; I < 64; ++I) {
		u16 Entry = 0;
		std::memcpy(&Entry, keyTableRaw + 0x18 + I * 2, 2);
		nz += (Entry != 0);
	}
	keyTableReady = keyTableReady && nz >= 8;
	if (!keyTableReady)
		std::memset(keyTableRaw, 0, sizeof(keyTableRaw));

	uint8_t KsBuf[KeystreamCount * 2]{};
	if (steam_decrypt::MemRead(moduleBase + KeystreamRvaLegacy, KsBuf, sizeof(KsBuf))) {
		int KsNz = 0;
		for (int I = 0; I < KeystreamCount; ++I) {
			std::memcpy(&keyTable[I], KsBuf + I * 2, 2);
			KsNz += (keyTable[I] != 0);
		}
		legacyReady = KsNz >= 8;
	}

	return true;
}

// ── UObject slot hash + slot selection ───────────────────────────────────────
inline uint32_t SlotHash(uint64_t ObjPtr) {
	return game::detail::uobject_selector(ObjPtr);
}

inline uint32_t NameSlot(uint64_t ObjPtr)  { return (SlotHash(ObjPtr) & 3u) ^ NameSlotXor; }
inline uint32_t ClassSlot(uint64_t ObjPtr) { return (SlotHash(ObjPtr) & 3u) ^ ClassSlotAdj; }
inline uint32_t OuterSlot(uint64_t ObjPtr) { return (SlotHash(ObjPtr) & 3u) ^ OuterSlotAdj; }

// Legacy selector — used only by the *Legacy fallback paths.
inline uint32_t SlotHashLegacy(uint64_t ObjPtr) {
	const uint64_t Seed = ObjPtr + 0x10;
	const uint32_t Lo = static_cast<uint32_t>(Seed);
	const uint32_t Hi = static_cast<uint32_t>(Seed >> 32);
	uint32_t H = HashPrime32 * RotateLeft32(Lo, SlotRolA) + SlotHashAdd;
	H = HashPrime32 * RotateLeft32(H, SlotRolB) + Hi + SlotHashAdd;
	H = HashPrime32 * RotateLeft32(H, SlotRolC) + SlotHashAdd;
	H = HashPrime32 * RotateLeft32(H, SlotRolD) + SlotHashAdd;
	return H ^ (H >> 16);
}

inline uint32_t NameSlotLegacy(uint64_t ObjPtr)  { return (SlotHashLegacy(ObjPtr) & 3u) ^ NameSlotXor; }
inline uint32_t ClassSlotLegacy(uint64_t ObjPtr) { return (SlotHashLegacy(ObjPtr) & 3u) ^ ClassSlotAdj; }

// SDK slot decrypt (game::detail::decode_uobject_slot). The name slot
// (nameprivate) applies ROL64(32) afterwards; the class slot does not.
inline uint64_t DecodeSlot16(uint64_t Lo, uint64_t Hi) {
	uint8_t Buf[16]{};
	std::memcpy(Buf, &Lo, sizeof(Lo));
	std::memcpy(Buf + 8, &Hi, sizeof(Hi));
	return game::detail::rol64(game::detail::decode_uobject_slot(Buf), 32);
}

inline uint64_t DecodeClassSlot16(uint64_t Lo, uint64_t Hi) {
	uint8_t Buf[16]{};
	std::memcpy(Buf, &Lo, sizeof(Lo));
	std::memcpy(Buf + 8, &Hi, sizeof(Hi));
	return game::detail::decode_uobject_slot(Buf);
}

// Legacy v20260818 slot decrypt: T=Hi^clmul(K1,Lo); V=clmul(K2,T)^Lo; ROL64(V,32).
inline uint64_t DecodeSlot16Legacy(uint64_t Lo, uint64_t Hi) {
	const uint64_t T = Hi ^ ClmulLo(SlotClmulK1, Lo);
	const uint64_t V = ClmulLo(SlotClmulK2, T) ^ Lo;
	return RotateLeft64(V, SlotRol64Final);
}

// SDK slot reads: one window covers the four 0x20-byte blocks at ObjBase+0x20.
constexpr uint64_t SlotWindowSize = 0x20 + 4 * 0x20;

inline bool ReadSlotWindow(uint64_t ObjBase, uint8_t (&Buf)[SlotWindowSize]) {
	return steam_decrypt::MemRead(ObjBase, Buf, sizeof(Buf));
}

inline uint64_t ReadNameSlot16(uint64_t ObjBase) {
	uint8_t Buf[SlotWindowSize]{};
	if (!ReadSlotWindow(ObjBase, Buf))
		return 0;
	return game::gasm::decode_uobject_nameprivate(ObjBase, Buf);
}

inline uint64_t ReadClassSlot16(uint64_t ObjBase) {
	uint8_t Buf[SlotWindowSize]{};
	if (!ReadSlotWindow(ObjBase, Buf))
		return 0;
	return game::gasm::decode_uobject_classprivate(ObjBase, Buf);
}

// Legacy v20260818 slot read.
inline uint64_t ReadSlot16Decoded(uint64_t ObjBase, uint32_t Slot) {
	const uint64_t Addr = ObjBase + 0x20 + static_cast<uint64_t>(Slot) * 0x20;
	uint8_t Raw[16]{};
	if (!steam_decrypt::MemRead(Addr, Raw, sizeof(Raw)))
		return 0;
	uint64_t Lo = 0, Hi = 0;
	std::memcpy(&Lo, Raw + 0, 8);
	std::memcpy(&Hi, Raw + 8, 8);
	if (!Lo && !Hi)
		return 0;
	return DecodeSlot16Legacy(Lo, Hi);
}

} // namespace GNames (resumes after OuterLink: the outer chain is not FName machinery)

// ── UObject outer chain ──────────────────────────────────────────────────────
// Two schemes put an object's Outer behind the four 0x20-byte slots at
// obj+0x20, and this build keeps both:
//
//   1. the v20260922 slot scheme the SDK's own decoders use for ClassPrivate /
//      NamePrivate (game::gasm): slot = (uobject_selector(obj) ^ adj) & 3, then
//      detail::decode_uobject_slot over the slot's 16 bytes. The SDK drop has no
//      outer decoder, so the name slot's extra ROL64(32) is tried as well.
//   2. the CL-1299607 cipher (drop OuterDecrypt, sub_1439B00C0): hash (obj+0x10)
//      -> slot ((h ^ (h >> 16)) & 3) ^ 2, read the qword there, ROL32 the lanes by
//      13, XOR 0x9A492C85DDF6F193, ROL64 by 39 - the same shape as the
//      PlayerController->ULocalPlayer decrypt, with its own constants.
//
// No static file can say which one is live, so every rung is tried in order and
// the first candidate that passes the caller's predicate wins. A bare range
// check is never enough: `ok` is what turns a candidate into a pointer, and the
// callers pass their own test (== the actor, ValidateGameInstance,
// ResolvePersistentLevelHelp), which is also what makes a wrong rung harmless.
namespace OuterLink {

enum class Rung : uint8_t {
	None = 0,
	SdkSlot,      // v20260922 slot, detail::decode_uobject_slot
	SdkSlotRol,   // the same, with the name slot's ROL64(32)
	DropCipher,   // CL-1299607 OuterDecrypt
	PlainOuter,   // UObject::OuterPrivate (0xA0) read straight
	PlainSlot,    // obj+0x20 read straight (pre-encryption layout)
};

inline const char* RungName(Rung rung)
{
	switch (rung) {
	case Rung::SdkSlot:    return "sdkSlot";
	case Rung::SdkSlotRol: return "sdkSlotRol";
	case Rung::DropCipher: return "drop";
	case Rung::PlainOuter: return "plainA0";
	case Rung::PlainSlot:  return "plain20";
	default:               return "none";
	}
}

struct Hop {
	uint64_t ptr = 0;
	Rung rung = Rung::None;
};

/** What every rung's candidate has to pass before it is called a pointer. */
inline bool Plausible(uint64_t p)
{
	return steam_decrypt::ValidPtr(p) && (p & 0x3u) == 0;
}

/** Live slot read: uncached, because these slots change with every outer walk. */
inline uint64_t NoCacheReader(uint64_t addr)
{
	return Memory::read_nocache<uint64_t>(addr);
}

inline uint32_t Ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t Ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

// ── pure maths (no memory reads; the suite drives these on synthetic values) ─

/** The drop's outer_hash(obj + 0x10). */
inline uint32_t Hash(uint64_t obj)
{
	const uint64_t seed =
		obj + static_cast<uint64_t>(Offsets::OuterDecrypt_HashSeedOff);
	const uint32_t lo = static_cast<uint32_t>(seed);
	const uint32_t hi = static_cast<uint32_t>(seed >> 32);
	const uint32_t prime = Offsets::OuterDecrypt_HashPrime;
	const uint32_t add = Offsets::OuterDecrypt_HashAdd;
	const int rot1 = static_cast<int>(Offsets::OuterDecrypt_HashRot1);
	const int rot2 = static_cast<int>(Offsets::OuterDecrypt_HashRot2);

	uint32_t h = prime * steam_decrypt::rotl32(lo, rot1) + add;
	h = prime * steam_decrypt::rotl32(h, rot2) + hi + add;
	h = prime * steam_decrypt::rotl32(h, rot1) + add;
	h = prime * steam_decrypt::rotl32(h, rot2) + add;
	return h;
}

/** Which of the four slots the drop's cipher puts the outer in. */
inline uint32_t DropSlot(uint64_t obj)
{
	const uint32_t h = Hash(obj);
	const uint32_t shifted = h >> Offsets::OuterDecrypt_HashShr;
	return ((h ^ shifted) & Offsets::OuterDecrypt_SlotMask) ^ Offsets::OuterDecrypt_SlotXor;
}

/** Encrypted qword -> outer pointer. Split from the read so it is testable. */
inline uint64_t DecryptSlot(uint64_t enc)
{
	if (enc == 0)
		return 0;

	const int laneRot = static_cast<int>(Offsets::OuterDecrypt_LaneRot);
	const uint64_t rot =
		static_cast<uint64_t>(steam_decrypt::rotl32(static_cast<uint32_t>(enc), laneRot)) |
		(static_cast<uint64_t>(steam_decrypt::rotl32(
			static_cast<uint32_t>(enc >> 32), laneRot)) << 32);
	const uint64_t blended = rot ^ Offsets::OuterDecrypt_XorMask;
	if (blended == 0)
		return 0;

	const uint64_t result = steam_decrypt::rotl64(
		blended, static_cast<int>(Offsets::OuterDecrypt_FinalRot));
	return Plausible(result) ? result : 0;
}

/** Inverse of DecryptSlot — the ciphertext a slot holds for a pointer. */
inline uint64_t EncryptSlot(uint64_t ptr)
{
	const int laneRot = static_cast<int>(Offsets::OuterDecrypt_LaneRot);
	const uint64_t r = Ror64(ptr, static_cast<int>(Offsets::OuterDecrypt_FinalRot))
		^ Offsets::OuterDecrypt_XorMask;
	const uint32_t lo = Ror32(static_cast<uint32_t>(r), laneRot);
	const uint32_t hi = Ror32(static_cast<uint32_t>(r >> 32), laneRot);
	return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
}

// ── the ladder ──────────────────────────────────────────────────────────────

template <typename ReadFn>
inline bool ReadSlot16(ReadFn read, uint64_t addr, uint8_t (&out)[16])
{
	const uint64_t lo = read(addr);
	const uint64_t hi = read(addr + 8);
	if (!lo && !hi)
		return false;
	std::memcpy(out, &lo, sizeof(lo));
	std::memcpy(out + 8, &hi, sizeof(hi));
	return true;
}

/**
 * One hop up the outer chain. `read(addr) -> uint64_t` is the only memory
 * access, so the live path passes NoCacheReader and the suite passes its fake.
 */
template <typename ReadFn, typename OkFn>
inline Hop FromObject(uint64_t obj, ReadFn read, OkFn ok)
{
	Hop hop{};
	if (!steam_decrypt::ValidPtr(obj))
		return hop;

	const uint64_t slotBase =
		obj + static_cast<uint64_t>(Offsets::OuterDecrypt_SlotBaseOff);
	const uint64_t stride =
		static_cast<uint64_t>(Offsets::OuterDecrypt_SlotStride);

	// 1. the slot the SDK's own name/class decoders would use for the outer
	alignas(16) uint8_t slot[16]{};
	if (ReadSlot16(read, slotBase + GNames::OuterSlot(obj) * stride, slot)) {
		const uint64_t decoded = game::detail::decode_uobject_slot(slot);
		if (ok(decoded))
			return Hop{ decoded, Rung::SdkSlot };
		const uint64_t roled = steam_decrypt::rotl64(decoded, 32);
		if (ok(roled))
			return Hop{ roled, Rung::SdkSlotRol };
	}

	// 2. the CL-1299607 cipher
	const uint64_t decrypted = DecryptSlot(read(slotBase + DropSlot(obj) * stride));
	if (ok(decrypted))
		return Hop{ decrypted, Rung::DropCipher };

	// 3. the plain fields last: the dump reflects UObject::OuterPrivate at 0xA0,
	//    and the pre-encryption layout had the outer at +0x20
	const uint64_t outer =
		read(obj + static_cast<uint64_t>(Offsets::UObject_OuterPrivate));
	if (ok(outer))
		return Hop{ outer, Rung::PlainOuter };
	const uint64_t first = read(slotBase);
	if (ok(first))
		return Hop{ first, Rung::PlainSlot };
	return hop;
}

/** Live hop: uncached reads, any plausible object accepted. */
inline Hop FromObject(uint64_t obj)
{
	return FromObject(obj, &NoCacheReader, &Plausible);
}

/**
 * UObject::GetOuter with the caller's own validation — a rung only counts when
 * its candidate passes `ok`, so `== actor` turns this into "is that object this
 * object's owner" instead of "did some mix produce a number".
 */
template <typename ReadFn, typename OkFn>
inline uint64_t GetOuterFrom(uint64_t obj, ReadFn read, OkFn ok,
	Rung* outRung = nullptr)
{
	const Hop hop = FromObject(obj, read, ok);
	if (outRung)
		*outRung = hop.rung;
	return hop.ptr;
}

template <typename OkFn>
inline uint64_t GetOuterChecked(uint64_t obj, OkFn ok, Rung* outRung = nullptr)
{
	return GetOuterFrom(obj, &NoCacheReader, ok, outRung);
}

/**
 * The structural "this object is an AActor" rule: it has a live RootComponent,
 * and that component's own Outer is the object itself. Nothing else in the
 * object graph is shaped that way, so no class name has to be consulted — and
 * the rule is templated on the reader so the suite can drive it.
 */
template <typename ReadFn>
inline bool OwnerIsActor(ReadFn read, uint64_t obj, uint64_t rootComponentOffset)
{
	const uint64_t root = read(obj + rootComponentOffset);
	if (!Plausible(root))
		return false;
	return GetOuterFrom(root, read, [obj](uint64_t p) { return p == obj; }) != 0;
}

/**
 * Walk UObject::Outer until `isOwner` accepts a hop's target (the standard
 * GetTypedOuter walk; the object itself is never considered). Stops on a cycle,
 * on a broken hop, or after maxHops.
 *
 * `hopOk` decides what a hop may land on, and it matters: `Plausible` alone
 * would let the first rung shadow every other one, because a wrong rung's mix is
 * still a 64-bit number in range. The live caller passes "the class pointer
 * resolves", which also rejects an unmapped address.
 */
template <typename ReadFn, typename HopOkFn, typename IsOwnerFn>
inline uint64_t ResolveOwner(uint64_t obj, ReadFn read, HopOkFn hopOk,
	IsOwnerFn isOwner, int maxHops = 8)
{
	uint64_t cur = obj;
	for (int hop = 0; hop < maxHops; ++hop) {
		const Hop up = FromObject(cur, read, hopOk);
		if (!up.ptr || up.ptr == cur)
			return 0;
		cur = up.ptr;
		if (isOwner(cur))
			return cur;
	}
	return 0;
}

} // namespace OuterLink

namespace GNames {

// ── Shard hash — hashes the ADDRESS of (ChunkAddr + ShardSeedOff) ────────────
inline void ShardHash(uint64_t SeedAddr, uint32_t& Bidx1, uint32_t& Bidx2) {
	const uint32_t Lo = static_cast<uint32_t>(SeedAddr);
	const uint32_t Hi = static_cast<uint32_t>(SeedAddr >> 32);
	uint32_t H = HashPrime32 * RotateLeft32(Lo, ShardRolA) + ShardHashAdd;
	H = HashPrime32 * RotateLeft32(H, ShardRolB) + Hi + ShardHashAdd;
	H = HashPrime32 * RotateLeft32(H, ShardRolC) + ShardHashAdd;
	H = HashPrime32 * (H >> ShardShrD) + ShardHashAdd;
	const uint32_t T = H ^ (H >> 16);
	Bidx1 = T & 7u;
	Bidx2 = (T + 1u) & 7u;
}

// Block decode: ROL64(4) ^ XOR, then per-dword ROL32(2).
inline uint64_t DecodeBlock(uint64_t Raw) {
	const uint64_t X = RotateLeft64(Raw, BlockRol64) ^ BlockXor;
	const uint32_t D0 = RotateLeft32(static_cast<uint32_t>(X), BlockRol32);
	const uint32_t D1 = RotateLeft32(static_cast<uint32_t>(X >> 32), BlockRol32);
	return static_cast<uint64_t>(D0) | (static_cast<uint64_t>(D1) << 32);
}

// ── CI → FNameEntry* (v818: PTR chain is NOP after Entry) ────────────────────
// SDK scheme (game::gasm::decode_fname_pool_address): the chunk window holds the
// selector seed at +0x40 and eight 0x20-byte blocks at +0x50; the decoded mix is
// the chunk's entry base, and the entry sits 2 bytes per name offset after it.
// ── SDK plain pipeline (sdk/CppSDK/SDK/Basic.hpp) ────────────────────────────
// FNamePool::GetEntryByIndex: block = index >> FNameBlockOffsetBits,
// entry = Blocks[block] + (index & 0xFFFF) * FNameEntryStride. This is the walk
// the dump's own SDK performs on this build, so it is tried first.
inline uint64_t ResolveNamePointerPlain(uint64_t moduleBase, int32_t CompIndex) {
	if (CompIndex <= 0 || !ready || !moduleBase)
		return 0;

	const uint32_t Ci = static_cast<uint32_t>(CompIndex);
	const uint32_t ChunkIndex = Ci >> PoolBlockBits;
	const uint32_t InChunk = Ci & ((1u << PoolBlockBits) - 1u);

	const uint64_t BlockPtr = steam_decrypt::MemReadVal<uint64_t>(
		moduleBase + gRuntimePoolRva + PoolBlocksOff +
		static_cast<uint64_t>(ChunkIndex) * sizeof(uint64_t));
	if (!steam_decrypt::ValidPtr(BlockPtr))
		return 0;

	const uint64_t EntryPtr = BlockPtr + static_cast<uint64_t>(InChunk) * EntryStride;
	return steam_decrypt::ValidPtr(EntryPtr) ? EntryPtr : 0;
}

inline uint64_t ResolveNamePointerSdk(uint64_t moduleBase, int32_t CompIndex) {
	if (CompIndex <= 0 || !keyTableReady)
		return 0;

	const uint32_t Ci = static_cast<uint32_t>(CompIndex);
	const uint32_t NameOff = Ci & 0xFFFFu;
	const uint32_t ChunkOff = (Ci >> 8) & 0xFFFF00u;
	const uint64_t ChunkAddr = moduleBase + gRuntimePoolRva + ChunkOff;

	uint8_t Chunk[ChunkWindowSize]{};
	if (!steam_decrypt::MemRead(ChunkAddr, Chunk, sizeof(Chunk)))
		return 0;

	const uint64_t Base = game::gasm::decode_fname_pool_address(ChunkAddr, Chunk, 0);
	if (!Base)
		return 0;

	const uint64_t EntryPtr = Base + 2ULL * NameOff;
	if (!steam_decrypt::ValidPtr(EntryPtr))
		return 0;
	return EntryPtr;
}

// Legacy v818: shard hash picks two 0x20 blocks, DecodeBlock + FNV mix + 2*NameOff.
inline uint64_t ResolveNamePointerLegacy(uint64_t moduleBase, int32_t CompIndex) {
	if (CompIndex <= 0)
		return 0;

	const uint32_t Ci = static_cast<uint32_t>(CompIndex);
	const uint32_t NameOff = Ci & 0xFFFFu;
	const uint32_t ChunkOff = (Ci >> 8) & 0xFFFF00u;
	const uint64_t ChunkAddr = moduleBase + gRuntimePoolRva + ChunkOff;

	uint32_t Bidx1 = 0, Bidx2 = 0;
	ShardHash(ChunkAddr + ShardSeedOff, Bidx1, Bidx2);

	const uint64_t BlockBase = ChunkAddr + ShardBlockBaseOff;
	const uint64_t Raw1 = steam_decrypt::MemReadVal<uint64_t>(
		BlockBase + ShardBlockStride * Bidx1);
	const uint64_t Raw2 = steam_decrypt::MemReadVal<uint64_t>(
		BlockBase + ShardBlockStride * Bidx2);
	if (!Raw1 && !Raw2)
		return 0;

	const uint64_t V13 = DecodeBlock(Raw1);
	const uint64_t V15 = DecodeBlock(Raw2);
	uint64_t Fv = FnvPrime * RotateLeft64(V13, FnvRol1) + FnvAdd;
	Fv = FnvPrime * RotateLeft64(Fv, FnvRol2) + FnvAdd;
	const uint64_t EntryPtr = V13 + (V15 ^ Fv) + 2ULL * NameOff;
	if (!steam_decrypt::ValidPtr(EntryPtr))
		return 0;
	return EntryPtr;
}

inline uint64_t ResolveNamePointer(uint64_t moduleBase, int32_t CompIndex) {
	if (const uint64_t Entry = ResolveNamePointerPlain(moduleBase, CompIndex))
		return Entry;
	if (const uint64_t Entry = ResolveNamePointerSdk(moduleBase, CompIndex))
		return Entry;
	return ResolveNamePointerLegacy(moduleBase, CompIndex);
}

// ── FNameEntry → string (v818 header + keystream) ────────────────────────────
// Cheap plausibility gate so the SDK and legacy pipelines can be ranked.
inline bool LooksLikeFName(const std::string& S) {
	if (S.empty() || S.size() > 128)
		return false;
	int Printable = 0;
	for (unsigned char C : S)
		if (C >= 32 && C <= 126)
			++Printable;
	return Printable * 5 >= static_cast<int>(S.size()) * 4;
}

// Gate for the plain (SDK) entry text. Real FName text is identifiers, digits
// and a small punctuation set, so a wrong pool layout can never publish garbage
// (the looser LooksLikeFName lets e.g. "2#52?/2#+" through).
inline bool LooksLikeNameEntry(const std::string& S) {
	if (S.empty() || S.size() > 128)
		return false;
	bool HasAlpha = false;
	for (unsigned char C : S) {
		const bool Alpha = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z');
		const bool Digit = (C >= '0' && C <= '9');
		const bool Punct = (C == '_' || C == '.' || C == '-' || C == ':' ||
		                    C == '/' || C == ' ' || C == '+');
		if (Alpha)
			HasAlpha = true;
		if (!Alpha && !Digit && !Punct)
			return false;
	}
	return HasAlpha;
}

// SDK string decode: len = ((hdr >> 3) & 0x3F8) + (hdr >> 13); bit 0 is bIsWide
// and is never touched by that formula. Narrow entries XOR against the key table
// through decode_fname; wide ones go through the SDK UTF-16 decoder.
// SDK plain string decode — FNameEntryHeader from sdk/CppSDK/SDK/Basic.hpp:
// bit 0 = bIsWide, bits 6..15 = Len, text at entry + 2 (FNameEntry::Name).
inline std::string DecodeStringPlain(uint64_t NameEntryPtr) {
	if (!NameEntryPtr)
		return {};

	const uint16_t Header = steam_decrypt::MemReadVal<uint16_t>(NameEntryPtr);
	const bool IsWide = (Header & Offsets::FNameEntry_WideBit) != 0;
	const uint32_t Length =
		(static_cast<uint32_t>(Header) >> static_cast<int>(Offsets::FNameEntry_LenShift)) &
		static_cast<uint32_t>(Offsets::FNameEntry_LenMask);
	if (!Length || Length > 1023)
		return {};

	std::string Out;
	Out.reserve(Length);

	if (!IsWide) {
		std::vector<uint8_t> Buf(Length);
		if (!steam_decrypt::MemRead(NameEntryPtr + EntryTextOff, Buf.data(), Buf.size()))
			return {};
		for (uint8_t Ch : Buf) {
			if (!Ch)
				break;
			Out.push_back((Ch >= 32 && Ch <= 126) ? static_cast<char>(Ch) : '?');
		}
		return Out;
	}

	std::vector<uint16_t> WBuf(Length);
	if (!steam_decrypt::MemRead(NameEntryPtr + EntryTextOff, WBuf.data(),
		WBuf.size() * sizeof(uint16_t)))
		return {};
	for (uint16_t W : WBuf) {
		if (!W)
			break;
		Out.push_back(W < 0x80 ? static_cast<char>(W) : '?');
	}
	return Out;
}

inline std::string DecodeStringSdk(uint64_t NameEntryPtr) {
	if (!NameEntryPtr || !keyTableReady)
		return {};

	const uint16_t Header = steam_decrypt::MemReadVal<uint16_t>(NameEntryPtr);
	const uint32_t Length = game::gasm::decode_fname_header_len(Header);
	if (!Length || Length > 1023)
		return {};

	std::string Out;
	Out.reserve(Length);

	// A decoded name never contains '?' unless the bytes were wrong
	// (non-printables are mapped to it), which is how the wide variant below
	// decides whether the FString fallback is needed.
	auto sane = [](const std::string& s) {
		return !s.empty() && s.find('?') == std::string::npos;
	};
	auto render = [](const auto& buf) {
		std::string s;
		for (auto ch : buf) {
			if (!ch)
				break;
			using T = typename std::decay<decltype(ch)>::type;
			s.push_back(static_cast<T>(ch) >= 32 && static_cast<T>(ch) <= 126
				? static_cast<char>(ch) : '?');
		}
		return s;
	};
	if ((Header & 0x1u) == 0) {
		std::vector<uint8_t> Buf(Length);
		if (!steam_decrypt::MemRead(NameEntryPtr + 2, Buf.data(), Buf.size()))
			return {};
		game::gasm::decode_fname(Header, Buf.data(), KeyTableBase());
		return render(Buf);
	}

	std::vector<uint16_t> WBuf(static_cast<size_t>(Length) + 1, 0);
	if (!steam_decrypt::MemRead(NameEntryPtr + 2, WBuf.data(),
		static_cast<size_t>(Length) * sizeof(uint16_t)))
		return {};
	// Wide entries: the current drop XORs the full u16 keystream word; the older
	// one ran the FString pipeline over them.
	{
		std::vector<uint16_t> V922 = WBuf;
		game::gasm::decode_fname_wide_v922(Header, V922.data(), KeyTableBase());
		std::string wide = render(V922);
		if (sane(wide))
			return wide;
	}
	game::gasm::decode_fstring(WBuf.data());
	for (uint16_t W : WBuf) {
		if (!W)
			break;
		Out.push_back(W < 0x80 ? static_cast<char>(W) : '?');
	}
	return Out;
}

// Legacy v818 string decode: len=hdr&0x03FF, wide=hdr&0x8000, keystream base 80.
inline std::string DecodeStringLegacy(uint64_t NameEntryPtr) {
	if (!NameEntryPtr || !legacyReady)
		return {};

	const uint16_t Header = steam_decrypt::MemReadVal<uint16_t>(NameEntryPtr);
	if (!Header)
		return {};

	const bool IsWide = (Header & HdrWideBit) != 0;
	const int Length = static_cast<int>(Header & HdrLengthMask);
	if (Length <= 0 || Length > 1023)
		return {};

	auto KsSlot = [&](uint32_t K) -> uint16_t {
		const int Idx = static_cast<int>((K & KeyIndexMask) + KeystreamBase);
		if (Idx < 0 || Idx >= 256)
			return 0;
		return keyTable[Idx];
	};

	const uint32_t BaseKey = static_cast<uint32_t>(Length) + KeyInitAdd;

	if (!IsWide) {
		std::vector<uint8_t> Buf(Length);
		if (!steam_decrypt::MemRead(NameEntryPtr + 2, Buf.data(), Buf.size()))
			return {};
		std::string Out;
		Out.reserve(Length);
		for (int I = 0; I < Length; ++I) {
			const uint32_t Slot = BaseKey + static_cast<uint32_t>(I) * KeyAdvance;
			const uint8_t Ch = static_cast<uint8_t>(
				Buf[I] ^ (KsSlot(Slot) >> NarrowKeyShift));
			if (!Ch)
				break;
			Out.push_back((Ch >= 32 && Ch <= 126) ? static_cast<char>(Ch) : '?');
		}
		return Out;
	}

	std::vector<uint16_t> WBuf(Length);
	if (!steam_decrypt::MemRead(NameEntryPtr + 2, WBuf.data(),
		WBuf.size() * sizeof(uint16_t)))
		return {};
	std::string Out;
	Out.reserve(Length);
	for (int I = 0; I < Length; ++I) {
		const uint32_t Slot = BaseKey + static_cast<uint32_t>(I) * KeyAdvance;
		const uint16_t W = static_cast<uint16_t>(WBuf[I] ^ KsSlot(Slot));
		if (!W)
			break;
		Out.push_back(W < 0x80 ? static_cast<char>(W) : '?');
	}
	return Out;
}

// Entry-pointer decode, in order: the SDK's plain pool (this build's real
// layout, sdk/CppSDK/SDK/Basic.hpp), then the retired SDK-drop slot pipeline,
// then the v20260818 keystream. Every candidate must pass the entry-text gate,
// so a candidate produced by the wrong layout is rejected rather than published.
inline std::string DecodeString(uint64_t NameEntryPtr) {
	if (const std::string Plain = DecodeStringPlain(NameEntryPtr); LooksLikeNameEntry(Plain))
		return Plain;
	if (const std::string Sdk = DecodeStringSdk(NameEntryPtr); LooksLikeNameEntry(Sdk))
		return Sdk;
	if (const std::string Legacy = DecodeStringLegacy(NameEntryPtr); LooksLikeNameEntry(Legacy))
		return Legacy;
	return {};
}

} // namespace GNames

// ── Steam decrypt (internal pipeline) ────────────────────────────────────────

namespace steam_decrypt {

// CL-1341255 / v20260818 — all pipeline constants live in GNames; the mirrors
// below keep the FNameState snapshot API stable.
inline constexpr uint64_t RVA_GNAMEPOOL = GNames::NamesOffset;
inline constexpr uint64_t RVA_KEYTABLE = GNames::KeyTableRva;          // SDK pipeline
inline constexpr uint64_t RVA_KEYSTREAM_LEGACY = GNames::KeystreamRvaLegacy;  // v818 fallback

inline constexpr uint64_t RVA_GUOBJECTARRAY_CHUNKS = Offsets::GUObjectArrayChunksRva;
inline constexpr uint64_t RVA_GOBJ_PSHUFB_MASK = Offsets::GObjPshufbMaskRva;

struct FNameState {
	uint64_t gnamePoolRva = 0;
	int keystreamBase = 0;
	bool ksLoaded = false;
	bool initialised = false;
};

inline FNameState& fname_state()
{
	static FNameState s;
	return s;
}

inline std::shared_mutex& g_name_cache_mtx()
{
	static std::shared_mutex m;
	return m;
}

inline std::unordered_map<int32_t, std::string>& g_name_cache()
{
	static std::unordered_map<int32_t, std::string> c;
	return c;
}

inline bool InitFNameState(uint64_t game_base)
{
	FNameState& s = fname_state();
	if (s.initialised)
		return true;
	if (!game_base)
		return false;

	s.gnamePoolRva = GNames::NamesOffset;
	s.keystreamBase = GNames::KeystreamBase;

	// SDK drop: the FName key table (game::offsets::KEYTABLE) is the gate; the
	// legacy v818 keystream is loaded best-effort for the fallback path.
	if (!GNames::Init(game_base))
		return false;
	s.ksLoaded = GNames::legacyReady;

	s.initialised = true;
	return true;
}

inline void ResetTables()
{
	FNameState& s = fname_state();
	s = FNameState{};
	{
		std::unique_lock<std::shared_mutex> lk(g_name_cache_mtx());
		g_name_cache().clear();
	}
	// Also reset new GNames state
	GNames::Reset();
}

inline void ClearNameCache()
{
	std::unique_lock<std::shared_mutex> lk(g_name_cache_mtx());
	g_name_cache().clear();
}

struct FNameStateSnapshot {
	FNameState state{};
	std::unordered_map<int32_t, std::string> names;
};

inline FNameStateSnapshot SnapshotFNameState()
{
	FNameStateSnapshot snap;
	snap.state = fname_state();
	std::shared_lock<std::shared_mutex> lk(g_name_cache_mtx());
	snap.names = g_name_cache();
	return snap;
}

inline void RestoreFNameState(const FNameStateSnapshot& snap)
{
	fname_state() = snap.state;
	std::unique_lock<std::shared_mutex> lk(g_name_cache_mtx());
	g_name_cache() = snap.names;
}

inline bool InitTables(uint64_t module_base)
{
	ResetTables();
	return InitFNameState(module_base);
}

// v20260818 slot selection (delegates to GNames).
inline uint32_t obj_name_slot(uint64_t ObjPtr)
{
	return GNames::NameSlot(ObjPtr);
}

inline uint32_t obj_class_slot(uint64_t ObjPtr)
{
	return GNames::ClassSlot(ObjPtr);
}

// SDK nameprivate slot decode; low 32 bits = comparison index. Falls back to
// the v818 slot decode (with its own selector) when the SDK path reads nothing.
inline uint64_t FindFNameSlot(uint64_t ObjBase)
{
	// SDK layout (sdk/CppSDK/SDK/CoreUObject_classes.hpp) first: UObject::Name is
	// a plain FName at +0x98 and its low dword is the comparison index.
	const uint64_t Plain = MemReadVal<uint64_t>(ObjBase + Offsets::UObject_NamePrivate);
	if ((Plain & 0xFFFFFFFFULL) > 1 && (Plain & 0xFFFFFFFFULL) < 0x2000000ULL)
		return Plain;
	if (const uint64_t Sdk = GNames::ReadNameSlot16(ObjBase); Sdk)
		return Sdk;
	return GNames::ReadSlot16Decoded(ObjBase, GNames::NameSlotLegacy(ObjBase));
}

inline uint64_t ResolveNamePtr(int32_t CompIndex, uint64_t game_base)
{
	return GNames::ResolveNamePointer(game_base, CompIndex);
}

inline std::string DecryptNameString(uint64_t NameEntryPtr)
{
	return GNames::DecodeString(NameEntryPtr);
}

inline bool IsPlausibleFNameText(const std::string& S)
{
	if (S.empty() || S.size() > 128)
		return false;
	int Printable = 0;
	for (unsigned char C : S)
		if (C >= 32 && C <= 126)
			++Printable;
	return Printable * 5 >= static_cast<int>(S.size()) * 4;
}

inline std::string CachedNameString(int32_t comp_index, uint64_t game_base)
{
	if (comp_index <= 0)
		return {};
	{
		std::shared_lock<std::shared_mutex> Lk(g_name_cache_mtx());
		auto It = g_name_cache().find(comp_index);
		if (It != g_name_cache().end())
			return It->second;
	}
	if (!InitFNameState(game_base))
		return {};
	uint64_t Ptr = ResolveNamePtr(comp_index, game_base);
	if (!Ptr)
		return {};
	std::string Str = DecryptNameString(Ptr);
	if (Str.empty() || !IsPlausibleFNameText(Str))
		return {};
	{
		std::unique_lock<std::shared_mutex> Lk(g_name_cache_mtx());
		g_name_cache().emplace(comp_index, Str);
	}
	return Str;
}

inline int32_t GetActorFNameId(uintptr_t actor_base)
{
	if (!actor_base || !ValidPtr(actor_base))
		return 0;
	const uint64_t game_base = Memory::getBaseAddress();
	if (!InitFNameState(game_base))
		return 0;
	uint64_t Raw = FindFNameSlot(actor_base);
	return static_cast<int32_t>(Raw & 0xFFFFFFFFu);
}

inline std::string GetActorFNameString(uintptr_t actor_base)
{
	if (!actor_base || !ValidPtr(actor_base))
		return {};
	const uint64_t game_base = Memory::getBaseAddress();
	if (!InitFNameState(game_base))
		return {};
	uint64_t Raw = FindFNameSlot(actor_base);
	int32_t CI = static_cast<int32_t>(Raw & 0xFFFFFFFFu);
	if (CI > 0) {
		std::string Str = CachedNameString(CI, game_base);
		if (!Str.empty())
			return Str;
	}
	int32_t RawCI = MemReadVal<int32_t>(actor_base + 0x18);
	if (RawCI > 1 && RawCI < 0x2000000) {
		std::string Str = CachedNameString(RawCI, game_base);
		if (!Str.empty())
			return Str;
	}
	return {};
}

inline uintptr_t GetActorClassPtr(uintptr_t ObjBase)
{
	if (!ObjBase || !ValidPtr(ObjBase))
		return 0;
	// SDK layout first (CoreUObject_classes.hpp): UObject::Class @ +0x20.
	const uint64_t PlainClass = MemReadVal<uint64_t>(ObjBase + Offsets::UObject_ClassPrivate);
	if (PlainClass >= 0x10000ULL && PlainClass < 0x800000000000ULL)
		return static_cast<uintptr_t>(PlainClass);

	// SDK classprivate slot decodes straight to a full u64 UClass*.
	if (const uint64_t Sdk = GNames::ReadClassSlot16(ObjBase);
		Sdk >= 0x10000ULL && Sdk < 0x800000000000ULL)
		return static_cast<uintptr_t>(Sdk);

	// Fallback: v818 class slot (same u64-shape contract).
	const uint64_t Decoded = GNames::ReadSlot16Decoded(ObjBase, GNames::ClassSlotLegacy(ObjBase));
	if (Decoded < 0x10000ULL || Decoded >= 0x800000000000ULL)
		return 0;
	return static_cast<uintptr_t>(Decoded);
}

inline std::string GetActorClassFName(uintptr_t obj_base)
{
	uintptr_t ClassPtr = GetActorClassPtr(obj_base);
	if (!ClassPtr)
		return {};
	return GetActorFNameString(ClassPtr);
}

// ── Player name ──────────────────────────────────────────────────────────────

inline void DecryptPlayerName(std::vector<uint16_t>& NameBuffer, int MaxLength)
{
	PlayerName::Decrypt(NameBuffer, MaxLength);
}

// CL-1341255 SIMD pipeline (mask read from game base @ 0xAD2FC50).
inline void DecryptPlayerNameSimd(std::vector<uint16_t>& NameBuffer, int MaxLength)
{
	PlayerName::DecryptSimd(NameBuffer, MaxLength);
}

// Legacy pre-CL-1341255 scramble (0xA7A3FF6B / rol 19) — fallback for older
// builds when the current key doesn't produce a plausible player name.
inline void DecryptPlayerNameLegacy(std::vector<uint16_t>& NameBuffer, int MaxLength)
{
	PlayerName::DecryptWithKey(NameBuffer, MaxLength,
		PlayerName::kKeyLegacy, PlayerName::kRotLegacy);
}

// Forum-pasted FText decode key (0x20003155 / rol 29) — the scramble that
// actually matches this build's APlayerState name strings. Tried after the
// CL-1341255 and legacy keys; plaintext-first stays the hot path.
inline void DecryptPlayerNameForum(std::vector<uint16_t>& NameBuffer, int MaxLength)
{
	PlayerName::DecryptWithKey(NameBuffer, MaxLength,
		0x20003155u, 29);
}

// SDK drop (sdk/sdk.txt, "v922") FString pipeline — state advance
// (rol 14) before the 5-bit XOR, printable-range correction chain.
inline void DecryptPlayerNameSdk(std::vector<uint16_t>& NameBuffer, int MaxLength)
{
	PlayerName::DecryptPlayerNameSdk(NameBuffer, MaxLength);
}

inline void DecryptName(std::vector<uint16_t>& nameBuffer, int maxLength)
{
	DecryptPlayerName(nameBuffer, maxLength);
}

// Verified player-name scramble index — set by the SDK FString self-check
// (Reflection::CheckFStringPipeline, driven from Engine::Update).
// 0=current, 1=simd, 2=legacy, 3=forum, -1=not verified yet.
inline std::atomic<int>& preferred_name_key()
{
	static std::atomic<int> key{ -1 };
	return key;
}

inline void SetPreferredNameKey(int key) { preferred_name_key().store(key, std::memory_order_relaxed); }
inline int PreferredNameKey() { return preferred_name_key().load(std::memory_order_relaxed); }

// Runs the verified scramble (if any). Returns false when nothing is verified.
inline bool TryPreferredPlayerNameKey(std::vector<uint16_t>& buffer, int maxLength)
{
	if (maxLength <= 0)
		return false;
	switch (PreferredNameKey()) {
	case 0: DecryptPlayerName(buffer, maxLength); return true;
	case 1: DecryptPlayerNameSimd(buffer, maxLength); return true;
	case 2: DecryptPlayerNameLegacy(buffer, maxLength); return true;
	case 3: DecryptPlayerNameForum(buffer, maxLength); return true;
	case 4: DecryptPlayerNameSdk(buffer, maxLength); return true;
	default: return false;
	}
}

inline bool IsPlausibleArcPlayerName(const std::string& name)
{
	if (name.size() < 2 || name.size() > 32)
		return false;

	// Control characters (0x00-0x1F, 0x7F) never appear in a real player name.
	// A name containing them is scrambled text that must fall through to the
	// decrypt paths — otherwise "q\x17"-style garbage short-circuits and gets
	// displayed as-is.
	for (unsigned char c : name) {
		if (c == 0)
			break;
		if (c < 0x20 || c == 0x7F)
			return false;
	}

	// Gamer tags start with a letter or digit, never punctuation/space.
	{
		const unsigned char first = static_cast<unsigned char>(name[0]);
		const bool alphaFirst = (first >= 'a' && first <= 'z') ||
			(first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9');
		if (!alphaFirst)
			return false;
	}

	int printable = 0;
	int vowels = 0;
	int letters = 0;
	int consonantRun = 0;
	int maxConsonantRun = 0;

	for (unsigned char c : name) {
		if (c == 0)
			break;
		const bool isAlpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		const bool isDigit = (c >= '0' && c <= '9');
		if (isAlpha || isDigit || c == '_' || c == '-')
			++printable;

		if (isAlpha) {
			++letters;
			const unsigned char lc = static_cast<unsigned char>(std::tolower(c));
			if (lc == 'a' || lc == 'e' || lc == 'i' || lc == 'o' || lc == 'u') {
				++vowels;
				consonantRun = 0;
			} else {
				++consonantRun;
				if (consonantRun > maxConsonantRun)
					maxConsonantRun = consonantRun;
			}
		} else if (isDigit || c == '_' || c == '-') {
			consonantRun = 0;
		}
	}

	if (printable < static_cast<int>(name.size()) / 2)
		return false;

	if (maxConsonantRun >= 5)
		return false;
	if (letters >= 4 && vowels == 0)
		return false;
	if (letters >= 6 && vowels * 4 < letters)
		return false;

	return true;
}

inline std::string WideCharsToPlayerName(const std::vector<uint16_t>& chars, int charLen)
{
	std::string result;
	result.reserve(static_cast<size_t>(charLen));
	for (int i = 0; i < charLen; ++i) {
		const uint16_t c = chars[static_cast<size_t>(i)];
		if (c == 0)
			break;
		if (c <= 0x7F)
			result.push_back(static_cast<char>(c));
	}
	return result;
}

inline bool IsValidNameHeapPtr(uint64_t ptr)
{
	return ptr > 0x10000 && ptr < 0x00007FFFFFFFFFFF && ValidPtr(ptr);
}

inline std::string ReadPlayerNameFromFString(uintptr_t fstringAddr)
{
	if (!fstringAddr || !ValidPtr(fstringAddr))
		return {};

	const uint64_t textData = Memory::read<uint64_t>(fstringAddr);
	const int32_t count = Memory::read<int32_t>(fstringAddr + 0x8);
	if (count <= 0 || count > 64)
		return {};

	const int lenCandidates[] = { count, count - 1 };
	for (int rawLen : lenCandidates) {
		if (rawLen <= 0 || rawLen > 64)
			continue;

		std::vector<uint16_t> chars(static_cast<size_t>(rawLen + 1), 0);
		bool readOk = false;

		if (IsValidNameHeapPtr(textData)) {
			readOk = true;
			for (int i = 0; i < rawLen; ++i) {
				chars[static_cast<size_t>(i)] = Memory::read<uint16_t>(
					static_cast<uintptr_t>(textData) + static_cast<uintptr_t>(i) * 2);
			}
		} else if (rawLen <= 8) {
			readOk = true;
			for (int i = 0; i < rawLen; ++i) {
				chars[static_cast<size_t>(i)] =
					Memory::read<uint16_t>(fstringAddr + static_cast<uintptr_t>(i) * 2);
			}
		}

		if (!readOk)
			continue;

		if (rawLen >= 2) {
			// LIVE-VERIFIED (debug-c190fb.log): names ARE scrambled on
			// CL-1341255 — "2!}lvfc" @ PS+0x448 decodes to "Execoper" with the
			// current key. Try the proven current-key scramble FIRST, then the
			// other decrypts, and raw LAST as a safety net. Plaintext-first was
			// removed: scrambled strings like "6`pW~{&o" pass the plausibility
			// gate and were being displayed as-is.
			std::string result;
			const std::vector<uint16_t> original = chars;

			// Scramble proven on the game's verification FString first (SDK drop
			// Offsets::FStringVerificationRva), then the default order.
			if (TryPreferredPlayerNameKey(chars, rawLen)) {
				result = WideCharsToPlayerName(chars, rawLen);
				if (IsPlausibleArcPlayerName(result))
					return result;
				chars = original;
			}

			// The drop's own pipeline next: it is the only variant whose rotate
			// amount (14) and state advance match sdk/sdk.txt. Without it every
			// decrypt failed and the raw ciphertext was displayed as a name.
			chars = original;
			DecryptPlayerNameSdk(chars, rawLen);
			result = WideCharsToPlayerName(chars, rawLen);
			if (IsPlausibleArcPlayerName(result))
				return result;

			chars = original;
			PlayerName::Decrypt(chars, rawLen);
			result = WideCharsToPlayerName(chars, rawLen);
			if (IsPlausibleArcPlayerName(result))
				return result;

			chars = original;
			DecryptPlayerNameSimd(chars, rawLen);
			result = WideCharsToPlayerName(chars, rawLen);
			if (IsPlausibleArcPlayerName(result))
				return result;

			chars = original;
			DecryptPlayerNameLegacy(chars, rawLen);
			result = WideCharsToPlayerName(chars, rawLen);
			if (!IsPlausibleArcPlayerName(result)) {
				chars = original;
				DecryptPlayerNameForum(chars, rawLen);
				result = WideCharsToPlayerName(chars, rawLen);
			}
			if (IsPlausibleArcPlayerName(result))
				return result;

			// Raw text is only a name when the verification FString proved that
			// names on this build are NOT scrambled. When a candidate did decode
			// it, the raw bytes here are ciphertext, and returning them painted
			// strings like "NbIe)xj[,U" as player names (debug-c190fb.log).
			if (PreferredNameKey() < 0) {
				chars = original;
				result = WideCharsToPlayerName(chars, rawLen);
				if (IsPlausibleArcPlayerName(result))
					return result;
			}
		} else {
			const std::string result = WideCharsToPlayerName(chars, rawLen);
			if (IsPlausibleArcPlayerName(result))
				return result;
		}
	}

	return {};
}

inline std::string ReadPlayerNameFromPlayerState(uintptr_t playerStateAddr)
{
	if (!playerStateAddr || !ValidPtr(playerStateAddr))
		return {};

	// 20260922 dump: APlayerState.PlayerNamePrivate 0x458, PawnPrivate 0x438.
	// The trailing pair are previous-build slots, kept as last-resort probes.
	static const std::ptrdiff_t kNameOffsets[] = {
		Offsets::PlayerNamePrivate,
		Offsets::PS_PlayerNamePrivate2, // drop's second name slot (fallback)
		Offsets::PlayerState_PawnPrivate,
		0x448,
		0x430,
	};

	for (std::ptrdiff_t off : kNameOffsets) {
		if (const std::string name = ReadPlayerNameFromFString(playerStateAddr + off);
			!name.empty()) {
			return name;
		}
	}
	return {};
}

inline std::string ResolvePlayerDisplayName(uintptr_t pawnAddr, uintptr_t playerStateAddr)
{
	if (pawnAddr && ValidPtr(pawnAddr)) {
		if (const std::string direct =
				ReadPlayerNameFromFString(pawnAddr + Offsets::PlayerNameOnPawn);
			!direct.empty()) {
			return direct;
		}
	}

	if (!playerStateAddr || !ValidPtr(playerStateAddr)) {
		if (pawnAddr && ValidPtr(pawnAddr))
			playerStateAddr = Memory::read<uintptr_t>(pawnAddr + Offsets::APlayerState);
	}

	if (playerStateAddr && ValidPtr(playerStateAddr))
		return ReadPlayerNameFromPlayerState(playerStateAddr);

	return {};
}

inline std::wstring GetPlayerNameFromPlayerState(uintptr_t player_state_addr)
{
	if (!player_state_addr || !ValidPtr(player_state_addr))
		return L"";

	const std::string utf8 = ResolvePlayerDisplayName(0, player_state_addr);
	if (utf8.empty())
		return L"";

	const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (wlen <= 0)
		return L"";

	std::wstring out(static_cast<size_t>(wlen - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), wlen);
	return out;
}

inline std::string GetPlayerNameFromPlayerStateUtf8(uintptr_t player_state_addr)
{
	const std::wstring ws = GetPlayerNameFromPlayerState(player_state_addr);
	if (ws.empty())
		return "";
	const int sz = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
		nullptr, 0, nullptr, nullptr);
	if (sz <= 0)
		return "";
	std::string out(static_cast<size_t>(sz), '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), sz,
		nullptr, nullptr);
	return out;
}

// ── Bone array ───────────────────────────────────────────────────────────────

inline std::uintptr_t GetBoneArrayDecrypt(std::uintptr_t MeshAddr)
{
	if (!MeshAddr || !ValidPtr(MeshAddr))
		return 0;

	const Bones::BoneArrayResult bones =
		Bones::DecryptBoneArray(static_cast<uint64_t>(MeshAddr));
	return static_cast<std::uintptr_t>(bones.Array);
}

// ── Mesh visibility (encrypted LastRenderTimeOnScreen — auto-scanning) ────
// Arc Raiders vis check, occlusion-based: Denuvo Anti-Cheat XOR-encrypts
// render times. Offset + XOR key are AUTO-SCANNED at runtime from a
// frustum-rendered mesh (bRecentlyRendered), verified by re-read, then:
//   LRTS decrypts to ~UWorld::TimeSeconds → mesh visible (not behind wall)
//   LRTS stale                            → behind wall / not rendered
// Replaces the old hardcoded 0x4C8 + 0xE1664254 pair that broke every build.

inline float DecryptRenderFloat(uint32_t encrypted, uint32_t key)
{
	const uint32_t bits = _byteswap_ulong(encrypted ^ key);
	float f = 0.f;
	std::memcpy(&f, &bits, sizeof(f));
	return f;
}

} // namespace steam_decrypt
