#pragma once

#include <cstdint>
#include <string>
#include <basetsd.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "../DMA/Memory.h"

#include <atomic>

// Global scatter telemetry for DMA governor overlay ([debugDma]).
inline std::atomic<uint64_t> g_dmaScatterPrepares{ 0 };
inline std::atomic<uint64_t> g_dmaScatterExecutes{ 0 };
inline std::atomic<uint64_t> g_dmaScatterLastBatch{ 0 };

inline void DmaScatterStats_Get(uint64_t& executes, uint64_t& prepares, uint64_t& lastBatch)
{
    executes = g_dmaScatterExecutes.load(std::memory_order_relaxed);
    prepares = g_dmaScatterPrepares.load(std::memory_order_relaxed);
    lastBatch = g_dmaScatterLastBatch.load(std::memory_order_relaxed);
}

// Reused scatter handle (MarvelRivals-DMA style) — one init, clear after each execute.
class PersistentScatter {
public:
    void init()
    {
        if (!m_handle && g_mem.IsInitialized())
            m_handle = g_mem.GetScatterHandle();
    }

    void shutdown()
    {
        if (m_handle) {
            g_mem.CloseScatterHandle(m_handle);
            m_handle = nullptr;
        }
    }

    bool valid() const { return m_handle != nullptr; }

    bool prepare(uintptr_t address, void* buffer, size_t size)
    {
        if (!m_handle || !buffer || size == 0)
            return false;
        return g_mem.RequestReadScatter(m_handle, address, buffer, size);
    }

    template <typename T>
    bool prepare(uintptr_t address, T& out)
    {
        return prepare(address, &out, sizeof(T));
    }

    bool execute()
    {
        if (!m_handle)
            return false;
        if (!g_mem.ExecuteReadScatter(m_handle))
            return false;
        g_mem.ClearScatterHandle(m_handle);
        return true;
    }

private:
    void* m_handle = nullptr;
};

inline PersistentScatter g_scatter;

// Batches multiple DMA reads into one scatter execute.
class ScatterSession {
public:
    // Default: NOCACHE (bones / world scan / camera-critical paths).
    ScatterSession() : ScatterSession(false) {}

    // cached=true → VMM page cache (PositionRefresh only).
    explicit ScatterSession(bool cached) : m_cached(cached) {
        if (!g_mem.IsInitialized())
            return;
        m_handle = m_cached ? g_mem.GetScatterHandleCached() : g_mem.GetScatterHandle();
    }

    ~ScatterSession() { close(); }

    ScatterSession(const ScatterSession&) = delete;
    ScatterSession& operator=(const ScatterSession&) = delete;

    bool isValid() const { return m_handle != nullptr; }

    bool prepare(uintptr_t address, void* buffer, size_t size) {
        if (!m_handle || !buffer || size == 0)
            return false;
        if (!g_mem.RequestReadScatter(m_handle, address, buffer, size))
            return false;
        ++m_prepared;
        g_dmaScatterPrepares.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    template <typename T>
    bool prepare(uintptr_t address, T& out) {
        return prepare(address, &out, sizeof(T));
    }

    bool execute() {
        if (!m_handle)
            return false;
        const bool ok = m_cached
            ? g_mem.ExecuteReadScatterCached(m_handle)
            : g_mem.ExecuteReadScatter(m_handle);
        if (ok) {
            g_dmaScatterExecutes.fetch_add(1, std::memory_order_relaxed);
            g_dmaScatterLastBatch.store(m_prepared, std::memory_order_relaxed);
        }
        m_prepared = 0;
        return ok;
    }

    void clear() {
        if (!m_handle)
            return;
        m_prepared = 0;
        if (m_cached)
            g_mem.ClearScatterHandleCached(m_handle);
        else
            g_mem.ClearScatterHandle(m_handle);
    }

    void close() {
        if (m_handle) {
            g_mem.CloseScatterHandle(m_handle);
            m_handle = nullptr;
        }
        m_prepared = 0;
    }

private:
    void* m_handle = nullptr;
    bool m_cached = false;
    uint64_t m_prepared = 0;
};

class Memory
{
public:
    enum LoadError
    {
        noProcessID,
        noBaseAddress,
        success
    };

    enum MemoryStatus
    {
        bad,
        initialized,
        loaded
    };

protected:
    inline static uint64_t baseAddress = 0;
    inline static int processID = 0;

private:
    inline static MemoryStatus status = bad;

public:
    Memory() = default;

