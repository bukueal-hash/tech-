#pragma once

// =============================================================================
// ArcDecryptBuild2 — the 0x6ACCA5BB build's decrypt set, ported 1:1 from the
// user-supplied reference drop. Reader-agnostic: every walk goes through
// steam_decrypt::MemRead (the project's DMA + test seam), so the same code the
// reference drives with its own reader runs here over DMA.
//
// Blocks (paste order):
//   ArcFName        FName pool shard/shard-block/descriptor + string decrypt
//   ArcGObjects     GUObjectArray global (clmul key) + chunk walk
//   ArcFField       FField name / class name / property offset
//   ArcPlayerName   APlayerState::PlayerNamePrivate
//   ArcGameInstance game instance via TEB/TLS stage-A blocks
//   ArcUWorld       UWorld via the TEB/TLS sibling group
//   ArcController   APlayerController -> pawn (8 encrypted slots)
//   ArcBones        USkeletalMeshComponent component-space bone array
//
// This build's ArcFName constants are the *Alt tier* SteamDecrypt.hpp already
// carries; they are re-stated here so this header stands alone.
// =============================================================================

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "SteamDecrypt.hpp" // steam_decrypt::MemRead

namespace arc2 {

// ---------------------------------------------------------------------------
// Reader seam. The reference drop has two shapes: a bool(*)(addr,dst,size)
// function pointer (ArcFName/ArcGObjects) and a `reader.Read<T>(addr)` /
// `reader.ReadBytes(addr,size,dst)` object (arc::*). One wrapper serves both.
// ---------------------------------------------------------------------------
using ReadMemoryFn = bool (*)(std::uint64_t address, void* output, std::size_t size);

inline bool ReadMemory(std::uint64_t address, void* output, std::size_t size)
{
    return steam_decrypt::MemRead(address, output, size);
}

struct MemReader
{
    template <typename T>
    T Read(std::uint64_t address) const
    {
        T v{};
        steam_decrypt::MemRead(address, &v, sizeof(T));
        return v;
    }

    bool ReadBytes(std::uint64_t address, std::size_t size, void* dst) const
    {
        return steam_decrypt::MemRead(address, dst, size);
    }
};

inline bool IsValidAddress(std::uint64_t address)
{
    return address > 0x10000 && address < 0x7FFFFFFFFFFF;
}

inline std::uint64_t CarrylessMultiplyLow(std::uint64_t a, std::uint64_t b)
{
    std::uint64_t result = 0;
    while (b) {
        if (b & 1)
            result ^= a;
        b >>= 1;
        a <<= 1;
    }
    return result;
}

} // namespace arc2

