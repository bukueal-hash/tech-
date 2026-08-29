#pragma once
// FName / player-name / bone decrypt — PlayerName/Bones/GNames namespaces.
// FName: CL-1341255 v20260818 PCLMULQDQ pipeline (GNamePool 0xE35AB00, keystream 0xE2997F4).
// Player-name scramble: CL-1341255 key 0xD351FEEC rol 28, legacy 0xA7A3FF6B rol 19 fallback.

#include "Memory.h"
#include "Offsets.h"
#include "Cache.hpp"

#include <algorithm>
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

constexpr uint32_t kKeyCurrent = 0xD351FEECu;
constexpr int      kRotCurrent = 28;
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

// ── Bones namespace (CL-1341255 v818 pipeline) ─────────────────────────────
// Theia-style SIMD pointer decrypt, bones are plaintext component-space transforms.
// CL-1341255 v818: seed @ mesh+0x7B0, LOD @ mesh+0x7F8, descriptor @ bone+0x48,
// stride 0x60, ROL64=50, ROL32=22, XOR key removed (0), PSHUFB mask below.
// Same pipeline on all maps — CTW @ 0x370 is what makes it work off-Stella.

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

	alignas(16) uint8_t seed[16]{};
	if (!steam_decrypt::MemRead(mesh + SeedOffset, seed, sizeof(seed)))
		return result;

	const uint64_t base = DecryptBoneArrayPointer(seed);
	if (!steam_decrypt::ValidPtr(base))
		return result;

	const uint32_t lodDword =
		steam_decrypt::MemReadVal<uint32_t>(mesh + LodOffset);
	const uint32_t lodIndex =
		(lodDword >> LodShiftRight) & LodBitMask;
	const uint64_t descriptor =
		base + lodIndex + DescriptorOffset;
	if (!steam_decrypt::ValidPtr(descriptor))
		return result;

	const uint64_t boneArray =
		steam_decrypt::MemReadVal<uint64_t>(descriptor);
	if (!steam_decrypt::ValidPtr(boneArray))
		return result;

	result.Array = boneArray;
	result.Count = steam_decrypt::MemReadVal<int32_t>(descriptor + 8);
	if (result.Count <= 0 || result.Count > MaxBoneCount)
		result.Count = 0;

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

// ── GNames namespace (CL-1341255 / v20260818 PCLMULQDQ pipeline) ─────────────
// Replaces the old SSE-mask pipeline (SeedXor1Rva 0xB42D320 & friends) which
// died on the 2026-08-18 build. Constants from the UC "v20260818" post:
//   Slot hash:   ROL32(0x19/0x0E/0x19/0x0E)*P + ADD 0xD4C2DB3A, Hi folded step 2
//   Slot idx:    name=(h&3)^2, class=(h&3)^0, outer=(h&3)^1
//   Slot decrypt: 16B (Lo,Hi); T=Hi^clmul(K1,Lo); V=clmul(K2,T)^Lo; ROL64(V,32)
//   Shard hash (hashes ADDRESS): ROL32(0x17/0x15/0x17)*P+ADD 0x30091BB7,
//                last step SHR(0x0B)*P+A, (H^H>>16)
//   Block decode: ROL64(4)^0xF31D220392B6800B, per-dword ROL32(2)
//   FNV:         FNV64_PRIME*ROL64(V,48/46)+ADD 0x6463CD794F959557; ptr chain NOP
//   Header:      len=hdr&0x03FF, wide=hdr&0x8000
//   String:      keystream[144] base=80, KEY_INIT=0xD917+len, +1/element, narrow ^= key>>3

