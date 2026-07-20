#include "DmaKeyboard.h"
#include "Memory.h"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

DmaKeyboard g_dmaKeyboard;

static constexpr uint64_t kKernelPtrMin = 0xFFFF000000000000ull;

bool DmaKeyboard::QueryWindowsBuild(int& buildOut) const {
    buildOut = 0;
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD build = 0;
    DWORD buildSize = sizeof(build);
    if (RegQueryValueExA(hKey, "CurrentBuild", nullptr, nullptr, reinterpret_cast<LPBYTE>(&build), &buildSize) !=
        ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return false;
    }
    RegCloseKey(hKey);
    buildOut = static_cast<int>(build);
    return buildOut > 0;
}

bool DmaKeyboard::Init() {
    ready_ = false;
    gafAsyncKeyStateExport_ = 0;
    consecutiveFailures_ = 0;
    if (!g_mem.IsInitialized()) {
        printf("[DmaKeyboard] DMA not initialized\n");
        return false;
    }
    if (!InitKeyboard()) {
        printf("[DmaKeyboard] init failed\n");
        return false;
    }
    ready_ = true;
    printf("[DmaKeyboard] OK export=0x%llX\n", static_cast<unsigned long long>(gafAsyncKeyStateExport_));
    return true;
}

bool DmaKeyboard::InitKeyboard() {
    int winver = 0;
    if (!QueryWindowsBuild(winver))
        return false;

    DmaMem dma(g_mem);
    VMM_HANDLE vmm = dma.GetVmm();
    if (!vmm)
        return false;

    winlogonPid_ = static_cast<int>(g_mem.GetPidFromName("winlogon.exe"));
    if (winlogonPid_ == 0)
        return false;

    if (winver > 22000) {
        const std::vector<DWORD> pids = g_mem.GetPidListFromName("csrss.exe");
        for (DWORD pid : pids) {
            PVMMDLL_MAP_MODULEENTRY win32kModuleInfo = nullptr;
            if (!VMMDLL_Map_GetModuleFromNameW(vmm, pid, const_cast<LPWSTR>(L"win32ksgd.sys"), &win32kModuleInfo,
                    VMMDLL_MODULE_FLAG_NORMAL)) {
                if (!VMMDLL_Map_GetModuleFromNameW(vmm, pid, const_cast<LPWSTR>(L"win32k.sys"), &win32kModuleInfo,
                        VMMDLL_MODULE_FLAG_NORMAL)) {
                    continue;
                }
            }

            const uintptr_t win32kBase = win32kModuleInfo->vaBase;
            const size_t win32kSize = win32kModuleInfo->cbImageSize;

            uint64_t gSessionPtr = dma.FindSignature("48 8B 05 ? ? ? ? 48 8B 04 C8", win32kBase, win32kBase + win32kSize,
                static_cast<int>(pid));
            if (!gSessionPtr) {
                gSessionPtr = dma.FindSignature("48 8B 05 ? ? ? ? FF C9", win32kBase, win32kBase + win32kSize,
                    static_cast<int>(pid));
                if (!gSessionPtr)
                    continue;
            }

            const int relative = dma.Read<int>(gSessionPtr + 3, static_cast<int>(pid));
            const uintptr_t gSessionGlobalSlots = gSessionPtr + 7 + relative;
            uintptr_t userSessionState = 0;
            for (int i = 0; i < 4; ++i) {
                userSessionState = dma.Read<uintptr_t>(
                    dma.Read<uintptr_t>(dma.Read<uintptr_t>(gSessionGlobalSlots, static_cast<int>(pid)) + 8ull * i,
                        static_cast<int>(pid)),
                    static_cast<int>(pid));
                if (userSessionState > 0x7FFFFFFFFFFF)
                    break;
            }

            PVMMDLL_MAP_MODULEENTRY win32kbaseModuleInfo = nullptr;
            if (!VMMDLL_Map_GetModuleFromNameW(vmm, pid, const_cast<LPWSTR>(L"win32kbase.sys"), &win32kbaseModuleInfo,
                    VMMDLL_MODULE_FLAG_NORMAL)) {
                continue;
            }

            const uintptr_t win32kbaseBase = win32kbaseModuleInfo->vaBase;
            const size_t win32kbaseSize = win32kbaseModuleInfo->cbImageSize;
            const uint64_t ptr = dma.FindSignature("48 8D 90 ? ? ? ? E8 ? ? ? ? 0F 57 C0", win32kbaseBase,
                win32kbaseBase + win32kbaseSize, static_cast<int>(pid));
            if (!ptr)
                continue;

            const uint32_t sessionOffset = dma.Read<uint32_t>(ptr + 3, static_cast<int>(pid));
            gafAsyncKeyStateExport_ = userSessionState + sessionOffset;
            if (gafAsyncKeyStateExport_ > kKernelPtrMin)
                return true;
        }
        return false;
    }

    const DWORD kernelPid =
        g_mem.GetPidFromName("winlogon.exe") | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY;

    PVMMDLL_MAP_EAT eatMap = nullptr;
    if (!VMMDLL_Map_GetEATU(vmm, kernelPid, const_cast<LPSTR>("win32kbase.sys"), &eatMap))
        return false;

    if (eatMap->dwVersion != VMMDLL_MAP_EAT_VERSION) {
        VMMDLL_MemFree(eatMap);
        return false;
    }

    for (DWORD i = 0; i < eatMap->cMap; ++i) {
        const PVMMDLL_MAP_EATENTRY entry = eatMap->pMap + i;
        if (strcmp(entry->uszFunction, "gafAsyncKeyState") == 0) {
            gafAsyncKeyStateExport_ = entry->vaFunction;
            break;
        }
    }
    VMMDLL_MemFree(eatMap);

    if (gafAsyncKeyStateExport_ < kKernelPtrMin) {
        PVMMDLL_MAP_MODULEENTRY moduleInfo = nullptr;
        if (!VMMDLL_Map_GetModuleFromNameW(vmm, kernelPid, L"win32kbase.sys", &moduleInfo, VMMDLL_MODULE_FLAG_NORMAL))
            return false;

        char pdbName[32]{};
        if (!VMMDLL_PdbLoad(vmm, kernelPid, moduleInfo->vaBase, pdbName))
            return false;

        uintptr_t gafAsyncKeyState = 0;
        if (!VMMDLL_PdbSymbolAddress(vmm, pdbName, const_cast<LPSTR>("gafAsyncKeyState"), &gafAsyncKeyState))
            return false;
        gafAsyncKeyStateExport_ = gafAsyncKeyState;
    }

    return gafAsyncKeyStateExport_ > kKernelPtrMin;
}