// ===========================================================================
// ArcFName
// ===========================================================================
namespace ArcFName {

using ReadMemoryFn = bool (*)(std::uint64_t address, void* output, std::size_t size);

constexpr std::uint64_t NamesRva    = 0x10EB1180;
constexpr std::uint64_t KeyTableRva = 0x10D4B25C;

constexpr std::uint32_t Fnv32Prime = 0x01000193;
constexpr std::uint64_t Fnv64Prime = 0x100000001B3;

constexpr std::uint64_t DescriptorXor  = 0xE8B09504E6B96A82;
constexpr std::uint64_t DescriptorPost = 0xF73AF93C1907D6F9;

constexpr std::uint32_t ShardHashAdd = 0x1CF6D60C;
constexpr std::uint64_t PoolMixAdd   = 0x9EC944098CCFFE8C;

constexpr std::uint64_t BlockKeyLo = 0xE973108E0AED0639;
constexpr std::uint64_t BlockKeyHi = 0x85FCE63D13122635;

constexpr std::uint32_t ChunkMask = 0x1FFF00;
constexpr std::uint32_t KeyBias   = 0x1312;
constexpr std::uint32_t KeyStep   = 0x235;
constexpr std::size_t   KeyCount  = 256;
constexpr std::size_t   KeyBase   = 88;

struct State
{
    std::uint64_t moduleBase{};
    ReadMemoryFn read{};
    std::array<std::uint16_t, KeyCount> keyTable{};
    bool initialized{};
};

struct UObjectMetadata
{
    std::uint64_t classPrivate{};
    std::uint64_t outerPrivate{};
    std::uint64_t nameDescriptor{};
    std::int32_t comparisonIndex{};
    std::uint32_t number{};
    bool valid{};
};

inline bool IsValidPointer(std::uint64_t address)
{
    return address > 0x10000 && address < 0x7FFFFFFFFFFF;
}

template <typename T>
bool Read(const State& state, std::uint64_t address, T& output)
{
    output = {};
    return state.read && state.read(address, &output, sizeof(T));
}

inline std::uint64_t CarrylessMultiplyLow(std::uint64_t a, std::uint64_t b)
{
    std::uint64_t result = 0;
    while (b) {
        if (b & 1)
            result ^= a;
        b >>= 1;
        a <<= 1;
    }
    return result;
}

inline bool Initialize(State& state, std::uint64_t moduleBase, ReadMemoryFn reader)
{
    state = {};
    state.moduleBase = moduleBase;
    state.read = reader;
    if (!moduleBase || !reader)
        return false;
    if (!reader(moduleBase + KeyTableRva, state.keyTable.data(), sizeof(state.keyTable)))
        return false;
    state.initialized = true;
    return true;
}

inline std::uint16_t GetKey(const State& state, std::uint32_t key)
{
    const std::size_t index = static_cast<std::size_t>((key & 0x3F) + KeyBase);
    return state.keyTable[index];
}

inline std::uint64_t DecryptDescriptor(std::uint64_t encrypted)
{
    const std::uint64_t value = std::rotl(encrypted ^ DescriptorXor, 27);
    const std::uint64_t word0 = (value >> 0) & 0xFFFF;
    const std::uint64_t word1 = (value >> 16) & 0xFFFF;
    const std::uint64_t word2 = (value >> 32) & 0xFFFF;
    const std::uint64_t word3 = (value >> 48) & 0xFFFF;
    const std::uint64_t shuffled = word3 | (word2 << 16) | (word0 << 32) | (word1 << 48);
    return shuffled ^ DescriptorPost;
}

inline UObjectMetadata ReadUObjectMetadata(const State& state, std::uint64_t object)
{
    UObjectMetadata result{};
    if (!state.initialized || !IsValidPointer(object))
        return result;

    std::array<std::uint64_t, 4> decoded{};
    std::uint32_t zeroSlot = 4;
    for (std::uint32_t slot = 0; slot < 4; ++slot) {
        std::uint64_t encrypted{};
        const std::uint64_t slotAddress = object + 0x20 + 0x20ULL * slot;
        if (!Read(state, slotAddress, encrypted))
            return {};
        decoded[slot] = DecryptDescriptor(encrypted);
        if (decoded[slot] == 0) {
            if (zeroSlot != 4)
                return {};
            zeroSlot = slot;
        }
    }
    if (zeroSlot == 4)
        return result;

    result.classPrivate    = decoded[(zeroSlot + 1) & 3];
    result.outerPrivate    = decoded[(zeroSlot + 2) & 3];
    result.nameDescriptor  = decoded[(zeroSlot + 3) & 3];
    result.comparisonIndex = static_cast<std::int32_t>(result.nameDescriptor >> 32);
    result.number          = static_cast<std::uint32_t>(result.nameDescriptor);
    result.valid = result.comparisonIndex >= 0 && IsValidPointer(result.classPrivate);
    return result;
}

inline std::uint64_t DecryptPoolBlock(const State& state, std::uint64_t address)
{
    std::array<std::uint64_t, 2> encrypted{};
    if (!state.read || !state.read(address, encrypted.data(), sizeof(encrypted)))
        return 0;
    const std::uint64_t intermediate = CarrylessMultiplyLow(encrypted[0], BlockKeyLo) ^ encrypted[1];
    return CarrylessMultiplyLow(intermediate, BlockKeyHi) ^ encrypted[0];
}

inline void GetShardIndices(std::uint64_t seedAddress, std::uint32_t& first, std::uint32_t& second)
{
    const std::uint32_t low = static_cast<std::uint32_t>(seedAddress);
    const std::uint32_t high = static_cast<std::uint32_t>(seedAddress >> 32);
    std::uint32_t hash = Fnv32Prime * std::rotl(low, 28) + ShardHashAdd;
    hash = Fnv32Prime * std::rotl(hash, 26) + high + ShardHashAdd;
    hash = Fnv32Prime * (hash >> 4) + ShardHashAdd;
    hash = Fnv32Prime * (hash >> 6) + ShardHashAdd;
    first = (hash ^ (hash >> 16)) & 7;
    second = (first + 1) & 7;
}

inline std::uint64_t ResolveEntry(const State& state, std::int32_t comparisonIndex)
{
    if (!state.initialized || comparisonIndex < 0)
        return 0;
    const std::uint32_t index = static_cast<std::uint32_t>(comparisonIndex);
    const std::uint64_t chunk =
        state.moduleBase + NamesRva + ((index >> 8) & ChunkMask);

    std::uint32_t firstShard{};
    std::uint32_t secondShard{};
    GetShardIndices(chunk + 0x10, firstShard, secondShard);

    const std::uint64_t firstBlock =
        DecryptPoolBlock(state, chunk + 0x20 + 0x20ULL * firstShard);
    const std::uint64_t secondBlock =
        DecryptPoolBlock(state, chunk + 0x20 + 0x20ULL * secondShard);
    if (!firstBlock && !secondBlock)
        return 0;

    std::uint64_t mix = Fnv64Prime * std::rotl(firstBlock, 43) + PoolMixAdd;
    mix = Fnv64Prime * std::rotl(mix, 33) + PoolMixAdd;
    return firstBlock + (secondBlock ^ mix) + 2ULL * static_cast<std::uint16_t>(index);
}

inline std::string DecodeNarrow(const State& state, std::uint64_t address, std::uint32_t length)
{
    if (!length || length > 1024)
        return {};
    std::vector<std::uint8_t> data(length);
    if (!state.read || !state.read(address, data.data(), data.size()))
        return {};
    const std::uint32_t key = length + KeyBias;
    std::string result;
    result.reserve(length);
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint16_t stream = GetKey(state, key + i * KeyStep);
        const std::uint8_t character = data[i] ^ static_cast<std::uint8_t>(stream >> 3);
        if (!character)
            break;
        result.push_back(character >= 0x20 && character <= 0x7E ? static_cast<char>(character) : '?');
    }
    return result;
}

