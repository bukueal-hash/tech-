#pragma once

// Fake backing store for the decode suites (Project.Tests only).
// steam_decrypt::g_memReadOverride routes every read through this, so the real
// decode math runs on synthetic buffers — no DMA hardware, no game.
//
// Kept out of tests_main.hpp so suites that don't need it (vector, json, aim,
// bone, collision) don't drag in SteamDecrypt.hpp's vmmdll/leechcore headers.

#pragma warning(push)
#pragma warning(disable : 4201) // vmmdll.h nameless struct/union (ThirdParty)
#include "Core/SteamDecrypt.hpp"
#pragma warning(pop)

#include <cstdint>
#include <map>

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
    void writeU32(uint64_t addr, uint32_t v) { write(addr, &v, sizeof(v)); }
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
    inline static FakeMem* s_instance = nullptr;
};

// Previous override, restored on scope exit (nested scopes stay correct).
inline bool (*g_savedMemReadOverride)(uint64_t, void*, size_t) = nullptr;

struct ScopedFakeMem {
    FakeMem mem;
    ScopedFakeMem()
    {
        g_savedMemReadOverride = steam_decrypt::g_memReadOverride;
        FakeMem::s_instance = &mem;
        steam_decrypt::g_memReadOverride = &FakeMem::readFn;
    }
    ~ScopedFakeMem()
    {
        steam_decrypt::g_memReadOverride = g_savedMemReadOverride;
        FakeMem::s_instance = nullptr;
    }
};

struct ScopedFailReader {
    static bool failFn(uint64_t, void*, size_t) { return false; }
    ScopedFailReader()
    {
        g_savedMemReadOverride = steam_decrypt::g_memReadOverride;
        steam_decrypt::g_memReadOverride = &failFn;
    }
    ~ScopedFailReader() { steam_decrypt::g_memReadOverride = g_savedMemReadOverride; }
};
