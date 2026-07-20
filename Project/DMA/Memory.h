#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include "DmaVmm.h"

struct vmProcess_t {
    DWORD dwProcID;
    uintptr_t dwModBase;
    DWORD dwImageSize;
};

inline vmProcess_t vmProcess = { 0 };

enum class DmaConnectionState : uint8_t {
    Disconnected = 0,
    Connecting,
    WaitingForProcess,
    Connected,
    Failed
};

struct MemoryTrafficStats {
    uint64_t readOperations = 0;
    uint64_t writeOperations = 0;
    uint64_t readSuccesses = 0;
    uint64_t readFailures = 0;
    uint64_t writeSuccesses = 0;
    uint64_t writeFailures = 0;
    uint64_t readBytesRequested = 0;
    uint64_t writeBytesRequested = 0;
    uint64_t scatterReadBatches = 0;
    uint64_t scatterReadRequests = 0;
    double sampleWindowSeconds = 0.0;
    double readOperationsPerSecond = 0.0;
    double readBytesRequestedPerSecond = 0.0;
    double scatterReadBatchesPerSecond = 0.0;
};

struct MemoryConnectionStats {
    bool vmmHandleValid = false;
    bool processInitialized = false;
    uint32_t processId = 0;
    std::string processName;
    uint64_t targetBaseAddress = 0;
    uint64_t targetBaseSize = 0;
    DmaConnectionState state = DmaConnectionState::Disconnected;
};

class PCIMemory {
private:
    static DWORD processId;
    static VMM_HANDLE hVMM;
    static uintptr_t moduleBase;
    static DWORD moduleImageSize;
    static bool s_memMap;
    static std::string attachedExe;
    static std::atomic<DmaConnectionState> s_dmaState;

    static std::atomic<uint64_t> s_readOps;
    static std::atomic<uint64_t> s_writeOps;
    static std::atomic<uint64_t> s_readOk;
    static std::atomic<uint64_t> s_readFail;
    static std::atomic<uint64_t> s_writeOk;
    static std::atomic<uint64_t> s_writeFail;
    static std::atomic<uint64_t> s_readBytes;
    static std::atomic<uint64_t> s_writeBytes;
    static std::atomic<uint64_t> s_scatterBatches;
    static std::atomic<uint64_t> s_scatterRequests;

    static void RecordDirectRead(size_t bytes, bool ok);
    static void RecordDirectWrite(size_t bytes, bool ok);
    static void RecordScatterRequest();
    static void RecordScatterBatch(bool ok);

public:
    PCIMemory() = default;
    explicit PCIMemory(const char* gameExe, bool memMap = false) {
        s_memMap = memMap;
        Initialize(gameExe);
    }
    ~PCIMemory();

    bool Initialize(const char* gameExe);
    static bool InitializeVmmOnly();
    static void SetVmmHandle(VMM_HANDLE h) { hVMM = h; }
    bool reconnect(const char* gameExe);
    uintptr_t GetBase() const { return moduleBase; }
    DWORD GetPID() const { return processId; }
    const std::string& GetAttachedExe() const { return attachedExe; }
    static VMM_HANDLE GetVmmHandle() { return hVMM; }
    DWORD GetPidFromName(const char* processName) const;
    std::vector<DWORD> GetPidListFromName(const char* processName) const;

    static DmaConnectionState GetDmaState() { return s_dmaState.load(std::memory_order_relaxed); }
    static bool IsDmaOperational() {
        return hVMM != nullptr && processId != 0 && moduleBase != 0
            && s_dmaState.load(std::memory_order_relaxed) == DmaConnectionState::Connected;
    }
    static MemoryTrafficStats GetTrafficStats();
    static void ResetTrafficStats();
    static MemoryConnectionStats GetConnectionStats();
    static std::string GetTrafficStatsString();

    template<typename T>
    static T ReadEx(uintptr_t address);

    template<typename T>
    static bool WriteEx(uintptr_t address, T value);

    template<typename T>
    static T Read(uintptr_t address);

    template<typename T>
    static bool Write(uintptr_t address, T value);