inline std::string DecodeWide(const State& state, std::uint64_t address, std::uint32_t length)
{
    if (!length || length > 1024)
        return {};
    std::vector<std::uint16_t> data(length);
    if (!state.read || !state.read(address, data.data(), data.size() * sizeof(std::uint16_t)))
        return {};
    const std::uint32_t key = length + KeyBias;
    std::string result;
    result.reserve(length);
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint16_t character = data[i] ^ GetKey(state, key + i * KeyStep);
        if (!character)
            break;
        result.push_back(character >= 0x20 && character <= 0x7E ? static_cast<char>(character) : '?');
    }
    return result;
}

inline std::string GetNameByIndex(const State& state, std::int32_t comparisonIndex)
{
    const std::uint64_t entry = ResolveEntry(state, comparisonIndex);
    if (!IsValidPointer(entry))
        return {};
    std::uint16_t header{};
    if (!Read(state, entry, header) || !header)
        return {};
    const bool isWide = (header & 0x8000) != 0;
    const std::uint32_t encodedLength = ((header >> 3) & 0x3FC) | ((header >> 13) & 3);
    if (!encodedLength || encodedLength > 1024)
        return {};
    if (isWide)
        return DecodeWide(state, entry + 2, encodedLength >> 1);
    return DecodeNarrow(state, entry + 2, encodedLength);
}

inline std::string GetObjectName(const State& state, std::uint64_t object)
{
    const UObjectMetadata metadata = ReadUObjectMetadata(state, object);
    if (!metadata.valid)
        return {};
    std::string name = GetNameByIndex(state, metadata.comparisonIndex);
    if (name.empty())
        return {};
    if (metadata.number > 0 && metadata.number <= 0x100000) {
        name += "_";
        name += std::to_string(metadata.number - 1);
    }
    return name;
}

} // namespace ArcFName