    static bool Initialize(const char* gameExe)
    {
        if (!gameExe || !gameExe[0])
            return false;

        if (g_mem.GetPID() && g_mem.GetBase()) {
            if (g_mem.GetAttachedExe() == gameExe) {
                baseAddress = g_mem.GetBase();
                processID = static_cast<int>(g_mem.GetPID());
                status = loaded;
                return true;
            }
            if (g_mem.reconnect(gameExe)) {
                baseAddress = g_mem.GetBase();
                processID = static_cast<int>(g_mem.GetPID());
                status = loaded;
                return true;
            }
        }

        if (g_mem.Initialize(gameExe)) {
            baseAddress = g_mem.GetBase();
            processID = static_cast<int>(g_mem.GetPID());
            status = loaded;
            return true;
        }
        status = bad;
        return false;
    }

    /** PioneerGame.exe (base), PioneerGame-e (EAC), PioneerGame-d (Denuvo). Single attach entry point. */
    static bool InitializeGame()
    {
        static const char* kGameExes[] = {
            "PioneerGame.exe",
            "PioneerGame-e.exe",
            "PioneerGame-d.exe",
        };
        for (const char* exe : kGameExes) {
            if (Initialize(exe))
                return true;
        }
        return false;
    }

    static const char* GetAttachedGameExe()
    {
        const std::string& exe = g_mem.GetAttachedExe();
        return exe.empty() ? nullptr : exe.c_str();
    }

    static void checkStatus()
    {
        if (g_mem.IsInitialized())
            status = loaded;
        else
            status = bad;
    }

    static MemoryStatus getStatus() { return status; }
    static int getProcessID() { return processID; }

    static uint64_t getBaseAddress()
    {
        if (g_mem.IsInitialized() && g_mem.GetBase())
            return g_mem.GetBase();
        return baseAddress;
    }

    static bool read(const void* address, void* buffer, DWORD64 size)
    {
        if (!g_mem.IsInitialized() || !address || !buffer || !size)
            return false;
        return g_mem.read(reinterpret_cast<uintptr_t>(address), buffer, static_cast<size_t>(size));
    }

    static bool read(DWORD64 address, void* buffer, DWORD64 size)
    {
        return read(reinterpret_cast<void*>(address), buffer, size);
    }

    template <typename T>
    static T read(void* address)
    {
        T buffer{};
        read(address, &buffer, sizeof(T));
        return buffer;
    }

    template <typename T>
    static T read(uint64_t address)
    {
        return read<T>(reinterpret_cast<void*>(address));
    }

    template <typename T>
    static T read_nocache(uint64_t address)
    {
        T buffer{};
        if (!g_mem.IsInitialized())
            return buffer;
        PCIMemory::ReadVirtualMemoryNoCache(address, &buffer, sizeof(T));
        return buffer;
    }

    static std::string read_string(DWORD_PTR address, size_t max_length = 256) {
        std::string result;
        result.reserve(max_length);

        for (size_t i = 0; i < max_length; ++i) {
            char c = read<char>(address + i);
            if (c == '\0' || c == 0) {
                break;
            }
            result += c;
        }

        if (result.size() > max_length) {
            result.resize(max_length);
        }

        return result;
    }

    static bool IsValidPtrFast2(uintptr_t address)
    {
        if (!address)
            return false;
        if (address < 0x10000 || address > 0x7FFFFFFFFFFF)
            return false;
        return true;
    }

    static bool ReadRaw(uintptr_t address, void* buffer, size_t size) {
        if (!IsValidPtrFast2(address) || !buffer || size == 0)
            return false;
        return read(reinterpret_cast<void*>(address), buffer, size);
    }

    static void write(void* address, const void* buffer, DWORD64 size)
    {
        if (!g_mem.IsInitialized() || !address || !buffer || !size)
            return;
        g_mem.write(reinterpret_cast<uintptr_t>(address), buffer, static_cast<size_t>(size));
    }

    static void write(DWORD64 address, DWORD64 buffer, DWORD64 size)
    {
        write(reinterpret_cast<void*>(address), reinterpret_cast<void*>(buffer), size);
    }

    template <typename T>
    static void write(void* address, const T& data)
    {
        write(address, &data, sizeof(T));
    }

    template <typename T>
    static void write(uint64_t address, const T& data)
    {
        write<T>(reinterpret_cast<void*>(address), data);
    }

};
