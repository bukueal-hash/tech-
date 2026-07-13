#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include "DmaVmm.h"

struct vmProcess_t {
    DWORD dwProcID;
    uintptr_t dwModBase;
    DWORD dwImageSize;
};

inline vmProcess_t vmProcess = { 0 };

class PCIMemory {
private:
    static DWORD processId;
    static VMM_HANDLE hVMM;
    static uintptr_t moduleBase;
    static DWORD moduleImageSize;
    static bool s_memMap;
    static std::string attachedExe;

public:
    PCIMemory() = default;
    explicit PCIMemory(const char* gameExe, bool memMap = false) {
        s_memMap = memMap;  // Store memMap flag for use in Initialize()
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

    template<typename T>
    static T ReadEx(uintptr_t address);

    template<typename T>
    static bool WriteEx(uintptr_t address, T value);

    template<typename T>
    static T Read(uintptr_t address);

    template<typename T>
    static bool Write(uintptr_t address, T value);

    static bool ReadVirtualMemoryEx(uintptr_t address, void* buffer, size_t size);
    static bool ReadVirtualMemory(uintptr_t address, void* buffer, size_t size);
    /** Per-frame camera / actor positions — bypass VMM data cache. */
    static bool ReadVirtualMemoryNoCache(uintptr_t address, void* buffer, size_t size);

    bool read(uintptr_t address, void* buffer, size_t size) const;
    bool write(uintptr_t address, const void* buffer, size_t size);

    bool IsInitialized() const { return hVMM != nullptr; }

    static bool FullRefresh();

    static void* GetScatterHandle();
    static bool RequestReadScatter(void* handle, uintptr_t address, void* buffer, size_t size);
    static bool ExecuteReadScatter(void* handle);
    /** Execute + clear handle (safe to close after). */
    static bool ExecuteReadScatterEx(void* handle);
    /** Call between Prepare/Execute cycles on the same handle (see vmmdll VMMDLL_Scatter_Clear). */
    static void ClearScatterHandle(void* handle);
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
    if (hVMM && processId) {
        VMMDLL_MemReadEx(hVMM, processId, address, reinterpret_cast<PBYTE>(&value), sizeof(T), nullptr, 0);
    }
    return value;
}

template<typename T>
bool PCIMemory::WriteEx(uintptr_t address, T value) {
    if (hVMM && processId) {
        return VMMDLL_MemWrite(hVMM, processId, address, reinterpret_cast<PBYTE>(&value), sizeof(T)) == TRUE;
    }
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

inline bool PCIMemory::ReadVirtualMemoryEx(uintptr_t address, void* buffer, size_t size) {
    if (hVMM && processId) {
        DWORD cbRead = 0;
        return VMMDLL_MemReadEx(hVMM, processId, address, static_cast<PBYTE>(buffer), static_cast<DWORD>(size), &cbRead, 0)
            && cbRead == size;
    }
    return false;
}

inline bool PCIMemory::ReadVirtualMemory(uintptr_t address, void* buffer, size_t size) {
    return ReadVirtualMemoryEx(address, buffer, size);
}

inline bool PCIMemory::ReadVirtualMemoryNoCache(uintptr_t address, void* buffer, size_t size) {
    if (hVMM && processId) {
        DWORD cbRead = 0;
        return VMMDLL_MemReadEx(
                   hVMM,
                   processId,
                   address,
                   static_cast<PBYTE>(buffer),
                   static_cast<DWORD>(size),
                   &cbRead,
                   VMMDLL_FLAG_NOCACHE)
            && cbRead == size;
    }
    return false;
}

inline void* PCIMemory::GetScatterHandle() {
    if (!hVMM || !processId) return nullptr;
    return reinterpret_cast<void*>(VMMDLL_Scatter_Initialize(hVMM, processId, VMMDLL_FLAG_NOCACHE));
}

inline bool PCIMemory::RequestReadScatter(void* handle, uintptr_t address, void* buffer, size_t size) {
    if (!handle || !buffer || size == 0) return false;
    DWORD cbRead = 0;
    return VMMDLL_Scatter_PrepareEx(
        reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle),
        address,
        static_cast<DWORD>(size),
        static_cast<PBYTE>(buffer),
        &cbRead
    ) == TRUE;
}

inline bool PCIMemory::ExecuteReadScatter(void* handle) {
    if (!handle || !hVMM || !processId)
        return false;
    const auto sh = reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle);
    const BOOL ok = VMMDLL_Scatter_ExecuteRead(sh);
    VMMDLL_Scatter_Clear(sh, processId, VMMDLL_FLAG_NOCACHE);
    return ok == TRUE;
}

inline bool PCIMemory::ExecuteReadScatterEx(void* handle) {
    return ExecuteReadScatter(handle);
}

inline void PCIMemory::ClearScatterHandle(void* handle) {
    if (!handle || !hVMM || !processId) return;
    VMMDLL_Scatter_Clear(reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle), processId, VMMDLL_FLAG_NOCACHE);
}

inline void PCIMemory::CloseScatterHandle(void* handle) {
    if (!handle) return;
    VMMDLL_Scatter_CloseHandle(reinterpret_cast<VMMDLL_SCATTER_HANDLE>(handle));
}