// ===========================================================================
// ArcGObjects
// ===========================================================================
namespace ArcGObjects {

using ReadMemoryFn = bool (*)(std::uint64_t address, void* output, std::size_t size);

// Verified against Offsets.h (CL-1426848 dump, 2026-10-10) and Dumper/gobjects.hxx:
// the global is a pure-XOR blob; NumElements and Chunks are byte-swapped after XOR.
constexpr std::uint64_t GlobalRva = 0x10E423D0;

constexpr std::uint64_t GlobalXor = 0x4993C8DE308F8828;

constexpr std::uint64_t NumOffset    = 0x14;
constexpr std::uint32_t CountXor    = 0xA4E88268;
constexpr std::uint64_t ChunksOffset = 0x20;
constexpr std::uint64_t ChunksXor   = 0x26D0446400000000;

constexpr std::uint32_t ChunkShift   = 16;
constexpr std::uint32_t ChunkMask    = 0xFFFF;
constexpr std::uint64_t ItemStride   = 0x18;
constexpr std::uint64_t ObjectOffset = 0x08;

struct State
{
    std::uint64_t moduleBase{};
    ReadMemoryFn read{};
};

struct ObjectArray
{
    std::uint64_t address{};
    std::uint64_t chunks{};
    std::uint32_t count{};
    bool valid{};
};

inline bool IsValidAddress(std::uint64_t address)
{
    return address > 0x10000 && address < 0x7FFFFFFFFFFF;
}

template <typename T>
inline bool Read(const State& state, std::uint64_t address, T& output)
{
    output = {};
    return state.read && state.read(address, &output, sizeof(T));
}

inline std::uint32_t ByteSwap32(std::uint32_t value)
{
    return ((value & 0x000000FFU) << 24) | ((value & 0x0000FF00U) << 8) |
           ((value & 0x00FF0000U) >> 8) | ((value & 0xFF000000U) >> 24);
}

inline std::uint64_t ByteSwap64(std::uint64_t value)
{
    return ((value & 0x00000000000000FFULL) << 56) | ((value & 0x000000000000FF00ULL) << 40) |
           ((value & 0x0000000000FF0000ULL) << 24) | ((value & 0x00000000FF000000ULL) << 8) |
           ((value & 0x000000FF00000000ULL) >> 8) | ((value & 0x0000FF0000000000ULL) >> 24) |
           ((value & 0x00FF000000000000ULL) >> 40) | ((value & 0xFF00000000000000ULL) >> 56);
}

inline ObjectArray GetObjectArray(const State& state)
{
    ObjectArray result{};
    if (!state.moduleBase || !state.read)
        return result;

    std::uint64_t encryptedGlobal{};
    if (!Read(state, state.moduleBase + GlobalRva, encryptedGlobal))
        return result;
    if (!encryptedGlobal)
        return result;

    result.address = encryptedGlobal ^ GlobalXor;
    if (!IsValidAddress(result.address))
        return {};

    std::uint32_t encryptedCount{};
    std::uint64_t encryptedChunks{};
    if (!Read(state, result.address + NumOffset, encryptedCount))
        return {};
    if (!Read(state, result.address + ChunksOffset, encryptedChunks))
        return {};

    result.count  = ByteSwap32(encryptedCount ^ CountXor);
    result.chunks = ByteSwap64(encryptedChunks ^ ChunksXor);
    result.valid = result.count >= 1000 && result.count <= 10000000 &&
                   IsValidAddress(result.chunks);
    return result;
}

inline std::uint64_t GetChunk(const State& state, const ObjectArray& objects, std::uint32_t index)
{
    if (!objects.valid || index >= objects.count)
        return 0;
    std::uint64_t chunk{};
    const std::uint64_t chunkAddress =
        objects.chunks + sizeof(std::uint64_t) * (index >> ChunkShift);
    if (!Read(state, chunkAddress, chunk))
        return 0;
    return IsValidAddress(chunk) ? chunk : 0;
}

inline std::uint64_t GetObjectByIndex(const State& state, const ObjectArray& objects, std::uint32_t index)
{
    const std::uint64_t chunk = GetChunk(state, objects, index);
    if (!chunk)
        return 0;
    const std::uint64_t item = chunk + ItemStride * (index & ChunkMask);
    std::uint64_t object{};
    if (!Read(state, item + ObjectOffset, object))
        return 0;
    return IsValidAddress(object) ? object : 0;
}

inline std::uint64_t GetItemFlags(const State& state, const ObjectArray& objects, std::uint32_t index)
{
    const std::uint64_t chunk = GetChunk(state, objects, index);
    if (!chunk)
        return 0;
    const std::uint64_t item = chunk + ItemStride * (index & ChunkMask);
    std::uint64_t flags{};
    if (!Read(state, item, flags))
        return 0;
    return flags;
}

} // namespace ArcGObjects