    /** Cached (useCache=true) uses VMM page cache; false uses NOCACHE. */
    static bool ReadVirtualMemoryEx(uintptr_t address, void* buffer, size_t size, bool useCache = true);
    static bool ReadVirtualMemory(uintptr_t address, void* buffer, size_t size);
    /** Per-frame camera / actor positions — bypass VMM data cache. */
    static bool ReadVirtualMemoryNoCache(uintptr_t address, void* buffer, size_t size);

    template<typename T>
    static bool TryRead(uintptr_t address, T& out, bool useCache = true) {
        static_assert(std::is_trivially_copyable_v<T>, "TryRead requires trivially copyable type");
        out = {};
        if (!address)
            return false;
        return ReadVirtualMemoryEx(address, &out, sizeof(T), useCache);
    }

    template<typename T>
    static std::optional<T> ReadOpt(uintptr_t address, bool useCache = true) {
        T value{};
        if (!TryRead(address, value, useCache))
            return std::nullopt;
        return value;
    }

    template<typename T>
    static T ReadOr(uintptr_t address, T fallback, bool useCache = true) {
        T value{};
        if (!TryRead(address, value, useCache))
            return fallback;
        return value;
    }

    static bool ReadChain(uint64_t base, const std::vector<uint64_t>& offsets, uint64_t& out, bool useCache = true);
    static uint64_t ReadChain(uint64_t base, const std::vector<uint64_t>& offsets, bool useCache = true);

    bool read(uintptr_t address, void* buffer, size_t size) const;
    bool write(uintptr_t address, const void* buffer, size_t size);

    bool IsInitialized() const { return hVMM != nullptr; }

    static bool FullRefresh();

    static void* GetScatterHandle();
    /** Scatter that uses VMM page cache (flags=0). Position refresh only. */
    static void* GetScatterHandleCached();
    static bool RequestReadScatter(void* handle, uintptr_t address, void* buffer, size_t size);
    static bool ExecuteReadScatter(void* handle);
    static bool ExecuteReadScatterCached(void* handle);
    /** Execute + clear handle (safe to close after). */
    static bool ExecuteReadScatterEx(void* handle);
    /** Call between Prepare/Execute cycles on the same handle (see vmmdll VMMDLL_Scatter_Clear). */
    static void ClearScatterHandle(void* handle);
    static void ClearScatterHandleCached(void* handle);
    static void CloseScatterHandle(void* handle);
};

inline PCIMemory g_mem;

class DmaMem {
public:
    explicit DmaMem(PCIMemory& mem) : m_mem(mem) {}

    bool isConnected() const { return m_mem.IsInitialized(); }
    VMM_HANDLE GetVmm() const { return PCIMemory::GetVmmHandle(); }
    DWORD GetPidFromName(const char* processName) const { return m_mem.GetPidFromName(processName); }
    uint64_t FindSignature(const char* signature, uint64_t rangeStart, uint64_t rangeEnd, int pid);

    template <typename T>
    T Read(uint64_t address, int pid) {
        T buffer{};
        DWORD cbRead = 0;
        auto h = GetVmm();
        if (!h)
            return buffer;
        if (VMMDLL_MemReadEx(h, static_cast<DWORD>(pid), address, reinterpret_cast<PBYTE>(&buffer), sizeof(T), &cbRead, 0) &&
            cbRead == sizeof(T)) {
            return buffer;
        }
        return T{};
    }

    template <typename T>
    bool Read(uint64_t address, T* buffer, size_t size, int pid) {
        if (!buffer || size == 0)
            return false;
        DWORD cbRead = 0;
        auto h = GetVmm();
        if (!h)
            return false;
        return VMMDLL_MemReadEx(h, static_cast<DWORD>(pid), address, reinterpret_cast<PBYTE>(buffer), static_cast<DWORD>(size), &cbRead, 0) &&
               cbRead == size;
    }

private:
    PCIMemory& m_mem;
};

template<typename T>
T PCIMemory::ReadEx(uintptr_t address) {
    T value{};
    TryRead(address, value, true);
    return value;
}