namespace GNames {

constexpr uint64_t NamesOffset = 0xE35AB00;   // RVA_GNAMEPOOL (v20260818; was 0xE38FA00)
constexpr uint64_t KeystreamRva = 0xE2997F4;  // RVA_KEYSTREAM  (v20260818; was 0xE2CE894)
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

inline uint16_t keyTable[256]{};
inline bool ready = false;
// Runtime pool override — Dumper injection; defaults to the compile-time v818 RVA.
inline uint64_t gRuntimePoolRva = NamesOffset;

inline uint32_t RotateLeft32(uint32_t value, int count) {
	return (value << count) | (value >> (32 - count));
}

inline uint64_t RotateLeft64(uint64_t value, int count) {
	return (value << count) | (value >> (64 - count));
}

// Portable GF(2) carry-less multiply (low 64 bits) — PCLMULQDQ emulation.
inline uint64_t ClmulLo(uint64_t X, uint64_t Y) {
	uint64_t R = 0;
	while (Y) {
#if defined(_MSC_VER)
		unsigned long B = 0;
		_BitScanForward64(&B, Y);
#else
		unsigned B = static_cast<unsigned>(__builtin_ctzll(Y));
#endif
		R ^= (X << B);
		Y &= Y - 1;
	}
	return R;
}

inline void Reset() {
	std::memset(keyTable, 0, sizeof(keyTable));
	gRuntimePoolRva = NamesOffset;
	ready = false;
}

inline bool Init(uint64_t moduleBase)
{
	if (ready)
		return true;

	uint8_t KsBuf[KeystreamCount * 2]{};
	const bool readOk = steam_decrypt::MemRead(moduleBase + KeystreamRva, KsBuf, sizeof(KsBuf));
	if (!readOk) {
		std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
		if (f) f << "{\"sessionId\":\"c190fb\",\"runId\":\"diag\",\"hypothesisId\":\"FNAME\","
		           << "\"location\":\"SteamDecrypt.hpp:Init\",\"message\":\"keystream_read_failed\","
		           << "\"data\":{\"rva\":\"0x" << std::hex << KeystreamRva << std::dec << "\"}}\n";
		return false;
	}

	int nz = 0;
	for (int I = 0; I < KeystreamCount; ++I) {
		std::memcpy(&keyTable[I], KsBuf + I * 2, 2);
		nz += (keyTable[I] != 0);
	}
	if (nz < 8) {
		std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
		if (f) f << "{\"sessionId\":\"c190fb\",\"runId\":\"diag\",\"hypothesisId\":\"FNAME\","
		           << "\"location\":\"SteamDecrypt.hpp:Init\",\"message\":\"keystream_low_nz\","
		           << "\"data\":{\"rva\":\"0x" << std::hex << KeystreamRva << std::dec
		           << "\",\"nz\":" << nz << "}}\n";
		return false;
	}

	ready = true;
	return true;
}

// ── UObject slot hash + slot selection ───────────────────────────────────────
inline uint32_t SlotHash(uint64_t ObjPtr) {
	const uint64_t Seed = ObjPtr + 0x10;
	const uint32_t Lo = static_cast<uint32_t>(Seed);
	const uint32_t Hi = static_cast<uint32_t>(Seed >> 32);
	uint32_t H = HashPrime32 * RotateLeft32(Lo, SlotRolA) + SlotHashAdd;
	H = HashPrime32 * RotateLeft32(H, SlotRolB) + Hi + SlotHashAdd;
	H = HashPrime32 * RotateLeft32(H, SlotRolC) + SlotHashAdd;
	H = HashPrime32 * RotateLeft32(H, SlotRolD) + SlotHashAdd;
	return H ^ (H >> 16);
}

inline uint32_t NameSlot(uint64_t ObjPtr)  { return (SlotHash(ObjPtr) & 3u) ^ NameSlotXor; }
inline uint32_t ClassSlot(uint64_t ObjPtr) { return (SlotHash(ObjPtr) & 3u) ^ ClassSlotAdj; }
inline uint32_t OuterSlot(uint64_t ObjPtr) { return (SlotHash(ObjPtr) & 3u) ^ OuterSlotAdj; }

// Slot decrypt (16-byte PCLMULQDQ):
//   T = Hi ^ clmul_lo(K1, Lo); V = clmul_lo(K2, T) ^ Lo; ROL64(V, 32)
// Low 32 bits of the decoded value are the FName comparison index for the
// name slot; the full u64 is a pointer for the class/outer slots.
inline uint64_t DecodeSlot16(uint64_t Lo, uint64_t Hi) {
	const uint64_t T = Hi ^ ClmulLo(SlotClmulK1, Lo);
	const uint64_t V = ClmulLo(SlotClmulK2, T) ^ Lo;
	return RotateLeft64(V, SlotRol64Final);
}

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
	return DecodeSlot16(Lo, Hi);
}

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
inline uint64_t ResolveNamePointer(uint64_t moduleBase, int32_t CompIndex) {
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

// ── FNameEntry → string (v818 header + keystream) ────────────────────────────
inline std::string DecodeString(uint64_t NameEntryPtr) {
	if (!NameEntryPtr || !ready)
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

} // namespace GNames

// ── Steam decrypt (internal pipeline) ────────────────────────────────────────

namespace steam_decrypt {

// CL-1341255 / v20260818 — all pipeline constants live in GNames; the mirrors
// below keep the FNameState snapshot API stable.
inline constexpr uint64_t RVA_GNAMEPOOL = GNames::NamesOffset;
inline constexpr uint64_t RVA_KEYSTREAM = GNames::KeystreamRva;

inline constexpr uint64_t RVA_GUOBJECTARRAY_CHUNKS = 0xE80BA10ULL;
inline constexpr uint64_t RVA_GOBJ_PSHUFB_MASK = 0xAD97CC0ULL;

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

	// v20260818: the keystream (144 u16 from RVA_KEYSTREAM) is the only
	// runtime state — no SIMD masks to load.
	s.ksLoaded = GNames::Init(game_base);
	if (!s.ksLoaded)
		return false;

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

// v20260818: 16-byte PCLMULQDQ slot decode; low 32 bits = comparison index.
inline uint64_t FindFNameSlot(uint64_t ObjBase)
{
	return GNames::ReadSlot16Decoded(ObjBase, obj_name_slot(ObjBase));
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
	// v20260818: class slot decodes to a full u64 UClass* (no CI split).
	const uint64_t Decoded = GNames::ReadSlot16Decoded(ObjBase, obj_class_slot(ObjBase));
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

inline void DecryptName(std::vector<uint16_t>& nameBuffer, int maxLength)
{
	DecryptPlayerName(nameBuffer, maxLength);
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

			chars = original;
			result = WideCharsToPlayerName(chars, rawLen);
			if (IsPlausibleArcPlayerName(result))
				return result;
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

	static const std::ptrdiff_t kNameOffsets[] = {
		Offsets::PlayerNamePrivate,
		0x448,
		0x438,
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