// ===========================================================================
// FField / FProperty crypto (reader-templated, as in the reference)
// ===========================================================================
namespace ArcFField {

template <typename TReader>
std::uint64_t DecryptFFieldName(std::uint64_t field, TReader& reader)
{
    if (!field)
        return 0;
    std::uint64_t encrypted = 0;
    if (!reader.Read(field + 0x80, &encrypted, sizeof(encrypted)))
        return 0;
    const std::uint64_t mixed = encrypted ^ 0x9B6F41BA96F6E7B5ULL;
    const std::uint32_t low  = _rotl(static_cast<std::uint32_t>(mixed), 11) + 0x6909184B;
    const std::uint32_t high = _rotl(static_cast<std::uint32_t>(mixed >> 32), 11) + 0x6490BE46;
    const std::uint64_t result = (static_cast<std::uint64_t>(high) << 32) | static_cast<std::uint64_t>(low);
    return _rotl64(result, 32);
}

template <typename TReader>
std::uint64_t GetFFieldClass(std::uint64_t field, TReader& reader)
{
    std::uint64_t fieldClass = 0;
    if (!field || !reader.Read(field + 0xA8, &fieldClass, sizeof(fieldClass)))
        return 0;
    return fieldClass;
}

template <typename TReader>
std::uint32_t DecryptFFieldClassName(std::uint64_t fieldClass, TReader& reader)
{
    std::uint32_t encrypted = 0;
    if (!fieldClass || !reader.Read(fieldClass + 0x40, &encrypted, sizeof(encrypted)))
        return 0;
    return _rotl(encrypted ^ 0x29941481u, 1) ^ 0x018A6E39u;
}

template <typename TReader>
std::int32_t DecryptPropertyOffset(std::uint64_t field, TReader& reader)
{
    std::uint32_t encrypted = 0;
    if (!field || !reader.Read(field + 0xC0, &encrypted, sizeof(encrypted)))
        return 0;
    return static_cast<std::int32_t>(_byteswap_ulong(encrypted ^ 0xBE0967DDu));
}

} // namespace ArcFField

// ===========================================================================
// APlayerState::PlayerNamePrivate
// ===========================================================================
namespace ArcPlayerName {

inline std::uint8_t RotateCharacter(std::uint8_t value, std::uint8_t low, std::uint8_t high, std::uint8_t shift)
{
    if (value < low || value > high)
        return value;
    const std::uint8_t range = high - low + 1;
    return low + ((value - low + shift) % range);
}

template <typename TReader>
std::string DecryptPlayerName(std::uint64_t playerState, TReader& reader)
{
    struct FString
    {
        std::uint64_t data;
        std::int32_t count;
        std::int32_t capacity;
    };

    FString value{};
    if (!playerState || !reader.Read(playerState + 0x458, &value, sizeof(value)))
        return {};
    if (!value.data || value.count <= 1 || value.count > 256 ||
        value.capacity < value.count || value.capacity > 4096)
        return {};

    std::vector<std::uint16_t> buffer(static_cast<std::size_t>(value.count));
    if (!reader.ReadBytes(value.data, buffer.size() * sizeof(std::uint16_t), buffer.data()))
        return {};

    constexpr std::uint32_t prime = 0x01000193u;
    constexpr std::uint32_t add   = 0x50480069u;

    std::uint32_t state = 0;
    std::string result;
    result.reserve(buffer.size());
    for (const std::uint16_t encrypted : buffer) {
        if (!encrypted)
            break;
        state = prime * (state + _rotl(prime * state + add, 14));
        std::uint8_t decoded = static_cast<std::uint8_t>((encrypted ^ (state & 0x1Fu)) & 0xFFu);
        decoded = RotateCharacter(decoded, 33, 126, 47);
        decoded = RotateCharacter(decoded, 48, 57, 5);
        decoded = RotateCharacter(decoded, 97, 122, 13);
        decoded = RotateCharacter(decoded, 65, 90, 13);
        decoded = RotateCharacter(decoded, 33, 126, 47);
        result.push_back(static_cast<char>(decoded));
    }
    return result;
}

} // namespace ArcPlayerName

// ===========================================================================
// Game instance via TEB/TLS stage-A blocks
// ===========================================================================
namespace ArcGameInstance {

inline std::uint64_t ClmulLow(std::uint64_t left, std::uint64_t right)
{
    std::uint64_t result{};
    for (std::uint32_t bit = 0; bit < 64; ++bit)
        if ((right >> bit) & 1)
            result ^= left << bit;
    return result;
}

inline std::uint64_t PackedRotate(std::uint64_t value, std::uint32_t count)
{
    return static_cast<std::uint64_t>(std::rotl(static_cast<std::uint32_t>(value), static_cast<int>(count))) |
           (static_cast<std::uint64_t>(std::rotl(static_cast<std::uint32_t>(value >> 32), static_cast<int>(count))) << 32);
}

inline std::uint64_t PackedAdd(std::uint64_t value, std::uint64_t addend)
{
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(value) + static_cast<std::uint32_t>(addend)) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(value >> 32) + static_cast<std::uint32_t>(addend >> 32)) << 32);
}

template <typename TReader>
std::uint64_t TlsBase(std::uint64_t moduleBase, std::uint64_t teb, TReader& reader)
{
    const auto index = reader.template Read<std::uint32_t>(moduleBase + 0x1149FA98);
    const auto slots = reader.template Read<std::uint64_t>(teb + 0x58);
    return reader.template Read<std::uint64_t>(slots + static_cast<std::uint64_t>(index) * 8);
}