template<typename T>
bool PCIMemory::WriteEx(uintptr_t address, T value) {
    if (hVMM && processId) {
        const bool ok = VMMDLL_MemWrite(hVMM, processId, address, reinterpret_cast<PBYTE>(&value), sizeof(T)) == TRUE;
        RecordDirectWrite(sizeof(T), ok);
        return ok;
    }
    RecordDirectWrite(sizeof(T), false);
    return false;
}

template<typename T>
T PCIMemory::Read(uintptr_t address) {
    return ReadEx<T>(address);
}

template<typename T>
bool PCIMemory::Write(uintptr_t address, T value) {
    return WriteEx<T>(address, value);
}

inline bool PCIMemory::ReadVirtualMemoryEx(uintptr_t address, void* buffer, size_t size, bool useCache) {
    if (hVMM && processId && buffer && size) {
        DWORD cbRead = 0;
        const DWORD flags = useCache ? 0u : VMMDLL_FLAG_NOCACHE;
        const bool ok = VMMDLL_MemReadEx(hVMM, processId, address, static_cast<PBYTE>(buffer),
                            static_cast<DWORD>(size), &cbRead, flags)
            && cbRead == size;
        RecordDirectRead(size, ok);
        return ok;
    }
    RecordDirectRead(size, false);
    return false;
}

inline bool PCIMemory::ReadVirtualMemory(uintptr_t address, void* buffer, size_t size) {
    return ReadVirtualMemoryEx(address, buffer, size, true);
}

inline bool PCIMemory::ReadVirtualMemoryNoCache(uintptr_t address, void* buffer, size_t size) {
    return ReadVirtualMemoryEx(address, buffer, size, false);
}

inline void* PCIMemory::GetScatterHandle() {
    if (!hVMM || !processId) return nullptr;
    return reinterpret_cast<void*>(VMMDLL_Scatter_Initialize(hVMM, processId, VMMDLL_FLAG_NOCACHE));
}

inline void* PCIMemory::GetScatterHandleCached() {
    if (!hVMM || !processId) return nullptr;
    return reinterpret_cast<void*>(VMMDLL_Scatter_Initialize(hVMM, processId, 0));
}

inline bool PCIMemory::RequestReadScatter(void* handle, uintptr_t address, void* buffer, size_t size) {
    if (!handle || !buffer || size == 0) return false;
    DWORD cbRead = 0;
    const bool ok = VMMDLL_Scatter_PrepareEx(
        reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle),
        address,
        static_cast<DWORD>(size),
        static_cast<PBYTE>(buffer),
        &cbRead
    ) == TRUE;
    if (ok)
        RecordScatterRequest();
    return ok;
}

inline bool PCIMemory::ExecuteReadScatter(void* handle) {
    if (!handle || !hVMM || !processId)
        return false;
    const auto sh = reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle);
    const BOOL ok = VMMDLL_Scatter_ExecuteRead(sh);
    VMMDLL_Scatter_Clear(sh, processId, VMMDLL_FLAG_NOCACHE);
    RecordScatterBatch(ok == TRUE);
    return ok == TRUE;
}

inline bool PCIMemory::ExecuteReadScatterCached(void* handle) {
    if (!handle || !hVMM || !processId)
        return false;
    const auto sh = reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle);
    const BOOL ok = VMMDLL_Scatter_ExecuteRead(sh);
    VMMDLL_Scatter_Clear(sh, processId, 0);
    RecordScatterBatch(ok == TRUE);
    return ok == TRUE;
}

inline bool PCIMemory::ExecuteReadScatterEx(void* handle) {
    return ExecuteReadScatter(handle);
}

inline void PCIMemory::ClearScatterHandle(void* handle) {
    if (!handle || !hVMM || !processId) return;
    VMMDLL_Scatter_Clear(reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle), processId, VMMDLL_FLAG_NOCACHE);
}

inline void PCIMemory::ClearScatterHandleCached(void* handle) {
    if (!handle || !hVMM || !processId) return;
    VMMDLL_Scatter_Clear(reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle), processId, 0);
}

inline void PCIMemory::CloseScatterHandle(void* handle) {
    if (!handle) return;
    VMMDLL_Scatter_CloseHandle(reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle));
}