void DmaKeyboard::UpdateKeys() {
    if (!g_mem.IsInitialized() || gafAsyncKeyStateExport_ < kKernelPtrMin)
        return;

    VMM_HANDLE vmm = PCIMemory::GetVmmHandle();
    if (!vmm)
        return;

    uint8_t tempBitmap[64]{};
    const BOOL readOk = VMMDLL_MemReadEx(vmm,
        static_cast<DWORD>(winlogonPid_) | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY,
        gafAsyncKeyStateExport_,
        reinterpret_cast<PBYTE>(tempBitmap),
        sizeof(tempBitmap),
        nullptr,
        VMMDLL_FLAG_NOCACHE);

    if (!readOk) {
        ++consecutiveFailures_;
        if (consecutiveFailures_ == 5) {
            const int newPid = static_cast<int>(g_mem.GetPidFromName("winlogon.exe"));
            if (newPid != 0 && newPid != winlogonPid_) {
                winlogonPid_ = newPid;
                consecutiveFailures_ = 0;
                ready_ = InitKeyboard();
            }
        } else if (consecutiveFailures_ > 50) {
            consecutiveFailures_ = 50;
        }
        return;
    }

    consecutiveFailures_ = 0;
    ready_ = true;
    memcpy(stateBitmap_, tempBitmap, sizeof(stateBitmap_));
}

bool DmaKeyboard::IsKeyDown(uint32_t virtualKeyCode) {
    if (gafAsyncKeyStateExport_ < kKernelPtrMin)
        return false;

    static auto lastUpdate = std::chrono::system_clock::now();
    const auto now = std::chrono::system_clock::now();
    if (now - lastUpdate > std::chrono::milliseconds(100)) {
        UpdateKeys();
        lastUpdate = now;
    }

    return (stateBitmap_[(virtualKeyCode * 2) / 8] & (1 << (virtualKeyCode % 4) * 2)) != 0;
}