inline std::uint32_t TlsHash(std::uint64_t address)
{
    std::uint32_t value = std::rotl(static_cast<std::uint32_t>(address), 23) * 0x01000193u + 0xE7B9324Bu;
    value = std::rotl(value, 17) * 0x01000193u + static_cast<std::uint32_t>(address >> 32) + 0xE7B9324Bu;
    value = std::rotl(value, 23) * 0x01000193u + 0xE7B9324Bu;
    return std::rotl(value, 17) * 0x01000193u + 0xE7B9324Bu;
}

template <typename TReader>
std::uint64_t Slot(std::uint64_t moduleBase, std::uint64_t tlsBase, std::uint32_t slot, TReader& reader)
{
    const auto entry = tlsBase + 0xED0 + static_cast<std::uint64_t>(slot) * 0x70;
    const auto vtable   = reader.template Read<std::uint64_t>(entry);
    const auto function = reader.template Read<std::uint64_t>(vtable + 0x20);
    const auto low  = reader.template Read<std::uint64_t>(entry + 0x60);
    const auto high = reader.template Read<std::uint64_t>(entry + 0x68);

    switch (function - moduleBase) {
    case 0x3FCDF00: {
        const auto mixed = PackedAdd(PackedRotate(low ^
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE6A0), 15),
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE6B0));
        return reader.template Read<std::uint64_t>(tlsBase + 0xA8) ^ mixed;
    }
    case 0x3FCF3E0:
        return reader.template Read<std::uint64_t>(tlsBase + 0x38) ^ low ^
            ClmulLow(0x5081FB5FF3601D48ull, 0x4380CF4C3E404F9Bull);
    case 0x3FCF220: {
        const auto mixed = PackedAdd(PackedRotate(low ^
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE790), 27),
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE7A0));
        return reader.template Read<std::uint64_t>(tlsBase + 0x18) ^ mixed;
    }
    case 0x3FCEEF0: {
        const auto folded = ClmulLow(0xA21312F453C53725ull, low) ^ high;
        return reader.template Read<std::uint64_t>(tlsBase + 0xC0) ^ low ^
            ClmulLow(0x0B89758913122635ull, folded);
    }
    case 0x3FCE6F0: {
        std::array<std::uint8_t, 16> input{};
        std::array<std::uint8_t, 16> mask{};
        reader.ReadBytes(entry + 0x60, input.size(), input.data());
        reader.ReadBytes(moduleBase + 0xDEBE760, mask.size(), mask.data());
        std::uint64_t shuffled{};
        for (std::uint32_t index = 0; index < 8; ++index)
            if (!(mask[index] & 0x80))
                shuffled |= static_cast<std::uint64_t>(input[mask[index] & 0xF]) << (index * 8);
        const auto mixed = shuffled ^ reader.template Read<std::uint64_t>(moduleBase + 0xDEBE770) ^
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE780);
        return reader.template Read<std::uint64_t>(tlsBase + 0x40) ^ PackedRotate(mixed, 30);
    }
    case 0x3FD0100: {
        const auto first  = std::rotl(0x3236B6FD03820915ull + low, 11) ^ high;
        const auto second = std::rotl(0xCDC81D3935FFF6EAull + first, 53) ^ low;
        return reader.template Read<std::uint64_t>(tlsBase + 0x10) ^ second;
    }
    case 0x3FCD200: {
        const auto mixed = PackedAdd(PackedRotate(low ^
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE5C0), 23),
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE5D0));
        return reader.template Read<std::uint64_t>(tlsBase + 0xD0) ^ mixed;
    }
    case 0x3FCE560: {
        const auto mixed = PackedRotate(low, 15) ^
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE720);
        return reader.template Read<std::uint64_t>(tlsBase + 0x40) ^ std::rotl(mixed, 44);
    }
    default:
        return 0;
    }
}

template <typename TReader>
std::uint64_t Decrypt(std::uint64_t moduleBase, std::uint64_t teb, TReader& reader)
{
    const auto tlsBase = TlsBase(moduleBase, teb, reader);
    if (!tlsBase || reader.template Read<std::uint8_t>(tlsBase + 0xEC0) != 1)
        return 0;
    const auto hash = TlsHash(tlsBase + 0xEC0);
    const auto firstSlot  = (hash ^ (hash >> 16)) & 7u;
    const auto secondSlot = (firstSlot + 1u) & 7u;
    const auto first  = Slot(moduleBase, tlsBase, firstSlot, reader);
    const auto second = Slot(moduleBase, tlsBase, secondSlot, reader);
    auto mixed = std::rotl(first, 44) * 0x100000001B3ull + 0x315F6A22E4E51F6Bull;
    mixed = std::rotl(mixed, 28) * 0x100000001B3ull + 0x315F6A22E4E51F6Bull;
    return first + (second ^ mixed);
}

} // namespace ArcGameInstance

