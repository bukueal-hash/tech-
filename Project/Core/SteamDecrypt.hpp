#pragma once
// CL-1315578 FName / player-name / bone decrypt — from help/New Text Document.txt (2026-07-09).
// No hybrid fallbacks to older CL pipelines.

#include "Memory.h"
#include "Offsets.h"
#include "Cache.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
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

inline bool MemRead(uint64_t addr, void* buf, size_t size)
{
	if (!addr || !buf || !size)
		return false;
	return Memory::ReadRaw(static_cast<uintptr_t>(addr), buf, size);
}

template <typename T>
inline T MemReadVal(uint64_t addr)
{
	T v{};
	MemRead(addr, &v, sizeof(T));
	return v;
}

// ── CL-1315578 FName constants ───────────────────────────────────────────────

inline constexpr uint32_t HASH_PRIME = 0x01000193u;
inline constexpr int SLOT_BASE_OFF = 0x20;
inline constexpr int SLOT_STRIDE = 0x20;
inline constexpr uint64_t FNV_PRIME_COMMON = 0x100000001B3ULL;

inline constexpr uint64_t RVA_GNAMEPOOL = 0xE4F2A00ULL;
inline constexpr uint64_t RVA_KEYTABLE = 0xE4318DCULL;
inline constexpr uint64_t RVA_BLOCK_MASK = 0xB523C50ULL;

inline constexpr uint32_t SLOT_HASH_ADD = 0xF3D8DA36u;
inline constexpr int HASH_ROL1 = 21;
inline constexpr int HASH_ROL2 = 13;
inline constexpr int HASH_ROL3 = 21;
inline constexpr int HASH_ROL4 = 13;

inline constexpr uint64_t CHUNK_HASH_SEED = 0x2F90ULL;
inline constexpr uint64_t CHUNK_BLOCK_BASE = 0x2FA0ULL;

inline constexpr uint32_t BLOCK_HASH_ADD = 0xD5AF8E52u;
inline constexpr uint64_t FNV_ADD = 0x10F3A73711CE0312ULL;

inline constexpr uint32_t STRING_BIAS_NARROW = 0xFFFFA7B4u;
inline constexpr uint32_t STRING_BIAS_WIDE = 0x0000A7B4u;
inline constexpr uint32_t STRING_MUL = 0x6DDC5690u;
inline constexpr uint32_t STRING_ADD = 0x5EBF2255u;
inline constexpr uint32_t STRING_ODD_MUL = 0xFFFF584Cu;
inline constexpr uint32_t STRING_ODD_ADD = 0xF629u;

inline constexpr uint64_t RVA_GUOBJECTARRAY_CHUNKS = 0xE3B61C0ULL;
inline constexpr uint64_t RVA_GOBJ_PSHUFB_MASK = 0xAD97CC0ULL;