// ===========================================================================
// UWorld via the TEB/TLS sibling group
// ===========================================================================
namespace ArcUWorld {

inline std::uint32_t TlsHash(std::uint64_t address)
{
    std::uint32_t value = std::rotl(static_cast<std::uint32_t>(address), 27) * 0x01000193u + 0x04CAF237u;
    value = std::rotl(value, 25) * 0x01000193u + static_cast<std::uint32_t>(address >> 32) + 0x04CAF237u;
    value = (value >> 5) * 0x01000193u + 0x04CAF237u;
    return (value >> 7) * 0x01000193u + 0x04CAF237u;
}

inline std::uint64_t DecryptValues(std::uint64_t first, std::uint64_t second)
{
    auto mixed = std::rotl(first, 56) * 0x100000001B3ull + 0xBB434E5A2F335BD7ull;
    mixed = std::rotl(mixed, 52) * 0x100000001B3ull + 0xBB434E5A2F335BD7ull;
    return first + (second ^ mixed);
}

template <typename TReader>
std::uint64_t Slot(std::uint64_t moduleBase, std::uint64_t tlsBase, std::uint32_t slot, TReader& reader)
{
    const auto entry = tlsBase + 0x1290 + static_cast<std::uint64_t>(slot) * 0x90;
    const auto vtable   = reader.template Read<std::uint64_t>(entry);
    const auto function = reader.template Read<std::uint64_t>(vtable + 0x48);
    const auto low  = reader.template Read<std::uint64_t>(entry + 0x80);
    const auto high = reader.template Read<std::uint64_t>(entry + 0x88);

    auto shuffleBytes = [&](std::uint64_t inputLow, std::uint64_t inputHigh, std::uint32_t maskRva) {
        std::array<std::uint8_t, 16> input{};
        std::array<std::uint8_t, 16> mask{};
        std::memcpy(input.data(), &inputLow, sizeof(inputLow));
        std::memcpy(input.data() + 8, &inputHigh, sizeof(inputHigh));
        reader.ReadBytes(moduleBase + maskRva, mask.size(), mask.data());
        std::uint64_t result{};
        for (std::uint32_t index = 0; index < 8; ++index)
            if (!(mask[index] & 0x80))
                result |= static_cast<std::uint64_t>(input[mask[index] & 0xF]) << (index * 8);
        return result;
    };

    auto shuffleWords = [](std::uint64_t value) {
        constexpr std::array<std::uint32_t, 4> order{ 2, 0, 3, 1 };
        std::uint64_t result{};
        for (std::uint32_t index = 0; index < order.size(); ++index)
            result |= ((value >> (order[index] * 16)) & 0xFFFFull) << (index * 16);
        return result;
    };

    switch (function - moduleBase) {
    case 0x3FDD3E0: {
        const auto first  = std::rotl(0x8136ED84EE020915ull + low, 11) ^ high;
        const auto second = std::rotl(0x7EC8FC3935FFF6EAull + first, 53) ^ low;
        return reader.template Read<std::uint64_t>(tlsBase + 0xD0) ^ second;
    }
    case 0x3FDFAA0: {
        const auto first  = std::rotl(0xC0B8F83808720915ull + low, 51) ^ high;
        const auto second = std::rotl(0x3F47269935FFF6EAull + first, 13) ^ low;
        return reader.template Read<std::uint64_t>(tlsBase + 0xB0) ^ second;
    }
    case 0x3FDDBA0: {
        const auto mixed = shuffleWords(std::rotl(low ^
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE670), 43)) ^
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE690);
        return reader.template Read<std::uint64_t>(tlsBase + 0x60) ^ mixed;
    }
    case 0x3FDC6D0: {
        const auto shuffled = shuffleBytes(std::rotl(low, 50), std::rotl(high, 50), 0xDEBE850);
        return reader.template Read<std::uint64_t>(tlsBase + 0x28) ^ ArcGameInstance::PackedRotate(shuffled, 22);
    }
    case 0x3FDCA10:
        return reader.template Read<std::uint64_t>(tlsBase + 0x30) ^ low ^
            ArcGameInstance::ClmulLow(0x30AE6C1569E5F218ull, 0x23AF58063E404F9Bull);
    case 0x3FDDD40: {
        const auto shuffled = shuffleBytes(std::rotl(low, 39), std::rotl(high, 39), 0xDEBE620) ^
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE630);
        return reader.template Read<std::uint64_t>(tlsBase + 0xC8) ^ shuffled ^
            reader.template Read<std::uint64_t>(moduleBase + 0xDEBE640);
    }
    case 0x3FDCF10: {
        const auto folded = ArcGameInstance::ClmulLow(0x7F0291D07CA1AAADull, low) ^ high;
        return reader.template Read<std::uint64_t>(tlsBase + 0x80) ^ low ^
            ArcGameInstance::ClmulLow(0x8A2D2DC113122635ull, folded);
    }
    default:
        return 0;
    }
}

template <typename TReader>
std::uint64_t Decrypt(std::uint64_t moduleBase, std::uint64_t teb, TReader& reader)
{
    const auto tlsBase = ArcGameInstance::TlsBase(moduleBase, teb, reader);
    if (!tlsBase || reader.template Read<std::uint8_t>(tlsBase + 0x1280) != 1)
        return 0;
    const auto hash = TlsHash(tlsBase + 0x1280);
    const auto firstSlot  = (hash ^ (hash >> 16)) & 7u;
    const auto secondSlot = (firstSlot + 1u) & 7u;
    return DecryptValues(
        Slot(moduleBase, tlsBase, firstSlot, reader),
        Slot(moduleBase, tlsBase, secondSlot, reader));
}

} // namespace ArcUWorld

// ===========================================================================
// APlayerController -> pawn
// ===========================================================================
namespace ArcController {

inline std::uint32_t PawnHash(std::uint64_t address)
{
    std::uint32_t value = (static_cast<std::uint32_t>(address) >> 3) * 0x01000193u + 0x2A5DB308u;
    value = (value >> 3) * 0x01000193u + static_cast<std::uint32_t>(address >> 32) + 0x2A5DB308u;
    value = (value >> 3) * 0x01000193u + 0x2A5DB308u;
    return (value >> 3) * 0x01000193u + 0x0005B308u;
}

template <typename TReader>
std::uint64_t PawnSlot(std::uint64_t controller, std::uint32_t slot, TReader& reader)
{
    const auto entry = controller + 0x4C0 + static_cast<std::uint64_t>(slot) * 0x20;
    const auto low  = reader.template Read<std::uint64_t>(entry);
    const auto high = reader.template Read<std::uint64_t>(entry + 8);
    const auto folded = ArcGameInstance::ClmulLow(0xA7C45C12A1B6F4D1ull, low) ^ high;
    return low ^ ArcGameInstance::ClmulLow(0x3526928513122635ull, folded);
}

inline std::uint64_t DecryptValues(std::uint64_t first, std::uint64_t second)
{
    auto mixed = std::rotl(first, 50) * 0x100000001B3ull + 0x2985D87FB78E6E08ull;
    mixed = std::rotl(mixed, 43) * 0x100000001B3ull + 0x2985D87FB78E6E08ull;
    return first + (second ^ mixed);
}

template <typename TReader>
std::uint64_t Decrypt(std::uint64_t controller, TReader& reader)
{
    if (!controller)
        return 0;
    const auto hash = PawnHash(controller + 0x4B0);
    const auto firstSlot  = (hash ^ (hash >> 16)) & 7u;
    const auto secondSlot = (firstSlot + 1u) & 7u;
    return DecryptValues(
        PawnSlot(controller, firstSlot, reader),
        PawnSlot(controller, secondSlot, reader));
}

} // namespace ArcController

// ===========================================================================
// USkeletalMeshComponent component-space bones
// ===========================================================================
namespace ArcBones {

struct ArrayHeader
{
    std::uint64_t data{};
    std::int32_t count{};
    std::int32_t capacity{};
};

template <typename TReader>
std::uint64_t DecryptBoneDescriptorBase(std::uint64_t mesh, TReader& reader)
{
    if (!mesh)
        return 0;
    const auto encrypted = reader.template Read<std::uint64_t>(mesh + 0x7E0);
    return encrypted ^ ArcGameInstance::ClmulLow(0x34107FED59EA72D8ull, 0x27114BFE3E404F9Bull);
}

template <typename TReader>
ArrayHeader DecryptComponentSpaceBones(std::uint64_t mesh, TReader& reader)
{
    const auto base = DecryptBoneDescriptorBase(mesh, reader);
    if (!base)
        return {};
    const auto slot = reader.template Read<std::uint32_t>(mesh + 0x87C) & 2u;
    return reader.template Read<ArrayHeader>(base + 0xA0 + static_cast<std::uint64_t>(slot) * 8);
}

} // namespace ArcBones