struct FNameState {
	uint64_t gnamePoolRva = 0;
	uint16_t keyTable[64]{};
	uint64_t blockMask = 0;
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

inline uint64_t Pshuflw64(uint64_t X, int Imm)
{
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

inline bool InitFNameState(uint64_t game_base)
{
	FNameState& s = fname_state();
	if (s.initialised)
		return true;
	if (!game_base)
		return false;

	s.gnamePoolRva = RVA_GNAMEPOOL;

	if (!s.ksLoaded) {
		uint8_t KtBuf[128]{};
		if (MemRead(game_base + RVA_KEYTABLE, KtBuf, sizeof(KtBuf))) {
			for (int I = 0; I < 64; ++I)
				std::memcpy(&s.keyTable[I], KtBuf + I * 2, 2);
			int Nz = 0;
			for (int I = 0; I < 64; ++I)
				Nz += (s.keyTable[I] != 0);
			if (Nz >= 8)
				s.ksLoaded = true;
		}
	}

	if (!MemRead(game_base + RVA_BLOCK_MASK, &s.blockMask, sizeof(uint64_t)))
		return false;

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
}

inline bool InitTables(uint64_t module_base)
{
	ResetTables();
	return InitFNameState(module_base);
}

inline uint8_t obj_slot_mix_byte(uint64_t ObjPtr)
{
	uint64_t P = ObjPtr + 0x10;
	uint32_t Lo = static_cast<uint32_t>(P);
	uint32_t Hi = static_cast<uint32_t>(P >> 32);

	uint32_t A = HASH_PRIME * rotl32(Lo, HASH_ROL1) + SLOT_HASH_ADD;
	uint32_t B = HASH_PRIME * rotl32(A, HASH_ROL2) + Hi + SLOT_HASH_ADD;
	uint32_t C = HASH_PRIME * rotl32(B, HASH_ROL3) + SLOT_HASH_ADD;
	uint32_t V = HASH_PRIME * rotl32(C, HASH_ROL4) + SLOT_HASH_ADD;

	return static_cast<uint8_t>(V) ^ static_cast<uint8_t>(V >> 16);
}

inline uint32_t obj_name_slot(uint64_t ObjPtr)
{
	return (static_cast<uint32_t>(obj_slot_mix_byte(ObjPtr)) & 3u) ^ 2u;
}

inline uint32_t obj_class_slot(uint64_t ObjPtr)
{
	return static_cast<uint32_t>(obj_slot_mix_byte(ObjPtr)) & 3u;
}

inline uint64_t DecryptUObjSlotName(uint64_t Raw)
{
	uint64_t V = rotl64(Raw, 29);
	V = Pshuflw64(V, 0x39);
	uint32_t Ci = rotl32(static_cast<uint32_t>(V >> 32), 5);
	uint32_t Number = static_cast<uint32_t>(V);
	return static_cast<uint64_t>(Ci) | (static_cast<uint64_t>(Number) << 32);
}

inline uint64_t DecryptUObjSlotClass(uint64_t Raw)
{
	uint64_t V = rotl64(Raw, 29);
	V = Pshuflw64(V, 0x39);
	return V;
}

inline uint64_t ReadSlotRaw(uint64_t ObjBase, uint32_t Slot)
{
	uint64_t Addr = ObjBase + SLOT_BASE_OFF + static_cast<uint64_t>(Slot) * SLOT_STRIDE;
	return MemReadVal<uint64_t>(Addr);
}

inline uint64_t FindFNameSlot(uint64_t ObjBase)
{
	uint32_t Ns = obj_name_slot(ObjBase);
	uint64_t Raw = ReadSlotRaw(ObjBase, Ns);
	if (!Raw)
		return 0;
	return DecryptUObjSlotName(Raw);
}

inline uint64_t ComputeNameSeed(int32_t CompIndex)
{
	uint64_t X = static_cast<uint64_t>(static_cast<uint32_t>(CompIndex));
	uint64_t T = (X << 16) >> 50;
	X = Pshuflw64((X << 30) | T, 0x72);
	X = Pshuflw64(X, 0x8C);
	X = (X >> 14) << 14;
	X = Pshuflw64(X, 0x72);
	return X;
}

inline uint64_t ResolveNamePtr(int32_t CompIndex, uint64_t game_base)
{
	if (CompIndex <= 0)
		return 0;

	const FNameState& S = fname_state();
	uint64_t Seed = ComputeNameSeed(CompIndex);

	uint32_t Eax = static_cast<uint32_t>(Pshuflw64(Seed, 0x8C) >> 30);
	uint16_t NameOff = static_cast<uint16_t>(Eax);
	uint64_t ChunkAddr = game_base + S.gnamePoolRva
		+ (static_cast<uint64_t>(Eax >> 8) & 0xFFFF00ULL);

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
	if (!ValidPtr(B1Addr) || !ValidPtr(B2Addr))
		return 0;

	uint64_t B1Raw = MemReadVal<uint64_t>(B1Addr);
	uint64_t B2Raw = MemReadVal<uint64_t>(B2Addr);

	auto DecryptBlock = [&](uint64_t Raw) -> uint64_t {
		uint64_t Bv = rotl64(Raw, 13);
		Bv = Pshuflw64(Bv, 0x93);
		return Bv ^ S.blockMask;
	};

	uint64_t V13 = DecryptBlock(B1Raw);
	uint64_t V15 = DecryptBlock(B2Raw);

	uint64_t Fv1 = FNV_PRIME_COMMON * rotl64(V13, 37) + FNV_ADD;
	uint64_t Fv2 = FNV_PRIME_COMMON * rotl64(Fv1, 40) + FNV_ADD;

	uint64_t NamePtr = (Fv2 ^ V15) + V13 + 2ULL * NameOff;
	if (!ValidPtr(NamePtr))
		return 0;
	return NamePtr;
}

inline std::string DecryptNameString(uint64_t NameEntryPtr)
{
	if (!NameEntryPtr)
		return {};

	const FNameState& S = fname_state();
	if (!S.ksLoaded)
		return {};

	uint16_t Header = 0;
	if (!MemRead(NameEntryPtr, &Header, sizeof(uint16_t)) || !Header)
		return {};

	int Length = static_cast<int>(Header >> 6);
	bool IsWide = (Header & 0x20u) != 0;
	if (Length <= 0 || Length > 256)
		return {};

	if (!IsWide) {
		uint8_t Bytes[256]{};
		if (!MemRead(NameEntryPtr + 2, Bytes, static_cast<size_t>(Length)))
			return {};

		uint32_t Eax = static_cast<uint32_t>(Length) + STRING_BIAS_NARROW;
		int N = Length & ~1;
		for (int K = 0; 2 * K < N; K++) {
			Bytes[2 * K] ^= static_cast<uint8_t>(S.keyTable[Eax & 0x3Fu] >> 3);
			Bytes[2 * K + 1] ^= static_cast<uint8_t>(
				S.keyTable[(Eax * STRING_ODD_MUL + STRING_ODD_ADD) & 0x3Du] >> 3);
			Eax = Eax * STRING_MUL + STRING_ADD;
		}
		if ((Length & 1) != 0)
			Bytes[Length - 1] ^= static_cast<uint8_t>(S.keyTable[Eax & 0x3Fu] >> 3);

		std::string Out;
		Out.reserve(static_cast<size_t>(Length));
		for (int J = 0; J < Length; ++J) {
			uint8_t Ch = Bytes[J];
			if (!Ch)
				break;
			Out.push_back((Ch >= 32 && Ch <= 126) ? static_cast<char>(Ch) : '?');
		}
		return Out;
	}

	uint16_t Wides[256]{};
	if (!MemRead(NameEntryPtr + 2, Wides, static_cast<size_t>(Length) * sizeof(uint16_t)))
		return {};

	uint32_t Eax = static_cast<uint32_t>(Length) + STRING_BIAS_WIDE;
	int N = Length & ~1;
	for (int K = 0; 2 * K < N; K++) {
		Wides[2 * K] ^= S.keyTable[Eax & 0x3Fu];
		Wides[2 * K + 1] ^= S.keyTable[(Eax * STRING_ODD_MUL + STRING_ODD_ADD) & 0x3Du];
		Eax = Eax * STRING_MUL + STRING_ADD;
	}
	if ((Length & 1) != 0)
		Wides[Length - 1] ^= S.keyTable[Eax & 0x3Fu];

	std::string Out;
	Out.reserve(static_cast<size_t>(Length));
	for (int J = 0; J < Length; ++J) {
		uint16_t Ch = Wides[J];
		if (!Ch)
			break;
		Out.push_back((Ch >= 32 && Ch <= 126) ? static_cast<char>(Ch) : '?');
	}
	return Out;
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
	uint32_t Slot = obj_class_slot(ObjBase);
	uint64_t Raw = ReadSlotRaw(ObjBase, Slot);
	if (!Raw)
		return 0;
	uint64_t Decoded = DecryptUObjSlotClass(Raw);
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

// ── Player name (CL-1315578 help) ────────────────────────────────────────────

inline void DecryptPlayerName(std::vector<uint16_t>& NameBuffer, int MaxLength)
{
	if (NameBuffer.empty() || MaxLength <= 0)
		return;
	int len = (std::min)(MaxLength, static_cast<int>(NameBuffer.size()));
	if (len < 1 || NameBuffer[0] == 0)
		return;

	uint32_t v8 = 0;
	int i = 0;
	for (; i < len && NameBuffer[static_cast<size_t>(i)] != 0; ++i) {
		uint32_t v9 = v8 + rotl32(16777619u * v8 + 0x20003155u, 23);
		v8 = 16777619u * v9;

		uint8_t mask = static_cast<uint8_t>((static_cast<uint32_t>(-109) * v9)) & 0x1Fu;
		int v10 = static_cast<int>(NameBuffer[static_cast<size_t>(i)] ^ mask);

		int v11 = 0;
		if (static_cast<uint32_t>(v10 - 80) < 0x2Fu)
			v11 = -47;
		if (static_cast<uint32_t>(v10 - 33) < 0x2Fu)
			v11 = 47;
		int v14 = v10 + v11;

		int v15 = 0;
		if (static_cast<uint32_t>(v14 - 53) < 5u)
			v15 = -5;
		if (static_cast<uint32_t>(v14 - 48) < 5u)
			v15 = 5;
		int v16 = v14 + v15;

		int v17 = 0;
		if (static_cast<uint32_t>(v16 - 110) < 0xDu)
			v17 = -13;
		if (static_cast<uint32_t>(v16 - 97) < 0xDu)
			v17 = 13;
		int v18 = v16 + v17;

		int v19 = 0;
		if (static_cast<uint32_t>(v18 - 78) < 0xDu)
			v19 = -13;
		if (static_cast<uint32_t>(v18 - 65) < 0xDu)
			v19 = 13;
		int v22 = v18 + v19;

		int v23 = 0;
		if (static_cast<uint32_t>(v22 - 80) < 0x2Fu)
			v23 = -47;
		if (static_cast<uint32_t>(v22 - 33) < 0x2Fu)
			v23 = 47;

		NameBuffer[static_cast<size_t>(i)] = static_cast<uint16_t>(v22 + v23);
	}
	if (i < static_cast<int>(NameBuffer.size()))
		NameBuffer[static_cast<size_t>(i)] = 0;
}

inline void DecryptName(std::vector<uint16_t>& nameBuffer, int maxLength)
{
	DecryptPlayerName(nameBuffer, maxLength);
}

inline bool IsPlausibleArcPlayerName(const std::string& name)
{
	if (name.size() < 2 || name.size() > 32)
		return false;

	int printable = 0;
	for (unsigned char c : name) {
		if (c == 0)
			break;
		if ((c >= 'a' && c <= 'z')
			|| (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9')
			|| c == '_' || c == '-')
			++printable;
	}
	return printable >= static_cast<int>(name.size()) / 2;
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

		if (rawLen >= 2)
			DecryptPlayerName(chars, rawLen);

		const std::string result = WideCharsToPlayerName(chars, rawLen);
		if (IsPlausibleArcPlayerName(result))
			return result;
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
			!name.empty())
			return name;
	}
	return {};
}

inline std::string ResolvePlayerDisplayName(uintptr_t pawnAddr, uintptr_t playerStateAddr)
{
	if (pawnAddr && ValidPtr(pawnAddr)) {
		if (const std::string direct =
				ReadPlayerNameFromFString(pawnAddr + Offsets::PlayerNameOnPawn);
			!direct.empty())
			return direct;
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

// ── Bone array (CL-1315578 help) ─────────────────────────────────────────────

inline std::uintptr_t GetBoneArrayDecrypt(std::uintptr_t MeshAddr)
{
	if (!MeshAddr || !ValidPtr(MeshAddr))
		return 0;

	alignas(16) __m128i Enc{};
	if (!Memory::ReadRaw(MeshAddr + Offsets::Encrypted, &Enc, sizeof(__m128i)))
		return 0;
	if (!_mm_cvtsi128_si64(Enc))
		return 0;

	alignas(16) const __m128i XorKey =
		_mm_set_epi64x(0LL, static_cast<int64_t>(0x878588013124D57FULL));
	__m128i V = _mm_xor_si128(Enc, XorKey);
	V = _mm_or_si128(_mm_slli_epi32(V, 7), _mm_srli_epi32(V, 25));
	V = _mm_shufflelo_epi16(V, 0x1B);

	uint64_t Base = static_cast<uint64_t>(_mm_cvtsi128_si64(V));
	if (!ValidPtr(Base))
		return 0;

	uint32_t LodDword = Memory::read<uint32_t>(MeshAddr + Offsets::LodSelect);
	uint32_t LodIndex = (LodDword >> 11) & 0x10;

	uint64_t BoneAddr = Base + LodIndex + Offsets::BoneArrayLodStride;
	if (!ValidPtr(BoneAddr))
		return 0;

	uint64_t BoneArray = Memory::read<uint64_t>(BoneAddr);
	if (!ValidPtr(BoneArray))
		return 0;
	return static_cast<std::uintptr_t>(BoneArray);
}

} // namespace steam_decrypt

#if 0 // CL-1233465 bone path removed — qwe900 CL-1315578 only
inline uint64_t DecryptBoneArrayPtr(const uint8_t seed[16])
{
	uint64_t s = 0;
	memcpy(&s, seed, 8);
	const uint64_t rotated = steam_decrypt::rotl64(s, 8);

	alignas(16) uint8_t r16[16]{};
	memcpy(r16, &rotated, 8);
	memcpy(r16 + 8, seed + 8, 8);

	alignas(16) __m128i shuffled =
		_mm_shufflelo_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(r16)), 0x1E);
	__m128i xored = _mm_xor_si128(
		shuffled,
		_mm_load_si128(reinterpret_cast<const __m128i*>(PTR_XOR_KEY)));
	__m128i result = _mm_shuffle_epi8(
		xored,
		_mm_load_si128(reinterpret_cast<const __m128i*>(PTR_SHUF_MASK)));

	uint64_t ptr = 0;
	memcpy(&ptr, &result, 8);
	return ptr;
}

inline FTransformD DecryptBone(const double raw[12], const double keys[12])
{
	FTransformD t{};

	const double kx = keys[0], ky = keys[1], kz = keys[2], kw = keys[3];
	const double rx = raw[0], ry = raw[1], rz = raw[2], rw = raw[3];
	t.RotW = kw * rw - kx * rx - ky * ry - kz * rz;
	t.RotX = kw * rx + kx * rw + ky * rz - kz * ry;
	t.RotY = kw * ry - kx * rz + ky * rw + kz * rx;
	t.RotZ = kw * rz + kx * ry - ky * rx + kz * rw;

	uint64_t tx = 0, ty = 0, tz = 0, kx4 = 0, ky4 = 0, kz4 = 0;
	memcpy(&tx, &raw[4], 8); memcpy(&kx4, &keys[4], 8);
	memcpy(&ty, &raw[5], 8); memcpy(&ky4, &keys[5], 8);
	memcpy(&tz, &raw[6], 8); memcpy(&kz4, &keys[6], 8);
	tx ^= kx4; ty ^= ky4; tz ^= kz4;
	memcpy(&t.TransX, &tx, 8); memcpy(&t.TransY, &ty, 8); memcpy(&t.TransZ, &tz, 8);

	uint64_t sx = 0, sy = 0, sz = 0, kx8 = 0, ky8 = 0, kz8 = 0;
	memcpy(&sx, &raw[8], 8); memcpy(&kx8, &keys[8], 8);
	memcpy(&sy, &raw[9], 8); memcpy(&ky8, &keys[9], 8);
	memcpy(&sz, &raw[10], 8); memcpy(&kz8, &keys[10], 8);
	sx ^= kx8; sy ^= ky8; sz ^= kz8;
	memcpy(&t.ScaleX, &sx, 8); memcpy(&t.ScaleY, &sy, 8); memcpy(&t.ScaleZ, &sz, 8);

	return t;
}

inline FTransform DecryptBoneToTransform(
	uintptr_t mesh,
	uintptr_t boneArray,
	int boneIndex)
{
	double raw[12]{};
	double keys[12]{};
	Memory::ReadRaw(boneArray + static_cast<uintptr_t>(boneIndex) * BONE_STRIDE, raw, sizeof(raw));
	Memory::ReadRaw(mesh + OFF_BONE_KEYS, keys, sizeof(keys));

	const FTransformD t = DecryptBone(raw, keys);
	FTransform out{};
	out.Rotation.x = t.RotX;
	out.Rotation.y = t.RotY;
	out.Rotation.z = t.RotZ;
	out.Rotation.w = t.RotW;
	out.Translation.x = t.TransX;
	out.Translation.y = t.TransY;
	out.Translation.z = t.TransZ;
	out.Scale3D.x = t.ScaleX;
	out.Scale3D.y = t.ScaleY;
	out.Scale3D.z = t.ScaleZ;
	return out;
}

inline std::uintptr_t GetBoneArrayDecrypt(std::uintptr_t meshAddr)
{
	if (!meshAddr || !steam_decrypt::ValidPtr(meshAddr))
		return 0;

	alignas(16) uint8_t seed[16]{};
	if (!Memory::ReadRaw(meshAddr + OFF_BONE_SEED, seed, sizeof(seed)))
		return 0;

	uint64_t probe = 0;
	memcpy(&probe, seed, 8);
	if (!probe)
		return 0;

	const uint64_t ptr = DecryptBoneArrayPtr(seed);
	if (!steam_decrypt::ValidPtr(ptr))
		return 0;
	return static_cast<std::uintptr_t>(ptr);
}

} // namespace bone_decrypt_cl1233465
#endif // CL-1233465 removed
